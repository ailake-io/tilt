#include "runtime/sqlite.hpp"

#include "runtime/compat.hpp"
#include "runtime/sql_params.hpp"

#include <cstdint>
#include <stdexcept>

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("sqlite: " + m); }

// Constantes do sqlite3.h (não incluímos o header para manter zero deps).
constexpr int kSqliteOk = 0;
constexpr int kSqliteRow = 100;
constexpr int kSqliteDone = 101;
constexpr int kSqliteOpenReadwrite = 0x00000002;
constexpr int kSqliteOpenCreate = 0x00000004;
constexpr int kSqliteInteger = 1;
constexpr int kSqliteFloat = 2;
constexpr int kSqliteText = 3;
constexpr int kSqliteBlob = 4;
constexpr int kSqliteNull = 5;

struct SqliteApi {
  void* lib = nullptr;
  int (*open_v2)(const char*, void**, int, const char*) = nullptr;
  int (*prepare_v2)(void*, const char*, int, void**, const void*) = nullptr;
  int (*step)(void*) = nullptr;
  int (*column_count)(void*) = nullptr;
  const char* (*column_name)(void*, int) = nullptr;
  int (*column_type)(void*, int) = nullptr;
  long long (*column_int64)(void*, int) = nullptr;
  double (*column_double)(void*, int) = nullptr;
  const unsigned char* (*column_text)(void*, int) = nullptr;
  const void* (*column_blob)(void*, int) = nullptr;
  int (*column_bytes)(void*, int) = nullptr;
  int (*finalize)(void*) = nullptr;
  int (*close)(void*) = nullptr;
  const char* (*errmsg)(void*) = nullptr;
  // Ligacao de parametros (Marco 3 / D1): `?` posicionais.
  int (*bind_int64)(void*, int, long long) = nullptr;
  int (*bind_double)(void*, int, double) = nullptr;
  int (*bind_text)(void*, int, const char*, int, void (*)(void*)) = nullptr;
  int (*bind_null)(void*, int) = nullptr;
  int (*bind_count)(void*) = nullptr;
};

template <typename F>
bool bind_sym(void* lib, F& fn, const char* name) {
  fn = reinterpret_cast<F>(tilt_dlsym(lib, name));
  return fn != nullptr;
}

// Carrega a biblioteca uma única vez. Falha de carga não lançam aqui:
// sqlite_query() verifica `lib` e morre com mensagem acionável.
const SqliteApi& api() {
  static const SqliteApi instance = [] {
    SqliteApi a;
#if defined(_WIN32)
    a.lib = tilt_dlopen("sqlite3.dll");
#else
    a.lib = tilt_dlopen("libsqlite3.so.0");
    if (!a.lib) a.lib = tilt_dlopen("libsqlite3.so");
    if (!a.lib) a.lib = tilt_dlopen("libsqlite3.dylib");
    if (!a.lib) a.lib = tilt_dlopen("libsqlite3.0.dylib");
#endif
    if (!a.lib) return a;
    const bool ok = bind_sym(a.lib, a.open_v2, "sqlite3_open_v2") &&
                    bind_sym(a.lib, a.prepare_v2, "sqlite3_prepare_v2") &&
                    bind_sym(a.lib, a.step, "sqlite3_step") &&
                    bind_sym(a.lib, a.column_count, "sqlite3_column_count") &&
                    bind_sym(a.lib, a.column_name, "sqlite3_column_name") &&
                    bind_sym(a.lib, a.column_type, "sqlite3_column_type") &&
                    bind_sym(a.lib, a.column_int64, "sqlite3_column_int64") &&
                    bind_sym(a.lib, a.column_double, "sqlite3_column_double") &&
                    bind_sym(a.lib, a.column_text, "sqlite3_column_text") &&
                    bind_sym(a.lib, a.column_blob, "sqlite3_column_blob") &&
                    bind_sym(a.lib, a.column_bytes, "sqlite3_column_bytes") &&
                    bind_sym(a.lib, a.finalize, "sqlite3_finalize") &&
                    bind_sym(a.lib, a.close, "sqlite3_close") &&
                    bind_sym(a.lib, a.errmsg, "sqlite3_errmsg") &&
                    bind_sym(a.lib, a.bind_int64, "sqlite3_bind_int64") &&
                    bind_sym(a.lib, a.bind_double, "sqlite3_bind_double") &&
                    bind_sym(a.lib, a.bind_text, "sqlite3_bind_text") &&
                    bind_sym(a.lib, a.bind_null, "sqlite3_bind_null") &&
                    bind_sym(a.lib, a.bind_count, "sqlite3_bind_parameter_count");
    if (!ok) {
      tilt_dlclose(a.lib);
      a = SqliteApi{};
    }
    return a;
  }();
  return instance;
}

