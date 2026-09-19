#include "runtime/duckdb.hpp"

#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include "runtime/compat.hpp"
#include "runtime/sql_params.hpp"
#include "runtime/sql_pool.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("duckdb: " + m); }

// Constantes de duckdb.h (não incluímos o header para manter zero deps).
// duckdb_state: 0 = DuckDBSuccess, 1 = DuckDBError.
constexpr int kDuckdbSuccess = 0;
// enum duckdb_type (valores estáveis da C API).
constexpr int kDuckdbBoolean = 1;
constexpr int kDuckdbTinyint = 2;
constexpr int kDuckdbSmallint = 3;
constexpr int kDuckdbInteger = 4;
constexpr int kDuckdbBigint = 5;
constexpr int kDuckdbFloat = 10;
constexpr int kDuckdbDouble = 11;
constexpr int kDuckdbVarchar = 17;

// duckdb_result é opaco para nós: struct de 6 ponteiros em que só passamos o
// endereço para a biblioteca (duckdb_query o preenche; duckdb_destroy_result
// libera). Espelha o layout de duckdb.h sem depender dele.
struct DuckdbResult {
  void* fields[6] = {};
};

struct DuckdbApi {
  void* lib = nullptr;
  int (*open)(const char*, void**) = nullptr;
  int (*connect)(void*, void**) = nullptr;
  int (*query)(void*, const char*, void*) = nullptr;
  void (*destroy_result)(void*) = nullptr;
  std::uint64_t (*column_count)(void*) = nullptr;
  std::uint64_t (*row_count)(void*) = nullptr;
  int (*column_type)(void*, std::uint64_t) = nullptr;
  const char* (*column_name)(void*, std::uint64_t) = nullptr;
  char* (*value_varchar)(void*, std::uint64_t, std::uint64_t) = nullptr;
  bool (*value_is_null)(void*, std::uint64_t, std::uint64_t) = nullptr;
  std::int64_t (*value_int64)(void*, std::uint64_t, std::uint64_t) = nullptr;
  double (*value_double)(void*, std::uint64_t, std::uint64_t) = nullptr;
  const char* (*result_error)(void*) = nullptr;
  void (*free_value)(void*) = nullptr;
  void (*disconnect)(void**) = nullptr;
  void (*close)(void**) = nullptr;
  // Statements preparados (Marco 3 / D1): `?` posicionais (1-based).
  // Opcional: libs antigas sem esses simbolos mantem o caminho legado
  // (prepared_ok == false; exec_params/transact falham com mensagem clara).
  bool prepared_ok = false;
  int (*prepare)(void*, const char*, void**) = nullptr;
  void (*destroy_prepare)(void**) = nullptr;
  const char* (*prepare_error)(void*) = nullptr;
  std::uint64_t (*nparams)(void*) = nullptr;
  int (*bind_boolean)(void*, std::uint64_t, bool) = nullptr;
  int (*bind_int8)(void*, std::uint64_t, std::int8_t) = nullptr;
  int (*bind_int64)(void*, std::uint64_t, std::int64_t) = nullptr;
  int (*bind_double)(void*, std::uint64_t, double) = nullptr;
  int (*bind_varchar)(void*, std::uint64_t, const char*) = nullptr;
  int (*bind_null)(void*, std::uint64_t) = nullptr;
  int (*execute_prepared)(void*, void*) = nullptr;
};

template <typename F>
bool bind_sym(void* lib, F& fn, const char* name) {
  fn = reinterpret_cast<F>(tilt_dlsym(lib, name));
  return fn != nullptr;
}

