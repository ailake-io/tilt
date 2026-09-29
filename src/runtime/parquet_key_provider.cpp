#include "runtime/parquet_key_provider.hpp"

#include <array>
#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "runtime/http_client.hpp"
#include "runtime/json.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& message) {
  throw std::runtime_error("provedor de chave Parquet: " + message);
}

std::string env_required(std::initializer_list<const char*> names) {
  for (const char* name : names) {
    if (const char* value = std::getenv(name); value && *value) return value;
  }
  std::string msg = "defina ";
  bool first = true;
  for (const char* name : names) {
    if (!first) msg += " ou ";
    first = false;
    msg += name;
  }
  die(msg);
}

std::string trim_slash(std::string value) {
  while (!value.empty() && value.back() == '/') value.pop_back();
  return value;
}

const Value& response_object(const HttpClientResponse& response, const char* provider) {
  if (!response.error.empty()) die(std::string(provider) + ": falha HTTP: " + response.error);
  if (response.status < 200 || response.status >= 300) {
    die(std::string(provider) + ": HTTP " + std::to_string(response.status) +
        " — " + response.body.substr(0, 300));
  }
  try {
    static thread_local Value parsed;
    parsed = json_parse(response.body);
    if (parsed.kind != ValueKind::Mapa || !parsed.map_ref()) die("resposta nao e um objeto JSON");
    return parsed;
  } catch (const std::exception& e) {
    die(std::string(provider) + ": JSON invalido: " + e.what());
  }
}

int base64_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::string base64_encode(const std::uint8_t* data, std::size_t size) {
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (std::size_t i = 0; i < size; i += 3) {
    const std::size_t remain = size - i;
    const unsigned a = data[i];
    const unsigned b = remain > 1 ? data[i + 1] : 0;
    const unsigned c = remain > 2 ? data[i + 2] : 0;
    out += alphabet[(a >> 2) & 63];
    out += alphabet[((a & 3) << 4) | (b >> 4)];
    out += remain > 1 ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
    out += remain > 2 ? alphabet[c & 63] : '=';
  }
  return out;
}

bool base64_decode(const std::string& input, std::string& output) {
  output.clear();
  if (input.empty() || input.size() % 4 != 0) return false;
  for (std::size_t i = 0; i < input.size(); i += 4) {
    const int a = base64_value(static_cast<unsigned char>(input[i]));
    const int b = base64_value(static_cast<unsigned char>(input[i + 1]));
    const bool pad2 = input[i + 2] == '=';
    const bool pad3 = input[i + 3] == '=';
    const int c = pad2 ? 0 : base64_value(static_cast<unsigned char>(input[i + 2]));
    const int d = pad3 ? 0 : base64_value(static_cast<unsigned char>(input[i + 3]));
    if (a < 0 || b < 0 || c < 0 || d < 0 || (pad2 && !pad3) ||
        (i + 4 != input.size() && (pad2 || pad3))) return false;
    const unsigned block = (static_cast<unsigned>(a) << 18) |
                           (static_cast<unsigned>(b) << 12) |
                           (static_cast<unsigned>(c) << 6) | static_cast<unsigned>(d);
    output.push_back(static_cast<char>((block >> 16) & 0xff));
    if (!pad2) output.push_back(static_cast<char>((block >> 8) & 0xff));
    if (!pad3) output.push_back(static_cast<char>(block & 0xff));
  }
  return true;
}

void random_key(std::array<std::uint8_t, 32>& out) {
  std::random_device rd;
  for (std::uint8_t& byte : out) byte = static_cast<std::uint8_t>(rd());
}

std::string json_field(const Value& value, const char* name, const char* provider) {
  const Value* field = value.map_ref() ? value.map_ref()->find(name) : nullptr;
  if (!field || field->kind != ValueKind::Texto || field->s.empty()) {
    die(std::string(provider) + ": resposta sem campo '" + name + "'");
  }
  return field->s;
}

std::string vault_url(const std::string& key_id, const char* operation) {
  std::string base = trim_slash(env_required({"VAULT_ADDR"}));
  std::string path = key_id;
  while (!path.empty() && path.front() == '/') path.erase(path.begin());
  if (path.rfind("v1/", 0) == 0) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) die("path Vault invalido");
    return base + "/" + path.substr(0, slash + 1) + operation + "/" + path.substr(slash + 1);
  }
  if (path.rfind("transit/", 0) == 0) path.erase(0, std::string("transit/").size());
  return base + "/v1/transit/" + operation + "/" + path;
}

HttpClientResponse request_json(const std::string& /*provider*/, const std::string& url,
                                const std::string& token, const std::string& body) {
  return http_request("POST", url,
                      {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + token}},
                      body, 30, false);
}

HttpClientResponse request_vault_json(const std::string& url, const std::string& token,
                                      const std::string& body) {
  // O Transit usa X-Vault-Token; Authorization: Bearer e aceito por alguns
  // proxies, mas nao e o header nativo e falha em instalacoes padrao.
  return http_request("POST", url,
                      {{"Content-Type", "application/json"}, {"X-Vault-Token", token}},
                      body, 30, false);
}

