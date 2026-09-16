#include "runtime/sql_pool.hpp"

#include <cstdlib>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace tilt::rt {

namespace {

struct IdleEntry {
  void* handle = nullptr;
  PooledConn::CloseFn close;
};

struct Registry {
  std::mutex mu;
  std::unordered_map<std::string, std::vector<IdleEntry>> idle;

  ~Registry() {
    // Fecha ociosas no fim do processo (as em uso fecham no proprio release).
    for (auto& [key, vec] : idle) {
      for (auto& e : vec) {
        if (e.handle) {
          try {
            e.close(e.handle);
          } catch (...) {
          }
        }
      }
    }
  }
};

Registry& registry() {
  static Registry reg;
  return reg;
}

bool pool_off() {
  static const bool off = [] {
    const char* v = std::getenv("TILT_SQL_POOL");
    return v && std::string(v) == "0";
  }();
  return off;
}

int pool_max() {
  static const int max = [] {
    if (const char* v = std::getenv("TILT_SQL_POOL_MAX")) {
      const int n = std::atoi(v);
      if (n > 0) return n;
    }
    return 8;
  }();
  return max;
}

bool pool_debug() {
  static const bool dbg = [] {
    const char* v = std::getenv("TILT_SQL_POOL_DEBUG");
    return v && std::string(v) == "1";
  }();
  return dbg;
}

void log_pool(const std::string& backend, const char* what) {
  if (!pool_debug()) return;
  static std::mutex log_mu;
  std::lock_guard<std::mutex> lk(log_mu);
  std::cerr << "[pool " << what << "] " << backend << "\n";
}

// Primeira palavra do SQL (maiuscula, sem espacos a esquerda). Statements que
// abrem estado de sessao/transacao nao podem voltar ao pool.
bool abre_estado(const std::string& sql) {
  std::size_t i = 0;
  while (i < sql.size() && (sql[i] == ' ' || sql[i] == '\t' || sql[i] == '\n' || sql[i] == '\r')) {
    ++i;
  }
  std::string kw;
  while (i < sql.size() && ((sql[i] >= 'a' && sql[i] <= 'z') || (sql[i] >= 'A' && sql[i] <= 'Z'))) {
    kw.push_back(static_cast<char>(sql[i] >= 'a' ? sql[i] - ('a' - 'A') : sql[i]));
    ++i;
  }
  if (kw == "BEGIN" || kw == "START" || kw == "SET") return true;
  return false;
}

}  // namespace

PooledConn::PooledConn(std::string backend, std::string dsn, OpenFn open, AliveFn alive,
                       CloseFn close, const std::string& sql_hint)
    : backend_(std::move(backend)), close_(std::move(close)) {
  if (backend_.empty()) {  // bypass: dedicada, sem registro
    handle_ = open();
    return;
  }
  key_ = backend_ + '\0' + dsn;
  if (!sql_hint.empty() && abre_estado(sql_hint)) pool_back_ = false;
  if (pool_off()) {
    handle_ = open();
    return;
  }
  // Retira uma ociosa (fora do mutex valida e abre, para nao segurar o lock
  // em IO de rede).
  IdleEntry cand{};
  {
    std::lock_guard<std::mutex> lk(registry().mu);
    auto it = registry().idle.find(key_);
    if (it != registry().idle.end() && !it->second.empty()) {
      cand = std::move(it->second.back());
      it->second.pop_back();
    }
  }
  if (cand.handle) {
    bool viva = false;
    try {
      viva = alive(cand.handle);
    } catch (...) {
      viva = false;
    }
    if (viva) {
      handle_ = cand.handle;
      log_pool(backend_, "hit");
      return;
    }
    try {
      cand.close(cand.handle);
    } catch (...) {
    }
    log_pool(backend_, "stale");
  }
  handle_ = open();
  log_pool(backend_, "miss");
}

PooledConn::~PooledConn() {
  if (!handle_) return;
  const char* motivo = nullptr;
  if (!backend_.empty() && pool_back_ && !pool_off()) {
    std::lock_guard<std::mutex> lk(registry().mu);
    auto& vec = registry().idle[key_];
    if (static_cast<int>(vec.size()) < pool_max()) {
      vec.push_back({handle_, close_});
      return;
    }
    motivo = "full";
  } else if (!pool_back_) {
    motivo = "discard";
  }
  // Fecha fora do mutex (close pode fazer IO).
  try {
    close_(handle_);
  } catch (...) {
  }
  if (motivo) log_pool(backend_, motivo);
}

}  // namespace tilt::rt
