#pragma once

#include <string>
#include <vector>

#include "runtime/value.hpp"

namespace tilt::rt {

struct LlmConfig {
  std::string provider = "anthropic";  // anthropic | openai | local | vllm
  std::string model;
  std::string api_key;
  std::string base_url;  // for local/vllm (OpenAI-compatible)
  double temperature = 0.2;
  int max_tokens = 1024;
  // Robustez (1a passada): nome p/ contabilidade, timeout, retry e teto.
  std::string nome;                 // nome do bloco `llm` (conta de tokens)
  int tempo_limite = 60;            // segundos por tentativa (curl --max-time)
  int tentativas = 3;               // tentativas em erro de transporte/429/5xx
  long long teto_tokens = 0;        // 0 = sem teto; >0 = falha antes de estourar
  bool cache = false;               // reutiliza respostas idempotentes no processo
  std::vector<std::string> reserva;  // nomes de outros `llm` (fallback em ordem)
  // Governanca: valores em moeda por mil tokens e arquivos JSONL opcionais.
  double custo_entrada_mil = 0.0;
  double custo_saida_mil = 0.0;
  std::string contabilidade;        // ledger persistente de uso/custo
  std::string observabilidade;       // eventos de prompt/resposta
  bool registrar_prompts = false;    // opt-in para nao vazar dados sensiveis
};

// Resposta com contabilidade: texto + tokens + modelo que respondeu.
struct RespostaLLM {
  std::string texto;
  long long tok_entrada = 0;
  long long tok_saida = 0;
  std::string modelo;
  double custo = 0.0;
};

// Backend selected by the TILT_LLM environment variable:
//   unset / "curl" -> real HTTP via the `curl` binary
//   "mock"          -> deterministic canned output (offline, for tests)
// Throws std::runtime_error on transport/HTTP failure.
std::string llm_chat(const LlmConfig& cfg, const std::string& system, const std::string& user);

// Cadeia com fallback: tenta cada config em ordem (primario + reservas);
// joga erro combinado se todas falharem. Cada uma aplica seu proprio
// tempo_limite/tentativas/teto_tokens. Em mock, a primeira responde.
RespostaLLM llm_chat_cadeia(const std::vector<LlmConfig>& cadeia, const std::string& system,
                             const std::string& user);

// Streaming SSE: solicita stream=true, materializa os deltas e aplica o mesmo retry/fallback.
RespostaLLM llm_chat_fluxo_cadeia(const std::vector<LlmConfig>& cadeia, const std::string& system,
                                  const std::string& user);

// --- Tool-calling nativo (Anthropic `tool_use` / OpenAI `tool_calls`) -------

// Ferramenta oferecida ao modelo: nome, descricao e JSON Schema dos argumentos.
struct FerramentaLLM {
  std::string nome;
  std::string descricao;
  Value schema;  // {"type":"object","properties":{...},"required":[...]}
};

struct ChamadaFerramenta {
  std::string id;
  std::string nome;
  Value argumentos;  // mapa
};

// Turno da conversa: "user", "assistant" (texto e/ou chamadas) ou "tool"
// (resultado de `chamada_id`).
struct MensagemLLM {
  std::string papel;
  std::string texto;
  std::vector<ChamadaFerramenta> chamadas;
  std::string chamada_id;
};

struct RespostaFerramentas {
  std::string texto;                        // vazio quando so ha chamadas
  std::vector<ChamadaFerramenta> chamadas;  // vazio => resposta final
  long long tok_entrada = 0;
  long long tok_saida = 0;
  std::string modelo;
  double custo = 0.0;
};

// Um turno de conversa com ferramentas nativas, com o mesmo retry/fallback/
// teto_tokens de llm_chat_cadeia. Em mock, chama cada ferramenta uma vez (na
// ordem) com argumentos vazios e depois responde texto fixo.
RespostaFerramentas llm_chat_ferramentas(const std::vector<LlmConfig>& cadeia,
                                         const std::string& system,
                                         const std::vector<MensagemLLM>& mensagens,
                                         const std::vector<FerramentaLLM>& ferramentas);

// Deterministic in mock mode; real embeddings via `curl` otherwise.
std::vector<float> llm_embed(const std::string& model, const std::string& text);
std::vector<float> llm_embed(const LlmConfig& cfg, const std::string& text);

// Consulta a contabilidade em memoria e, quando configurado, o ledger JSONL
// persistente. O mapa devolvido e estavel para uso em Tilt:
// {entrada, saida, total, custo, chamadas}.
Value llm_metricas(const LlmConfig& cfg);
long long llm_uso_total(const LlmConfig& cfg);

// Contexto de rastreamento por thread. O interpretador usa este escopo para
// associar eventos a agente, sessão e trace_id sem alterar a API do provedor.
class LlmContextoGuard {
 public:
  LlmContextoGuard(std::string agente, std::string sessao, std::string trace_id = {});
  ~LlmContextoGuard();
  LlmContextoGuard(const LlmContextoGuard&) = delete;
  LlmContextoGuard& operator=(const LlmContextoGuard&) = delete;
  const std::string& trace_id() const { return trace_id_; }
 private:
  bool ativo_ = false;
  std::string trace_id_;
};

bool llm_is_mock();

}  // namespace tilt::rt
