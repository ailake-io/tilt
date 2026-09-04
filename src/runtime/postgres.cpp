#include "runtime/postgres.hpp"

#include <dlfcn.h>

#include <cstdlib>
#include <stdexcept>

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("postgres: " + m); }

// Constantes do libpq-fe.h / catalog OIDs (não incluímos os headers).
constexpr int kConnectionOk = 0;
constexpr int kPgTuplesOk = 2;
constexpr int kPgCommandOk = 1;
// OIDs de tipos escalares do catálogo pg_type.
constexpr unsigned kOidBool = 16;
constexpr unsigned kOidInt8 = 20;
constexpr unsigned kOidInt2 = 21;
constexpr unsigned kOidInt4 = 23;
constexpr unsigned kOidFloat4 = 700;
constexpr unsigned kOidFloat8 = 701;
constexpr unsigned kOidNumeric = 1700;

struct PqApi {
  void* lib = nullptr;
  void* (*connectdb)(const char*) = nullptr;
  int (*status)(const void*) = nullptr;
  void* (*exec)(void*, const char*) = nullptr;
  int (*result_status)(const void*) = nullptr;
  int (*ntuples)(const void*) = nullptr;
  int (*nfields)(const void*) = nullptr;
  char* (*fname)(const void*, int) = nullptr;
  unsigned (*ftype)(const void*, int) = nullptr;
  char* (*getvalue)(const void*, int, int) = nullptr;
  int (*getisnull)(const void*, int, int) = nullptr;
  void (*clear)(void*) = nullptr;
  void (*finish)(void*) = nullptr;
  char* (*error_message)(const void*) = nullptr;
  void (*set_notice_processor)(void*, void (*)(void*, const char*), void*) = nullptr;
};

template <typename F>
bool bind_sym(void* lib, F& fn, const char* name) {
  fn = reinterpret_cast<F>(::dlsym(lib, name));
  return fn != nullptr;
}

// Carrega a biblioteca uma única vez. Falha de carga não lança aqui:
// postgres_query() verifica `lib` e morre com mensagem acionável.
const PqApi& api() {
  static const PqApi instance = [] {
    PqApi a;
    a.lib = ::dlopen("libpq.so.5", RTLD_NOW | RTLD_LOCAL);
    if (!a.lib) return a;
    const bool ok = bind_sym(a.lib, a.connectdb, "PQconnectdb") &&
                    bind_sym(a.lib, a.status, "PQstatus") &&
                    bind_sym(a.lib, a.exec, "PQexec") &&
                    bind_sym(a.lib, a.result_status, "PQresultStatus") &&
                    bind_sym(a.lib, a.ntuples, "PQntuples") &&
                    bind_sym(a.lib, a.nfields, "PQnfields") &&
                    bind_sym(a.lib, a.fname, "PQfname") &&
                    bind_sym(a.lib, a.ftype, "PQftype") &&
                    bind_sym(a.lib, a.getvalue, "PQgetvalue") &&
                    bind_sym(a.lib, a.getisnull, "PQgetisnull") &&
                    bind_sym(a.lib, a.clear, "PQclear") &&
                    bind_sym(a.lib, a.finish, "PQfinish") &&
                    bind_sym(a.lib, a.error_message, "PQerrorMessage") &&
                    bind_sym(a.lib, a.set_notice_processor, "PQsetNoticeProcessor");
    if (!ok) {
      ::dlclose(a.lib);
      a = PqApi{};
    }
    return a;
  }();
  return instance;
}

// Silencia NOTICE/aviso do servidor (ex.: "extension already exists") para
// nao poluir o stderr do programa tilt.
void swallow_notice(void*, const char*) {}

void* connect_or_die(const PqApi& pq, const std::string& url) {
  void* conn = pq.connectdb(url.c_str());
  if (!conn) die("falha de memoria ao conectar");
  if (pq.set_notice_processor) pq.set_notice_processor(conn, swallow_notice, nullptr);
  if (pq.status(conn) != kConnectionOk) {
    const std::string msg = pq.error_message(conn) ? pq.error_message(conn) : "erro desconhecido";
    pq.finish(conn);
    die("falha na conexao: " + msg);
  }
  return conn;
}

}  // namespace

Value postgres_query(const std::string& url, const std::string& sql) {
  const PqApi& pq = api();
  if (!pq.lib) die("libpq.so.5 nao encontrada; instale o pacote libpq5");

  void* conn = connect_or_die(pq, url);

  void* res = pq.exec(conn, sql.c_str());
  if (!res) {
    const std::string msg = pq.error_message(conn) ? pq.error_message(conn) : "erro desconhecido";
    pq.finish(conn);
    die("falha ao executar consulta: " + msg);
  }
  if (pq.result_status(res) != kPgTuplesOk) {
    const char* err = pq.error_message(conn);
    const std::string detail = (err && *err) ? ": " + std::string(err) : "";
    pq.clear(res);
    pq.finish(conn);
    die("apenas consultas SELECT sao suportadas nesta versao" + detail);
  }

  const int nrows = pq.ntuples(res);
  const int ncols = pq.nfields(res);
  ValueList rows;
  rows.reserve(static_cast<std::size_t>(nrows));
  for (int r = 0; r < nrows; ++r) {
    Value row = Value::mapa();
    for (int c = 0; c < ncols; ++c) {
      const char* name = pq.fname(res, c);
      const std::string col = name ? name : ("coluna" + std::to_string(c + 1));
      if (pq.getisnull(res, r, c)) {
        row.map->set(col, Value::nulo());
        continue;
      }
      const char* raw = pq.getvalue(res, r, c);
      const std::string val = raw ? raw : "";
      switch (pq.ftype(res, c)) {
        case kOidBool:
          row.map->set(col, Value::logico(!val.empty() && val[0] == 't'));
          break;
        case kOidInt8:
        case kOidInt2:
        case kOidInt4:
          row.map->set(col, Value::inteiro(std::strtoll(val.c_str(), nullptr, 10)));
          break;
        case kOidFloat4:
        case kOidFloat8:
        case kOidNumeric:
          row.map->set(col, Value::decimal(std::strtod(val.c_str(), nullptr)));
          break;
        default:
          row.map->set(col, Value::texto(val));
          break;
      }
    }
    rows.push_back(std::move(row));
  }

  pq.clear(res);
  pq.finish(conn);
  return Value::tabela(std::move(rows));
}

void postgres_exec(const std::string& url, const std::string& sql) {
  const PqApi& pq = api();
  if (!pq.lib) die("libpq.so.5 nao encontrada; instale o pacote libpq5");

  void* conn = connect_or_die(pq, url);
  void* res = pq.exec(conn, sql.c_str());
  if (!res) {
    const std::string msg = pq.error_message(conn) ? pq.error_message(conn) : "erro desconhecido";
    pq.finish(conn);
    die("falha ao executar comando: " + msg);
  }
  const int status = pq.result_status(res);
  const char* err = pq.error_message(conn);
  pq.clear(res);
  pq.finish(conn);
  if (status != kPgCommandOk && status != kPgTuplesOk) {
    die(std::string("comando rejeitado pelo servidor: ") + (err && *err ? err : "erro desconhecido"));
  }
}

}  // namespace tilt::rt
