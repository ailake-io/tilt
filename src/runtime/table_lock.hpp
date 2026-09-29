#pragma once

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "runtime/compat.hpp"

namespace tilt::rt {

// Lock cooperativo de tabela. O mkdir do diretorio e atomico no filesystem,
// portanto dois processos nunca entram na mesma secao critica. O lock nao e
// removido automaticamente depois do timeout: isso evita que um processo
// mate uma escrita ainda ativa; o operador pode remover um .tilt.lock.d
// abandonado depois de verificar o processo indicado em owner.
class TableLock {
 public:
  explicit TableLock(std::string table_dir, const char* kind = "tabela")
      : lock_dir_(std::move(table_dir) + "/.tilt.lock.d"), kind_(kind) {
    std::error_code parent_error;
    std::filesystem::create_directories(std::filesystem::path(lock_dir_).parent_path(), parent_error);
    if (parent_error) {
      throw std::runtime_error("nao foi possivel preparar lock de " + kind_ + ": " +
                               parent_error.message());
    }
    for (const std::string& held : held_locks()) {
      if (held == lock_dir_) {
        reentrant_ = true;
        return;
      }
    }
    int timeout_ms = 30000;
    if (const char* raw = std::getenv("TILT_TABLE_LOCK_TIMEOUT_MS")) {
      try {
        const int parsed = std::stoi(raw);
        if (parsed >= 0) timeout_ms = parsed;
      } catch (...) {
      }
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (true) {
      if (tilt_mkdir(lock_dir_) == 0) {
        std::ofstream owner(lock_dir_ + "/owner", std::ios::trunc);
        if (owner) {
          owner << "pid=" << tilt_getpid() << "\n";
          owner << "kind=" << kind_ << "\n";
          owner << "started_ms="
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count()
                << "\n";
        }
        locked_ = true;
        held_locks().push_back(lock_dir_);
        return;
      }
      if (errno != EEXIST) {
        throw std::runtime_error("nao foi possivel criar lock de " + kind_ + " em '" +
                                 lock_dir_ + "'");
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        throw std::runtime_error("timeout aguardando lock de " + kind_ + " em '" +
                                 lock_dir_ + "' (verifique owner)");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }

  TableLock(const TableLock&) = delete;
  TableLock& operator=(const TableLock&) = delete;

  ~TableLock() {
    if (reentrant_ || !locked_) return;
    auto& held = held_locks();
    held.erase(std::remove(held.begin(), held.end(), lock_dir_), held.end());
    std::error_code ec;
    std::filesystem::remove_all(lock_dir_, ec);
  }

 private:
  static std::vector<std::string>& held_locks() {
    thread_local std::vector<std::string> held;
    return held;
  }

  std::string lock_dir_;
  std::string kind_;
  bool locked_ = false;
  bool reentrant_ = false;
};

}  // namespace tilt::rt
