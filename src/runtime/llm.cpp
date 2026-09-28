#include "runtime/llm.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <iomanip>
#include <map>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "runtime/compat.hpp"
#include "runtime/http_client.hpp"
#include "runtime/json.hpp"
#include "runtime/sha256.hpp"
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

std::string agora_utc_iso() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

// Sem falhar em 4xx/5xx: o chamador decide retry para 429/5xx.
struct HttpResult {
  long status = 0;
  std::string body;
  long retry_after = -1;  // segundos; -1 = ausente/invalido
};

std::string trim_http(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

long retry_after_seconds(const std::string& headers) {
  long result = -1;
  std::istringstream lines(headers);
  std::string line;
  while (std::getline(lines, line)) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name != "retry-after") continue;
    const std::string value = trim_http(line.substr(colon + 1));
    try {
      std::size_t used = 0;
      const long seconds = std::stol(value, &used);
      if (used == value.size() && seconds >= 0) result = std::min(seconds, 300L);
      continue;
    } catch (...) {
    }
    std::tm date{};
    std::istringstream parsed(value);
    parsed >> std::get_time(&date, "%a, %d %b %Y %H:%M:%S GMT");
    if (parsed.fail()) continue;
#if defined(_WIN32)
    const std::time_t target = _mkgmtime(&date);
#else
    const std::time_t target = timegm(&date);
#endif
    const std::time_t now = std::time(nullptr);
    if (target >= now)
      result = std::min(static_cast<long>(target - now), 300L);
    else
      result = 0;
  }
  return result;
}

HttpResult http_post_status(const std::string& url, const std::vector<std::string>& headers,
                            const std::string& body, int timeout_s) {
  std::vector<std::pair<std::string, std::string>> request_headers = {
      {"content-type", "application/json"}};
  for (const std::string& line : headers) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0)
      throw std::runtime_error("header LLM invalido");
    request_headers.emplace_back(line.substr(0, colon), trim_http(line.substr(colon + 1)));
  }
  std::vector<std::pair<std::string, std::string>> response_headers;
  HttpClientResponse response =
      http_request("POST", url, request_headers, body, timeout_s, false, &response_headers);
  if (!response.error.empty())
    throw std::runtime_error("falha de transporte: " + response.error);
  std::string header_text;
  for (const auto& [name, value] : response_headers)
    header_text += name + ": " + value + "\n";
  return {response.status, std::move(response.body), retry_after_seconds(header_text)};
}

// Contabilidade de tokens por llm. O mapa acelera o processo atual; o ledger
// JSONL opcional torna o teto e os custos persistentes entre execucoes.
std::mutex g_uso_mu;
std::map<std::string, std::pair<long long, long long>> g_uso;  // nome -> {entrada, saida}
std::map<std::string, double> g_custo;
std::map<std::string, long long> g_chamadas;
std::mutex g_arquivo_mu;

struct LlmContexto {
  std::string agente;
  std::string sessao;
  std::string trace_id;
};
thread_local LlmContexto g_contexto;
std::atomic<unsigned long long> g_trace_seq{0};

std::string novo_trace_id() {
  const auto seq = ++g_trace_seq;
  std::ostringstream out;
  out << std::hex << std::hash<std::thread::id>{}(std::this_thread::get_id()) << '-' << seq;
  return out.str();
}

double custo_tokens(const LlmConfig& cfg, long long entrada, long long saida) {
  return (static_cast<double>(entrada) * cfg.custo_entrada_mil +
          static_cast<double>(saida) * cfg.custo_saida_mil) /
         1000.0;
}

void soma_uso(const LlmConfig& cfg, long long entrada, long long saida, double custo) {
  std::lock_guard<std::mutex> lk(g_uso_mu);
  auto& u = g_uso[cfg.nome];
  u.first += entrada;
  u.second += saida;
  g_custo[cfg.nome] += custo;
  g_chamadas[cfg.nome]++;
}