std::string blob_hex(const void* data, int n) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::string out = "0x";
  out.reserve(static_cast<std::size_t>(n) * 2 + 2);
  for (int k = 0; k < n; ++k) {
    out.push_back(kHex[bytes[k] >> 4]);
    out.push_back(kHex[bytes[k] & 0x0F]);
  }
  return out;
}

}  // namespace

Value sqlite_query(const std::string& db_path, const std::string& sql) {
  const SqliteApi& db = api();
  if (!db.lib) {
#if defined(_WIN32)
    die("sqlite3.dll nao encontrada; instale o SQLite para Windows");
#else
    die("libsqlite3.so.0 nao encontrada; instale o pacote libsqlite3");
#endif
  }
  if (!tilt_file_exists(db_path)) {
    die("banco '" + db_path + "' nao encontrado");
  }

  void* conn = nullptr;
  if (db.open_v2(db_path.c_str(), &conn, kSqliteOpenReadwrite, nullptr) != kSqliteOk) {
    die("nao foi possivel abrir o banco '" + db_path + "'");
  }

  void* stmt = nullptr;
  const int rc = db.prepare_v2(conn, sql.c_str(), static_cast<int>(sql.size()) + 1, &stmt, nullptr);
  if (rc != kSqliteOk) {
    const std::string msg = db.errmsg(conn) ? db.errmsg(conn) : "erro desconhecido";
    db.close(conn);
    die("falha ao preparar consulta: " + msg);
  }

  const int ncols = db.column_count(stmt);
  if (ncols == 0) {
    db.finalize(stmt);
    db.close(conn);
    die("apenas consultas SELECT sao suportadas nesta versao");
  }

  ValueList rows;
  while (true) {
    const int step_rc = db.step(stmt);
    if (step_rc == kSqliteRow) {
      Value row = Value::mapa();
      for (int c = 0; c < ncols; ++c) {
        const char* name = db.column_name(stmt, c);
        const std::string col = name ? name : ("coluna" + std::to_string(c + 1));
        switch (db.column_type(stmt, c)) {
          case kSqliteInteger:
            row.map->set(col, Value::inteiro(static_cast<std::int64_t>(db.column_int64(stmt, c))));
            break;
          case kSqliteFloat:
            row.map->set(col, Value::decimal(db.column_double(stmt, c)));
            break;
          case kSqliteText: {
            const auto* txt = db.column_text(stmt, c);
            row.map->set(col, Value::texto(txt ? reinterpret_cast<const char*>(txt) : ""));
            break;
          }
          case kSqliteBlob:
            row.map->set(col, Value::texto(blob_hex(db.column_blob(stmt, c), db.column_bytes(stmt, c))));
            break;
          case kSqliteNull:
          default:
            row.map->set(col, Value::nulo());
            break;
        }
      }
      rows.push_back(std::move(row));
      continue;
    }
    if (step_rc == kSqliteDone) break;
    const std::string msg = db.errmsg(conn) ? db.errmsg(conn) : "erro desconhecido";
    db.finalize(stmt);
    db.close(conn);
    die("falha ao executar consulta: " + msg);
  }

  db.finalize(stmt);
  db.close(conn);
  return Value::tabela(std::move(rows));
}

void sqlite_exec(const std::string& db_path, const std::string& sql) {
  const SqliteApi& db = api();
  if (!db.lib) {
#if defined(_WIN32)
    die("sqlite3.dll nao encontrada; instale o SQLite para Windows");
#else
    die("libsqlite3.so.0 nao encontrada; instale o pacote libsqlite3");
#endif
  }

  void* conn = nullptr;
  const int flags = kSqliteOpenReadwrite | kSqliteOpenCreate;
  if (db.open_v2(db_path.c_str(), &conn, flags, nullptr) != kSqliteOk) {
    die("nao foi possivel abrir o banco '" + db_path + "'");
  }

  void* stmt = nullptr;
  const int rc = db.prepare_v2(conn, sql.c_str(), static_cast<int>(sql.size()) + 1, &stmt, nullptr);
  if (rc != kSqliteOk || stmt == nullptr) {
    const std::string msg = db.errmsg(conn) ? db.errmsg(conn) : "erro desconhecido";
    db.close(conn);
    die("falha ao preparar comando: " + msg);
  }

  const int step_rc = db.step(stmt);
  const std::string msg = db.errmsg(conn) ? db.errmsg(conn) : "erro desconhecido";
  db.finalize(stmt);
  db.close(conn);
  if (step_rc != kSqliteDone && step_rc != kSqliteRow) {
    die("falha ao executar comando: " + msg);
  }
}

