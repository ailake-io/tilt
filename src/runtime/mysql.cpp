#include "runtime/mysql.hpp"

#include "runtime/compat.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("mysql: " + m); }

// Constantes de mysql.h / enum_field_types (não incluímos o header para
// manter zero deps). Valores estáveis da C API do MySQL/MariaDB.
constexpr int kTypeDecimal = 0;
constexpr int kTypeTiny = 1;
constexpr int kTypeShort = 2;
constexpr int kTypeLong = 3;
constexpr int kTypeFloat = 4;
constexpr int kTypeDouble = 5;
constexpr int kTypeLonglong = 8;
constexpr int kTypeInt24 = 9;
constexpr int kTypeYear = 13;
constexpr int kTypeNewdecimal = 246;

// MYSQL_FIELD é opaco para nós: espelhamos o layout público estável da C API
// (64-bit) só para ler `name` e `type`, sem depender do header.
struct MysqlField {
  char* name;
  char* org_name;
  char* table;
  char* org_table;
  char* db;
  char* catalog;
  char* def;
  unsigned long length;
  unsigned long max_length;
  unsigned int name_length;
  unsigned int org_name_length;
  unsigned int table_length;
  unsigned int org_table_length;
  unsigned int db_length;
  unsigned int catalog_length;
  unsigned int def_length;
  unsigned int flags;
  unsigned int decimals;
  unsigned int charsetnr;
  int type;  // enum enum_field_types
  void* extension;
};

struct MysqlApi {
  void* lib = nullptr;
  void* (*init)(void*) = nullptr;
  void* (*real_connect)(void*, const char*, const char*, const char*, const char*, unsigned int,
                        const char*, unsigned long) = nullptr;
  int (*query)(void*, const char*) = nullptr;
  void* (*store_result)(void*) = nullptr;
  unsigned int (*num_fields)(void*) = nullptr;
  std::uint64_t (*num_rows)(void*) = nullptr;
  char** (*fetch_row)(void*) = nullptr;
  unsigned long* (*fetch_lengths)(void*) = nullptr;
  MysqlField* (*fetch_field)(void*) = nullptr;
  void (*free_result)(void*) = nullptr;
  const char* (*error)(void*) = nullptr;
  void (*close)(void*) = nullptr;
};

template <typename F>
bool bind_sym(void* lib, F& fn, const char* name) {
  fn = reinterpret_cast<F>(tilt_dlsym(lib, name));
  return fn != nullptr;
}

// Carrega a biblioteca uma única vez. Falha de carga não lança aqui:
// mysql_query()/mysql_exec() verificam `lib` e morrem com mensagem acionável.
const MysqlApi& api() {
  static const MysqlApi instance = [] {
    MysqlApi a;
#if defined(_WIN32)
    a.lib = tilt_dlopen("libmariadb.dll");
    if (!a.lib) a.lib = tilt_dlopen("libmysql.dll");
#else
    a.lib = tilt_dlopen("libmariadb.so.3");
    if (!a.lib) a.lib = tilt_dlopen("libmariadb.so");
    if (!a.lib) a.lib = tilt_dlopen("libmysqlclient.so.21");
    if (!a.lib) a.lib = tilt_dlopen("libmysqlclient.so");
    if (!a.lib) a.lib = tilt_dlopen("libmariadb.3.dylib");
    if (!a.lib) a.lib = tilt_dlopen("libmariadb.dylib");
    if (!a.lib) a.lib = tilt_dlopen("libmysqlclient.21.dylib");
    if (!a.lib) a.lib = tilt_dlopen("libmysqlclient.dylib");
#endif
    if (!a.lib) return a;
    const bool ok = bind_sym(a.lib, a.init, "mysql_init") &&
                    bind_sym(a.lib, a.real_connect, "mysql_real_connect") &&
                    bind_sym(a.lib, a.query, "mysql_query") &&
                    bind_sym(a.lib, a.store_result, "mysql_store_result") &&
                    bind_sym(a.lib, a.num_fields, "mysql_num_fields") &&
                    bind_sym(a.lib, a.num_rows, "mysql_num_rows") &&
                    bind_sym(a.lib, a.fetch_row, "mysql_fetch_row") &&
                    bind_sym(a.lib, a.fetch_lengths, "mysql_fetch_lengths") &&
                    bind_sym(a.lib, a.fetch_field, "mysql_fetch_field") &&
                    bind_sym(a.lib, a.free_result, "mysql_free_result") &&
                    bind_sym(a.lib, a.error, "mysql_error") &&
                    bind_sym(a.lib, a.close, "mysql_close");
    if (!ok) {
      tilt_dlclose(a.lib);
      a = MysqlApi{};
    }
    return a;
  }();
  return instance;
}

