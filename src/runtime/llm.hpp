#pragma once

#include <string>
#include <vector>

namespace tilt::rt {

struct LlmConfig {
  std::string provider = "anthropic";  // anthropic | openai | local | vllm
  std::string model;
  std::string api_key;
  std::string base_url;  // for local/vllm (OpenAI-compatible)
  double temperature = 0.2;
  int max_tokens = 1024;
};

// Backend selected by the TILT_LLM environment variable:
//   unset / "curl" -> real HTTP via the `curl` binary
//   "mock"          -> deterministic canned output (offline, for tests)
// Throws std::runtime_error on transport/HTTP failure.
std::string llm_chat(const LlmConfig& cfg, const std::string& system, const std::string& user);

// Deterministic in mock mode; real embeddings via `curl` otherwise.
std::vector<float> llm_embed(const std::string& model, const std::string& text);

bool llm_is_mock();

}  // namespace tilt::rt
