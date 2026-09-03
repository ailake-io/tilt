#include "runtime/llm.hpp"

#include <unistd.h>

#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <stdexcept>

#include "runtime/json.hpp"
#include "runtime/value.hpp"

namespace tilt::rt {

namespace {

std::string env_mode() {
  const char* m = std::getenv("TILT_LLM");
  return m ? std::string(m) : std::string();
}

std::string truncate(const std::string& s, std::size_t n) {
  return s.size() <= n ? s : s.substr(0, n) + "...";
}

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

// Runs a command, returns its stdout. Throws on non-zero exit.
std::string run(const std::string& cmd) {
  std::string out;
  std::array<char, 4096> buf{};
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (!pipe) throw std::runtime_error("nao foi possivel executar 'curl'");
  std::size_t n;
  while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) out.append(buf.data(), n);
  int rc = ::pclose(pipe);
  if (rc != 0) {
    throw std::runtime_error("curl retornou codigo " + std::to_string(rc) +
                             " (verifique rede/chave/URL)");
  }
  return out;
}

// Writes `body` to a temp file and POSTs it with curl; returns the response body.
std::string http_post_json(const std::string& url, const std::vector<std::string>& headers,
                           const std::string& body) {
  char tmpl[] = "/tmp/tilt_llm_XXXXXX";
  int fd = ::mkstemp(tmpl);
  if (fd < 0) throw std::runtime_error("nao foi possivel criar arquivo temporario");
  std::size_t written = 0;
  while (written < body.size()) {
    ssize_t w = ::write(fd, body.data() + written, body.size() - written);
    if (w <= 0) {
      ::close(fd);
      ::unlink(tmpl);
      throw std::runtime_error("falha ao escrever o corpo da requisicao");
    }
    written += static_cast<std::size_t>(w);
  }
  ::close(fd);

  std::string cmd = "curl -sS --fail-with-body -X POST -H 'content-type: application/json'";
  for (const std::string& h : headers) cmd += " -H " + shell_quote(h);
  cmd += " --data @" + std::string(tmpl) + " " + shell_quote(url);

  std::string resp;
  try {
    resp = run(cmd);
  } catch (...) {
    ::unlink(tmpl);
    throw;
  }
  ::unlink(tmpl);
  return resp;
}

const Value* dig(const Value& v, std::initializer_list<const char*> path) {
  const Value* cur = &v;
  for (const char* key : path) {
    if (!cur) return nullptr;
    if (cur->kind == ValueKind::Mapa && cur->map) {
      cur = cur->map->find(key);
    } else if (cur->kind == ValueKind::Lista && cur->list && !cur->list->empty() &&
               std::strcmp(key, "0") == 0) {
      cur = &(*cur->list)[0];
    } else {
      return nullptr;
    }
  }
  return cur;
}

}  // namespace

bool llm_is_mock() { return env_mode() == "mock"; }

std::string llm_chat(const LlmConfig& cfg, const std::string& system, const std::string& user) {
  if (llm_is_mock()) {
    return "[mock] resposta para: " + truncate(user, 200);
  }

  Value body = Value::mapa();
  body.map->set("model", Value::texto(cfg.model));
  body.map->set("temperature", Value::decimal(cfg.temperature));

  std::string url;
  std::vector<std::string> headers;

  if (cfg.provider == "anthropic") {
    url = "https://api.anthropic.com/v1/messages";
    headers = {"x-api-key: " + cfg.api_key, "anthropic-version: 2023-06-01"};
    body.map->set("max_tokens", Value::inteiro(cfg.max_tokens));
    if (!system.empty()) body.map->set("system", Value::texto(system));
    Value msg = Value::mapa();
    msg.map->set("role", Value::texto("user"));
    msg.map->set("content", Value::texto(user));
    body.map->set("messages", Value::lista({msg}));
  } else {
    url = cfg.base_url.empty() ? "https://api.openai.com/v1/chat/completions"
                              : cfg.base_url + "/chat/completions";
    headers = {"Authorization: Bearer " + cfg.api_key};
    Value msgs = Value::lista();
    if (!system.empty()) {
      Value s = Value::mapa();
      s.map->set("role", Value::texto("system"));
      s.map->set("content", Value::texto(system));
      msgs.list->push_back(s);
    }
    Value u = Value::mapa();
    u.map->set("role", Value::texto("user"));
    u.map->set("content", Value::texto(user));
    msgs.list->push_back(u);
    body.map->set("messages", msgs);
  }

  const std::string raw = http_post_json(url, headers, json_dump(body));
  Value resp = json_parse(raw);

  if (const Value* t = dig(resp, {"content", "0", "text"})) {
    if (t->kind == ValueKind::Texto) return t->s;
  }
  if (const Value* t = dig(resp, {"choices", "0", "message", "content"})) {
    if (t->kind == ValueKind::Texto) return t->s;
  }
  throw std::runtime_error("resposta do LLM em formato inesperado");
}

std::vector<float> llm_embed(const std::string& model, const std::string& text) {
  if (llm_is_mock()) {
    // Deterministic hashed bag-of-tokens, L2-normalized. Same words -> same
    // vector; shared vocabulary -> higher cosine similarity.
    constexpr std::size_t kDim = 16;
    std::vector<float> v(kDim, 0.0F);
    std::string tok;
    auto flush = [&] {
      if (tok.empty()) return;
      std::uint64_t h = 1469598103934665603ULL;
      for (char c : tok) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
      }
      v[h % kDim] += 1.0F;
      tok.clear();
    };
    for (char c : text) {
      if (std::isalnum(static_cast<unsigned char>(c))) {
        tok += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      } else {
        flush();
      }
    }
    flush();
    float norm = 0.0F;
    for (float x : v) norm += x * x;
    norm = norm > 0.0F ? std::sqrt(norm) : 1.0F;
    for (float& x : v) x /= norm;
    return v;
  }

  Value body = Value::mapa();
  body.map->set("model", Value::texto(model));
  body.map->set("input", Value::texto(text));
  const std::string raw =
      http_post_json("https://api.openai.com/v1/embeddings",
                     {"Authorization: Bearer " + std::string(std::getenv("OPENAI_API_KEY")
                                                                 ? std::getenv("OPENAI_API_KEY")
                                                                 : "")},
                     json_dump(body));
  Value resp = json_parse(raw);
  std::vector<float> out;
  if (const Value* arr = dig(resp, {"data", "0", "embedding"});
      arr && arr->kind == ValueKind::Lista && arr->list) {
    for (const Value& e : *arr->list) out.push_back(static_cast<float>(e.as_number()));
  }
  if (out.empty()) throw std::runtime_error("resposta de embeddings em formato inesperado");
  return out;
}

}  // namespace tilt::rt