// Carrega a biblioteca uma única vez. Falha de carga não lança aqui:
// duckdb_query()/duckdb_exec() verificam `lib` e morrem com mensagem acionável.
const DuckdbApi& api() {
  static const DuckdbApi instance = [] {
    DuckdbApi a;
#if defined(_WIN32)
    a.lib = tilt_dlopen("duckdb.dll");
#else
    a.lib = tilt_dlopen("libduckdb.so");
    if (!a.lib) a.lib = tilt_dlopen("libduckdb.so.0");
    if (!a.lib) a.lib = tilt_dlopen("libduckdb.dylib");
    if (!a.lib) a.lib = tilt_dlopen("libduckdb.0.dylib");
#endif
    if (!a.lib) return a;
    const bool ok = bind_sym(a.lib, a.open, "duckdb_open") &&
                    bind_sym(a.lib, a.connect, "duckdb_connect") &&
                    bind_sym(a.lib, a.query, "duckdb_query") &&
                    bind_sym(a.lib, a.destroy_result, "duckdb_destroy_result") &&
                    bind_sym(a.lib, a.column_count, "duckdb_column_count") &&
                    bind_sym(a.lib, a.row_count, "duckdb_row_count") &&
                    bind_sym(a.lib, a.column_type, "duckdb_column_type") &&
                    bind_sym(a.lib, a.column_name, "duckdb_column_name") &&
                    bind_sym(a.lib, a.value_varchar, "duckdb_value_varchar") &&
                    bind_sym(a.lib, a.value_is_null, "duckdb_value_is_null") &&
                    bind_sym(a.lib, a.value_int64, "duckdb_value_int64") &&
                    bind_sym(a.lib, a.value_double, "duckdb_value_double") &&
                    bind_sym(a.lib, a.result_error, "duckdb_result_error") &&
                    bind_sym(a.lib, a.free_value, "duckdb_free") &&
                    bind_sym(a.lib, a.disconnect, "duckdb_disconnect") &&
                    bind_sym(a.lib, a.close, "duckdb_close");
    // Prepared opcional (nao derruba o legado se a lib for antiga).
    a.prepared_ok = bind_sym(a.lib, a.prepare, "duckdb_prepare") &&
                    bind_sym(a.lib, a.destroy_prepare, "duckdb_destroy_prepare") &&
                    bind_sym(a.lib, a.prepare_error, "duckdb_prepare_error") &&
                    bind_sym(a.lib, a.nparams, "duckdb_nparams") &&
                    bind_sym(a.lib, a.bind_boolean, "duckdb_bind_boolean") &&
                    bind_sym(a.lib, a.bind_int8, "duckdb_bind_int8") &&
                    bind_sym(a.lib, a.bind_int64, "duckdb_bind_int64") &&
                    bind_sym(a.lib, a.bind_double, "duckdb_bind_double") &&
                    bind_sym(a.lib, a.bind_varchar, "duckdb_bind_varchar") &&
                    bind_sym(a.lib, a.bind_null, "duckdb_bind_null") &&
                    bind_sym(a.lib, a.execute_prepared, "duckdb_execute_prepared");
    if (!ok) {
      tilt_dlclose(a.lib);
      a = DuckdbApi{};
    }
    return a;
  }();
  return instance;
}

// A C API materializada do DuckDB devolve DDL/DML como um resultado com uma
// coluna "Count" (cols=1), entao a checagem de column_count nao distingue
// SELECT de INSERT/CREATE. Verificamos a primeira palavra do SQL (pulando
// espacos e comentarios) — so consultas que retornam linhas passam.
bool returns_rows(const std::string& sql) {
  std::size_t i = 0;
  auto skip_space_and_comments = [&]() {
    while (i < sql.size()) {
      if (sql[i] == ' ' || sql[i] == '\t' || sql[i] == '\n' || sql[i] == '\r') {
        ++i;
      } else if (sql[i] == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
        while (i < sql.size() && sql[i] != '\n') ++i;
      } else if (sql[i] == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
        const std::size_t end = sql.find("*/", i + 2);
        i = end == std::string::npos ? sql.size() : end + 2;
      } else {
        break;
      }
    }
  };
  skip_space_and_comments();
  std::string kw;
  while (i < sql.size() &&
         ((sql[i] >= 'a' && sql[i] <= 'z') || (sql[i] >= 'A' && sql[i] <= 'Z'))) {
    kw.push_back(static_cast<char>(sql[i] >= 'a' ? sql[i] - ('a' - 'A') : sql[i]));
    ++i;
  }
  return kw == "SELECT" || kw == "WITH" || kw == "PRAGMA" || kw == "VALUES" ||
         kw == "EXPLAIN" || kw == "DESCRIBE" || kw == "SUMMARIZE" || kw == "SHOW";
}

// Par banco+conexao guardado no pool como um handle opaco.
struct DbConn {
  void* database = nullptr;
  void* connection = nullptr;
};

// Abre banco+conexao; em qualquer falha já morre com mensagem clara.
DbConn* abre_banco(const DuckdbApi& db, const std::string& db_path) {
  auto* p = new DbConn;
  if (db.open(db_path.c_str(), &p->database) != kDuckdbSuccess || p->database == nullptr) {
    delete p;
    die("nao foi possivel abrir o banco '" + db_path + "'");
  }
  if (db.connect(p->database, &p->connection) != kDuckdbSuccess || p->connection == nullptr) {
    db.close(&p->database);
    delete p;
    die("nao foi possivel conectar no banco '" + db_path + "'");
  }
  return p;
}

