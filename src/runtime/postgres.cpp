#include "runtime/postgres.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <utility>

#include "runtime/compat.hpp"
#include "runtime/sql_params.hpp"
#include "runtime/sql_pool.hpp"

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
  char* (*error_message)(void*) = nullptr;
  void (*set_notice_processor)(void*, void (*)(void*, const char*), void*) = nullptr;
  // Ligacao de parametros (Marco 3 / D1): PQexecParams com tudo em texto.
  void* (*exec_params)(void*, const char*, int, const unsigned*, const char* const*, const int*,
                       const int*, int) = nullptr;
};

template <typename F>
bool bind_sym(void* lib, F& fn, const char* name) {
  fn = reinterpret_cast<F>(tilt_dlsym(lib, name));
  return fn != nullptr;
}

// Carrega a biblioteca uma única vez. Falha de carga não lança aqui:
// postgres_query() verifica `lib` e morre com mensagem acionável.
const PqApi& api() {
  static const PqApi instance = [] {
    PqApi a;
#if defined(_WIN32)
    a.lib = tilt_dlopen("libpq.dll");
#else
    a.lib = tilt_dlopen("libpq.so.5");
    if (!a.lib) a.lib = tilt_dlopen("libpq.so");
    if (!a.lib) a.lib = tilt_dlopen("libpq.5.dylib");
    if (!a.lib) a.lib = tilt_dlopen("libpq.dylib");
#endif
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
                    bind_sym(a.lib, a.set_notice_processor, "PQsetNoticeProcessor") &&
                    bind_sym(a.lib, a.exec_params, "PQexecParams");
    if (!ok) {
      tilt_dlclose(a.lib);
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

// Materializa um PGresult de tuplas em tabela (tipos por OID do catalogo).
Value materializa_pg(const PqApi& pq, void* res) {
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
  return Value::tabela(std::move(rows));
}

}  // namespace

Value postgres_query(const std::string& url, const std::string& sql) {
  const PqApi& pq = api();
  if (!pq.lib) {
#if defined(_WIN32)
    die("libpq.dll nao encontrada; instale o PostgreSQL client para Windows");
#else
    die("libpq.so.5 nao encontrada; instale o pacote libpq5");
#endif
  }

  // Pool por (backend, url): a conexao volta ao idle no fim do escopo
  // (validada com PQstatus na proxima aquisicao). Erros dao die() e o
  // destructor recicla — sem pq.finish manual aqui.
  PooledConn pool("postgres", url, [&] { return connect_or_die(pq, url); },
                  [&](void* h) { return pq.status(h) == kConnectionOk; },
                  [&](void* h) { pq.finish(h); }, sql);
  void* conn = pool.get();

  void* res = pq.exec(conn, sql.c_str());
  if (!res) {
    const std::string msg = pq.error_message(conn) ? pq.error_message(conn) : "erro desconhecido";
    die("falha ao executar consulta: " + msg);
  }
  if (pq.result_status(res) != kPgTuplesOk) {
    const char* err = pq.error_message(conn);
    const std::string detail = (err && *err) ? ": " + std::string(err) : "";
    pq.clear(res);
    die("apenas consultas SELECT sao suportadas nesta versao" + detail);
  }

  Value out = materializa_pg(pq, res);
  pq.clear(res);
  return out;
}

void postgres_exec(const std::string& url, const std::string& sql) {
  const PqApi& pq = api();
  if (!pq.lib) {
#if defined(_WIN32)
    die("libpq.dll nao encontrada; instale o PostgreSQL client para Windows");
#else
    die("libpq.so.5 nao encontrada; instale o pacote libpq5");
#endif
  }

  PooledConn pool("postgres", url, [&] { return connect_or_die(pq, url); },
                  [&](void* h) { return pq.status(h) == kConnectionOk; },
                  [&](void* h) { pq.finish(h); }, sql);
  void* conn = pool.get();
  void* res = pq.exec(conn, sql.c_str());
  if (!res) {
    const std::string msg = pq.error_message(conn) ? pq.error_message(conn) : "erro desconhecido";
    die("falha ao executar comando: " + msg);
  }
  const int status = pq.result_status(res);
  const std::string err = pq.error_message(conn) ? pq.error_message(conn) : "";
  pq.clear(res);
  if (status != kPgCommandOk && status != kPgTuplesOk) {
    die("comando rejeitado pelo servidor: " + (err.empty() ? "erro desconhecido" : err));
  }
}

// Formata um parametro em texto para PQexecParams (resultFormat 0).
std::string pg_param_texto(const SqlParam& p) {
  switch (p.tipo) {
    case SqlParam::Tipo::Inteiro: return std::to_string(p.i);
    case SqlParam::Tipo::Decimal: {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.17g", p.d);
      return buf;
    }
    case SqlParam::Tipo::Texto: return p.s;
    case SqlParam::Tipo::Logico: return p.b ? "true" : "false";
    case SqlParam::Tipo::Nulo: return "";
  }
  return "";
}

bool pg_termina_palavra(const std::string& sql, const std::string& palavra,
                        std::size_t* inicio = nullptr) {
  std::size_t fim = sql.size();
  while (fim > 0 && std::isspace(static_cast<unsigned char>(sql[fim - 1]))) --fim;
  if (fim < palavra.size()) return false;
  const std::size_t inicio_palavra = fim - palavra.size();
  for (std::size_t k = 0; k < palavra.size(); ++k) {
    const unsigned char a = static_cast<unsigned char>(sql[inicio_palavra + k]);
    const unsigned char b = static_cast<unsigned char>(palavra[k]);
    if (std::toupper(a) != std::toupper(b)) return false;
  }
  if (fim > palavra.size() &&
      (std::isalnum(static_cast<unsigned char>(sql[fim - palavra.size() - 1])) ||
       sql[fim - palavra.size() - 1] == '_')) {
    return false;
  }
  if (inicio) *inicio = fim - palavra.size();
  return true;
}

// PostgreSQL nao aceita `IS $1`; quando o parametro e nulo, a forma SQL
// equivalente e `IS NULL` (ou `IS NOT NULL`) sem bind para esse placeholder.
std::string pg_rewrite_sql(const std::string& sql, const std::vector<SqlParam>& params,
                           std::vector<std::size_t>& bind_indices) {
  std::string out;
  std::size_t raw = 0;
  varrer_sql(
      sql,
      [&](std::string& emitted) {
        bool is_context = false;
        std::size_t is_start = 0;
        if (pg_termina_palavra(emitted, "IS", &is_start)) {
          is_context = true;
        } else {
          std::size_t not_start = 0;
          if (pg_termina_palavra(emitted, "NOT", &not_start)) {
            const std::string before_not = emitted.substr(0, not_start);
            is_context = pg_termina_palavra(before_not, "IS", &is_start);
          }
        }
        if (raw < params.size() && params[raw].tipo == SqlParam::Tipo::Nulo && is_context) {
          emitted.resize(is_start);
          emitted += "IS NULL";
        } else {
          bind_indices.push_back(raw);
          emitted += "$" + std::to_string(bind_indices.size());
        }
        ++raw;
      },
      out);
  return out;
}

// Executa numa conexao aberta, com `?` reescritos para $N e ligados em texto.
void exec_um(const PqApi& pq, void* conn, const std::string& sql,
             const std::vector<SqlParam>& params, const std::string& passo) {
  const auto nq = rewrite_qmarks(sql, "nenhum").second;
  if (nq != params.size()) {
    die(passo + "esperava " + std::to_string(params.size()) + " parametro(s), mas o SQL tem " +
        std::to_string(nq) + " '?'");
  }
  std::vector<std::size_t> bind_indices;
  const std::string reescrito = pg_rewrite_sql(sql, params, bind_indices);
  std::vector<std::string> textos;
  std::vector<const char*> valores;
  textos.reserve(bind_indices.size());
  valores.reserve(bind_indices.size());
  for (std::size_t bind_index : bind_indices) {
    const SqlParam& p = params[bind_index];
    if (p.tipo == SqlParam::Tipo::Nulo) {
      valores.push_back(nullptr);  // NULL de verdade
    } else {
      textos.push_back(pg_param_texto(p));
      valores.push_back(textos.back().c_str());
    }
  }
  void* res = pq.exec_params(conn, reescrito.c_str(), static_cast<int>(valores.size()), nullptr,
                             valores.data(), nullptr, nullptr, 0);
  if (!res) {
    const std::string msg = pq.error_message(conn) ? pq.error_message(conn) : "erro desconhecido";
    die(passo + "falha ao executar comando: " + msg);
  }
  const int status = pq.result_status(res);
  const std::string err = pq.error_message(conn) ? pq.error_message(conn) : "";
  pq.clear(res);
  if (status != kPgCommandOk && status != kPgTuplesOk) {
    die(passo + "comando rejeitado pelo servidor: " + (err.empty() ? "erro desconhecido" : err));
  }
}

void postgres_exec_params(const std::string& url, const std::string& sql,
                          const std::vector<SqlParam>& params) {
  const PqApi& pq = api();
  if (!pq.lib) {
#if defined(_WIN32)
    die("libpq.dll nao encontrada; instale o PostgreSQL client para Windows");
#else
    die("libpq.so.5 nao encontrada; instale o pacote libpq5");
#endif
  }
  PooledConn pool("postgres", url, [&] { return connect_or_die(pq, url); },
                  [&](void* h) { return pq.status(h) == kConnectionOk; },
                  [&](void* h) { pq.finish(h); }, sql);
  exec_um(pq, pool.get(), sql, params, "");
}

// Consulta com `?` ligados em texto via PQexecParams (SELECT com params).
Value postgres_query_params(const std::string& url, const std::string& sql,
                            const std::vector<SqlParam>& params) {
  const PqApi& pq = api();
  if (!pq.lib) {
#if defined(_WIN32)
    die("libpq.dll nao encontrada; instale o PostgreSQL client para Windows");
#else
    die("libpq.so.5 nao encontrada; instale o pacote libpq5");
#endif
  }
  const auto nq = rewrite_qmarks(sql, "nenhum").second;
  if (nq != params.size()) {
    die("esperava " + std::to_string(params.size()) + " parametro(s), mas o SQL tem " +
        std::to_string(nq) + " '?'");
  }
  std::vector<std::size_t> bind_indices;
  const std::string reescrito = pg_rewrite_sql(sql, params, bind_indices);
  std::vector<std::string> textos;
  std::vector<const char*> valores;
  textos.reserve(bind_indices.size());
  valores.reserve(bind_indices.size());
  for (std::size_t bind_index : bind_indices) {
    const SqlParam& p = params[bind_index];
    if (p.tipo == SqlParam::Tipo::Nulo) {
      valores.push_back(nullptr);
    } else {
      textos.push_back(pg_param_texto(p));
      valores.push_back(textos.back().c_str());
    }
  }
  PooledConn pool(
      "postgres", url, [&] { return connect_or_die(pq, url); },
      [&](void* h) { return pq.status(h) == kConnectionOk; }, [&](void* h) { pq.finish(h); }, sql);
  void* conn = pool.get();
  void* res = pq.exec_params(conn, reescrito.c_str(), static_cast<int>(valores.size()), nullptr,
                             valores.data(), nullptr, nullptr, 0);
  if (!res) {
    const std::string msg = pq.error_message(conn) ? pq.error_message(conn) : "erro desconhecido";
    die("falha ao executar consulta: " + msg);
  }
  if (pq.result_status(res) != kPgTuplesOk) {
    const char* err = pq.error_message(conn);
    const std::string detail = (err && *err) ? ": " + std::string(err) : "";
    pq.clear(res);
    die("apenas consultas SELECT sao suportadas nesta versao" + detail);
  }
  Value out = materializa_pg(pq, res);
  pq.clear(res);
  return out;
}

void postgres_transact(const std::string& url,
                       const std::vector<std::pair<std::string, std::vector<SqlParam>>>& passos) {
  const PqApi& pq = api();
  if (!pq.lib) {
#if defined(_WIN32)
    die("libpq.dll nao encontrada; instale o PostgreSQL client para Windows");
#else
    die("libpq.so.5 nao encontrada; instale o pacote libpq5");
#endif
  }
  auto simples = [&](void* conn, const char* sql) {
    void* res = pq.exec(conn, sql);
    const int status = res ? pq.result_status(res) : -1;
    const std::string err =
        (res && status != kPgCommandOk) && pq.error_message(conn) ? pq.error_message(conn) : "";
    if (res) pq.clear(res);
    if (!res || status != kPgCommandOk) {
      die(std::string("falha em ") + sql + (err.empty() ? "" : ": " + err));
    }
  };
  void* conn = connect_or_die(pq, url);
  try {
    simples(conn, "BEGIN");
    for (std::size_t k = 0; k < passos.size(); ++k) {
      try {
        exec_um(pq, conn, passos[k].first, passos[k].second,
                "passo " + std::to_string(k + 1) + ": ");
      } catch (...) {
        try {
          simples(conn, "ROLLBACK");
        } catch (...) {
        }
        throw;
      }
    }
    simples(conn, "COMMIT");
  } catch (...) {
    pq.finish(conn);
    throw;
  }
  pq.finish(conn);
}

}  // namespace tilt::rt
