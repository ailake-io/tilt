#include "runtime/mysql.hpp"

#include "runtime/compat.hpp"
#include "runtime/sql_params.hpp"
#include "runtime/sql_pool.hpp"

#include <cstdint>
#include <cstdio>
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
  // Prepared server-side (Marco 3 / D2): mysql_stmt_*. Retornos my_bool/bool
  // (1 byte nos dois) vao em unsigned char; fetch: 0 = linha, 1 = erro,
  // 100 = sem dados, 101 = truncado (MYSQL_NO_DATA/DATA_TRUNCATED).
  void* (*stmt_init)(void*) = nullptr;
  int (*stmt_prepare)(void*, const char*, unsigned long) = nullptr;
  unsigned long (*stmt_param_count)(void*) = nullptr;
  unsigned char (*stmt_bind_param)(void*, void*) = nullptr;
  int (*stmt_execute)(void*) = nullptr;
  unsigned int (*stmt_field_count)(void*) = nullptr;
  int (*stmt_store_result)(void*) = nullptr;
  void* (*stmt_result_metadata)(void*) = nullptr;
  unsigned char (*stmt_bind_result)(void*, void*) = nullptr;
  int (*stmt_fetch)(void*) = nullptr;
  int (*stmt_fetch_column)(void*, void*, unsigned int, unsigned long) = nullptr;
  unsigned char (*stmt_free_result)(void*) = nullptr;
  unsigned char (*stmt_close)(void*) = nullptr;
  const char* (*stmt_error)(void*) = nullptr;
  // Validacao do pool (mysql_ping existe em toda libmysqlclient/mariadb).
  int (*ping)(void*) = nullptr;
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
                    bind_sym(a.lib, a.close, "mysql_close") &&
                    bind_sym(a.lib, a.stmt_init, "mysql_stmt_init") &&
                    bind_sym(a.lib, a.stmt_prepare, "mysql_stmt_prepare") &&
                    bind_sym(a.lib, a.stmt_param_count, "mysql_stmt_param_count") &&
                    bind_sym(a.lib, a.stmt_bind_param, "mysql_stmt_bind_param") &&
                    bind_sym(a.lib, a.stmt_execute, "mysql_stmt_execute") &&
                    bind_sym(a.lib, a.stmt_field_count, "mysql_stmt_field_count") &&
                    bind_sym(a.lib, a.stmt_store_result, "mysql_stmt_store_result") &&
                    bind_sym(a.lib, a.stmt_result_metadata, "mysql_stmt_result_metadata") &&
                    bind_sym(a.lib, a.stmt_bind_result, "mysql_stmt_bind_result") &&
                    bind_sym(a.lib, a.stmt_fetch, "mysql_stmt_fetch") &&
                    bind_sym(a.lib, a.stmt_fetch_column, "mysql_stmt_fetch_column") &&
                    bind_sym(a.lib, a.stmt_free_result, "mysql_stmt_free_result") &&
                    bind_sym(a.lib, a.stmt_close, "mysql_stmt_close") &&
                    bind_sym(a.lib, a.stmt_error, "mysql_stmt_error") &&
                    bind_sym(a.lib, a.ping, "mysql_ping");
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

// Abre; em qualquer falha já morre com a mensagem do servidor.
void* abre_conn(const MysqlApi& db, const MysqlUrl& url) {
  void* conn = db.init(nullptr);
  if (!conn) die("falha de memoria ao iniciar a conexao");
  void* ok = db.real_connect(conn, url.host.c_str(), url.user.empty() ? nullptr : url.user.c_str(),
                             url.pass.empty() ? nullptr : url.pass.c_str(),
                             url.db.empty() ? nullptr : url.db.c_str(), url.port, nullptr, 0);
  if (!ok) {
    const std::string msg = db.error(conn) ? db.error(conn) : "erro desconhecido";
    db.close(conn);
    die("falha na conexao: " + msg);
  }
  return conn;
}

// Conexao via pool (statements avulsos) ou dedicada (transacao, pooled=false).
// O callback de fechar fica no pool depois do Conn morrer: capture a API (singleton),
// nunca `this`. O handle continua em `conn`; o pool valida com mysql_ping no checkout.
struct Conn {
  const MysqlApi& db;
  PooledConn pool;
  void* conn = nullptr;

