#include "runtime/chroma.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "runtime/http_client.hpp"
#include "runtime/json.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("chroma: " + m); }

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

// Percent-encoding minimo para segmento de path (o id da colecao).
std::string url_escape(const std::string& s) {
  std::string out;
  char buf[4];
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      std::snprintf(buf, sizeof buf, "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

// Mensagem do servidor: {"error": ...} (formato do Chroma) ou
// {"detail": ...} (FastAPI); fallback no corpo truncado.
std::string motivo_erro(const std::string& body) {
  if (body.empty()) return "erro desconhecido (resposta vazia)";
  try {
    const Value j = json_parse(body);
    if (j.kind == ValueKind::Mapa && j.map) {
      for (const char* chave : {"error", "detail", "message"}) {
        const Value* err = j.map->find(chave);
        if (!err) continue;
        if (err->kind == ValueKind::Texto && !err->s.empty()) return err->s;
        if (err->kind == ValueKind::Mapa && err->map) {
          const Value* msg = err->map->find("message");
          if (msg && msg->kind == ValueKind::Texto && !msg->s.empty()) return msg->s;
        }
      }
    }
  } catch (const std::exception&) {
    // corpo nao-JSON: cai no truncamento abaixo
  }
  return truncar(body, 300);
}

[[noreturn]] void die_http(const HttpClientResponse& r) {
  if (!r.error.empty()) die(r.error + ": verifique URL/colecao/servidor. Resposta: " +
                            truncar(r.body, 200));
  if (r.status == 0) die("resposta sem codigo de status");
  die(motivo_erro(r.body));
}

// Chroma open-source padrao: sem auth, so JSON.
std::vector<std::pair<std::string, std::string>> headers() {
  return {{"content-type", "application/json"}};
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

// Get-or-create da colecao; devolve o id que endereca add/query.
std::string ensure_colecao(const std::string& base, const std::string& colecao) {
  const std::string body = "{\"name\":\"" + json_escape(colecao) + "\",\"get_or_create\":true}";
  const std::string resp = http_json("POST", base + "/api/v1/collections", body);
  Value parsed;
  try {
    parsed = json_parse(resp);
  } catch (const std::exception& e) {
    die("resposta invalida do servidor: " + std::string(e.what()));
  }
  if (parsed.kind == ValueKind::Mapa && parsed.map) {
    if (const Value* id = parsed.map->find("id");
        id && id->kind == ValueKind::Texto && !id->s.empty()) {
      return id->s;
    }
  }
  die("resposta de get-or-create sem 'id' da colecao");
}

}  // namespace

void chroma_upsert(const std::string& base, const std::string& colecao,
                   const std::string& id, const std::string& text,
                   const std::vector<float>& vec) {
  if (vec.empty()) die("vetor vazio para o id '" + id + "'");
  const std::string cid = ensure_colecao(base, colecao);
  const std::string body = "{\"ids\":[\"" + json_escape(id) + "\"],\"embeddings\":[" +
                           vec_json(vec) + "],\"metadatas\":[{\"texto\":\"" +
                           json_escape(text) + "\"}],\"documents\":[\"" +
                           json_escape(text) + "\"]}";
  http_json("POST", base + "/api/v1/collections/" + url_escape(cid) + "/add", body);
}

std::vector<std::pair<std::string, double>> chroma_search(const std::string& base,
                                                          const std::string& colecao,
                                                          const std::vector<float>& vec,
                                                          std::size_t k) {
  if (vec.empty()) die("vetor de consulta vazio");
  const std::string cid = ensure_colecao(base, colecao);
  const std::string body = "{\"query_embeddings\":[" + vec_json(vec) +
                           "],\"n_results\":" + std::to_string(k) +
                           ",\"include\":[\"metadatas\",\"distances\"]}";
  const std::string resp =
      http_json("POST", base + "/api/v1/collections/" + url_escape(cid) + "/query", body);
  Value parsed;
  try {
    parsed = json_parse(resp);
  } catch (const std::exception& e) {
    die("resposta invalida do servidor: " + std::string(e.what()));
  }
  std::vector<std::pair<std::string, double>> out;
  if (parsed.kind != ValueKind::Mapa || !parsed.map) return out;
  const Value* ids = parsed.map->find("ids");
  const Value* dists = parsed.map->find("distances");
  // O Chroma devolve listas aninhadas por consulta: ids[[...]], distances[[...]].
  if (!ids || ids->kind != ValueKind::Lista || !ids->list || ids->list->empty()) return out;
  if (!dists || dists->kind != ValueKind::Lista || !dists->list || dists->list->empty()) {
    return out;
  }
  const Value& ids_q = (*ids->list)[0];
  const Value& dists_q = (*dists->list)[0];
  if (ids_q.kind != ValueKind::Lista || !ids_q.list || dists_q.kind != ValueKind::Lista ||
      !dists_q.list) {
    return out;
  }
  const std::size_t n = ids_q.list->size() < dists_q.list->size() ? ids_q.list->size()
                                                                  : dists_q.list->size();
  for (std::size_t i = 0; i < n; ++i) {
    const Value& id = (*ids_q.list)[i];
    const Value& d = (*dists_q.list)[i];
    const std::string id_s = id.kind == ValueKind::Texto ? id.s : "?";
    // hnsw:space cosine: distance = 1 - cosseno -> score = 1 - distance.
    const double sc = d.is_number() ? 1.0 - d.as_number() : 0.0;
    out.emplace_back(id_s, sc);
  }
  return out;
}

}  // namespace tilt::rt