void fecha_banco(const DuckdbApi& db, void* h) {
  auto* p = static_cast<DbConn*>(h);
  if (!p) return;
  db.disconnect(&p->connection);
  db.close(&p->database);
  delete p;
}

// DuckDB usa conexoes dedicadas em todos os caminhos: manter uma conexao ociosa
// do pool aberta enquanto outra transaciona o mesmo arquivo causa conflitos
// de catalogo no DuckDB.
// DuckDB e em-processo: a conexao nao "cai" sozinha, validacao e trivial.
struct Conn {
  const DuckdbApi& db;
  PooledConn pool;
  void* connection = nullptr;

  Conn(const DuckdbApi& d, const std::string& db_path, const std::string& sql, bool pooled = true)
      : db(d),
        pool(pooled ? "duckdb" : "", db_path, [&] { return abre_banco(db, db_path); },
             [](void* h) {
               auto* p = static_cast<DbConn*>(h);
               return p && p->connection;
             },
             [&](void* h) { fecha_banco(db, h); }, pooled ? sql : "") {
    connection = static_cast<DbConn*>(pool.get())->connection;
  }
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;
};

std::string result_error(const DuckdbApi& db, void* result) {
  const char* err = db.result_error(result);
  return err ? err : "erro desconhecido";
}

bool duckdb_termina_palavra(const std::string& sql, const std::string& palavra,
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
       sql[fim - palavra.size() - 1] == 95)) {
    return false;
  }
  if (inicio) *inicio = fim - palavra.size();
  return true;
}

std::string duckdb_rewrite_sql(const std::string& sql, const std::vector<SqlParam>& params,
                               std::vector<SqlParam>& bind_params) {
  std::string out;
  std::size_t raw = 0;
  varrer_sql(
      sql,
      [&](std::string& emitted) {
        std::size_t is_start = 0;
        bool is_context = duckdb_termina_palavra(emitted, "IS", &is_start);
        bool is_not_context = false;
        if (!is_context) {
          std::size_t not_start = 0;
          if (duckdb_termina_palavra(emitted, "NOT", &not_start)) {
            const std::string before_not = emitted.substr(0, not_start);
            is_context = duckdb_termina_palavra(before_not, "IS", &is_start);
            is_not_context = is_context;
          }
        }
        if (raw < params.size() && params[raw].tipo == SqlParam::Tipo::Nulo && is_context) {
          emitted.resize(is_start);
          emitted += is_not_context ? "IS NOT NULL" : "IS NULL";
        } else {
          emitted += "?";
          bind_params.push_back(params[raw]);
        }
        ++raw;
      },
      out);
  return out;
}

}  // namespace