[[noreturn]] void die_lib_not_found() {
#if defined(_WIN32)
  die("libmariadb/libmysqlclient nao encontrada: instale o MariaDB/MySQL client "
      "(libmariadb.dll no PATH)");
#else
  die("libmariadb/libmysqlclient nao encontrada: instale o pacote (MariaDB/MySQL client)");
#endif
}

struct MysqlUrl {
  std::string user;
  std::string pass;
  std::string host = "localhost";
  std::string db;
  unsigned int port = 3306;
};

// "mysql://usuario:senha@host:porta/banco" (esquema "mariadb://" tambem
// aceito). Userinfo e porta sao opcionais; tudo que falta usa o default do
// cliente C (host "localhost", porta 3306, sem schema).
MysqlUrl parse_url(const std::string& url) {
  std::string rest = url;
  const bool mysql_scheme = rest.rfind("mysql://", 0) == 0;
  const bool maria_scheme = rest.rfind("mariadb://", 0) == 0;
  if (!mysql_scheme && !maria_scheme) {
    die("url invalida: '" + url + "' (use mysql://usuario:senha@host:porta/banco)");
  }
  rest = rest.substr(maria_scheme ? 10 : 8);

  MysqlUrl out;
  const std::size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  if (slash != std::string::npos) {
    out.db = rest.substr(slash + 1);
    if (out.db.empty() || out.db.find('/') != std::string::npos ||
        out.db.find('?') != std::string::npos || out.db.find('#') != std::string::npos) {
      die("url invalida: '" + url + "' (banco deve ser um unico nome, sem query string)");
    }
  }

  const std::size_t at = authority.rfind('@');
  std::string hostport = authority;
  if (at != std::string::npos) {
    const std::string userinfo = authority.substr(0, at);
    hostport = authority.substr(at + 1);
    const std::size_t colon = userinfo.find(':');
    out.user = colon == std::string::npos ? userinfo : userinfo.substr(0, colon);
    out.pass = colon == std::string::npos ? "" : userinfo.substr(colon + 1);
  }
  if (hostport.empty()) hostport = "localhost";

  const std::size_t colon = hostport.rfind(':');
  if (colon != std::string::npos) {
    const std::string port_str = hostport.substr(colon + 1);
    if (port_str.empty() ||
        port_str.find_first_not_of("0123456789") != std::string::npos) {
      die("url invalida: '" + url + "' (porta deve ser numerica)");
    }
    try {
      const unsigned long p = std::stoul(port_str);
      if (p == 0 || p > 65535) throw std::out_of_range("porta");
      out.port = static_cast<unsigned int>(p);
    } catch (const std::exception&) {
      die("url invalida: '" + url + "' (porta fora da faixa 1-65535)");
    }
    out.host = hostport.substr(0, colon);
    if (out.host.empty()) out.host = "localhost";
  } else {
    out.host = hostport;
  }
  return out;
}