  Conn(const MysqlApi& d, const std::string& url, const MysqlUrl& parsed, const std::string& sql,
       bool pooled = true)
      : db(d),
        pool(pooled ? "mysql" : "", url, [&] { return abre_conn(db, parsed); },
             [&](void* h) { return !db.ping || db.ping(h) == 0; },
             [&d](void* h) { d.close(h); }, pooled ? sql : "") {
    conn = pool.get();
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

Value executa_select(const MysqlApi& db, void* conn, const std::string& final_sql) {
  if (db.query(conn, final_sql.c_str()) != 0) {
    die(std::string("falha ao executar consulta: ") +
        (db.error(conn) ? db.error(conn) : "erro desconhecido"));
  }
  void* res = db.store_result(conn);
  if (!res) {
    die(std::string("falha ao obter resultado: ") +
        (db.error(conn) ? db.error(conn) : "consulta nao retornou linhas"));
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

Value mysql_query(const std::string& url, const std::string& sql) {
  const MysqlApi& db = api();
  if (!db.lib) die_lib_not_found();
  const MysqlUrl parsed = parse_url(url);
  if (!returns_rows(sql)) {
    die("apenas consultas SELECT sao suportadas nesta versao; para INSERT/UPDATE/DDL use executar_sql");
  }
  Conn conn(db, url, parsed, sql);
  return executa_select(db, conn.conn, sql);
}

void mysql_exec(const std::string& url, const std::string& sql) {
  const MysqlApi& db = api();
  if (!db.lib) die_lib_not_found();
  const MysqlUrl parsed = parse_url(url);

  Conn conn(db, url, parsed, sql);
  if (db.query(conn.conn, sql.c_str()) != 0) {
    die(std::string("falha ao executar comando: ") +
        (db.error(conn.conn) ? db.error(conn.conn) : "erro desconhecido"));
  }
  // Comando com resultado inesperado (ex.: um SELECT): consome o result set
  // para nao deixar a conexao fora de sincronia antes do close.
  if (void* res = db.store_result(conn.conn)) db.free_result(res);
}

// Espelho de MYSQL_BIND (Marco 3 / D2): so os campos que usamos, com os
// mesmos tipos dos dois headers (ponteiros/unsigned long/unsigned int/int;
// indicadores de 1 byte como unsigned char). libmysqlclient usa bool e
// libmariadb usa my_bool (char) — ambos 1 byte, 0/1 identicos; `param_number`
// (MySQL) e `flags` (MariaDB) ocupam o mesmo offset/tamanho e ficam zerados.
// Verificado contra mysql.h 8.4 e mariadb_stmt.h (master).
struct MysqlBind {
  unsigned long* length = nullptr;
  unsigned char* is_null = nullptr;
  void* buffer = nullptr;
  unsigned char* error = nullptr;
  void* row_ptr = nullptr;
  void* store_param_func = nullptr;
  void* fetch_result = nullptr;
  void* skip_result = nullptr;
  unsigned long buffer_length = 0;
  unsigned long offset = 0;
  unsigned long length_value = 0;
  unsigned int param_number = 0;
  unsigned int pack_length = 0;
  int buffer_type = 0;
  unsigned char error_value = 0;
  unsigned char is_unsigned = 0;
  unsigned char long_data_used = 0;
  unsigned char is_null_value = 0;
  void* extension = nullptr;
};

// Códigos de tipo do protocolo (iguais nas duas libs): STRING/VAR_STRING
// para ligar tudo como texto (o servidor coage para o tipo da coluna),
// BLOB para ler qualquer coluna como bytes.
constexpr int kStmtString = 254;
constexpr int kStmtBlob = 252;
// Retornos de mysql_stmt_fetch: 0 = linha, 1 = erro, 100 = sem mais dados,
// 101 = valor truncado (buscar o resto com fetch_column).
constexpr int kFetchOk = 0;
constexpr int kFetchNoData = 100;
constexpr int kFetchTruncated = 101;

// Statement com RAII (fecha no fim; reset implicito pelo close).
struct Stmt {
  const MysqlApi& db;
  void* st = nullptr;
  explicit Stmt(const MysqlApi& d, void* conn) : db(d) {
    st = db.stmt_init(conn);
    if (!st) die("falha de memoria ao iniciar prepared statement");
  }
  ~Stmt() {
    if (st) db.stmt_close(st);
  }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  const char* erro() const {
    const char* m = st ? db.stmt_error(st) : nullptr;
    return m && *m ? m : "erro desconhecido";
  }
};

// Formata um parametro como texto (o servidor coage para o tipo da coluna);
// nulo e sinalizado pelo indicador (buffer vazio, ignorado).
std::string mysql_param_texto(const SqlParam& p, unsigned char& nulo) {
  nulo = 0;
  switch (p.tipo) {
    case SqlParam::Tipo::Nulo: nulo = 1; return "";
    case SqlParam::Tipo::Inteiro: return std::to_string(p.i);
    case SqlParam::Tipo::Decimal: {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.17g", p.d);
      return buf;
    }
    case SqlParam::Tipo::Logico: return p.b ? "1" : "0";
    case SqlParam::Tipo::Texto: return p.s;
  }
  return "";
}

// Prepara `sql` e liga `params` como texto (binary protocol, sem escape e
// sem interpolacao). `acao` e "comando" ou "consulta", so para mensagens.
void prepara_e_liga(const MysqlApi& db, Stmt& st, const std::string& sql,
                    const std::vector<SqlParam>& params, const std::string& passo,
                    const std::string& acao,
                    std::vector<std::string>& textos, std::vector<unsigned long>& lens,
                    std::vector<unsigned char>& nulos, std::vector<MysqlBind>& binds) {
  if (db.stmt_prepare(st.st, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
    die(passo + std::string("falha ao preparar ") + acao + ": " + st.erro());
  }
  const unsigned long nq = db.stmt_param_count(st.st);
  if (nq != params.size()) {
    die(passo + "esperava " + std::to_string(params.size()) + " parametro(s), mas o SQL tem " +
        std::to_string(nq) + " '?'");
  }
  textos.clear();
  textos.reserve(params.size());
  lens.assign(params.size(), 0);
  nulos.assign(params.size(), 0);
  binds.assign(params.size(), MysqlBind{});
  for (std::size_t k = 0; k < params.size(); ++k) {
    textos.push_back(mysql_param_texto(params[k], nulos[k]));
    lens[k] = static_cast<unsigned long>(textos.back().size());
  }
  // So depois de todos os push_back (sem mais realocacao): os ponteiros de
  // buffer ficam estaveis ate o fim do execute.
  for (std::size_t k = 0; k < params.size(); ++k) {
    MysqlBind& b = binds[k];
    b.buffer_type = kStmtString;
    b.buffer = textos[k].data();
    b.buffer_length = lens[k];
    b.length = &lens[k];
    b.is_null = &nulos[k];
  }
  if (!binds.empty() && db.stmt_bind_param(st.st, binds.data()) != 0) {
    die(passo + std::string("falha ao ligar parametros: ") + st.erro());
  }
}

void exec_um(const MysqlApi& db, void* conn, const std::string& sql,
             const std::vector<SqlParam>& params, const std::string& passo) {
  Stmt st(db, conn);
  std::vector<std::string> textos;
  std::vector<unsigned long> lens;
  std::vector<unsigned char> nulos;
  std::vector<MysqlBind> binds;
  prepara_e_liga(db, st, sql, params, passo, "comando", textos, lens, nulos, binds);
  if (db.stmt_execute(st.st) != 0) {
    die(passo + std::string("falha ao executar comando: ") + st.erro());
  }
  // SELECT acidental num passo: consome o result set para nao deixar a
  // conexao fora de sincronia (o valor e descartado; leitura e via ler).
  if (db.stmt_field_count(st.st) > 0) {
    if (db.stmt_store_result(st.st) == 0) db.stmt_free_result(st.st);
  }
}

void mysql_exec_params(const std::string& url, const std::string& sql,
                       const std::vector<SqlParam>& params) {
  const MysqlApi& db = api();
  if (!db.lib) die_lib_not_found();
  const MysqlUrl parsed = parse_url(url);
  Conn conn(db, url, parsed, sql);
  exec_um(db, conn.conn, sql, params, "");
}

// Consulta com `?` via prepared server-side (SELECT com params): executa o
// statement, le todas as colunas como bytes (BLOB) e converte pelos tipos do
// metadata — mesmo mapeamento do caminho textual. Truncacao (buffer inicial
// de 256B) e resolvida por coluna com fetch_column.
Value mysql_query_params(const std::string& url, const std::string& sql,
                         const std::vector<SqlParam>& params) {
  const MysqlApi& db = api();
  if (!db.lib) die_lib_not_found();
  const MysqlUrl parsed = parse_url(url);
  if (!returns_rows(sql)) {
    die("apenas consultas SELECT sao suportadas nesta versao; para INSERT/UPDATE/DDL use executar_sql");
  }
  Conn conn(db, url, parsed, sql);
  Stmt st(db, conn.conn);
  std::vector<std::string> textos;
  std::vector<unsigned long> lens;
  std::vector<unsigned char> nulos;
  std::vector<MysqlBind> binds;
  prepara_e_liga(db, st, sql, params, "", "consulta", textos, lens, nulos, binds);
  if (db.stmt_execute(st.st) != 0) {
    die(std::string("falha ao executar consulta: ") + st.erro());
  }
  if (db.stmt_field_count(st.st) == 0) {
    die("apenas consultas SELECT sao suportadas nesta versao");
  }
  if (db.stmt_store_result(st.st) != 0) {
    die(std::string("falha ao obter resultado: ") + st.erro());
  }
  void* meta = db.stmt_result_metadata(st.st);
  if (!meta) {
    db.stmt_free_result(st.st);
    die(std::string("falha ao obter resultado: ") + st.erro());
  }
  const unsigned int ncols = db.num_fields(meta);
  std::vector<std::string> names;
  std::vector<int> types;
  names.reserve(ncols);
  types.reserve(ncols);
  for (unsigned int c = 0; c < ncols; ++c) {
    const MysqlField* f = db.fetch_field(meta);
    names.emplace_back(f && f->name && *f->name ? f->name : ("coluna" + std::to_string(c + 1)));
    types.push_back(f ? f->type : -1);
  }

  struct Coluna {
    std::vector<char> buf = std::vector<char>(256);
    unsigned long len = 0;
    unsigned char nulo = 0;
    unsigned char erro = 0;
  };
  std::vector<Coluna> cols(ncols);
  std::vector<MysqlBind> saidas(ncols);
  for (unsigned int c = 0; c < ncols; ++c) {
    MysqlBind& b = saidas[c];
    b.buffer_type = kStmtBlob;
    b.buffer = cols[c].buf.data();
    b.buffer_length = static_cast<unsigned long>(cols[c].buf.size());
    b.length = &cols[c].len;
    b.is_null = &cols[c].nulo;
    b.error = &cols[c].erro;
  }
  if (db.stmt_bind_result(st.st, saidas.data()) != 0) {
    db.free_result(meta);
    db.stmt_free_result(st.st);
    die(std::string("falha ao ligar resultado: ") + st.erro());
  }

  auto monta_linha = [&]() {
    Value row = Value::mapa();
    for (unsigned int c = 0; c < ncols; ++c) {
      const std::string& col = names[c];
      if (cols[c].nulo) {
        row.map->set(col, Value::nulo());
        continue;
      }
      const char* val = cols[c].buf.data();
      const std::size_t n = static_cast<std::size_t>(cols[c].len);
      if (is_integer_type(types[c])) {
        // Buffer BLOB nao e NUL-terminado: copia antes de converter.
        row.map->set(col, Value::inteiro(std::strtoll(std::string(val, n).c_str(), nullptr, 10)));
      } else if (is_decimal_type(types[c])) {
        row.map->set(col, Value::decimal(std::strtod(std::string(val, n).c_str(), nullptr)));
      } else {
        row.map->set(col, Value::texto(std::string(val, n)));
      }
    }
    return row;
  };

  ValueList rows;
  while (true) {
    const int rc = db.stmt_fetch(st.st);
    if (rc == kFetchNoData) break;
    if (rc != kFetchOk && rc != kFetchTruncated) {
      db.free_result(meta);
      db.stmt_free_result(st.st);
      die(std::string("falha ao ler linha: ") + st.erro());
    }
    // Coluna maior que o buffer: refaz com o tamanho real e religa o buffer
    // (o length real vem no proprio indicador, mesmo truncado).
    for (unsigned int c = 0; c < ncols; ++c) {
      if (!cols[c].erro || cols[c].nulo) continue;
      cols[c].buf.assign(static_cast<std::size_t>(cols[c].len), '\0');
      MysqlBind unico{};
      unico.buffer_type = kStmtBlob;
      unico.buffer = cols[c].buf.data();
      unico.buffer_length = static_cast<unsigned long>(cols[c].buf.size());
      unico.length = &cols[c].len;
      unico.is_null = &cols[c].nulo;
      if (db.stmt_fetch_column(st.st, &unico, c, 0) != 0) {
        db.free_result(meta);
        db.stmt_free_result(st.st);
        die(std::string("falha ao ler coluna truncada: ") + st.erro());
      }
      cols[c].erro = 0;
      saidas[c].buffer = cols[c].buf.data();
      saidas[c].buffer_length = static_cast<unsigned long>(cols[c].buf.size());
    }
    rows.push_back(monta_linha());
  }

  db.free_result(meta);
  db.stmt_free_result(st.st);
  return Value::tabela(std::move(rows));
}

void mysql_transact(const std::string& url,
                    const std::vector<std::pair<std::string, std::vector<SqlParam>>>& passos) {
  const MysqlApi& db = api();
  if (!db.lib) die_lib_not_found();
  const MysqlUrl parsed = parse_url(url);
  Conn conn(db, url, parsed, "", false);  // transacao: conexao dedicada, fora do pool
  auto simples = [&](const char* sql) {
    if (db.query(conn.conn, sql) != 0) {
      die(std::string("falha em ") + sql + ": " +
          (db.error(conn.conn) ? db.error(conn.conn) : "erro desconhecido"));
    }
    if (void* res = db.store_result(conn.conn)) db.free_result(res);
  };
  simples("START TRANSACTION");
  for (std::size_t k = 0; k < passos.size(); ++k) {
    try {
      exec_um(db, conn.conn, passos[k].first, passos[k].second,
              "passo " + std::to_string(k + 1) + ": ");
    } catch (...) {
      try {
        simples("ROLLBACK");
      } catch (...) {
      }
      throw;
    }
  }
  simples("COMMIT");
}

}  // namespace tilt::rt