// Materializa as linhas de um resultado ja executado. Exige colunas
// (o chamador verifica ncols == 0 e falha como nao-SELECT antes).
ValueList materializa_duckdb(const DuckdbApi& db, DuckdbResult* result) {
  const auto ncols = db.column_count(result);
  const auto nrows = db.row_count(result);

  ValueList rows;
  rows.reserve(static_cast<std::size_t>(nrows));
  for (std::uint64_t r = 0; r < nrows; ++r) {
    Value row = Value::mapa();
    for (std::uint64_t c = 0; c < ncols; ++c) {
      const char* name = db.column_name(result, c);
      const std::string col = (name && *name) ? name : ("coluna" + std::to_string(c + 1));
      if (db.value_is_null(result, c, r)) {
        row.map->set(col, Value::nulo());
        continue;
      }
      switch (db.column_type(result, c)) {
        case kDuckdbBoolean:
        case kDuckdbTinyint:
        case kDuckdbSmallint:
        case kDuckdbInteger:
        case kDuckdbBigint:
          row.map->set(col, Value::inteiro(db.value_int64(result, c, r)));
          break;
        case kDuckdbFloat:
        case kDuckdbDouble:
          row.map->set(col, Value::decimal(db.value_double(result, c, r)));
          break;
        case kDuckdbVarchar:
        default: {
          // VARCHAR e demais tipos (DATE, TIMESTAMP, DECIMAL, UUID, BLOB...)
          // chegam como texto; o valor precisa ser liberado com duckdb_free.
          char* txt = db.value_varchar(result, c, r);
          row.map->set(col, Value::texto(txt ? txt : ""));
          if (txt) db.free_value(txt);
          break;
        }
      }
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

Value duckdb_query(const std::string& db_path, const std::string& sql) {
  const DuckdbApi& db = api();
  if (!db.lib) {
#if defined(_WIN32)
    die("libduckdb nao encontrada: instale o pacote duckdb (duckdb.dll no PATH)");
#else
    die("libduckdb nao encontrada: instale o pacote duckdb");
#endif
  }
  if (db_path != ":memory:" && !tilt_file_exists(db_path)) {
    die("banco '" + db_path + "' nao encontrado");
  }
  if (!returns_rows(sql)) {
    die("apenas consultas SELECT sao suportadas nesta versao; para INSERT/UPDATE/DDL use executar_sql");
  }

  Conn conn(db, db_path, sql, false);
  DuckdbResult result;
  if (db.query(conn.connection, sql.c_str(), &result) != kDuckdbSuccess) {
    const std::string msg = result_error(db, &result);
    db.destroy_result(&result);
    die("falha ao executar consulta: " + msg);
  }

  const auto ncols = db.column_count(&result);
  if (ncols == 0) {
    db.destroy_result(&result);
    die("apenas consultas SELECT sao suportadas nesta versao");
  }

  ValueList rows = materializa_duckdb(db, &result);

  db.destroy_result(&result);
  return Value::tabela(std::move(rows));
}

void duckdb_exec(const std::string& db_path, const std::string& sql) {
  const DuckdbApi& db = api();
  if (!db.lib) {
#if defined(_WIN32)
    die("libduckdb nao encontrada: instale o pacote duckdb (duckdb.dll no PATH)");
#else
    die("libduckdb nao encontrada: instale o pacote duckdb");
#endif
  }

  // A C API nao expoe duckdb_execute_statements nesta versao; DDL/DML roda via
  // duckdb_query (sucesso = DuckDBSuccess, sem linhas) e o erro vem de
  // duckdb_result_error.
  Conn conn(db, db_path, sql, false);
  DuckdbResult result;
  if (db.query(conn.connection, sql.c_str(), &result) != kDuckdbSuccess) {
    const std::string msg = result_error(db, &result);
    db.destroy_result(&result);
    die("falha ao executar comando: " + msg);
  }
  db.destroy_result(&result);
}

// Prepara `sql` e liga `params` por posicao (1-based) num prepared novo.
// DuckDB PreparedStatement e um ponteiro opaco alocado por duckdb_prepare e
// liberado por duckdb_destroy_prepare. `acao` e "comando" ou "consulta".
void* prepara_e_liga(const DuckdbApi& db, void* conn, const std::string& sql,
                     const std::vector<SqlParam>& params, const std::string& passo,
                     const std::string& acao) {
  if (!db.prepared_ok) {
    die(passo +
        "parametros exigem prepared statements (libduckdb sem duckdb_prepare; atualize a lib)");
  }
  const auto nq = rewrite_qmarks(sql, "nenhum").second;
  if (nq != params.size()) {
    die(passo + "esperava " + std::to_string(params.size()) + " parametro(s), mas o SQL tem " +
        std::to_string(nq) + " ? ");
  }
  std::vector<SqlParam> bind_params;
  const std::string reescrito = duckdb_rewrite_sql(sql, params, bind_params);
  void* prep = nullptr;
  if (db.prepare(conn, reescrito.c_str(), &prep) != kDuckdbSuccess || prep == nullptr) {
    const char* err = db.prepare_error(prep);
    std::string msg = err ? err : "erro desconhecido";
    if (prep) db.destroy_prepare(&prep);
    die(passo + "falha ao preparar " + acao + ": " + msg);
  }
  const std::uint64_t prepared_nq = db.nparams(prep);
  if (prepared_nq != bind_params.size()) {
    db.destroy_prepare(&prep);
    die(passo + "parametros ligados divergem do SQL preparado");
  }
  for (std::size_t k = 0; k < bind_params.size(); ++k) {
    const std::uint64_t idx = static_cast<std::uint64_t>(k + 1);
    const SqlParam& p = bind_params[k];
    int brc = kDuckdbSuccess;
    switch (p.tipo) {
      case SqlParam::Tipo::Nulo: brc = db.bind_null(prep, idx); break;
      case SqlParam::Tipo::Inteiro: brc = db.bind_int64(prep, idx, p.i); break;
      case SqlParam::Tipo::Decimal: brc = db.bind_double(prep, idx, p.d); break;
      case SqlParam::Tipo::Texto: brc = db.bind_varchar(prep, idx, p.s.c_str()); break;
      case SqlParam::Tipo::Logico: brc = db.bind_boolean(prep, idx, p.b); break;
    }
    if (brc != kDuckdbSuccess) {
      const char* err = db.prepare_error(prep);
      std::string msg = err ? err : "erro desconhecido";
      db.destroy_prepare(&prep);
      die(passo + "falha ao ligar parametro " + std::to_string(idx) + ": " + msg);
    }
  }
  return prep;
}

// Executa um prepared numa conexao aberta, com `?` ligados por posicao
// (1-based).
void exec_um(const DuckdbApi& db, void* conn, const std::string& sql,
             const std::vector<SqlParam>& params, const std::string& passo) {
  void* prep = prepara_e_liga(db, conn, sql, params, passo, "comando");
  DuckdbResult result;
  const int rc = db.execute_prepared(prep, &result);
  const std::string msg = rc != kDuckdbSuccess ? result_error(db, &result) : "";
  db.destroy_result(&result);
  db.destroy_prepare(&prep);
  if (rc != kDuckdbSuccess) {
    die(passo + "falha ao executar comando: " + msg);
  }
}

void duckdb_exec_params(const std::string& db_path, const std::string& sql,
                        const std::vector<SqlParam>& params) {
  const DuckdbApi& db = api();
  if (!db.lib) {
#if defined(_WIN32)
    die("libduckdb nao encontrada: instale o pacote duckdb (duckdb.dll no PATH)");
#else
    die("libduckdb nao encontrada: instale o pacote duckdb");
#endif
  }
  Conn conn(db, db_path, sql, false);
  exec_um(db, conn.connection, sql, params, "");
}

Value duckdb_query_params(const std::string& db_path, const std::string& sql,
                          const std::vector<SqlParam>& params) {
  const DuckdbApi& db = api();
  if (!db.lib) {
#if defined(_WIN32)
    die("libduckdb nao encontrada: instale o pacote duckdb (duckdb.dll no PATH)");
#else
    die("libduckdb nao encontrada: instale o pacote duckdb");
#endif
  }
  if (db_path != ":memory:" && !tilt_file_exists(db_path)) {
    die("banco '" + db_path + "' nao encontrado");
  }
  if (!returns_rows(sql)) {
    die("apenas consultas SELECT sao suportadas nesta versao; para INSERT/UPDATE/DDL use executar_sql");
  }
  Conn conn(db, db_path, sql, false);
  void* prep = prepara_e_liga(db, conn.connection, sql, params, "", "consulta");
  DuckdbResult result;
  const int rc = db.execute_prepared(prep, &result);
  if (rc != kDuckdbSuccess) {
    const std::string msg = result_error(db, &result);
    db.destroy_result(&result);
    db.destroy_prepare(&prep);
    die("falha ao executar consulta: " + msg);
  }
  if (db.column_count(&result) == 0) {
    db.destroy_result(&result);
    db.destroy_prepare(&prep);
    die("apenas consultas SELECT sao suportadas nesta versao");
  }
  ValueList rows = materializa_duckdb(db, &result);
  db.destroy_result(&result);
  db.destroy_prepare(&prep);
  return Value::tabela(std::move(rows));
}

void duckdb_transact(const std::string& db_path,
                     const std::vector<std::pair<std::string, std::vector<SqlParam>>>& passos) {
  const DuckdbApi& db = api();
  if (!db.lib) {
#if defined(_WIN32)
    die("libduckdb nao encontrada: instale o pacote duckdb (duckdb.dll no PATH)");
#else
    die("libduckdb nao encontrada: instale o pacote duckdb");
#endif
  }
  auto simples = [&](void* conn, const char* sql) {
    DuckdbResult result;
    const int rc = db.query(conn, sql, &result);
    const std::string msg = rc != kDuckdbSuccess ? result_error(db, &result) : "";
    db.destroy_result(&result);
    if (rc != kDuckdbSuccess) die(std::string("falha em ") + sql + ": " + msg);
  };
  Conn conn(db, db_path, "", false);  // transacao: conexao dedicada, fora do pool
  simples(conn.connection, "BEGIN TRANSACTION");
  for (std::size_t k = 0; k < passos.size(); ++k) {
    try {
      exec_um(db, conn.connection, passos[k].first, passos[k].second,
              "passo " + std::to_string(k + 1) + ": ");
    } catch (...) {
      try {
        simples(conn.connection, "ROLLBACK");
      } catch (...) {
      }
      throw;
    }
  }
  simples(conn.connection, "COMMIT");
}

}  // namespace tilt::rt
