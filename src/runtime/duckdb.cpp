#include "runtime/duckdb.hpp"

#include "runtime/compat.hpp"

#include <cstdint>
#include <stdexcept>

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

// Abre o banco e uma conexão; em qualquer falha já morre com mensagem clara.
// A conexão e o banco precisam ser fechados pelo caller (disconnect/close
// aceitam ponteiros e zeram os handles).
struct Conn {
  const DuckdbApi& db;
  void* database = nullptr;
  void* connection = nullptr;

  explicit Conn(const DuckdbApi& d, const std::string& db_path) : db(d) {
    if (db.open(db_path.c_str(), &database) != kDuckdbSuccess || database == nullptr) {
      die("nao foi possivel abrir o banco '" + db_path + "'");
    }
    if (db.connect(database, &connection) != kDuckdbSuccess || connection == nullptr) {
      db.close(&database);
      die("nao foi possivel conectar no banco '" + db_path + "'");
    }
  }
  ~Conn() {
    db.disconnect(&connection);
    db.close(&database);
  }
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;
};

std::string result_error(const DuckdbApi& db, void* result) {
  const char* err = db.result_error(result);
  return err ? err : "erro desconhecido";
}

}  // namespace

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

  Conn conn(db, db_path);
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
  const auto nrows = db.row_count(&result);

  ValueList rows;
  rows.reserve(static_cast<std::size_t>(nrows));
  for (std::uint64_t r = 0; r < nrows; ++r) {
    Value row = Value::mapa();
    for (std::uint64_t c = 0; c < ncols; ++c) {
      const char* name = db.column_name(&result, c);
      const std::string col = (name && *name) ? name : ("coluna" + std::to_string(c + 1));
      if (db.value_is_null(&result, c, r)) {
        row.map->set(col, Value::nulo());
        continue;
      }
      switch (db.column_type(&result, c)) {
        case kDuckdbBoolean:
        case kDuckdbTinyint:
        case kDuckdbSmallint:
        case kDuckdbInteger:
        case kDuckdbBigint:
          row.map->set(col, Value::inteiro(db.value_int64(&result, c, r)));
          break;
        case kDuckdbFloat:
        case kDuckdbDouble:
          row.map->set(col, Value::decimal(db.value_double(&result, c, r)));
          break;
        case kDuckdbVarchar:
        default: {
          // VARCHAR e demais tipos (DATE, TIMESTAMP, DECIMAL, UUID, BLOB...)
          // chegam como texto; o valor precisa ser liberado com duckdb_free.
          char* txt = db.value_varchar(&result, c, r);
          row.map->set(col, Value::texto(txt ? txt : ""));
          if (txt) db.free_value(txt);
          break;
        }
      }
    }
    rows.push_back(std::move(row));
  }

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
  Conn conn(db, db_path);
  DuckdbResult result;
  if (db.query(conn.connection, sql.c_str(), &result) != kDuckdbSuccess) {
    const std::string msg = result_error(db, &result);
    db.destroy_result(&result);
    die("falha ao executar comando: " + msg);
  }
  db.destroy_result(&result);
}

}  // namespace tilt::rt
