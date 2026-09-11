#include "runtime/weaviate.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "runtime/http_client.hpp"
#include "runtime/json.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("weaviate: " + m); }

std::string truncar(const std::string& s, std::size_t n) {
  return s.size() <= n ? s : s.substr(0, n);
}

std::string json_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out += c; break;
    }
  }
  return out;
}

// Percent-encoding minimo para segmento de path (o id do objeto).
std::string url_escape(const std::string& s) {
  std::string out;
  char buf[4];
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      std::snprintf(buf, sizeof buf, "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

// Weaviate exige nome de classe GraphQL: /[A-Z][_0-9A-Za-z]*/. Como o nome
// entra interpolado na query GraphQL, qualquer outra coisa e rejeitada aqui.
bool classe_valida(const std::string& c) {
  if (c.empty() || !std::isupper(static_cast<unsigned char>(c[0]))) return false;
  for (char ch : c) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (!std::isalnum(u) && ch != '_') return false;
  }
  return true;
}

// Mensagem do servidor: {"error": [{"message": ...}]} (REST) ou
// {"errors": [{"message": ...}]} (GraphQL); fallback no corpo truncado.
std::string motivo_erro(const std::string& body) {
  if (body.empty()) return "erro desconhecido (resposta vazia)";
  try {
    const Value j = json_parse(body);
    if (j.kind == ValueKind::Mapa && j.map) {
      for (const char* chave : {"error", "errors"}) {
        const Value* err = j.map->find(chave);
        if (!err) continue;
        const Value* primeiro = err;
        if (err->kind == ValueKind::Lista && err->list && !err->list->empty()) {
          primeiro = &(*err->list)[0];
        }
        if (primeiro->kind == ValueKind::Mapa && primeiro->map) {
          const Value* msg = primeiro->map->find("message");
          if (msg && msg->kind == ValueKind::Texto && !msg->s.empty()) return msg->s;
        }
        if (primeiro->kind == ValueKind::Texto && !primeiro->s.empty()) return primeiro->s;
      }
    }
  } catch (const std::exception&) {
    // corpo nao-JSON: cai no truncamento abaixo
  }
  return truncar(body, 300);
}

// JSON em todo request; Authorization: Bearer <WEAVIATE_API_KEY> quando a
// env existe, senao requisicao anonima.
std::vector<std::pair<std::string, std::string>> headers() {
  std::vector<std::pair<std::string, std::string>> hs = {{"content-type", "application/json"}};
  if (const char* k = std::getenv("WEAVIATE_API_KEY"); k && *k) {
    hs.emplace_back("authorization", std::string("Bearer ") + k);
  }
  return hs;
}

[[noreturn]] void die_http(const HttpClientResponse& r) {
  if (!r.error.empty()) die(r.error + ": verifique URL/classe/servidor. Resposta: " +
                            truncar(r.body, 200));
  if (r.status == 0) die("resposta sem codigo de status");
  die(motivo_erro(r.body));
}

// Envia a requisicao via cliente HTTP generico do runtime. Status >= 400,
// transporte ou execucao -> die com a mensagem do servidor.
std::string http_json(const std::string& method, const std::string& url, const std::string& body) {
  const HttpClientResponse r = http_request(method, url, headers(), body, 0);
  if (r.status >= 400 || r.status == 0 || !r.error.empty()) die_http(r);
  return r.body;
}

std::string vec_json(const std::vector<float>& vec) {
  std::string out = "[";
  char num[32];
  for (std::size_t k = 0; k < vec.size(); ++k) {
    if (k) out += ',';
    std::snprintf(num, sizeof num, "%.9g", static_cast<double>(vec[k]));
    out += num;
  }
  out += ']';
  return out;
}

void ensure_classe(const std::string& base, const std::string& classe) {
  const HttpClientResponse r = http_request("GET", base + "/v1/schema/" + classe, headers(), "", 0);
  if (r.status == 200) return;  // ja existe
  if (r.status != 404) die_http(r);
  const std::string body = "{\"class\":\"" + json_escape(classe) +
                           "\",\"vectorizer\":\"none\",\"properties\":[{\"name\":\"texto\","
                           "\"dataType\":[\"text\"]}]}";
  http_json("POST", base + "/v1/schema", body);
}

}  // namespace

void weaviate_upsert(const std::string& base, const std::string& classe,
                     const std::string& id, const std::string& text,
                     const std::vector<float>& vec) {
  if (vec.empty()) die("vetor vazio para o id '" + id + "'");
  if (!classe_valida(classe)) {
    die("nome de classe invalido '" + classe + "' (Weaviate exige [A-Z][_a-zA-Z0-9]*)");
  }
  ensure_classe(base, classe);
  const std::string body = "{\"class\":\"" + json_escape(classe) +
                           "\",\"properties\":{\"texto\":\"" + json_escape(text) +
                           "\"},\"vector\":" + vec_json(vec) + "}";
  http_json("PUT", base + "/v1/objects/" + classe + "/" + url_escape(id), body);
}

std::vector<std::pair<std::string, double>> weaviate_search(const std::string& base,
                                                            const std::string& classe,
                                                            const std::vector<float>& vec,
                                                            std::size_t k) {
  if (vec.empty()) die("vetor de consulta vazio");
  if (!classe_valida(classe)) {
    die("nome de classe invalido '" + classe + "' (Weaviate exige [A-Z][_a-zA-Z0-9]*)");
  }
  const std::string body = "{\"query\":\"{ Get { " + classe + "(nearVector: {vector: " +
                           vec_json(vec) + "}, limit: " + std::to_string(k) +
                           ") { _additional { id distance } } } }\"}";
  const std::string resp = http_json("POST", base + "/v1/graphql", body);
  Value parsed;
  try {
    parsed = json_parse(resp);
  } catch (const std::exception& e) {
    die("resposta invalida do servidor: " + std::string(e.what()));
  }
  std::vector<std::pair<std::string, double>> out;
  if (parsed.kind != ValueKind::Mapa || !parsed.map) return out;
  // Erros GraphQL chegam com status 200: {"errors": [{"message": ...}]}.
  if (const Value* errs = parsed.map->find("errors");
      errs && errs->kind == ValueKind::Lista && errs->list && !errs->list->empty()) {
    die(motivo_erro(resp));
  }
  const Value* data = parsed.map->find("data");
  if (!data || data->kind != ValueKind::Mapa || !data->map) return out;
  const Value* get = data->map->find("Get");
  if (!get || get->kind != ValueKind::Mapa || !get->map) return out;
  const Value* hits = get->map->find(classe);
  if (!hits || hits->kind != ValueKind::Lista || !hits->list) return out;
  for (const Value& hit : *hits->list) {
    if (hit.kind != ValueKind::Mapa || !hit.map) continue;
    const Value* add = hit.map->find("_additional");
    if (!add || add->kind != ValueKind::Mapa || !add->map) continue;
    const Value* id = add->map->find("id");
    const Value* dist = add->map->find("distance");
    const std::string id_s = id && id->kind == ValueKind::Texto ? id->s : "?";
    const double d = dist && dist->is_number() ? dist->as_number() : 0.0;
    out.emplace_back(id_s, 1.0 - d);  // score = 1 - distancia de cosseno
  }
  return out;
}

}  // namespace tilt::rt
