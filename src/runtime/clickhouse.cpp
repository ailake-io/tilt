#include "runtime/clickhouse.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "runtime/http_client.hpp"
#include "runtime/json.hpp"
#include "runtime/sql_params.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("clickhouse: " + m); }

std::string truncar(const std::string& s, std::size_t n) {
  return s.size() <= n ? s : s.substr(0, n);
}

// ClickHouse devolve o erro em texto puro no corpo, multilinha; normaliza
// para uma linha so antes de truncar (mesma ideia do truncar do http_client).
std::string corpo_erro(const std::string& body) {
  std::string out = body;
  for (char& c : out) {
    if (c == '\n' || c == '\r') c = ' ';
  }
  return truncar(out, 300);
}

// Percent-encoding para a query string (RFC 3986 unreserved, como no
// uri_encode do iceberg). O SQL vai inteiro no parametro `query`.
std::string uri_encode(const std::string& s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0xF];
    }
  }
  return out;
}

struct ClickHouseUrl {
  std::string user;
  std::string pass;
  std::string host = "localhost";
  std::string db;
  unsigned int port = 8123;
};

// "clickhouse://[usuario[:senha]@]host[:porta][/banco]". Userinfo e porta sao
// opcionais; sem userinfo, usuario/senha caem para CLICKHOUSE_USER/
// CLICKHOUSE_PASSWORD e, na ausencia delas, "default"/vazia.
ClickHouseUrl parse_url(const std::string& url) {
  std::string rest = url;
  if (rest.rfind("clickhouse://", 0) != 0) {
    die("url invalida: '" + url + "' (use clickhouse://[usuario[:senha]@]host[:porta][/banco])");
  }
  rest = rest.substr(13);

  ClickHouseUrl out;
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
  } else {
    if (const char* u = std::getenv("CLICKHOUSE_USER"); u && *u) out.user = u;
    if (const char* p = std::getenv("CLICKHOUSE_PASSWORD"); p && *p) out.pass = p;
  }
  if (out.user.empty()) out.user = "default";
  if (hostport.empty()) hostport = "localhost";

  const std::size_t colon = hostport.rfind(':');
  if (colon != std::string::npos) {
    const std::string port_str = hostport.substr(colon + 1);
    if (port_str.empty() || port_str.find_first_not_of("0123456789") != std::string::npos) {
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

// "http://host:porta/?query=<sql>&user=...&password=...&database=...".
// Autenticacao por parametros de query (o ClickHouse tambem aceita headers
// X-ClickHouse-User/Key; a query string e o que o proprio clickhouse-client
// usa). O POST sempre leva corpo (mesmo vazio): o http_request manda --data
// @arquivo, o que garante Content-Length — o ClickHouse rejeita POST sem ele.
std::string monta_url(const ClickHouseUrl& u, const std::string& sql) {
  std::string out = "http://" + u.host + ":" + std::to_string(u.port) + "/?query=" +
                    uri_encode(sql);
  out += "&user=" + uri_encode(u.user);
  if (!u.pass.empty()) out += "&password=" + uri_encode(u.pass);
  if (!u.db.empty()) out += "&database=" + uri_encode(u.db);
  return out;
}

// FORMAT ja especificado pelo autor? Procura a palavra "format" inteira,
// case-insensitiva (evita casar "reformatar" etc.).
bool tem_format(const std::string& sql) {
  auto ident = [](unsigned char c) {
    return std::isalnum(c) || c == '_';
  };
  for (std::size_t i = 0; i + 6 <= sql.size(); ++i) {
    bool match = true;
    for (std::size_t k = 0; k < 6; ++k) {
      if (std::tolower(static_cast<unsigned char>(sql[i + k])) != "format"[k]) {
        match = false;
        break;
      }
    }
    if (!match) continue;
    if (i > 0 && ident(static_cast<unsigned char>(sql[i - 1]))) continue;
    if (i + 6 < sql.size() && ident(static_cast<unsigned char>(sql[i + 6]))) continue;
    return true;
  }
  return false;
}

// Converte um valor ja parseado do NDJSON no Value de saida. Int*/UInt* vem
// como inteiro JSON, Float*/Decimal como numero JSON, String/FixedString/
// Date/DateTime como texto JSON, Nullable como null (ou o marcador "ᴺᵁᴸᴸ"
// quando o usuario pediu um FORMAT TSV/CSV, que devolve tudo como texto).
// Arrays/Tuples/Maps aninhados viram texto com a serializacao JSON.
Value valor_de_json(const Value& j) {
  switch (j.kind) {
    case ValueKind::Nulo:
      return Value::nulo();
    case ValueKind::Logico:
    case ValueKind::Inteiro:
    case ValueKind::Decimal:
      return j;
    case ValueKind::Texto:
      // marcador "ᴺᵁᴸᴸ" (U+1D3F3 U+1D3F1 U+1D3F5 U+1D3F5) dos FORMATs
      // TSV/CSV como texto puro; escapes hex para nao depender do charset do
      // fonte no MSVC.
      if (j.s == "\xE1\xB4\xBA\xE1\xB5\x81\xE1\xB4\xB8\xE1\xB4\xB8") return Value::nulo();
      return j;
    default:
      return Value::texto(json_dump(j));
  }
}

[[noreturn]] void die_http(const HttpClientResponse& r) {
  if (!r.error.empty()) die(r.error);
  if (r.status == 0) die("resposta sem codigo de status");
  die(corpo_erro(r.body));
}

HttpClientResponse post_sql(const ClickHouseUrl& u, const std::string& sql) {
  // Timeout 60s: consultas analiticas podem demorar bem mais que o default
  // 30s do cliente generico.
  return http_request("POST", monta_url(u, sql), {}, "", 60);
}

}  // namespace

// Definido abaixo (parse do NDJSON compartilhado entre query e query_params).
Value parse_json_each_row(const std::string& body);

Value clickhouse_query(const std::string& url, const std::string& sql) {
  const ClickHouseUrl u = parse_url(url);
  std::string final = sql;
  if (!tem_format(final)) final += "\nFORMAT JSONEachRow";
  const HttpClientResponse r = post_sql(u, final);
  if (r.status >= 400 || r.status == 0 || !r.error.empty()) die_http(r);

  return parse_json_each_row(r.body);
}

void clickhouse_exec(const std::string& url, const std::string& sql) {
  const ClickHouseUrl u = parse_url(url);
  const HttpClientResponse r = post_sql(u, sql);
  if (r.status >= 400 || r.status == 0 || !r.error.empty()) die_http(r);
}

// Reescreve `?` como `{pN:Tipo}` e devolve (sql_reescrito, query_params).
// Seguro por construcao: valores vao em param_pN, nunca interpolados.
// Tipos: inteiro->Int64, decimal->Float64, texto->String, logico->UInt8,
// nulo->NULL inline (parametro Nullable vazio nao e NULL).
std::pair<std::string, std::vector<std::pair<std::string, std::string>>> reescreve_params(
    const std::string& sql, const std::vector<SqlParam>& params) {
  std::vector<std::pair<std::string, std::string>> query_params;
  std::size_t nq = 0;
  std::string reescrito;
  varrer_sql(sql,
             [&](std::string& o) {
               if (nq >= params.size()) {
                 throw std::runtime_error("faltam parametros: o SQL tem mais '?' que valores");
               }
               const SqlParam& p = params[nq++];
               switch (p.tipo) {
                 case SqlParam::Tipo::Nulo:
                   o += "NULL";
                   break;
                 case SqlParam::Tipo::Inteiro:
                   o += "{p" + std::to_string(nq - 1) + ":Int64}";
                   query_params.emplace_back("param_p" + std::to_string(nq - 1),
                                             std::to_string(p.i));
                   break;
                 case SqlParam::Tipo::Decimal: {
                   char buf[32];
                   std::snprintf(buf, sizeof buf, "%.17g", p.d);
                   o += "{p" + std::to_string(nq - 1) + ":Float64}";
                   query_params.emplace_back("param_p" + std::to_string(nq - 1), buf);
                   break;
                 }
                 case SqlParam::Tipo::Texto:
                   o += "{p" + std::to_string(nq - 1) + ":String}";
                   query_params.emplace_back("param_p" + std::to_string(nq - 1), p.s);
                   break;
                 case SqlParam::Tipo::Logico:
                   o += "{p" + std::to_string(nq - 1) + ":UInt8}";
                   query_params.emplace_back("param_p" + std::to_string(nq - 1),
                                             p.b ? "1" : "0");
                   break;
               }
             },
             reescrito);
  if (nq != params.size()) {
    die("esperava " + std::to_string(params.size()) + " parametro(s), mas o SQL tem " +
        std::to_string(nq) + " '?'");
  }
  return {reescrito, query_params};
}

// Parse do corpo NDJSON (uma linha = um objeto) em tabela.
Value parse_json_each_row(const std::string& body) {
  ValueList rows;
  std::size_t pos = 0;
  while (pos < body.size()) {
    const std::size_t fim = body.find('\n', pos);
    std::string linha =
        body.substr(pos, fim == std::string::npos ? std::string::npos : fim - pos);
    pos = fim == std::string::npos ? body.size() : fim + 1;
    while (!linha.empty() &&
           (linha.back() == '\r' || linha.back() == ' ' || linha.back() == '\t')) {
      linha.pop_back();
    }
    if (linha.empty()) continue;
    Value j;
    try {
      j = json_parse(linha);
    } catch (const std::exception&) {
      die("resposta nao e JSONEachRow (linha invalida); remova o FORMAT do SQL "
          "ou use FORMAT JSONEachRow");
    }
    if (j.kind != ValueKind::Mapa || !j.map) {
      die("resposta nao e JSONEachRow (linha sem objeto); remova o FORMAT do SQL "
          "ou use FORMAT JSONEachRow");
    }
    Value row = Value::mapa();
    for (const auto& [chave, valor] : j.map->items) {
      row.map->set(chave, valor_de_json(valor));
    }
    rows.push_back(std::move(row));
  }
  return Value::tabela(std::move(rows));
}

// Liga `?` como query params `{pN:Tipo}` (Marco 3 / D1): seguro por
// construcao (valores vao em param_pN, nunca interpolados). Tipos:
// inteiro->Int64, decimal->Float64, texto->String, logico->UInt8,
// nulo->NULL inline (parametro Nullable vazio nao e NULL).
void clickhouse_exec_params(const std::string& url, const std::string& sql,
                            const std::vector<SqlParam>& params) {
  const ClickHouseUrl u = parse_url(url);
  const auto [reescrito, query_params] = reescreve_params(sql, params);
  std::string destino = monta_url(u, reescrito);
  for (const auto& [k, v] : query_params) destino += "&" + k + "=" + uri_encode(v);
  const HttpClientResponse r = http_request("POST", destino, {}, "", 60);
  if (r.status >= 400 || r.status == 0 || !r.error.empty()) die_http(r);
}

// Consulta com `?` ligados como `{pN:Tipo}` (SELECT com params): mesmo
// rewrite do exec_params, com FORMAT JSONEachRow anexado quando ausente.
Value clickhouse_query_params(const std::string& url, const std::string& sql,
                              const std::vector<SqlParam>& params) {
  const ClickHouseUrl u = parse_url(url);
  const auto [reescrito, query_params] = reescreve_params(sql, params);
  std::string final = reescrito;
  if (!tem_format(final)) final += "\nFORMAT JSONEachRow";
  std::string destino = monta_url(u, final);
  for (const auto& [k, v] : query_params) destino += "&" + k + "=" + uri_encode(v);
  const HttpClientResponse r = http_request("POST", destino, {}, "", 60);
  if (r.status >= 400 || r.status == 0 || !r.error.empty()) die_http(r);
  return parse_json_each_row(r.body);
}

void clickhouse_transact(
    const std::string& url,
    const std::vector<std::pair<std::string, std::vector<SqlParam>>>& passos) {
  (void)url;
  (void)passos;
  // HTTP sem estado: sem BEGIN/COMMIT entre requests. Emular sequencia seria
  // mentir atomicidade — erro claro em vez disso.
  die("transacao: clickhouse nao tem transacoes multi-comando via HTTP; execute "
      "os comandos com executar_sql um a um");
}

}  // namespace tilt::rt
