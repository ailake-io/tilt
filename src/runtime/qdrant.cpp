#include "runtime/qdrant.hpp"

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

#include "runtime/compat.hpp"
#include "runtime/json.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("qdrant: " + m); }

std::string shell_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
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

// Executa `curl -X <metodo>` com corpo JSON opcional; HTTP >= 400 -> die com
// o corpo do erro. Retorna o corpo da resposta.
std::string http_json(const std::string& method, const std::string& url, const std::string& body) {
  std::string body_file;
  std::string cmd =
      "curl -s --fail-with-body -X " + method + " -H 'content-type: application/json'";
  if (!body.empty()) {
    std::string body_path;
    const int fd = tilt_tempfile("qdrant", body_path);
    if (fd < 0) die("nao foi possivel criar arquivo temporario");
    tilt_close_file(fd);
    {
      std::ofstream out(body_path, std::ios::trunc);
      out << body;
    }
    body_file = body_path;
    cmd += " --data @" + body_file;
  }
  cmd += " " + shell_quote(url);

  std::string resp;
  {
    std::array<char, 4096> buf{};
    FILE* pipe = tilt_popen(cmd.c_str(), "r");
    if (!pipe) {
      if (!body_file.empty()) std::remove(body_file.c_str());
      die("nao foi possivel executar 'curl'");
    }
    std::size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) resp.append(buf.data(), n);
    const int rc = tilt_pclose(pipe);
    if (!body_file.empty()) std::remove(body_file.c_str());
    if (rc != 0) {
      die("requisicao falhou (curl codigo " + std::to_string(rc) +
          "): verifique URL/colecao/servidor. Resposta: " + resp.substr(0, 200));
    }
  }
  return resp;
}

// UUID deterministico (versao 5 simplificada) a partir do id em texto:
// FNV-1a 64 bits duas vezes com seeds diferentes + bits de versao/variante.
std::string uuid_from_id(const std::string& id) {
  auto fnv = [&](std::uint64_t seed) {
    std::uint64_t h = 1469598103934665603ULL ^ seed;
    for (unsigned char c : id) {
      h ^= c;
      h *= 1099511628211ULL;
    }
    return h;
  };
  const std::uint64_t a = fnv(0);
  const std::uint64_t b = fnv(0x9E3779B97F4A7C15ULL);
  // a: time_low(32) time_mid(16) | b: time_hi(16) clock(16) node(48)
  const std::uint32_t time_low = static_cast<std::uint32_t>(a >> 32);
  const std::uint16_t time_mid = static_cast<std::uint16_t>(a & 0xFFFFu);
  const std::uint16_t time_hi = static_cast<std::uint16_t>((b >> 48) & 0x0FFFu) | 0x5000u;
  const std::uint16_t clock_seq = static_cast<std::uint16_t>((b >> 32) & 0x3FFFu) | 0x8000u;
  const std::uint64_t node = b & 0xFFFFFFFFFFFFULL;
  char buf[37];
  std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx", time_low, time_mid, time_hi,
                clock_seq, static_cast<unsigned long long>(node));
  return buf;
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

void ensure_collection(const std::string& base, const std::string& collection,
                       std::size_t dims) {
  const std::string url = base + "/collections/" + collection;
  try {
    http_json("GET", url, "");
    return;  // ja existe
  } catch (const std::exception&) {
    // nao existe (ou erro transitório): tenta criar
  }
  const std::string body = "{\"vectors\":{\"size\":" + std::to_string(dims) +
                           ",\"distance\":\"Cosine\"}}";
  http_json("PUT", url, body);
}

}  // namespace

void qdrant_upsert(const std::string& base, const std::string& collection,
                   const std::string& id, const std::string& text,
                   const std::vector<float>& vec) {
  if (vec.empty()) die("vetor vazio para o id '" + id + "'");
  ensure_collection(base, collection, vec.size());
  const std::string body = "{\"points\":[{\"id\":\"" + uuid_from_id(id) + "\",\"vector\":" +
                           vec_json(vec) + ",\"payload\":{\"text\":\"" + json_escape(text) +
                           "\"}}]}";
  http_json("PUT", base + "/collections/" + collection + "/points?wait=true", body);
}

std::vector<std::pair<std::string, double>> qdrant_search(const std::string& base,
                                                          const std::string& collection,
                                                          const std::vector<float>& vec,
                                                          std::size_t k) {
  const std::string body = "{\"vector\":" + vec_json(vec) + ",\"limit\":" + std::to_string(k) +
                           ",\"with_payload\":false}";
  const std::string resp = http_json("POST", base + "/collections/" + collection + "/points/search",
                                     body);
  Value parsed;
  try {
    parsed = json_parse(resp);
  } catch (const std::exception& e) {
    die("resposta invalida do servidor: " + std::string(e.what()));
  }
  std::vector<std::pair<std::string, double>> out;
  if (parsed.kind != ValueKind::Mapa || !parsed.map) return out;
  const Value* result = parsed.map->find("result");
  if (!result || result->kind != ValueKind::Lista || !result->list) return out;
  for (const Value& hit : *result->list) {
    if (hit.kind != ValueKind::Mapa || !hit.map) continue;
    const Value* id = hit.map->find("id");
    const Value* score = hit.map->find("score");
    const std::string id_s = id && id->kind == ValueKind::Texto ? id->s : "?";
    const double sc = score && (score->kind == ValueKind::Decimal || score->kind == ValueKind::Inteiro)
                          ? score->as_number()
                          : 0.0;
    out.emplace_back(id_s, sc);
  }
  return out;
}

}  // namespace tilt::rt
