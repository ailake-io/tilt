#include "runtime/llm.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "runtime/compat.hpp"
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
  FILE* pipe = tilt_popen(cmd.c_str(), "r");
  if (!pipe) throw std::runtime_error("nao foi possivel executar 'curl'");
  std::size_t n;
  while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) out.append(buf.data(), n);
  int rc = tilt_pclose(pipe);
#if defined(_WIN32)
  const int codigo = rc;
#else
  // pclose devolve wait-status (codigo << 8); extrai a saida real (28 =
  // timeout do --max-time, 6 = DNS, 7 = conexao recusada).
  const int codigo = WIFEXITED(rc) ? WEXITSTATUS(rc) : rc;
#endif
  if (codigo != 0) {
    throw std::runtime_error("curl retornou codigo " + std::to_string(codigo) +
                             " (verifique rede/chave/URL)");
  }
  return out;
}

// Writes `body` to a temp file and POSTs it with curl; returns {status, body}.
// Sem --fail: 4xx/5xx voltam com o corpo para decidir retry (429/5xx) ou
// falha rapida (demais 4xx). Erro de transporte (DNS, conexao, timeout)
// joga runtime_error com o codigo do curl.
struct HttpResult {
  long status = 0;
  std::string body;
};

HttpResult http_post_status(const std::string& url, const std::vector<std::string>& headers,
                            const std::string& body, int timeout_s) {
  std::string body_file;
  const int fd = tilt_tempfile("llm", body_file);
  if (fd < 0) throw std::runtime_error("nao foi possivel criar arquivo temporario");
  tilt_close_file(fd);
  {
    std::ofstream out(body_file, std::ios::trunc);
    out << body;
    if (!out) {
      std::remove(body_file.c_str());
      throw std::runtime_error("falha ao escrever o corpo da requisicao");
    }
  }

  std::string cmd = "curl -sS -X POST -H 'content-type: application/json'";
  for (const std::string& h : headers) cmd += " -H " + shell_quote(h);
  if (timeout_s > 0) cmd += " --max-time " + std::to_string(timeout_s);
  cmd += " --data @" + body_file + " -w '\n%{http_code}' " + shell_quote(url);

  std::string resp;
  try {
    resp = run(cmd);
  } catch (const std::exception& e) {
    std::remove(body_file.c_str());
    throw std::runtime_error(std::string("falha de transporte: ") + e.what());
  }
  std::remove(body_file.c_str());
  // Ultima linha = codigo HTTP; o resto = corpo (pode conter \n).
  std::size_t nl = resp.rfind('\n');
  long status = 0;
  std::string corpo = resp;
  if (nl != std::string::npos) {
    try {
      status = std::stol(resp.substr(nl + 1));
    } catch (...) {
      status = 0;
    }
    corpo = resp.substr(0, nl);
    if (!corpo.empty() && corpo.back() == '\r') corpo.pop_back();
  }
  return {status, corpo};
}

// Contabilidade de tokens por llm (processo; protege rotas paralelas).
std::mutex g_uso_mu;
std::map<std::string, std::pair<long long, long long>> g_uso;  // nome -> {entrada, saida}

void soma_uso(const std::string& nome, long long entrada, long long saida) {
  std::lock_guard<std::mutex> lk(g_uso_mu);
  auto& u = g_uso[nome];
  u.first += entrada;
  u.second += saida;
}

long long uso_total(const std::string& nome) {
  std::lock_guard<std::mutex> lk(g_uso_mu);
  auto it = g_uso.find(nome);
  return it == g_uso.end() ? 0 : it->second.first + it->second.second;
}

// Heuristica de tokens p/ o mock (chars/4 por lado; deterministica).
long long mock_tokens(const std::string& s) {
  return static_cast<long long>((s.size() + 3) / 4);
}

