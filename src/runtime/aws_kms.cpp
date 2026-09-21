#include "runtime/aws_kms.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "runtime/aws_sigv4.hpp"
#include "runtime/http_client.hpp"
#include "runtime/json.hpp"
#include "runtime/sha256.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& message) { throw std::runtime_error("kms: " + message); }

std::string env_or(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return value && *value ? std::string(value) : fallback;
}

std::string env_required(const char* name) {
  const char* value = std::getenv(name);
  if (!value || !*value) {
    die("defina AWS_ACCESS_KEY_ID e AWS_SECRET_ACCESS_KEY");
  }
  return value;
}

std::string endpoint_host(const std::string& endpoint) {
  std::string host = endpoint;
  const std::size_t scheme = host.find("://");
  if (scheme != std::string::npos) host.erase(0, scheme + 3);
  const std::size_t slash = host.find('/');
  if (slash != std::string::npos) host.resize(slash);
  if (host.empty()) die("KMS_ENDPOINT invalido");
  return host;
}

void wipe(std::string& value) {
  volatile char* p = value.empty() ? nullptr : value.data();
  for (std::size_t i = 0; i < value.size(); ++i) p[i] = 0;
  value.clear();
}

int base64_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool base64_decode(const std::string& input, std::string& output) {
  output.clear();
  if (input.empty() || input.size() % 4 != 0) return false;
  output.reserve(input.size() / 4 * 3);
  for (std::size_t i = 0; i < input.size(); i += 4) {
    const bool last = i + 4 == input.size();
    const int a = base64_value(static_cast<unsigned char>(input[i]));
    const int b = base64_value(static_cast<unsigned char>(input[i + 1]));
    if (a < 0 || b < 0) return false;
    const bool pad2 = input[i + 2] == '=';
    const bool pad3 = input[i + 3] == '=';
    if ((!last && (pad2 || pad3)) || (pad2 && !pad3)) return false;
    const int c = pad2 ? 0 : base64_value(static_cast<unsigned char>(input[i + 2]));
    const int d = pad3 ? 0 : base64_value(static_cast<unsigned char>(input[i + 3]));
    if (c < 0 || d < 0 || (pad2 && (b & 0x0F) != 0) || (pad3 && !pad2 && (c & 0x03) != 0)) {
      return false;
    }
    const unsigned int block = (static_cast<unsigned int>(a) << 18) |
                               (static_cast<unsigned int>(b) << 12) |
                               (static_cast<unsigned int>(c) << 6) | static_cast<unsigned int>(d);
    output.push_back(static_cast<char>((block >> 16) & 0xFF));
    if (!pad2) output.push_back(static_cast<char>((block >> 8) & 0xFF));
    if (!pad3) output.push_back(static_cast<char>(block & 0xFF));
  }
  return true;
}

std::string required_text(const Value& object, const char* name) {
  const Value* field = object.map ? object.map->find(name) : nullptr;
  if (!field || field->kind != ValueKind::Texto || field->s.empty()) {
    die(std::string("resposta KMS sem campo ") + name);
  }
  return field->s;
}

std::string error_text(const std::string& body) {
  try {
    const Value value = json_parse(body);
    if (value.kind == ValueKind::Mapa && value.map) {
      const Value* message = value.map->find("message");
      if (!message) message = value.map->find("Message");
      if (message && message->kind == ValueKind::Texto && !message->s.empty()) {
        return message->s.substr(0, 200);
      }
    }
  } catch (const std::exception&) {
  }
  return body.substr(0, 200);
}

