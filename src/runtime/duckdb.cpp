#include "runtime/duckdb.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include "runtime/compat.hpp"
#include "runtime/columnar.hpp"
#include "runtime/json.hpp"
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
constexpr int kDuckdbUtinyint = 6;
constexpr int kDuckdbUsmallint = 7;
constexpr int kDuckdbUinteger = 8;
constexpr int kDuckdbUbigint = 9;
constexpr int kDuckdbHugeint = 16;
constexpr int kDuckdbVarchar = 17;
constexpr int kDuckdbDecimal = 19;

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
  // Appender (carga em massa de tabelas tilt para o SQL local); opcional.
  bool appender_ok = false;
  int (*appender_create)(void*, const char*, const char*, void**) = nullptr;
  int (*appender_end_row)(void*) = nullptr;
  int (*appender_destroy)(void**) = nullptr;
  int (*append_null)(void*) = nullptr;
  int (*append_int64)(void*, std::int64_t) = nullptr;
  int (*append_double)(void*, double) = nullptr;
  int (*append_varchar)(void*, const char*) = nullptr;
  // Data chunks (opcional): permite carregar tabelas colunares em blocos,
  // sem uma chamada ao appender por linha.
  bool chunk_ok = false;
  void* (*create_logical_type)(int) = nullptr;
  void (*destroy_logical_type)(void**) = nullptr;
  void* (*create_data_chunk)(void**, std::uint64_t) = nullptr;
  void (*destroy_data_chunk)(void**) = nullptr;
  void (*data_chunk_reset)(void*) = nullptr;
  void (*data_chunk_set_size)(void*, std::uint64_t) = nullptr;
  void* (*data_chunk_get_vector)(void*, std::uint64_t) = nullptr;
  void* (*vector_get_data)(void*) = nullptr;
  std::uint64_t* (*vector_get_validity)(void*) = nullptr;
  void (*vector_ensure_validity_writable)(void*) = nullptr;
  void (*vector_assign_string_element)(void*, std::uint64_t, const char*) = nullptr;
  int (*append_data_chunk)(void*, void*) = nullptr;
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
    a.appender_ok = bind_sym(a.lib, a.appender_create, "duckdb_appender_create") &&
                    bind_sym(a.lib, a.appender_end_row, "duckdb_appender_end_row") &&
                    bind_sym(a.lib, a.appender_destroy, "duckdb_appender_destroy") &&
                    bind_sym(a.lib, a.append_null, "duckdb_append_null") &&
                    bind_sym(a.lib, a.append_int64, "duckdb_append_int64") &&
                    bind_sym(a.lib, a.append_double, "duckdb_append_double") &&
                    bind_sym(a.lib, a.append_varchar, "duckdb_append_varchar");
    a.chunk_ok = bind_sym(a.lib, a.create_logical_type, "duckdb_create_logical_type") &&
                 bind_sym(a.lib, a.destroy_logical_type, "duckdb_destroy_logical_type") &&
                 bind_sym(a.lib, a.create_data_chunk, "duckdb_create_data_chunk") &&
                 bind_sym(a.lib, a.destroy_data_chunk, "duckdb_destroy_data_chunk") &&
                 bind_sym(a.lib, a.data_chunk_reset, "duckdb_data_chunk_reset") &&
                 bind_sym(a.lib, a.data_chunk_set_size, "duckdb_data_chunk_set_size") &&
                 bind_sym(a.lib, a.data_chunk_get_vector, "duckdb_data_chunk_get_vector") &&
                 bind_sym(a.lib, a.vector_get_data, "duckdb_vector_get_data") &&
                 bind_sym(a.lib, a.vector_get_validity, "duckdb_vector_get_validity") &&
                 bind_sym(a.lib, a.vector_ensure_validity_writable,
                          "duckdb_vector_ensure_validity_writable") &&
                 bind_sym(a.lib, a.vector_assign_string_element,
                          "duckdb_vector_assign_string_element") &&
                 bind_sym(a.lib, a.append_data_chunk, "duckdb_append_data_chunk");
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
        pool(
            pooled ? "duckdb" : "", db_path, [&] { return abre_banco(db, db_path); },
            [](void* h) {
              auto* p = static_cast<DbConn*>(h);
              return p && p->connection;
            },
            [&d](void* h) { fecha_banco(d, h); }, pooled ? sql : "") {
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
        row.map_ref()->set(col, Value::nulo());
        continue;
      }
      switch (db.column_type(result, c)) {
        case kDuckdbBoolean:
        case kDuckdbTinyint:
        case kDuckdbSmallint:
        case kDuckdbInteger:
        case kDuckdbBigint:
        case kDuckdbUtinyint:
        case kDuckdbUsmallint:
        case kDuckdbUinteger:
        case kDuckdbUbigint:
        case kDuckdbHugeint:  // sum(bigint) e count(*) em versoes novas
          row.map_ref()->set(col, Value::inteiro(db.value_int64(result, c, r)));
          break;
        case kDuckdbFloat:
        case kDuckdbDecimal:
        case kDuckdbDouble:
          row.map_ref()->set(col, Value::decimal(db.value_double(result, c, r)));
          break;
        case kDuckdbVarchar:
        default: {
          // VARCHAR e demais tipos (DATE, TIMESTAMP, DECIMAL, UUID, BLOB...)
          // chegam como texto; o valor precisa ser liberado com duckdb_free.
          char* txt = db.value_varchar(result, c, r);
          row.map_ref()->set(col, Value::texto(txt ? txt : ""));
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

namespace {

std::string ident_duck(const std::string& nome) {
  std::string out = "\"";
  for (const char c : nome) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + "\"";
}

struct ColunaDuck {
  std::string nome;
  bool viu_int = false;
  bool viu_real = false;
  bool viu_outro = false;
  const char* tipo() const {
    if (viu_outro) return "VARCHAR";
    if (viu_real) return "DOUBLE";
    return viu_int ? "BIGINT" : "VARCHAR";
  }
};

void exec_simples(const DuckdbApi& db, void* conn, const std::string& sql) {
  DuckdbResult r;
  if (db.query(conn, sql.c_str(), &r) != kDuckdbSuccess) {
    const std::string msg = result_error(db, &r);
    db.destroy_result(&r);
    die("falha ao executar comando: " + msg);
  }
  db.destroy_result(&r);
}

}  // namespace

bool duckdb_disponivel() { return api().lib != nullptr && api().appender_ok; }

Value duckdb_consulta_tabelas(const std::string& sql,
                              const std::vector<std::pair<std::string, Value>>& tabelas,
                              const std::vector<SqlParam>& params) {
  const DuckdbApi& db = api();
  if (!db.lib) {
#if defined(_WIN32)
    die("libduckdb nao encontrada: instale o pacote duckdb (duckdb.dll no PATH)");
#else
    die("libduckdb nao encontrada: instale o pacote duckdb (libduckdb.so no LD_LIBRARY_PATH)");
#endif
  }
  if (!tabelas.empty() && !db.appender_ok) {
    die("libduckdb sem a API de appender; atualize o DuckDB (>= 0.9)");
  }
  DbConn* h = abre_banco(db, ":memory:");
  try {
    for (const auto& [nome, tabela] : tabelas) {
      // Tabelas colunares entram diretamente no appender. O caminho anterior
      // chamava materialize_rows(), criando um mapa e um Value para cada
      // célula antes de a mesma célula ser copiada novamente para o DuckDB.
      if (const ColumnarTable* col = tabela.columnar(); col && !tabela.list_ref()) {
        struct ColunaColunar {
          std::string nome;
          const ColumnarColumn* coluna = nullptr;
          ColumnarColumn::Type tipo = ColumnarColumn::Type::Mixed;
        };
        std::vector<ColunaColunar> colunas;
        colunas.reserve(col->names.size());
        for (const std::string& coluna_nome : col->names) {
          const ColumnarColumn* coluna = col->find(coluna_nome);
          if (!coluna) die("'" + nome + "' nao contem a coluna '" + coluna_nome + "'");
          colunas.push_back({coluna_nome, coluna, coluna->type});
        }
        if (colunas.empty()) die("a tabela '" + nome + "' esta vazia: sem colunas para criar");
        std::string ddl = "CREATE TABLE " + ident_duck(nome) + " (";
        for (std::size_t k = 0; k < colunas.size(); ++k) {
          const auto tipo = colunas[k].tipo;
          const char* sql_tipo = tipo == ColumnarColumn::Type::Integer ? "BIGINT" :
                                 tipo == ColumnarColumn::Type::Decimal ? "DOUBLE" :
                                 tipo == ColumnarColumn::Type::Boolean ? "BIGINT" :
                                 tipo == ColumnarColumn::Type::Text || tipo == ColumnarColumn::Type::TextPlain
                                     ? "VARCHAR"
                                     : "VARCHAR";
          ddl += (k ? ", " : "") + ident_duck(colunas[k].nome) + " " + sql_tipo;
        }
        exec_simples(db, h->connection, ddl + ")");
        void* app = nullptr;
        if (db.appender_create(h->connection, nullptr, nome.c_str(), &app) != kDuckdbSuccess)
          die("falha ao abrir a carga de '" + nome + "'");
        if (db.chunk_ok) {
          std::vector<void*> logical_types;
          logical_types.reserve(colunas.size());
          for (const ColunaColunar& item : colunas) {
            const int tipo = item.tipo == ColumnarColumn::Type::Integer ||
                                     item.tipo == ColumnarColumn::Type::Boolean
                                 ? kDuckdbBigint
                                 : item.tipo == ColumnarColumn::Type::Decimal ? kDuckdbDouble
                                 : kDuckdbVarchar;
            void* logical = db.create_logical_type(tipo);
            if (!logical) {
              for (void* type : logical_types) db.destroy_logical_type(&type);
              db.appender_destroy(&app);
              die("DuckDB nao criou o tipo logico do bloco colunar");
            }
            logical_types.push_back(logical);
          }
          void* chunk = db.create_data_chunk(logical_types.data(), logical_types.size());
          if (!chunk) {
            for (void* type : logical_types) db.destroy_logical_type(&type);
            db.appender_destroy(&app);
            die("DuckDB nao criou o bloco colunar");
          }
          constexpr std::size_t kChunkRows = 4096;
          for (std::size_t inicio = 0; inicio < col->rows; inicio += kChunkRows) {
            const std::size_t count = std::min(kChunkRows, col->rows - inicio);
            if (inicio != 0) db.data_chunk_reset(chunk);
            for (std::size_t ci = 0; ci < colunas.size(); ++ci) {
              const ColunaColunar& item = colunas[ci];
              const ColumnarColumn& coluna = *item.coluna;
              void* vector = db.data_chunk_get_vector(chunk, ci);
              std::uint64_t* validity = nullptr;
              for (std::size_t offset = 0; offset < count; ++offset) {
                const std::size_t physical = col->physical_row(inicio + offset);
                if (coluna.null_at(physical)) {
                  if (!validity) {
                    db.vector_ensure_validity_writable(vector);
                    validity = db.vector_get_validity(vector);
                  }
                  if (validity) validity[offset / 64] &= ~(std::uint64_t{1} << (offset % 64));
                  continue;
                }
                switch (item.tipo) {
                  case ColumnarColumn::Type::Integer:
                    static_cast<std::int64_t*>(db.vector_get_data(vector))[offset] =
                        coluna.integers[physical];
                    break;
                  case ColumnarColumn::Type::Decimal:
                    static_cast<double*>(db.vector_get_data(vector))[offset] =
                        coluna.decimals[physical];
                    break;
                  case ColumnarColumn::Type::Boolean:
                    static_cast<std::int64_t*>(db.vector_get_data(vector))[offset] =
                        coluna.booleans[physical] ? 1 : 0;
                    break;
                  case ColumnarColumn::Type::Text:
                  case ColumnarColumn::Type::TextPlain: {
                    const std::string text = coluna.key_at(physical);
                    db.vector_assign_string_element(vector, offset, text.c_str());
                    break;
                  }
                  default: {
                    const std::string text = json_dump_compacto(coluna.at(physical));
                    db.vector_assign_string_element(vector, offset, text.c_str());
                    break;
                  }
                }
              }
            }
            db.data_chunk_set_size(chunk, count);
            if (db.append_data_chunk(app, chunk) != kDuckdbSuccess) {
              db.destroy_data_chunk(&chunk);
              for (void* type : logical_types) db.destroy_logical_type(&type);
              db.appender_destroy(&app);
              die("falha ao anexar bloco colunar no DuckDB");
            }
          }
          db.destroy_data_chunk(&chunk);
          for (void* type : logical_types) db.destroy_logical_type(&type);
          db.appender_destroy(&app);
          continue;
        }
        for (std::size_t row = 0; row < col->rows; ++row) {
          const std::size_t physical = col->physical_row(row);
          for (const ColunaColunar& item : colunas) {
            const ColumnarColumn& coluna = *item.coluna;
            if (coluna.null_at(physical)) {
              db.append_null(app);
              continue;
            }
            switch (item.tipo) {
              case ColumnarColumn::Type::Integer:
                db.append_int64(app, coluna.integers[physical]);
                break;
              case ColumnarColumn::Type::Decimal:
                db.append_double(app, coluna.decimals[physical]);
                break;
              case ColumnarColumn::Type::Boolean:
                db.append_int64(app, coluna.booleans[physical] ? 1 : 0);
                break;
              case ColumnarColumn::Type::Text:
              case ColumnarColumn::Type::TextPlain: {
                const std::string text = coluna.key_at(physical);
                db.append_varchar(app, text.c_str());
                break;
              }
              default: {
                const std::string text = json_dump_compacto(coluna.at(physical));
                db.append_varchar(app, text.c_str());
                break;
              }
            }
          }
          db.appender_end_row(app);
        }
        db.appender_destroy(&app);
        continue;
      }
      Value materializada = tabela;
      materializada.materialize_rows();
      if ((materializada.kind != ValueKind::Tabela && materializada.kind != ValueKind::Lista) ||
          !materializada.list_ref()) {
        die("'" + nome + "' deve ser uma tabela (lista de mapas)");
      }
      const ValueList& linhas = *materializada.list_ref();
      std::vector<ColunaDuck> colunas;
      for (const Value& linha : linhas) {
        if (linha.kind != ValueKind::Mapa || !linha.map_ref()) {
          die("'" + nome + "' deve ser uma tabela (lista de mapas)");
        }
        for (const auto& [chave, v] : linha.map_ref()->items) {
          ColunaDuck* c = nullptr;
          for (ColunaDuck& e : colunas) {
            if (e.nome == chave) {
              c = &e;
              break;
            }
          }
          if (c == nullptr) {
            colunas.push_back(ColunaDuck{chave});
            c = &colunas.back();
          }
          switch (v.kind) {
            case ValueKind::Nulo:
              break;
            case ValueKind::Logico:
            case ValueKind::Inteiro:
              c->viu_int = true;
              break;
            case ValueKind::Decimal:
              c->viu_real = true;
              break;
            default:
              c->viu_outro = true;
              break;
          }
        }
      }
      if (colunas.empty()) die("a tabela '" + nome + "' esta vazia: sem colunas para criar");
      std::string ddl = "CREATE TABLE " + ident_duck(nome) + " (";
      for (std::size_t k = 0; k < colunas.size(); ++k) {
        ddl += (k ? ", " : "") + ident_duck(colunas[k].nome) + " " + colunas[k].tipo();
      }
      exec_simples(db, h->connection, ddl + ")");
      void* app = nullptr;
      if (db.appender_create(h->connection, nullptr, nome.c_str(), &app) != kDuckdbSuccess) {
        die("falha ao abrir a carga de '" + nome + "'");
      }
      for (const Value& linha : linhas) {
        for (const ColunaDuck& col : colunas) {
          const Value* v = linha.map_ref()->find(col.nome);
          if (v == nullptr || v->kind == ValueKind::Nulo) {
            db.append_null(app);
          } else if (v->kind == ValueKind::Logico) {
            db.append_int64(app, v->b ? 1 : 0);
          } else if (v->kind == ValueKind::Inteiro && !col.viu_outro) {
            col.viu_real ? db.append_double(app, static_cast<double>(v->i))
                         : db.append_int64(app, v->i);
          } else if (v->kind == ValueKind::Decimal && !col.viu_outro) {
            db.append_double(app, v->d);
          } else {
            const std::string txt = v->kind == ValueKind::Texto ? v->s : json_dump_compacto(*v);
            db.append_varchar(app, txt.c_str());
          }
        }
        db.appender_end_row(app);
      }
      db.appender_destroy(&app);  // descarrega o que faltava
    }

    DuckdbResult result;
    if (params.empty()) {
      if (db.query(h->connection, sql.c_str(), &result) != kDuckdbSuccess) {
        const std::string msg = result_error(db, &result);
        db.destroy_result(&result);
        die("falha ao executar consulta: " + msg);
      }
    } else {
      if (!db.prepared_ok) die("libduckdb sem statements preparados; atualize o DuckDB");
      void* stmt = nullptr;
      if (db.prepare(h->connection, sql.c_str(), &stmt) != kDuckdbSuccess) {
        const std::string msg = db.prepare_error(stmt) ? db.prepare_error(stmt) : "erro";
        db.destroy_prepare(&stmt);
        die("falha ao preparar consulta: " + msg);
      }
      if (db.nparams(stmt) != params.size()) {
        db.destroy_prepare(&stmt);
        die("esperava " + std::to_string(params.size()) + " parametro(s), mas o SQL tem " +
            std::to_string(db.nparams(stmt)) + " '?'");
      }
      for (std::size_t k = 0; k < params.size(); ++k) {
        const std::uint64_t idx = k + 1;
        const SqlParam& p = params[k];
        switch (p.tipo) {
          case SqlParam::Tipo::Nulo:
            db.bind_null(stmt, idx);
            break;
          case SqlParam::Tipo::Inteiro:
            db.bind_int64(stmt, idx, p.i);
            break;
          case SqlParam::Tipo::Decimal:
            db.bind_double(stmt, idx, p.d);
            break;
          case SqlParam::Tipo::Texto:
            db.bind_varchar(stmt, idx, p.s.c_str());
            break;
          case SqlParam::Tipo::Logico:
            db.bind_int64(stmt, idx, p.b ? 1 : 0);
            break;
        }
      }
      const int rc = db.execute_prepared(stmt, &result);
      db.destroy_prepare(&stmt);
      if (rc != kDuckdbSuccess) {
        const std::string msg = result_error(db, &result);
        db.destroy_result(&result);
        die("falha ao executar consulta: " + msg);
      }
    }
    if (db.column_count(&result) == 0) {
      db.destroy_result(&result);
      die("sql espera uma consulta que devolva linhas (SELECT/WITH)");
    }
    ValueList rows = materializa_duckdb(db, &result);
    db.destroy_result(&result);
    fecha_banco(db, h);
    return Value::tabela(std::move(rows));
  } catch (...) {
    fecha_banco(db, h);
    throw;
  }
}

Value duckdb_agrupar(const Value& tabela, const std::string& chave,
                     const std::vector<DuckdbAggSpec>& agregacoes) {
  if (chave.empty() || agregacoes.empty()) die("agrupar: chave e agregacoes sao obrigatorias");
  std::string sql = "SELECT " + ident_duck(chave);
  for (const DuckdbAggSpec& agg : agregacoes) {
    const std::string col = agg.coluna.empty() ? "*" : ident_duck(agg.coluna);
    const std::string numero = agg.coluna.empty() ? "0" : "COALESCE(" + col + ", 0)";
    std::string expr;
    if (agg.funcao == "contar") expr = "COUNT(*)";
    else if (agg.funcao == "somar") expr = "SUM(" + numero + ")";
    else if (agg.funcao == "media") expr = "SUM(" + numero + ") / COUNT(*)";
    else if (agg.funcao == "min") expr = "MIN(" + numero + ")";
    else if (agg.funcao == "max") expr = "MAX(" + numero + ")";
    else if (agg.funcao == "variancia") expr = "VAR_POP(" + numero + ")";
    else if (agg.funcao == "distintos") expr = "COUNT(DISTINCT " + col + ")";
    else if (agg.funcao == "quantil" || agg.funcao == "quantil_aproximado")
      expr = "QUANTILE_CONT(" + col + ", " + std::to_string(agg.quantil) + ")";
    else die("agrupar: funcao '" + agg.funcao + "' fora do motor DuckDB");
    sql += ", " + expr + " AS " + ident_duck(agg.nome);
  }
  sql += " FROM \"t\" GROUP BY " + ident_duck(chave);
  return duckdb_consulta_tabelas(sql, {{"t", tabela}}, {});
}

Value duckdb_juntar(const Value& esquerda, const Value& direita,
                    const std::vector<std::string>& chaves, const std::string& tipo) {
  if (chaves.empty()) die("juntar: nenhuma chave informada");
  Value esq = esquerda;
  Value dir = direita;
  // Preserve the columnar representation all the way to the DuckDB bridge.
  // The old path materialized every row map just to discover the schema,
  // defeating the data_chunk/appender fast path for joins.
  auto nomes_tabela = [](Value& tabela) {
    std::vector<std::string> nomes;
    if (ColumnarTable* colunas = tabela.columnar()) {
      nomes = colunas->names;
      return nomes;
    }
    if (!tabela.list_ref()) die("tabela invalida para DuckDB");
    for (const Value& linha : *tabela.list_ref()) {
      if (!linha.map_ref()) die("tabela invalida para DuckDB");
      for (const auto& [nome, _] : linha.map_ref()->items)
        if (std::find(nomes.begin(), nomes.end(), nome) == nomes.end()) nomes.push_back(nome);
    }
    return nomes;
  };
  const std::vector<std::string> nomes_esq = nomes_tabela(esq);
  const std::vector<std::string> nomes_dir = nomes_tabela(dir);
  if (nomes_esq.empty() || nomes_dir.empty()) die("tabelas invalidas para DuckDB: sem colunas");
  std::string sql = "SELECT ";
  bool first = true;
  for (const std::string& nome : nomes_esq) {
    if (!first) sql += ", ";
    first = false;
    sql += "e." + ident_duck(nome) + " AS " + ident_duck(nome);
  }
  for (const std::string& nome : nomes_dir) {
    if (std::find(chaves.begin(), chaves.end(), nome) != chaves.end()) continue;
    std::string saida = nome;
    if (std::find(nomes_esq.begin(), nomes_esq.end(), saida) != nomes_esq.end()) saida += "_direita";
    if (!first) sql += ", ";
    first = false;
    sql += "d." + ident_duck(nome) + " AS " + ident_duck(saida);
  }
  if (first) sql += "e." + ident_duck(chaves.front());
  std::string join;
  if (tipo == "esquerda" || tipo == "left") join = " LEFT JOIN ";
  else if (tipo == "direita" || tipo == "right") join = " RIGHT JOIN ";
  else if (tipo == "completa" || tipo == "full" || tipo == "outer") join = " FULL OUTER JOIN ";
  else join = " INNER JOIN ";
  sql += " FROM \"l\" AS e" + join + "\"r\" AS d ON ";
  for (std::size_t i = 0; i < chaves.size(); ++i) {
    if (i) sql += " AND ";
    sql += "e." + ident_duck(chaves[i]) + " = d." + ident_duck(chaves[i]);
  }
  return duckdb_consulta_tabelas(sql, {{"l", esquerda}, {"r", direita}}, {});
}

}  // namespace tilt::rt