// Só consultas que retornam linhas passam (mesmo padrao do conector
// duckdb): a C API nao distingue "sem resultado" de erro sem
// mysql_field_count, entao a primeira palavra do SQL decide.
bool returns_rows(const std::string& sql) {
  std::size_t i = 0;
  auto skip_space_and_comments = [&]() {
    while (i < sql.size()) {
      if (sql[i] == ' ' || sql[i] == '\t' || sql[i] == '\n' || sql[i] == '\r') {
        ++i;
      } else if (sql[i] == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
        while (i < sql.size() && sql[i] != '\n') ++i;
      } else if (sql[i] == '#' && i + 1 < sql.size() && sql[i + 1] == ' ') {
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
  return kw == "SELECT" || kw == "WITH" || kw == "SHOW" || kw == "DESCRIBE" ||
         kw == "DESC" || kw == "EXPLAIN";
}

// Conecta; em qualquer falha já morre com a mensagem do servidor.
struct Conn {
  const MysqlApi& db;
  void* conn = nullptr;

  explicit Conn(const MysqlApi& d, const MysqlUrl& url) : db(d) {
    conn = db.init(nullptr);
    if (!conn) die("falha de memoria ao iniciar a conexao");
    void* ok = db.real_connect(conn, url.host.c_str(), url.user.empty() ? nullptr : url.user.c_str(),
                               url.pass.empty() ? nullptr : url.pass.c_str(),
                               url.db.empty() ? nullptr : url.db.c_str(), url.port, nullptr, 0);
    if (!ok) {
      const std::string msg = db.error(conn) ? db.error(conn) : "erro desconhecido";
      db.close(conn);
      die("falha na conexao: " + msg);
    }
  }
  ~Conn() {
    if (conn) db.close(conn);
  }
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;
};

bool is_integer_type(int t) {
  return t == kTypeTiny || t == kTypeShort || t == kTypeLong || t == kTypeInt24 ||
         t == kTypeLonglong || t == kTypeYear;
}

bool is_decimal_type(int t) {
  return t == kTypeDecimal || t == kTypeNewdecimal || t == kTypeFloat || t == kTypeDouble;
}

}  // namespace

Value mysql_query(const std::string& url, const std::string& sql) {
  const MysqlApi& db = api();
  if (!db.lib) die_lib_not_found();
  const MysqlUrl parsed = parse_url(url);
  if (!returns_rows(sql)) {
    die("apenas consultas SELECT sao suportadas nesta versao; para INSERT/UPDATE/DDL use executar_sql");
  }

  Conn conn(db, parsed);
  if (db.query(conn.conn, sql.c_str()) != 0) {
    die(std::string("falha ao executar consulta: ") +
        (db.error(conn.conn) ? db.error(conn.conn) : "erro desconhecido"));
  }
  void* res = db.store_result(conn.conn);
  if (!res) {
    die(std::string("falha ao obter resultado: ") +
        (db.error(conn.conn) ? db.error(conn.conn) : "consulta nao retornou linhas"));
  }

  const unsigned int ncols = db.num_fields(res);
  const auto nrows = db.num_rows(res);

  // Os metadados das colunas vem de um cursor sequencial no result set —
  // lemos todos antes de iterar as linhas.
  std::vector<std::string> names;
  std::vector<int> types;
  names.reserve(ncols);
  types.reserve(ncols);
  for (unsigned int c = 0; c < ncols; ++c) {
    const MysqlField* f = db.fetch_field(res);
    names.emplace_back(f && f->name && *f->name ? f->name : ("coluna" + std::to_string(c + 1)));
    types.push_back(f ? f->type : -1);
  }

  ValueList rows;
  rows.reserve(static_cast<std::size_t>(nrows));
  char** raw = nullptr;
  while ((raw = db.fetch_row(res)) != nullptr) {
    const unsigned long* lens = db.fetch_lengths(res);
    Value row = Value::mapa();
    for (unsigned int c = 0; c < ncols; ++c) {
      const std::string& col = names[c];
      if (!raw[c]) {
        row.map->set(col, Value::nulo());
        continue;
      }
      const char* val = raw[c];
      if (is_integer_type(types[c])) {
        row.map->set(col, Value::inteiro(std::strtoll(val, nullptr, 10)));
      } else if (is_decimal_type(types[c])) {
        row.map->set(col, Value::decimal(std::strtod(val, nullptr)));
      } else {
        row.map->set(col, Value::texto(std::string(val, lens ? lens[c] : std::strlen(val))));
      }
    }
    rows.push_back(std::move(row));
  }

  db.free_result(res);
  return Value::tabela(std::move(rows));
}

void mysql_exec(const std::string& url, const std::string& sql) {
  const MysqlApi& db = api();
  if (!db.lib) die_lib_not_found();
  const MysqlUrl parsed = parse_url(url);

  Conn conn(db, parsed);
  if (db.query(conn.conn, sql.c_str()) != 0) {
    die(std::string("falha ao executar comando: ") +
        (db.error(conn.conn) ? db.error(conn.conn) : "erro desconhecido"));
  }
  // Comando com resultado inesperado (ex.: um SELECT): consome o result set
  // para nao deixar a conexao fora de sincronia antes do close.
  if (void* res = db.store_result(conn.conn)) db.free_result(res);
}

}  // namespace tilt::rt