Value kms_request(const std::string& operation, const Value& request) {
  const AwsSigV4Credentials credentials{
      env_required("AWS_ACCESS_KEY_ID"),
      env_required("AWS_SECRET_ACCESS_KEY"),
      env_or("AWS_SESSION_TOKEN", ""),
      env_or("AWS_REGION", "us-east-1"),
  };
  const std::string endpoint =
      env_or("KMS_ENDPOINT", "https://kms." + credentials.region + ".amazonaws.com");
  const std::string host = endpoint_host(endpoint);
  std::string base = endpoint;
  while (!base.empty() && base.back() == '/') base.pop_back();
  const std::string url = base + "/";
  const std::string body = json_dump_compacto(request);
  const std::string payload_hash = sha256_hex(body);
  const std::string target = "TrentService." + operation;
  const std::vector<std::pair<std::string, std::string>> signed_extra = {
      {"content-type", "application/x-amz-json-1.1"},
      {"x-amz-target", target},
  };
  const AwsSigV4Signature signature =
      aws_sigv4_sign(credentials, "kms", "POST", "/", "", host, payload_hash, signed_extra);
  std::vector<std::pair<std::string, std::string>> headers = {
      {"Host", host},
      {"X-Amz-Content-Sha256", payload_hash},
      {"X-Amz-Date", signature.amz_date},
      {"Authorization", signature.authorization},
      {"Content-Type", "application/x-amz-json-1.1"},
      {"X-Amz-Target", target},
  };
  if (!credentials.session_token.empty()) {
    headers.emplace_back("X-Amz-Security-Token", credentials.session_token);
  }
  HttpClientResponse response = http_request("POST", url, headers, body, 30, false);
  if (!response.error.empty()) die("falha HTTP: " + response.error);
  if (response.status < 200 || response.status >= 300) {
    const std::string message = error_text(response.body);
    wipe(response.body);
    die("AWS KMS HTTP " + std::to_string(response.status) + ": " + message);
  }
  try {
    Value parsed = json_parse(response.body);
    wipe(response.body);
    if (parsed.kind != ValueKind::Mapa || !parsed.map) {
      die("resposta AWS KMS nao e um objeto JSON");
    }
    return parsed;
  } catch (const std::runtime_error& error) {
    wipe(response.body);
    die(std::string("resposta JSON invalida: ") + error.what());
  }
}

void decode_plaintext(Value& response, std::array<std::uint8_t, 32>& plaintext) {
  Value* field = response.map ? response.map->find("Plaintext") : nullptr;
  if (!field || field->kind != ValueKind::Texto) {
    die("resposta AWS KMS sem Plaintext");
  }
  std::string decoded;
  const bool valid = base64_decode(field->s, decoded);
  wipe(field->s);
  if (!valid || decoded.size() != plaintext.size()) {
    wipe(decoded);
    die("AWS KMS retornou uma data key que nao e AES-256");
  }
  std::copy(decoded.begin(), decoded.end(), plaintext.begin());
  wipe(decoded);
}

}  // namespace

void aws_kms_generate_data_key(const std::string& key_id, std::array<std::uint8_t, 32>& plaintext,
                               std::string& ciphertext_blob, std::string& resolved_key_id) {
  if (key_id.empty()) die("KeyId vazio para GenerateDataKey");
  Value request = Value::mapa();
  request.map->set("KeyId", Value::texto(key_id));
  request.map->set("KeySpec", Value::texto("AES_256"));
  Value response = kms_request("GenerateDataKey", request);
  const std::string blob = required_text(response, "CiphertextBlob");
  const std::string resolved = required_text(response, "KeyId");
  decode_plaintext(response, plaintext);
  ciphertext_blob = blob;
  resolved_key_id = resolved;
}

void aws_kms_decrypt_data_key(const std::string& key_id, const std::string& ciphertext_blob,
                              std::array<std::uint8_t, 32>& plaintext) {
  if (key_id.empty() || ciphertext_blob.empty()) {
    die("KeyId ou CiphertextBlob vazio para Decrypt");
  }
  Value request = Value::mapa();
  request.map->set("KeyId", Value::texto(key_id));
  request.map->set("CiphertextBlob", Value::texto(ciphertext_blob));
  Value response = kms_request("Decrypt", request);
  decode_plaintext(response, plaintext);
}

}  // namespace tilt::rt
