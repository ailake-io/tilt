#include "runtime/elasticsearch.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "runtime/http_client.hpp"
#include "runtime/json.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("elasticsearch: " + m); }

std::string truncar(const std::string& s, std::size_t n) {
  return s.size() <= n ? s : s.substr(0, n);
}

std::string para_maiusculas(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// Base64 padrao (RFC 4648) com padding — so para codificar o par user:senha
// do Basic Auth (decodificacao nao e necessaria aqui).
std::string base64_encode(const std::string& in) {
  static constexpr char kAlfabeto[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < in.size(); i += 3) {
    const unsigned int b0 = static_cast<unsigned char>(in[i]);
    const unsigned int b1 = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0;
    const unsigned int b2 = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0;
    out += kAlfabeto[(b0 >> 2) & 0x3F];
    out += kAlfabeto[((b0 << 4) | (b1 >> 4)) & 0x3F];
    out += i + 1 < in.size() ? kAlfabeto[((b1 << 2) | (b2 >> 6)) & 0x3F] : '=';
    out += i + 2 < in.size() ? kAlfabeto[b2 & 0x3F] : '=';
  }
  return out;
}

struct EsUrl {
  std::string base;    // "http://host:porta"
  std::string indice;  // vazio = todos os indices
  std::string auth;    // valor do header Authorization, vazio = sem auth
};

// "elasticsearch://[usuario[:senha]@]host[:porta][/indice]" (ou opensearch://,
// mesmo formato e porta default 9200). Sem userinfo, usuario/senha caem para
// ELASTIC_USER/ELASTIC_PASSWORD do ambiente; sem nenhum dos dois, nenhum
// header de autenticacao e enviado.
EsUrl parse_url(const std::string& url) {
  std::string rest;
  if (url.rfind("elasticsearch://", 0) == 0) {
    rest = url.substr(16);
  } else if (url.rfind("opensearch://", 0) == 0) {
    rest = url.substr(13);
  } else {
    die("url invalida: '" + url +
        "' (use elasticsearch://[usuario[:senha]@]host[:porta][/indice])");
  }

  EsUrl out;
  const std::size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  if (slash != std::string::npos) {
    out.indice = rest.substr(slash + 1);
    if (out.indice.empty() || out.indice.find('/') != std::string::npos ||
        out.indice.find('?') != std::string::npos || out.indice.find('#') != std::string::npos) {
      die("url invalida: '" + url + "' (indice deve ser um unico nome, sem query string)");
    }
  }

  std::string user;
  std::string pass;
  const std::size_t at = authority.rfind('@');
  std::string hostport = authority;
  if (at != std::string::npos) {
    const std::string userinfo = authority.substr(0, at);
    hostport = authority.substr(at + 1);
    const std::size_t colon = userinfo.find(':');
    user = colon == std::string::npos ? userinfo : userinfo.substr(0, colon);
    pass = colon == std::string::npos ? "" : userinfo.substr(colon + 1);
  } else {
    if (const char* u = std::getenv("ELASTIC_USER"); u && *u) user = u;
    if (const char* p = std::getenv("ELASTIC_PASSWORD"); p && *p) pass = p;
  }
  if (!user.empty()) {
    out.auth = "Basic " + base64_encode(user + ":" + pass);
  }
  if (hostport.empty()) hostport = "localhost";

  unsigned int port = 9200;
  std::string host = hostport;
  const std::size_t colon = hostport.rfind(':');
  if (colon != std::string::npos) {
    const std::string port_str = hostport.substr(colon + 1);
    if (port_str.empty() || port_str.find_first_not_of("0123456789") != std::string::npos) {
      die("url invalida: '" + url + "' (porta deve ser numerica)");
    }
    try {
      const unsigned long p = std::stoul(port_str);
      if (p == 0 || p > 65535) throw std::out_of_range("porta");
      port = static_cast<unsigned int>(p);
    } catch (const std::exception&) {
      die("url invalida: '" + url + "' (porta fora da faixa 1-65535)");
    }
    host = hostport.substr(0, colon);
    if (host.empty()) host = "localhost";
  }
  out.base = "http://" + host + ":" + std::to_string(port);
  return out;
}

// Extrai o motivo do JSON de erro do ES/OpenSearch: {"error": {"reason": "..."}}
// (ES 7/8), {"error": "..."} (ES 5/6) ou corpo truncado quando nao ha JSON.
std::string motivo_erro(const std::string& body) {
  if (body.empty()) return "erro desconhecido (resposta vazia)";
  try {
    const Value j = json_parse(body);
    if (j.kind == ValueKind::Mapa && j.map) {
      const Value* err = j.map->find("error");
      if (err) {
        if (err->kind == ValueKind::Texto) return err->s;
        if (err->kind == ValueKind::Mapa && err->map) {
          const Value* reason = err->map->find("reason");
          if (reason && reason->kind == ValueKind::Texto && !reason->s.empty()) return reason->s;
        }
      }
    }
  } catch (const std::exception&) {
    // corpo nao-JSON: cai no truncamento abaixo
  }
  return truncar(body, 300);
}

[[noreturn]] void die_http(const HttpClientResponse& r) {
  if (!r.error.empty()) die(r.error + ": verifique URL/indice/servidor");
  if (r.status == 0) die("resposta sem codigo de status");
  die(motivo_erro(r.body));
}

std::vector<std::pair<std::string, std::string>> headers_com_auth(const EsUrl& u) {
  std::vector<std::pair<std::string, std::string>> headers = {{"content-type", "application/json"}};
  if (!u.auth.empty()) headers.emplace_back("authorization", u.auth);
  return headers;
}

// Texto e enviado como corpo cru (JSON ja pronto); demais valores (mapa,
// lista, numero...) sao serializados. Nulo = sem corpo.
std::string corpo_de(const Value& v, bool* tem_corpo) {
  if (v.kind == ValueKind::Nulo) {
    *tem_corpo = false;
    return "";
  }
  *tem_corpo = true;
  if (v.kind == ValueKind::Texto) return v.s;
  return json_dump(v);
}

Value json_do_corpo(const std::string& body, const std::string& contexto) {
  try {
    return json_parse(body);
  } catch (const std::exception& e) {
    die("resposta invalida do servidor em " + contexto + ": " + std::string(e.what()));
  }
}

}  // namespace