// Executa um comando ja preparado numa conexao aberta, com `?` ligados por
// posicao (1-based). Falhas de contagem e de tipo viram erro claro.
void exec_um(const SqliteApi& db, void* conn, const std::string& sql,
             const std::vector<SqlParam>& params, const std::string& passo) {
  void* stmt = nullptr;
  const int rc = db.prepare_v2(conn, sql.c_str(), static_cast<int>(sql.size()) + 1, &stmt, nullptr);
  if (rc != kSqliteOk || stmt == nullptr) {
    const std::string msg = db.errmsg(conn) ? db.errmsg(conn) : "erro desconhecido";
    die(passo + "falha ao preparar comando: " + msg);
  }
  const int nq = db.bind_count(stmt);
  if (nq != static_cast<int>(params.size())) {
    db.finalize(stmt);
    die(passo + "esperava " + std::to_string(params.size()) + " parametro(s), mas o SQL tem " +
        std::to_string(nq) + " '?'");
  }
  // SQLITE_TRANSIENT: copia os bytes na ligacao.
  void (*transiente)(void*) = reinterpret_cast<void (*)(void*)>(-1);
  for (std::size_t k = 0; k < params.size(); ++k) {
    const int idx = static_cast<int>(k + 1);
    const SqlParam& p = params[k];
    int brc = kSqliteOk;
    switch (p.tipo) {
      case SqlParam::Tipo::Nulo: brc = db.bind_null(stmt, idx); break;
      case SqlParam::Tipo::Inteiro: brc = db.bind_int64(stmt, idx, p.i); break;
      case SqlParam::Tipo::Decimal: brc = db.bind_double(stmt, idx, p.d); break;
      case SqlParam::Tipo::Texto:
        brc = db.bind_text(stmt, idx, p.s.c_str(), static_cast<int>(p.s.size()), transiente);
        break;
      case SqlParam::Tipo::Logico: brc = db.bind_int64(stmt, idx, p.b ? 1 : 0); break;
    }
    if (brc != kSqliteOk) {
      const std::string msg = db.errmsg(conn) ? db.errmsg(conn) : "erro desconhecido";
      db.finalize(stmt);
      die(passo + "falha ao ligar parametro " + std::to_string(idx) + ": " + msg);
    }
  }
  const int step_rc = db.step(stmt);
  const std::string msg = db.errmsg(conn) ? db.errmsg(conn) : "erro desconhecido";
  db.finalize(stmt);
  if (step_rc != kSqliteDone && step_rc != kSqliteRow) {
    die(passo + "falha ao executar comando: " + msg);
  }
  // SELECT acidental num passo: consome as linhas para nao travar a conexao.
  // (O valor e descartado; leitura e via fonte/ler.)
}

void sqlite_exec_params(const std::string& db_path, const std::string& sql,
                        const std::vector<SqlParam>& params) {
  const SqliteApi& db = api();
  if (!db.lib) {
#if defined(_WIN32)
    die("sqlite3.dll nao encontrada; instale o SQLite para Windows");
#else
    die("libsqlite3.so.0 nao encontrada; instale o pacote libsqlite3");
#endif
  }
  void* conn = nullptr;
  const int flags = kSqliteOpenReadwrite | kSqliteOpenCreate;
  if (db.open_v2(db_path.c_str(), &conn, flags, nullptr) != kSqliteOk) {
    die("nao foi possivel abrir o banco '" + db_path + "'");
  }
  try {
    exec_um(db, conn, sql, params, "");
  } catch (...) {
    db.close(conn);
    throw;
  }
  db.close(conn);
}

void sqlite_transact(const std::string& db_path,
                     const std::vector<std::pair<std::string, std::vector<SqlParam>>>& passos) {
  const SqliteApi& db = api();
  if (!db.lib) {
#if defined(_WIN32)
    die("sqlite3.dll nao encontrada; instale o SQLite para Windows");
#else
    die("libsqlite3.so.0 nao encontrada; instale o pacote libsqlite3");
#endif
  }
  void* conn = nullptr;
  const int flags = kSqliteOpenReadwrite | kSqliteOpenCreate;
  if (db.open_v2(db_path.c_str(), &conn, flags, nullptr) != kSqliteOk) {
    die("nao foi possivel abrir o banco '" + db_path + "'");
  }
  try {
    exec_um(db, conn, "BEGIN", {}, "");
    for (std::size_t k = 0; k < passos.size(); ++k) {
      try {
        exec_um(db, conn, passos[k].first, passos[k].second,
                "passo " + std::to_string(k + 1) + ": ");
      } catch (...) {
        try {
          exec_um(db, conn, "ROLLBACK", {}, "");
        } catch (...) {
          // Mantem o erro original do passo.
        }
        throw;
      }
    }
    exec_um(db, conn, "COMMIT", {}, "");
  } catch (...) {
    db.close(conn);
    throw;
  }
  db.close(conn);
}

}  // namespace tilt::rt