// Dorme entre tentativas: 1s, 2s, 4s... teto 15s (sem jitter: deterministico).
void espera_retry(int tentativa) {
  long espera = 1L << (tentativa - 1);
  if (espera > 15) espera = 15;
  std::this_thread::sleep_for(std::chrono::seconds(espera));
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

// Lines "- <name>:" that follow the exact `marker` line (protocol blocks
// emitted by the agent planner / team supervisor).
std::vector<std::string> block_names(const std::string& text, const std::string& marker) {
  std::vector<std::string> names;
  bool in_block = false;
  std::string line;
  for (char c : text) {
    if (c != '\n') {
      line += c;
      continue;
    }
    if (!in_block) {
      if (line == marker) in_block = true;
    } else if (line.rfind("- ", 0) == 0) {
      const std::size_t colon = line.find(':');
      if (colon != std::string::npos) names.push_back(line.substr(2, colon - 2));
    } else if (!line.empty()) {
      break;
    }
    line.clear();
  }
  return names;
}

// Content of the line that starts with `prefix` (e.g. "Pedido do usuario:"),
// without leading/trailing whitespace.
std::string field_line(const std::string& text, const std::string& prefix) {
  std::string line;
  auto trim = [](const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
  };
  for (char c : text) {
    if (c != '\n') {
      line += c;
      continue;
    }
    if (line.rfind(prefix, 0) == 0) return trim(line.substr(prefix.size()));
    line.clear();
  }
  if (line.rfind(prefix, 0) == 0) return trim(line.substr(prefix.size()));
  return "";
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
  for (const std::string& e : v) {
    if (e == s) return true;
  }
  return false;
}

// Mock for the iterative planner protocol: calls each tool listed in the
// system prompt exactly once (in order), then answers. Same shape for the
// team supervisor, delegating to each member once.
std::string mock_chat(const std::string& system, const std::string& user) {
  if (system.find("Ferramentas disponiveis:") != std::string::npos) {
    const auto tools = block_names(system, "Ferramentas disponiveis:");
    const auto done = block_names(user, "Observacoes ate agora:");
    for (const std::string& t : tools) {
      if (!contains(done, t)) return "chamar " + t;
    }
    return "responder: [mock] resposta para: " + truncate(field_line(user, "Pedido do usuario:"), 200);
  }
  if (system.find("Agentes disponiveis:") != std::string::npos) {
    const auto members = block_names(system, "Agentes disponiveis:");
    const auto done = block_names(user, "Resultados ate agora:");
    const std::string pedido = field_line(user, "Pedido:");
    for (const std::string& m : members) {
      if (!contains(done, m)) return "delegar " + m + " " + pedido;
    }
    return "responder: [mock] resposta para: " + truncate(pedido, 200);
  }
  return "[mock] resposta para: " + truncate(user, 200);
}

struct PedidoLLM {
  std::string url;
  std::vector<std::string> headers;
  std::string corpo;
  // Extrai (texto, tok_entrada, tok_saida) da resposta por provedor.
  std::string texto_de(const Value& resp, long long& tok_in, long long& tok_out) const {
    if (const Value* u = dig(resp, {"usage", "input_tokens"})) {
      if (u->kind == ValueKind::Inteiro) tok_in = u->i;
    }
    if (const Value* u = dig(resp, {"usage", "output_tokens"})) {
      if (u->kind == ValueKind::Inteiro) tok_out = u->i;
    }
    if (const Value* u = dig(resp, {"usage", "prompt_tokens"})) {
      if (u->kind == ValueKind::Inteiro) tok_in = u->i;
    }
    if (const Value* u = dig(resp, {"usage", "completion_tokens"})) {
      if (u->kind == ValueKind::Inteiro) tok_out = u->i;
    }
    if (const Value* t = dig(resp, {"content", "0", "text"})) {
      if (t->kind == ValueKind::Texto) return t->s;
    }
    if (const Value* t = dig(resp, {"choices", "0", "message", "content"})) {
      if (t->kind == ValueKind::Texto) return t->s;
    }
    throw std::runtime_error("resposta do LLM em formato inesperado");
  }
};

PedidoLLM monta_chat(const LlmConfig& cfg, const std::string& system, const std::string& user) {
  PedidoLLM p;
  Value body = Value::mapa();
  body.map->set("model", Value::texto(cfg.model));
  body.map->set("temperature", Value::decimal(cfg.temperature));

  if (cfg.provider == "anthropic") {
    // Sem base_url: API da Anthropic; com base_url: endpoint compativel
    // (mock local nos testes) mantendo path e corpo Anthropic.
    p.url = cfg.base_url.empty() ? "https://api.anthropic.com/v1/messages"
                                 : cfg.base_url + "/v1/messages";
    p.headers = {"x-api-key: " + cfg.api_key, "anthropic-version: 2023-06-01"};
    body.map->set("max_tokens", Value::inteiro(cfg.max_tokens));
    if (!system.empty()) body.map->set("system", Value::texto(system));
    Value msg = Value::mapa();
    msg.map->set("role", Value::texto("user"));
    msg.map->set("content", Value::texto(user));
    body.map->set("messages", Value::lista({msg}));
  } else {
    // openai | local | vllm. Sem base_url: API da OpenAI; com base_url:
    // "<base>/chat/completions" (compativel OpenAI, como local/vllm).
    p.url = cfg.base_url.empty() ? "https://api.openai.com/v1/chat/completions"
                                 : cfg.base_url + "/chat/completions";
    p.headers = {"Authorization: Bearer " + cfg.api_key};
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
  p.corpo = json_dump(body);
  return p;
}

// Uma config, com retry/backoff/timeout/teto. Devolve texto + tokens.
RespostaLLM chat_uma(const LlmConfig& cfg, const std::string& system, const std::string& user) {
  if (llm_is_mock()) {
    if (cfg.teto_tokens > 0 && uso_total(cfg.nome) >= cfg.teto_tokens) {
      throw std::runtime_error("teto_tokens " + std::to_string(cfg.teto_tokens) + " estourado em '" +
                               cfg.nome + "' (mock)");
    }
    const std::string t = mock_chat(system, user);
    const long long tin = mock_tokens(system + user);
    const long long tout = mock_tokens(t);
    soma_uso(cfg.nome, tin, tout);
    return {t, tin, tout, cfg.model};
  }

  if (cfg.teto_tokens > 0 && uso_total(cfg.nome) >= cfg.teto_tokens) {
    throw std::runtime_error("teto_tokens " + std::to_string(cfg.teto_tokens) + " estourado em '" +
                             cfg.nome + "' (acumulado " + std::to_string(uso_total(cfg.nome)) + ")");
  }
  const PedidoLLM ped = monta_chat(cfg, system, user);
  const int tents = cfg.tentativas < 1 ? 1 : cfg.tentativas;
  std::string ultimo_erro;
  for (int t = 1; t <= tents; ++t) {
    HttpResult r;
    try {
      r = http_post_status(ped.url, ped.headers, ped.corpo, cfg.tempo_limite);
    } catch (const std::exception& e) {
      ultimo_erro = e.what();
      if (t < tents) {
        espera_retry(t);
        continue;
      }
      throw std::runtime_error(std::string(ultimo_erro) + " apos " + std::to_string(tents) +
                               " tentativa(s) (ajuste tempo_limite:/tentativas: em '" + cfg.nome +
                               "')");
    }
    if (r.status >= 200 && r.status < 300) {
      Value resp = json_parse(r.body);
      long long tin = 0, tout = 0;
      const std::string texto = ped.texto_de(resp, tin, tout);
      soma_uso(cfg.nome, tin, tout);
      return {texto, tin, tout, cfg.model};
    }
    if (r.status == 429 || (r.status >= 500 && r.status < 600)) {
      ultimo_erro = "HTTP " + std::to_string(r.status) + ": " + truncate(r.body, 200);
      if (t < tents) {
        espera_retry(t);
        continue;
      }
      throw std::runtime_error(ultimo_erro + " apos " + std::to_string(tents) +
                               " tentativa(s) (ajuste tentativas:/reserva: em '" + cfg.nome + "')");
    }
    // 4xx (menos 429): erro do pedido, retry nao adianta.
    throw std::runtime_error("HTTP " + std::to_string(r.status) + ": " + truncate(r.body, 300));
  }
  throw std::runtime_error(ultimo_erro);
}

RespostaLLM llm_chat_cadeia(const std::vector<LlmConfig>& cadeia, const std::string& system,
                             const std::string& user) {
  if (cadeia.empty()) throw std::runtime_error("cadeia de LLMs vazia");
  std::string erros;
  for (std::size_t k = 0; k < cadeia.size(); ++k) {
    try {
      return chat_uma(cadeia[k], system, user);
    } catch (const std::exception& e) {
      if (!erros.empty()) erros += "; ";
      erros += "'" + cadeia[k].nome + "': " + e.what();
    }
  }
  throw std::runtime_error(erros);
}

std::string llm_chat(const LlmConfig& cfg, const std::string& system, const std::string& user) {
  return llm_chat_cadeia({cfg}, system, user).texto;
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
  const std::string payload = json_dump(body);
  const std::vector<std::string> headers = {
      "Authorization: Bearer " +
      std::string(std::getenv("OPENAI_API_KEY") ? std::getenv("OPENAI_API_KEY") : "")};
  // Mesmo transporte do chat (timeout 60s, 3 tentativas em 429/5xx/transporte).
  std::string raw;
  std::string ultimo_erro;
  for (int t = 1; t <= 3; ++t) {
    HttpResult r;
    try {
      r = http_post_status("https://api.openai.com/v1/embeddings", headers, payload, 60);
    } catch (const std::exception& e) {
      ultimo_erro = e.what();
      if (t < 3) {
        espera_retry(t);
        continue;
      }
      throw std::runtime_error(std::string(ultimo_erro) + " apos 3 tentativa(s)");
    }
    if (r.status >= 200 && r.status < 300) {
      raw = r.body;
      break;
    }
    if (r.status == 429 || (r.status >= 500 && r.status < 600)) {
      ultimo_erro = "HTTP " + std::to_string(r.status);
      if (t < 3) {
        espera_retry(t);
        continue;
      }
      throw std::runtime_error(ultimo_erro + " apos 3 tentativa(s): " + truncate(r.body, 200));
    }
    throw std::runtime_error("HTTP " + std::to_string(r.status) + ": " + truncate(r.body, 300));
  }
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