void persistir_evento(const LlmConfig& cfg, const std::string& operacao, long long entrada,
                      long long saida, double custo, const std::string& sistema,
                      const std::string& usuario, const std::string& resposta, bool cache) {
  if (cfg.contabilidade.empty() && cfg.observabilidade.empty()) return;
  std::lock_guard<std::mutex> lk(g_arquivo_mu);
  const std::string agora = agora_utc_iso();
  auto compactar = [](std::string s) {
    s.erase(std::remove(s.begin(), s.end(), '\n'), s.end());
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
  };
  if (!cfg.contabilidade.empty()) {
    Value e = Value::mapa();
    e.map_ref()->set("timestamp", Value::texto(agora));
    e.map_ref()->set("llm", Value::texto(cfg.nome));
    e.map_ref()->set("provedor", Value::texto(cfg.provider));
    e.map_ref()->set("modelo", Value::texto(cfg.model));
    e.map_ref()->set("operacao", Value::texto(operacao));
    if (!g_contexto.agente.empty()) e.map_ref()->set("agente", Value::texto(g_contexto.agente));
    if (!g_contexto.sessao.empty()) e.map_ref()->set("sessao", Value::texto(g_contexto.sessao));
    if (!g_contexto.trace_id.empty()) e.map_ref()->set("trace_id", Value::texto(g_contexto.trace_id));
    e.map_ref()->set("entrada", Value::inteiro(entrada));
    e.map_ref()->set("saida", Value::inteiro(saida));
    e.map_ref()->set("custo", Value::decimal(custo));
    e.map_ref()->set("cache", Value::logico(cache));
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(cfg.contabilidade).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::ofstream out(cfg.contabilidade, std::ios::app);
    if (out) out << compactar(json_dump(e)) << '\n';
  }
  if (!cfg.observabilidade.empty()) {
    Value e = Value::mapa();
    e.map_ref()->set("timestamp", Value::texto(agora));
    e.map_ref()->set("llm", Value::texto(cfg.nome));
    e.map_ref()->set("modelo", Value::texto(cfg.model));
    e.map_ref()->set("operacao", Value::texto(operacao));
    if (!g_contexto.agente.empty()) e.map_ref()->set("agente", Value::texto(g_contexto.agente));
    if (!g_contexto.sessao.empty()) e.map_ref()->set("sessao", Value::texto(g_contexto.sessao));
    if (!g_contexto.trace_id.empty()) e.map_ref()->set("trace_id", Value::texto(g_contexto.trace_id));
    e.map_ref()->set("entrada", Value::inteiro(entrada));
    e.map_ref()->set("saida", Value::inteiro(saida));
    e.map_ref()->set("custo", Value::decimal(custo));
    if (cfg.registrar_prompts) {
      e.map_ref()->set("sistema", Value::texto(sistema));
      e.map_ref()->set("usuario", Value::texto(usuario));
      e.map_ref()->set("resposta", Value::texto(resposta));
    } else {
      e.map_ref()->set("prompt_registrado", Value::logico(false));
      e.map_ref()->set("sistema_hash", Value::texto(sha256_hex(sistema)));
      e.map_ref()->set("usuario_hash", Value::texto(sha256_hex(usuario)));
      e.map_ref()->set("resposta_hash", Value::texto(sha256_hex(resposta)));
    }
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(cfg.observabilidade).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::ofstream out(cfg.observabilidade, std::ios::app);
    if (out) out << compactar(json_dump(e)) << '\n';
  }
}

void registrar_uso(const LlmConfig& cfg, long long entrada, long long saida,
                  const std::string& sistema = {}, const std::string& usuario = {},
                  const std::string& resposta = {}, const std::string& operacao = "chat",
                  bool cache = false) {
  const double custo = custo_tokens(cfg, entrada, saida);
  soma_uso(cfg, entrada, saida, custo);
  persistir_evento(cfg, operacao, entrada, saida, custo, sistema, usuario, resposta, cache);
}

