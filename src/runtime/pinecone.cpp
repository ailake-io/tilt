#include "runtime/pinecone.hpp"

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

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("pinecone: " + m); }

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

// PINECONE_API_KEY e obrigatoria: falha acionavel antes de tocar a rede.
std::string chave() {
  const char* k = std::getenv("PINECONE_API_KEY");
  if (!k || !*k) die("PINECONE_API_KEY nao definida (env com a chave do Pinecone)");
  return k;
}

// Mensagem do servidor: {"message": ...} (formato tipico do Pinecone),
// {"error": ...} ou {"errors": [{...}]}; fallback no corpo truncado.
std::string motivo_erro(const std::string& body) {
  if (body.empty()) return "erro desconhecido (resposta vazia)";
  try {
    const Value j = json_parse(body);
    if (j.kind == ValueKind::Mapa && j.map) {
      if (const Value* msg = j.map->find("message");
          msg && msg->kind == ValueKind::Texto && !msg->s.empty()) {
        return msg->s;
      }
      for (const char* chave_campo : {"error", "errors"}) {
        const Value* err = j.map->find(chave_campo);
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

[[noreturn]] void die_http(const HttpClientResponse& r) {
  if (!r.error.empty()) die(r.error + ": verifique URL/namespace/servidor. Resposta: " +
                            truncar(r.body, 200));
  if (r.status == 0) die("resposta sem codigo de status");
  die(motivo_erro(r.body));
}

// JSON em todo request; Api-Key do Pinecone (data plane exige chave).
std::vector<std::pair<std::string, std::string>> headers() {
  return {{"content-type", "application/json"}, {"Api-Key", chave()}};
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

}  // namespace

void pinecone_upsert(const std::string& base, const std::string& ns,
                     const std::string& id, const std::string& text,
                     const std::vector<float>& vec) {
  if (vec.empty()) die("vetor vazio para o id '" + id + "'");
  const std::string body = "{\"namespace\":\"" + json_escape(ns) +
                           "\",\"vectors\":[{\"id\":\"" + json_escape(id) +
                           "\",\"values\":" + vec_json(vec) + ",\"metadata\":{\"texto\":\"" +
                           json_escape(text) + "\"}}]}";
  http_json("POST", base + "/vectors/upsert", body);
}

std::vector<std::pair<std::string, double>> pinecone_search(const std::string& base,
                                                            const std::string& ns,
                                                            const std::vector<float>& vec,
                                                            std::size_t k) {
  if (vec.empty()) die("vetor de consulta vazio");
  const std::string body = "{\"namespace\":\"" + json_escape(ns) +
                           "\",\"vector\":" + vec_json(vec) + ",\"topK\":" +
                           std::to_string(k) + ",\"includeMetadata\":true}";
  const std::string resp = http_json("POST", base + "/query", body);
  Value parsed;
  try {
    parsed = json_parse(resp);
  } catch (const std::exception& e) {
    die("resposta invalida do servidor: " + std::string(e.what()));
  }
  std::vector<std::pair<std::string, double>> out;
  if (parsed.kind != ValueKind::Mapa || !parsed.map) return out;
  const Value* matches = parsed.map->find("matches");
  if (!matches || matches->kind != ValueKind::Lista || !matches->list) return out;
  for (const Value& hit : *matches->list) {
    if (hit.kind != ValueKind::Mapa || !hit.map) continue;
    const Value* id = hit.map->find("id");
    const Value* score = hit.map->find("score");
    const std::string id_s = id && id->kind == ValueKind::Texto ? id->s : "?";
    // score do Pinecone ja e similaridade de cosseno (maior = melhor).
    const double sc = score && score->is_number() ? score->as_number() : 0.0;
    out.emplace_back(id_s, sc);
  }
  return out;
}

}  // namespace tilt::rt