Value es_query(const std::string& url, const Value& dsl) {
  const EsUrl u = parse_url(url);
  bool tem_corpo = false;
  const std::string body = corpo_de(dsl, &tem_corpo);
  if (!tem_corpo) {
    die("dsl deve ser texto JSON ou mapa, ex.: {query: {match_all: {}}}");
  }
  const std::string alvo = u.base + "/" + (u.indice.empty() ? "" : u.indice + "/") + "_search";
  const HttpClientResponse r = http_request("POST", alvo, headers_com_auth(u), body, 30);
  if (r.status >= 400 || r.status == 0 || !r.error.empty()) die_http(r);

  const Value resp = json_do_corpo(r.body, "_search");
  if (resp.kind != ValueKind::Mapa || !resp.map) {
    die("resposta de _search sem objeto no corpo");
  }
  const Value* hits = resp.map->find("hits");
  if (!hits || hits->kind != ValueKind::Mapa || !hits->map) {
    die("resposta de _search sem 'hits'");
  }

  // total: ES 7+ devolve {"value": N, "relation": "eq"}; ES 6, numero puro.
  Value total = Value::inteiro(0);
  if (const Value* t = hits->map->find("total")) {
    if (t->kind == ValueKind::Mapa && t->map) {
      const Value* v = t->map->find("value");
      if (v && v->is_number()) total = Value::inteiro(static_cast<std::int64_t>(v->as_number()));
    } else if (t->is_number()) {
      total = Value::inteiro(static_cast<std::int64_t>(t->as_number()));
    }
  }

  // Cada hit vira um mapa: campos de _source achatados um nivel + "_id".
  ValueList linhas;
  if (const Value* hh = hits->map->find("hits"); hh && hh->kind == ValueKind::Lista && hh->list) {
    for (const Value& hit : *hh->list) {
      if (hit.kind != ValueKind::Mapa || !hit.map) continue;
      Value linha = Value::mapa();
      if (const Value* src = hit.map->find("_source");
          src && src->kind == ValueKind::Mapa && src->map) {
        for (const auto& [chave, valor] : src->map->items) linha.map->set(chave, valor);
      }
      const Value* id = hit.map->find("_id");
      linha.map->set("_id", Value::texto(id && id->kind == ValueKind::Texto ? id->s : "?"));
      linhas.push_back(std::move(linha));
    }
  }

  Value out = Value::mapa();
  out.map->set("total", std::move(total));
  out.map->set("hits", Value::tabela(std::move(linhas)));
  if (const Value* aggs = resp.map->find("aggregations"); aggs && aggs->kind == ValueKind::Mapa) {
    out.map->set("agregacoes", *aggs);
  }
  return out;
}

Value es_exec(const std::string& url, const std::string& metodo, const std::string& caminho,
              const Value& corpo) {
  const EsUrl u = parse_url(url);
  const std::string met = para_maiusculas(metodo);
  if (met != "GET" && met != "POST" && met != "PUT" && met != "PATCH" && met != "DELETE" &&
      met != "HEAD") {
    die("metodo '" + metodo + "' invalido (use GET, POST, PUT, PATCH, DELETE ou HEAD)");
  }
  std::string path = caminho;
  if (path.empty() || path[0] != '/') path = "/" + path;
  if (path.find("://") != std::string::npos) {
    die("caminho deve ser relativo ao servidor (ex.: \"/meuindice/_doc/1\")");
  }

  bool tem_corpo = false;
  const std::string body = corpo_de(corpo, &tem_corpo);
  // Caminho relativo ao indice da URL quando ha um (ex.: URL ".../artigos" +
  // "/_doc/1" -> /artigos/_doc/1); com URL sem indice, relativo a raiz.
  const std::string alvo =
      u.base + (u.indice.empty() ? "" : "/" + u.indice) + path;
  const HttpClientResponse r = http_request(met, alvo, headers_com_auth(u), body, 30);
  if (r.status >= 400 || r.status == 0 || !r.error.empty()) die_http(r);
  if (r.body.empty()) return Value::nulo();
  try {
    return json_parse(r.body);
  } catch (const std::exception&) {
    // Endpoints como _cat devolvem TSV/texto puro: devolve como texto.
    return Value::texto(r.body);
  }
}

}  // namespace tilt::rt