std::pair<long long, long long> ler_ledger(const std::string& arquivo, const std::string& nome,
                                           double* custo = nullptr, long long* chamadas = nullptr) {
  std::pair<long long, long long> soma{0, 0};
  if (custo) *custo = 0.0;
  if (chamadas) *chamadas = 0;
  std::ifstream in(arquivo);
  std::string conteudo((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::string> objetos;
  std::size_t inicio = std::string::npos;
  int profundidade = 0;
  bool string_json = false;
  bool escape = false;
  for (std::size_t p = 0; p < conteudo.size(); ++p) {
    const char c = conteudo[p];
    if (string_json) {
      if (escape) escape = false;
      else if (c == '\\') escape = true;
      else if (c == '"') string_json = false;
      continue;
    }
    if (c == '"') { string_json = true; continue; }
    if (c == '{') { if (profundidade++ == 0) inicio = p; }
    else if (c == '}' && profundidade > 0 && --profundidade == 0 && inicio != std::string::npos) {
      objetos.push_back(conteudo.substr(inicio, p - inicio + 1));
      inicio = std::string::npos;
    }
  }
  for (const std::string& linha : objetos) {
    try {
      Value e = json_parse(linha);
      if (e.kind != ValueKind::Mapa || !e.map_ref()) continue;
      const Value* l = e.map_ref()->find("llm");
      if (l && l->kind == ValueKind::Texto && !nome.empty() && l->s != nome) continue;
      const Value* i = e.map_ref()->find("entrada");
      const Value* o = e.map_ref()->find("saida");
      if (i && i->is_number()) soma.first += static_cast<long long>(i->as_number());
      if (o && o->is_number()) soma.second += static_cast<long long>(o->as_number());
      if (custo) {
        const Value* c = e.map_ref()->find("custo");
        if (c && c->is_number()) *custo += c->as_number();
      }
      if (chamadas) ++*chamadas;
    } catch (...) {
      // Linhas incompletas nao invalidam o restante do ledger.
    }
  }
  return soma;
}

long long uso_total(const LlmConfig& cfg) {
  if (!cfg.contabilidade.empty()) {
    const auto s = ler_ledger(cfg.contabilidade, cfg.nome);
    return s.first + s.second;
  }
  std::lock_guard<std::mutex> lk(g_uso_mu);
  auto it = g_uso.find(cfg.nome);
  return it == g_uso.end() ? 0 : it->second.first + it->second.second;
}

std::mutex g_cache_mu;
std::map<std::string, RespostaLLM> g_chat_cache;

std::string llm_cache_key(const LlmConfig& cfg, const std::string& system,
                          const std::string& user) {
  std::ostringstream key;
  auto add = [&key](const std::string& value) { key << value.size() << ':' << value; };
  add(cfg.provider);
  add(cfg.nome);
  add(cfg.model);
  add(cfg.base_url);
  key << std::setprecision(17) << cfg.temperature << ':' << cfg.max_tokens << ':';
  add(system);
  add(user);
  return key.str();
}

bool cache_read(const std::string& key, RespostaLLM& response) {
  std::lock_guard<std::mutex> lk(g_cache_mu);
  const auto it = g_chat_cache.find(key);
  if (it == g_chat_cache.end()) return false;
  response = it->second;
  return true;
}

void cache_write(const std::string& key, const RespostaLLM& response) {
  std::lock_guard<std::mutex> lk(g_cache_mu);
  g_chat_cache[key] = response;
}

// Heuristica de tokens p/ o mock (chars/4 por lado; deterministica).
long long mock_tokens(const std::string& s) {
  return static_cast<long long>((s.size() + 3) / 4);
}

// Dorme entre tentativas: 1s, 2s, 4s... teto 15s (sem jitter: deterministico).
void espera_retry(int tentativa, long retry_after = -1) {
  long espera = retry_after >= 0 ? retry_after : 1L << (tentativa - 1);
  if (espera > 300) espera = 300;
  std::this_thread::sleep_for(std::chrono::seconds(espera));
}

const Value* dig(const Value& v, std::initializer_list<const char*> path) {
  const Value* cur = &v;
  for (const char* key : path) {
    if (!cur) return nullptr;
    if (cur->kind == ValueKind::Mapa && cur->map_ref()) {
      cur = cur->map_ref()->find(key);
    } else if (cur->kind == ValueKind::Lista && cur->list_ref() && !cur->list_ref()->empty() &&
               std::strcmp(key, "0") == 0) {
      cur = &(*cur->list_ref())[0];
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

std::string texto_de_fluxo(const PedidoLLM& ped, const std::string& raw, long long& tok_in,
                           long long& tok_out) {
  if (raw.find("data:") == std::string::npos) {
    const Value response = json_parse(raw);
    return ped.texto_de(response, tok_in, tok_out);
  }
  std::string texto;
  std::istringstream lines(raw);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("data:", 0) != 0) continue;
    const std::string payload = trim_http(line.substr(5));
    if (payload.empty() || payload == "[DONE]") continue;
    Value event;
    try {
      event = json_parse(payload);
    } catch (...) {
      continue;
    }
    auto usage = [&](const char* path0, const char* path1, long long& target) {
      const Value* value = dig(event, {path0, path1});
      if (value && value->kind == ValueKind::Inteiro) target = value->i;
    };
    usage("usage", "input_tokens", tok_in);
    usage("usage", "prompt_tokens", tok_in);
    usage("usage", "output_tokens", tok_out);
    usage("usage", "completion_tokens", tok_out);
    usage("message_delta", "output_tokens", tok_out);
    const Value* delta = dig(event, {"delta", "text"});
    if (delta && delta->kind == ValueKind::Texto) texto += delta->s;
    delta = dig(event, {"choices", "0", "delta", "content"});
    if (delta && delta->kind == ValueKind::Texto) texto += delta->s;
    delta = dig(event, {"choices", "0", "text"});
    if (delta && delta->kind == ValueKind::Texto) texto += delta->s;
  }
  if (texto.empty()) throw std::runtime_error("resposta SSE do LLM sem texto");
  return texto;
}

PedidoLLM monta_chat(const LlmConfig& cfg, const std::string& system, const std::string& user,
                     bool fluxo = false) {
  PedidoLLM p;
  Value body = Value::mapa();
  body.map_ref()->set("model", Value::texto(cfg.model));
  body.map_ref()->set("temperature", Value::decimal(cfg.temperature));
  if (fluxo) body.map_ref()->set("stream", Value::logico(true));

  if (cfg.provider == "anthropic") {
    // Sem base_url: API da Anthropic; com base_url: endpoint compativel
    // (mock local nos testes) mantendo path e corpo Anthropic.
    p.url = cfg.base_url.empty() ? "https://api.anthropic.com/v1/messages"
                                 : cfg.base_url + "/v1/messages";
    p.headers = {"x-api-key: " + cfg.api_key, "anthropic-version: 2023-06-01"};
    body.map_ref()->set("max_tokens", Value::inteiro(cfg.max_tokens));
    if (!system.empty()) body.map_ref()->set("system", Value::texto(system));
    Value msg = Value::mapa();
    msg.map_ref()->set("role", Value::texto("user"));
    msg.map_ref()->set("content", Value::texto(user));
    body.map_ref()->set("messages", Value::lista({msg}));
  } else {
    // openai | local | vllm. Sem base_url: API da OpenAI; com base_url:
    // "<base>/chat/completions" (compativel OpenAI, como local/vllm).
    p.url = cfg.base_url.empty() ? "https://api.openai.com/v1/chat/completions"
                                 : cfg.base_url + "/chat/completions";
    p.headers = {"Authorization: Bearer " + cfg.api_key};
    Value msgs = Value::lista();
    if (!system.empty()) {
      Value s = Value::mapa();
      s.map_ref()->set("role", Value::texto("system"));
      s.map_ref()->set("content", Value::texto(system));
      msgs.list_ref()->push_back(s);
    }
    Value u = Value::mapa();
    u.map_ref()->set("role", Value::texto("user"));
    u.map_ref()->set("content", Value::texto(user));
    msgs.list_ref()->push_back(u);
    body.map_ref()->set("messages", msgs);
  }
  p.corpo = json_dump(body);
  return p;
}

// Uma config, com retry/backoff/timeout/teto. Devolve texto + tokens.
RespostaLLM chat_uma(const LlmConfig& cfg, const std::string& system, const std::string& user) {
  const std::string cache_key = cfg.cache ? llm_cache_key(cfg, system, user) : std::string();
  if (cfg.cache) {
    RespostaLLM cached;
    if (cache_read(cache_key, cached)) return cached;
  }
  if (llm_is_mock()) {
    if (cfg.teto_tokens > 0 && uso_total(cfg) >= cfg.teto_tokens) {
      throw std::runtime_error("teto_tokens " + std::to_string(cfg.teto_tokens) + " estourado em '" +
                               cfg.nome + "' (mock)");
    }
    const std::string t = mock_chat(system, user);
    const long long tin = mock_tokens(system + user);
    const long long tout = mock_tokens(t);
    RespostaLLM response{t, tin, tout, cfg.model};
    response.custo = custo_tokens(cfg, tin, tout);
    registrar_uso(cfg, tin, tout, system, user, t, "chat");
    if (cfg.cache) cache_write(cache_key, response);
    return response;
  }

  if (cfg.teto_tokens > 0 && uso_total(cfg) >= cfg.teto_tokens) {
    throw std::runtime_error("teto_tokens " + std::to_string(cfg.teto_tokens) + " estourado em '" +
                             cfg.nome + "' (acumulado " + std::to_string(uso_total(cfg)) + ")");
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
        espera_retry(t, r.retry_after);
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
      RespostaLLM response{texto, tin, tout, cfg.model};
      response.custo = custo_tokens(cfg, tin, tout);
      registrar_uso(cfg, tin, tout, system, user, texto, "chat");
      if (cfg.cache) cache_write(cache_key, response);
      return response;
    }
    if (r.status == 429 || (r.status >= 500 && r.status < 600)) {
      ultimo_erro = "HTTP " + std::to_string(r.status) + ": " + truncate(r.body, 200);
      if (t < tents) {
        espera_retry(t, r.retry_after);
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

// --- ferramentas nativas ---------------------------------------------------

Value mensagens_anthropic(const std::vector<MensagemLLM>& mensagens) {
  Value out = Value::lista();
  Value* resultados = nullptr;  // user com tool_result consecutivos (mesmo turno)
  for (const MensagemLLM& m : mensagens) {
    if (m.papel == "tool") {
      if (!resultados) {
        Value turno = Value::mapa();
        turno.map_ref()->set("role", Value::texto("user"));
        turno.map_ref()->set("content", Value::lista());
        out.list_ref()->push_back(std::move(turno));
        resultados = out.list_ref()->back().map_ref()->find("content");
      }
      Value bloco = Value::mapa();
      bloco.map_ref()->set("type", Value::texto("tool_result"));
      bloco.map_ref()->set("tool_use_id", Value::texto(m.chamada_id));
      bloco.map_ref()->set("content", Value::texto(m.texto));
      resultados->list_ref()->push_back(std::move(bloco));
      continue;
    }
    resultados = nullptr;
    Value turno = Value::mapa();
    turno.map_ref()->set("role", Value::texto(m.papel));
    if (m.papel == "assistant" && !m.chamadas.empty()) {
      Value blocos = Value::lista();
      if (!m.texto.empty()) {
        Value t = Value::mapa();
        t.map_ref()->set("type", Value::texto("text"));
        t.map_ref()->set("text", Value::texto(m.texto));
        blocos.list_ref()->push_back(std::move(t));
      }
      for (const ChamadaFerramenta& c : m.chamadas) {
        Value u = Value::mapa();
        u.map_ref()->set("type", Value::texto("tool_use"));
        u.map_ref()->set("id", Value::texto(c.id));
        u.map_ref()->set("name", Value::texto(c.nome));
        u.map_ref()->set("input", c.argumentos.kind == ValueKind::Mapa ? c.argumentos : Value::mapa());
        blocos.list_ref()->push_back(std::move(u));
      }
      turno.map_ref()->set("content", std::move(blocos));
    } else {
      turno.map_ref()->set("content", Value::texto(m.texto));
    }
    out.list_ref()->push_back(std::move(turno));
  }
  return out;
}

Value mensagens_openai(const std::string& system, const std::vector<MensagemLLM>& mensagens) {
  Value out = Value::lista();
  if (!system.empty()) {
    Value s = Value::mapa();
    s.map_ref()->set("role", Value::texto("system"));
    s.map_ref()->set("content", Value::texto(system));
    out.list_ref()->push_back(std::move(s));
  }
  for (const MensagemLLM& m : mensagens) {
    Value turno = Value::mapa();
    if (m.papel == "tool") {
      turno.map_ref()->set("role", Value::texto("tool"));
      turno.map_ref()->set("tool_call_id", Value::texto(m.chamada_id));
      turno.map_ref()->set("content", Value::texto(m.texto));
    } else {
      turno.map_ref()->set("role", Value::texto(m.papel));
      turno.map_ref()->set("content", m.texto.empty() && !m.chamadas.empty() ? Value::nulo()
                                                                       : Value::texto(m.texto));
      if (!m.chamadas.empty()) {
        Value chamadas = Value::lista();
        for (const ChamadaFerramenta& c : m.chamadas) {
          Value fn = Value::mapa();
          fn.map_ref()->set("name", Value::texto(c.nome));
          fn.map_ref()->set("arguments", Value::texto(json_dump(c.argumentos)));
          Value item = Value::mapa();
          item.map_ref()->set("id", Value::texto(c.id));
          item.map_ref()->set("type", Value::texto("function"));
          item.map_ref()->set("function", std::move(fn));
          chamadas.list_ref()->push_back(std::move(item));
        }
        turno.map_ref()->set("tool_calls", std::move(chamadas));
      }
    }
    out.list_ref()->push_back(std::move(turno));
  }
  return out;
}

PedidoLLM monta_ferramentas(const LlmConfig& cfg, const std::string& system,
                            const std::vector<MensagemLLM>& mensagens,
                            const std::vector<FerramentaLLM>& ferramentas) {
  PedidoLLM p;
  Value body = Value::mapa();
  body.map_ref()->set("model", Value::texto(cfg.model));
  body.map_ref()->set("temperature", Value::decimal(cfg.temperature));
  Value tools = Value::lista();
  if (cfg.provider == "anthropic") {
    p.url = cfg.base_url.empty() ? "https://api.anthropic.com/v1/messages"
                                 : cfg.base_url + "/v1/messages";
    p.headers = {"x-api-key: " + cfg.api_key, "anthropic-version: 2023-06-01"};
    body.map_ref()->set("max_tokens", Value::inteiro(cfg.max_tokens));
    if (!system.empty()) body.map_ref()->set("system", Value::texto(system));
    for (const FerramentaLLM& f : ferramentas) {
      Value t = Value::mapa();
      t.map_ref()->set("name", Value::texto(f.nome));
      t.map_ref()->set("description", Value::texto(f.descricao));
      t.map_ref()->set("input_schema", f.schema);
      tools.list_ref()->push_back(std::move(t));
    }
    body.map_ref()->set("tools", std::move(tools));
    body.map_ref()->set("messages", mensagens_anthropic(mensagens));
  } else {
    p.url = cfg.base_url.empty() ? "https://api.openai.com/v1/chat/completions"
                                 : cfg.base_url + "/chat/completions";
    p.headers = {"Authorization: Bearer " + cfg.api_key};
    for (const FerramentaLLM& f : ferramentas) {
      Value fn = Value::mapa();
      fn.map_ref()->set("name", Value::texto(f.nome));
      fn.map_ref()->set("description", Value::texto(f.descricao));
      fn.map_ref()->set("parameters", f.schema);
      Value t = Value::mapa();
      t.map_ref()->set("type", Value::texto("function"));
      t.map_ref()->set("function", std::move(fn));
      tools.list_ref()->push_back(std::move(t));
    }
    body.map_ref()->set("tools", std::move(tools));
    body.map_ref()->set("messages", mensagens_openai(system, mensagens));
  }
  p.corpo = json_dump(body);
  return p;
}

// Extrai texto, chamadas e tokens da resposta de cada provedor.
RespostaFerramentas resposta_ferramentas(const LlmConfig& cfg, const Value& resp) {
  RespostaFerramentas out;
  out.modelo = cfg.model;
  auto inteiro = [&](std::initializer_list<const char*> caminho) -> long long {
    const Value* v = dig(resp, caminho);
    return v && v->kind == ValueKind::Inteiro ? v->i : 0;
  };
  if (cfg.provider == "anthropic") {
    out.tok_entrada = inteiro({"usage", "input_tokens"});
    out.tok_saida = inteiro({"usage", "output_tokens"});
    const Value* blocos = resp.map_ref() ? resp.map_ref()->find("content") : nullptr;
    if (blocos && blocos->kind == ValueKind::Lista && blocos->list_ref()) {
      for (const Value& b : *blocos->list_ref()) {
        const Value* tipo = b.map_ref() ? b.map_ref()->find("type") : nullptr;
        if (!tipo || tipo->kind != ValueKind::Texto) continue;
        if (tipo->s == "text") {
          if (const Value* t = b.map_ref()->find("text"); t && t->kind == ValueKind::Texto) {
            out.texto += t->s;
          }
        } else if (tipo->s == "tool_use") {
          ChamadaFerramenta c;
          if (const Value* v = b.map_ref()->find("id"); v && v->kind == ValueKind::Texto) c.id = v->s;
          if (const Value* v = b.map_ref()->find("name"); v && v->kind == ValueKind::Texto) c.nome = v->s;
          const Value* in = b.map_ref()->find("input");
          c.argumentos = in && in->kind == ValueKind::Mapa ? *in : Value::mapa();
          out.chamadas.push_back(std::move(c));
        }
      }
    }
  } else {
    out.tok_entrada = inteiro({"usage", "prompt_tokens"});
    out.tok_saida = inteiro({"usage", "completion_tokens"});
    if (const Value* c = dig(resp, {"choices", "0", "message", "content"});
        c && c->kind == ValueKind::Texto) {
      out.texto = c->s;
    }
    if (const Value* tc = dig(resp, {"choices", "0", "message", "tool_calls"});
        tc && tc->kind == ValueKind::Lista && tc->list_ref()) {
      for (const Value& item : *tc->list_ref()) {
        ChamadaFerramenta c;
        if (const Value* v = item.map_ref() ? item.map_ref()->find("id") : nullptr;
            v && v->kind == ValueKind::Texto) {
          c.id = v->s;
        }
        const Value* fn = item.map_ref() ? item.map_ref()->find("function") : nullptr;
        if (!fn || fn->kind != ValueKind::Mapa || !fn->map_ref()) continue;
        if (const Value* v = fn->map_ref()->find("name"); v && v->kind == ValueKind::Texto) c.nome = v->s;
        c.argumentos = Value::mapa();
        if (const Value* a = fn->map_ref()->find("arguments"); a && a->kind == ValueKind::Texto) {
          try {
            Value parsed = json_parse(a->s);
            if (parsed.kind == ValueKind::Mapa) c.argumentos = std::move(parsed);
          } catch (const std::exception&) {
            // argumentos malformados: o interpretador completa com best-effort
          }
        }
        out.chamadas.push_back(std::move(c));
      }
    }
  }
  return out;
}

RespostaFerramentas ferramentas_uma(const LlmConfig& cfg, const std::string& system,
                                    const std::vector<MensagemLLM>& mensagens,
                                    const std::vector<FerramentaLLM>& ferramentas) {
  if (cfg.teto_tokens > 0 && uso_total(cfg) >= cfg.teto_tokens) {
    throw std::runtime_error("teto_tokens " + std::to_string(cfg.teto_tokens) + " estourado em '" +
                             cfg.nome + "'");
  }
  if (llm_is_mock()) {
    // Planner deterministico: uma chamada por ferramenta, na ordem declarada.
    std::size_t resultados = 0;
    for (const MensagemLLM& m : mensagens) resultados += m.papel == "tool" ? 1 : 0;
    RespostaFerramentas out;
    out.modelo = cfg.model;
    if (resultados < ferramentas.size()) {
      ChamadaFerramenta c;
      c.id = "mock_call_" + std::to_string(resultados + 1);
      c.nome = ferramentas[resultados].nome;
      c.argumentos = Value::mapa();
      out.chamadas.push_back(std::move(c));
    } else {
      out.texto = "[mock] resposta final apos " + std::to_string(resultados) + " ferramenta(s)";
    }
    std::string entrada = system;
    for (const MensagemLLM& m : mensagens) entrada += m.texto;
    out.tok_entrada = mock_tokens(entrada);
    out.tok_saida = mock_tokens(out.texto);
    out.custo = custo_tokens(cfg, out.tok_entrada, out.tok_saida);
    registrar_uso(cfg, out.tok_entrada, out.tok_saida, system, {}, out.texto, "ferramentas");
    return out;
  }
  const PedidoLLM ped = monta_ferramentas(cfg, system, mensagens, ferramentas);
  const int tents = cfg.tentativas < 1 ? 1 : cfg.tentativas;
  std::string ultimo_erro;
  for (int t = 1; t <= tents; ++t) {
    HttpResult r;
    try {
      r = http_post_status(ped.url, ped.headers, ped.corpo, cfg.tempo_limite);
    } catch (const std::exception& e) {
      ultimo_erro = e.what();
      if (t < tents) {
        espera_retry(t, r.retry_after);
        continue;
      }
      throw std::runtime_error(ultimo_erro + " apos " + std::to_string(tents) +
                               " tentativa(s) (ajuste tempo_limite:/tentativas: em '" + cfg.nome +
                               "')");
    }
    if (r.status >= 200 && r.status < 300) {
      RespostaFerramentas out = resposta_ferramentas(cfg, json_parse(r.body));
      out.custo = custo_tokens(cfg, out.tok_entrada, out.tok_saida);
      registrar_uso(cfg, out.tok_entrada, out.tok_saida, system, {}, out.texto, "ferramentas");
      return out;
    }
    if (r.status == 429 || (r.status >= 500 && r.status < 600)) {
      ultimo_erro = "HTTP " + std::to_string(r.status) + ": " + truncate(r.body, 200);
      if (t < tents) {
        espera_retry(t, r.retry_after);
        continue;
      }
      throw std::runtime_error(ultimo_erro + " apos " + std::to_string(tents) +
                               " tentativa(s) (ajuste tentativas:/reserva: em '" + cfg.nome + "')");
    }
    throw std::runtime_error("HTTP " + std::to_string(r.status) + ": " + truncate(r.body, 300));
  }
  throw std::runtime_error(ultimo_erro);
}

RespostaLLM chat_fluxo_uma(const LlmConfig& cfg, const std::string& system,
                           const std::string& user) {
  const std::string cache_key = cfg.cache ? llm_cache_key(cfg, system, user) : std::string();
  if (cfg.cache) {
    RespostaLLM cached;
    if (cache_read(cache_key, cached)) return cached;
  }
  if (llm_is_mock()) {
    if (cfg.teto_tokens > 0 && uso_total(cfg) >= cfg.teto_tokens) {
      throw std::runtime_error("teto_tokens excedido no streaming");
    }
    const std::string texto = mock_chat(system, user);
    RespostaLLM response{texto, mock_tokens(system + user), mock_tokens(texto), cfg.model};
    response.custo = custo_tokens(cfg, response.tok_entrada, response.tok_saida);
    registrar_uso(cfg, response.tok_entrada, response.tok_saida, system, user, response.texto, "fluxo");
    if (cfg.cache) cache_write(cache_key, response);
    return response;
  }
  if (cfg.teto_tokens > 0 && uso_total(cfg) >= cfg.teto_tokens) {
    throw std::runtime_error("teto_tokens excedido no streaming");
  }
  const PedidoLLM ped = monta_chat(cfg, system, user, true);
  const int tents = cfg.tentativas < 1 ? 1 : cfg.tentativas;
  std::string ultimo_erro;
  for (int t = 1; t <= tents; ++t) {
    HttpResult r;
    try {
      r = http_post_status(ped.url, ped.headers, ped.corpo, cfg.tempo_limite);
    } catch (const std::exception& e) {
      ultimo_erro = e.what();
      if (t < tents) {
        espera_retry(t, r.retry_after);
        continue;
      }
      throw std::runtime_error(ultimo_erro + " apos " + std::to_string(tents) +
                               " tentativa(s) no streaming");
    }
    if (r.status >= 200 && r.status < 300) {
      long long tok_in = 0, tok_out = 0;
      const std::string texto = texto_de_fluxo(ped, r.body, tok_in, tok_out);
      RespostaLLM response{texto, tok_in, tok_out, cfg.model};
      response.custo = custo_tokens(cfg, tok_in, tok_out);
      registrar_uso(cfg, tok_in, tok_out, system, user, texto, "fluxo");
      if (cfg.cache) cache_write(cache_key, response);
      return response;
    }
    if (r.status == 429 || (r.status >= 500 && r.status < 600)) {
      ultimo_erro = "HTTP " + std::to_string(r.status) + ": " + truncate(r.body, 200);
      if (t < tents) {
        espera_retry(t, r.retry_after);
        continue;
      }
      throw std::runtime_error(ultimo_erro + " apos " + std::to_string(tents) +
                               " tentativa(s) no streaming");
    }
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

RespostaFerramentas llm_chat_ferramentas(const std::vector<LlmConfig>& cadeia,
                                         const std::string& system,
                                         const std::vector<MensagemLLM>& mensagens,
                                         const std::vector<FerramentaLLM>& ferramentas) {
  if (cadeia.empty()) throw std::runtime_error("cadeia de LLMs vazia");
  std::string erros;
  for (std::size_t k = 0; k < cadeia.size(); ++k) {
    try {
      return ferramentas_uma(cadeia[k], system, mensagens, ferramentas);
    } catch (const std::exception& e) {
      if (!erros.empty()) erros += "; ";
      erros += "'" + cadeia[k].nome + "': " + e.what();
    }
  }
  throw std::runtime_error(erros);
}

RespostaLLM llm_chat_fluxo_cadeia(const std::vector<LlmConfig>& cadeia, const std::string& system,
                                  const std::string& user) {
  if (cadeia.empty()) throw std::runtime_error("cadeia de LLMs vazia");
  std::string erros;
  for (std::size_t k = 0; k < cadeia.size(); ++k) {
    try {
      return chat_fluxo_uma(cadeia[k], system, user);
    } catch (const std::exception& e) {
      if (!erros.empty()) erros += "; ";
      erros += "stream " + cadeia[k].nome + ": " + e.what();
    }
  }
  throw std::runtime_error(erros);
}

std::string llm_chat(const LlmConfig& cfg, const std::string& system, const std::string& user) {
  return llm_chat_cadeia({cfg}, system, user).texto;
}

std::vector<float> llm_embed_impl(const std::string& model, const std::string& text,
                                  long long* tokens) {
  if (llm_is_mock()) {
    // Deterministic hashed token + subword trigram embedding, L2-normalized.
    // Tokens keep exact-word precision; boundary-padded trigrams reduce the
    // brittleness of the old bag-of-tokens mock for related spellings.
    constexpr std::size_t kDim = 16;
    constexpr float kTrigramWeight = 0.25F;
    std::vector<float> v(kDim, 0.0F);
    auto hash_text = [](const std::string& value) {
      std::uint64_t h = 1469598103934665603ULL;
      for (unsigned char c : value) {
        h ^= c;
        h *= 1099511628211ULL;
      }
      return h;
    };
    auto add_token = [&](const std::string& token) {
      if (token.empty()) return;
      v[hash_text(token) % kDim] += 1.0F;
      const std::string padded = "^" + token + "$";
      if (padded.size() < 3) return;
      for (std::size_t i = 0; i + 3 <= padded.size(); ++i) {
        v[hash_text(padded.substr(i, 3)) % kDim] += kTrigramWeight;
      }
    };
    std::string token;
    for (char c : text) {
      if (std::isalnum(static_cast<unsigned char>(c))) {
        token += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      } else {
        add_token(token);
        token.clear();
      }
    }
    add_token(token);
    float norm = 0.0F;
    for (float x : v) norm += x * x;
    norm = norm > 0.0F ? std::sqrt(norm) : 1.0F;
    for (float& x : v) x /= norm;
    if (tokens) *tokens = mock_tokens(text);
    return v;
  }

  Value body = Value::mapa();
  body.map_ref()->set("model", Value::texto(model));
  body.map_ref()->set("input", Value::texto(text));
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
        espera_retry(t, r.retry_after);
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
        espera_retry(t, r.retry_after);
        continue;
      }
      throw std::runtime_error(ultimo_erro + " apos 3 tentativa(s): " + truncate(r.body, 200));
    }
    throw std::runtime_error("HTTP " + std::to_string(r.status) + ": " + truncate(r.body, 300));
  }
  Value resp = json_parse(raw);
  if (tokens) {
    *tokens = mock_tokens(text);
    if (const Value* usage = resp.map_ref() ? resp.map_ref()->find("usage") : nullptr;
        usage && usage->kind == ValueKind::Mapa && usage->map_ref()) {
      if (const Value* p = usage->map_ref()->find("prompt_tokens"); p && p->is_number())
        *tokens = static_cast<long long>(p->as_number());
      else if (const Value* p = usage->map_ref()->find("total_tokens"); p && p->is_number())
        *tokens = static_cast<long long>(p->as_number());
    }
  }
  std::vector<float> out;
  if (const Value* arr = dig(resp, {"data", "0", "embedding"});
      arr && arr->kind == ValueKind::Lista && arr->list_ref()) {
    for (const Value& e : *arr->list_ref()) out.push_back(static_cast<float>(e.as_number()));
  }
  if (out.empty()) throw std::runtime_error("resposta de embeddings em formato inesperado");
  return out;
}

std::vector<float> llm_embed(const std::string& model, const std::string& text) {
  return llm_embed_impl(model, text, nullptr);
}

std::vector<float> llm_embed(const LlmConfig& cfg, const std::string& text) {
  if (cfg.teto_tokens > 0 && uso_total(cfg) >= cfg.teto_tokens) {
    throw std::runtime_error("teto_tokens " + std::to_string(cfg.teto_tokens) +
                             " estourado em '" + cfg.nome + "' antes do embedding");
  }
  long long tokens = 0;
  std::vector<float> out = llm_embed_impl(cfg.model, text, &tokens);
  registrar_uso(cfg, tokens, 0, {}, text, {}, "embedding");
  return out;
}

long long llm_uso_total(const LlmConfig& cfg) { return uso_total(cfg); }

Value llm_metricas(const LlmConfig& cfg) {
  long long entrada = 0;
  long long saida = 0;
  double custo = 0.0;
  long long chamadas = 0;
  if (!cfg.contabilidade.empty()) {
    const auto s = ler_ledger(cfg.contabilidade, cfg.nome, &custo, &chamadas);
    entrada = s.first;
    saida = s.second;
  } else {
    std::lock_guard<std::mutex> lk(g_uso_mu);
    const auto it = g_uso.find(cfg.nome);
    if (it != g_uso.end()) {
      entrada = it->second.first;
      saida = it->second.second;
    }
    custo = g_custo[cfg.nome];
    chamadas = g_chamadas[cfg.nome];
  }
  Value out = Value::mapa();
  out.map_ref()->set("llm", Value::texto(cfg.nome));
  out.map_ref()->set("entrada", Value::inteiro(entrada));
  out.map_ref()->set("saida", Value::inteiro(saida));
  out.map_ref()->set("total", Value::inteiro(entrada + saida));
  out.map_ref()->set("custo", Value::decimal(custo));
  out.map_ref()->set("chamadas", Value::inteiro(chamadas));
  if (!cfg.contabilidade.empty()) out.map_ref()->set("arquivo", Value::texto(cfg.contabilidade));
  return out;
}

LlmContextoGuard::LlmContextoGuard(std::string agente, std::string sessao, std::string trace_id) {
  g_contexto.agente = std::move(agente);
  g_contexto.sessao = std::move(sessao);
  trace_id_ = trace_id.empty() ? novo_trace_id() : std::move(trace_id);
  g_contexto.trace_id = trace_id_;
  ativo_ = true;
}

LlmContextoGuard::~LlmContextoGuard() {
  if (!ativo_) return;
  g_contexto = {};
}

}  // namespace tilt::rt