void azure(const std::string& key_id, std::array<std::uint8_t, 32>& plaintext,
           std::string& ciphertext, bool decrypt) {
  const std::string token = env_required({"AZURE_KEY_VAULT_TOKEN", "AZURE_ACCESS_TOKEN"});
  std::string url = key_id;
  while (!url.empty() && url.back() == '/') url.pop_back();
  const std::size_t query = url.find('?');
  if (query != std::string::npos) url.resize(query);
  const std::size_t slash = url.find_last_of('/');
  if (slash == std::string::npos) die("Azure Key Vault key id invalido");
  url += decrypt ? "/unwrap?api-version=7.4" : "/wrap?api-version=7.4";
  const std::string value = decrypt ? ciphertext : base64_encode(plaintext.data(), plaintext.size());
  const std::string body = "{\"alg\":\"RSA-OAEP-256\",\"value\":\"" + value + "\"}";
  const Value& response = response_object(request_json("Azure Key Vault", url, token, body), "Azure Key Vault");
  const std::string wrapped = json_field(response, "value", "Azure Key Vault");
  if (decrypt) {
    std::string decoded;
    if (!base64_decode(wrapped, decoded) || decoded.size() != plaintext.size()) die("data key descriptografada invalida");
    std::copy(decoded.begin(), decoded.end(), plaintext.begin());
  } else {
    ciphertext = wrapped;
  }
}

void gcp(const std::string& key_id, std::array<std::uint8_t, 32>& plaintext,
         std::string& ciphertext, bool decrypt) {
  const std::string token = env_required({"GOOGLE_OAUTH_ACCESS_TOKEN", "GCP_ACCESS_TOKEN"});
  std::string base = trim_slash(std::getenv("GCP_KMS_ENDPOINT") ? std::getenv("GCP_KMS_ENDPOINT") : "https://cloudkms.googleapis.com");
  if (base.size() < 3 || base.rfind("/v1") != base.size() - 3) base += "/v1";
  const std::string op = decrypt ? ":decrypt" : ":encrypt";
  const std::string url = base + "/" + key_id + op;
  const std::string field = decrypt ? ciphertext : base64_encode(plaintext.data(), plaintext.size());
  const std::string body = decrypt ? "{\"ciphertext\":\"" + field + "\"}" : "{\"plaintext\":\"" + field + "\"}";
  const Value& response = response_object(request_json("GCP KMS", url, token, body), "GCP KMS");
  const std::string encoded = json_field(response, decrypt ? "plaintext" : "ciphertext", "GCP KMS");
  if (decrypt) {
    std::string decoded;
    if (!base64_decode(encoded, decoded) || decoded.size() != plaintext.size()) die("data key descriptografada invalida");
    std::copy(decoded.begin(), decoded.end(), plaintext.begin());
  } else {
    ciphertext = encoded;
  }
}

void vault(const std::string& key_id, std::array<std::uint8_t, 32>& plaintext,
           std::string& ciphertext, bool decrypt) {
  const std::string token = env_required({"VAULT_TOKEN"});
  const std::string url = vault_url(key_id, decrypt ? "decrypt" : "encrypt");
  const std::string body = decrypt ? "{\"ciphertext\":\"" + ciphertext + "\"}"
                                   : "{\"plaintext\":\"" + base64_encode(plaintext.data(), plaintext.size()) + "\"}";
  const Value& response = response_object(request_vault_json(url, token, body), "Vault Transit");
  const Value* data = response.map_ref() ? response.map_ref()->find("data") : nullptr;
  if (!data || data->kind != ValueKind::Mapa || !data->map_ref()) die("resposta sem data");
  const std::string encoded = json_field(*data, decrypt ? "plaintext" : "ciphertext", "Vault Transit");
  if (decrypt) {
    std::string decoded;
    if (!base64_decode(encoded, decoded) || decoded.size() != plaintext.size()) die("data key descriptografada invalida");
    std::copy(decoded.begin(), decoded.end(), plaintext.begin());
  } else {
    ciphertext = encoded;
  }
}

}  // namespace

void parquet_cloud_generate(const std::string& provider, const std::string& key_id,
                            std::array<std::uint8_t, 32>& plaintext,
                            std::string& ciphertext, std::string& resolved_key_id) {
  if (key_id.empty()) die("identificador de chave vazio");
  random_key(plaintext);
  if (provider == "azure-key-vault-v1") azure(key_id, plaintext, ciphertext, false);
  else if (provider == "gcp-kms-v1") gcp(key_id, plaintext, ciphertext, false);
  else if (provider == "vault-transit-v1") vault(key_id, plaintext, ciphertext, false);
  else die("provedor desconhecido: " + provider);
  resolved_key_id = key_id;
}

void parquet_cloud_decrypt(const std::string& provider, const std::string& key_id,
                           const std::string& ciphertext,
                           std::array<std::uint8_t, 32>& plaintext) {
  if (key_id.empty() || ciphertext.empty()) die("identificador ou ciphertext vazio");
  std::string wrapped = ciphertext;
  if (provider == "azure-key-vault-v1") azure(key_id, plaintext, wrapped, true);
  else if (provider == "gcp-kms-v1") gcp(key_id, plaintext, wrapped, true);
  else if (provider == "vault-transit-v1") vault(key_id, plaintext, wrapped, true);
  else die("provedor desconhecido: " + provider);
}

}  // namespace tilt::rt
