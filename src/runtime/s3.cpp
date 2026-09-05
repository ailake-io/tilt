#include "runtime/s3.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

#include "runtime/sha256.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("s3: " + m); }

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

struct Objeto {
  std::string bucket;
  std::string chave;
};

Objeto parse_url(const std::string& url) {
  const std::string prefix = "s3://";
  if (url.rfind(prefix, 0) != 0) {
    die("esperado 's3://bucket/chave'");
  }
  const std::string rest = url.substr(prefix.size());
  const std::size_t slash = rest.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size()) {
    die("esperado 's3://bucket/chave'");
  }
  Objeto o;
  o.bucket = rest.substr(0, slash);
  o.chave = rest.substr(slash + 1);
  return o;
}

std::string env_ou(const char* nome, const std::string& padrao) {
  const char* v = std::getenv(nome);
  return (v && *v) ? std::string(v) : padrao;
}

std::string env_obrigatorio(const char* nome) {
  const char* v = std::getenv(nome);
  if (!v || !*v) die("defina AWS_ACCESS_KEY_ID e AWS_SECRET_ACCESS_KEY");
  return v;
}

// Codifica cada segmento (entre '/') preservando os caracteres unreserved do
// SigV4 (RFC 3986): letras, digitos, '-', '_', '.', '~'. Espaco vira %20 etc.
std::string uri_encode_segmentos(const std::string& path) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : path) {
    const bool unreserved =
        std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
    if (c == '/') {
      out += '/';
    } else if (unreserved) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[(c >> 4) & 0x0F];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

// Host (com porta) do endpoint, p/ header Host e assinatura. Aceita
// "http://host:porta/base" ou so "host:porta".
std::string endpoint_host(const std::string& endpoint) {
  std::string rest = endpoint;
  const std::size_t scheme = rest.find("://");
  if (scheme != std::string::npos) rest = rest.substr(scheme + 3);
  const std::size_t slash = rest.find('/');
  if (slash != std::string::npos) rest = rest.substr(0, slash);
  return rest;
}

std::string endpoint_base(const std::string& endpoint) {
  std::string base = endpoint;
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base;
}

struct Credenciais {
  std::string ak;
  std::string sk;
  std::string token;
  std::string region;
  std::string endpoint;
};

Credenciais credenciais() {
  Credenciais c;
  c.ak = env_obrigatorio("AWS_ACCESS_KEY_ID");
  c.sk = env_obrigatorio("AWS_SECRET_ACCESS_KEY");
  c.token = env_ou("AWS_SESSION_TOKEN", "");
  c.region = env_ou("AWS_REGION", "us-east-1");
  c.endpoint = env_ou("S3_ENDPOINT", "https://s3." + c.region + ".amazonaws.com");
  return c;
}

struct Assinatura {
  std::string authorization;
  std::string amz_date;
};

// AWS SigV4 (service "s3", query string vazia nesta 1a passada).
std::string hex_lower(const std::array<std::uint8_t, 32>& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out(64, '0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[2 * i] = kDigits[(bytes[i] >> 4) & 0x0F];
    out[2 * i + 1] = kDigits[bytes[i] & 0x0F];
  }
  return out;
}

Assinatura assinar(const Credenciais& c, const std::string& method,
                   const std::string& canonical_uri, const std::string& host,
                   const std::string& payload_hash) {
  const std::time_t agora = std::time(nullptr);
  std::tm tm_utc{};
  gmtime_r(&agora, &tm_utc);
  char data_buf[9];
  std::strftime(data_buf, sizeof data_buf, "%Y%m%d", &tm_utc);
  const std::string date_stamp = data_buf;
  char amz_buf[17];
  std::strftime(amz_buf, sizeof amz_buf, "%Y%m%dT%H%M%SZ", &tm_utc);
  const std::string amz_date = amz_buf;

  const std::string scope = date_stamp + "/" + c.region + "/s3/aws4_request";

  std::string canonical_headers = "host:" + host + "\n" +
                                  "x-amz-content-sha256:" + payload_hash + "\n" +
                                  "x-amz-date:" + amz_date + "\n";
  std::string signed_headers = "host;x-amz-content-sha256;x-amz-date";
  if (!c.token.empty()) {
    canonical_headers += "x-amz-security-token:" + c.token + "\n";
    signed_headers += ";x-amz-security-token";
  }

  const std::string canonical_request = method + "\n" + canonical_uri + "\n" + "\n" +
                                        canonical_headers + "\n" + signed_headers +
                                        "\n" + payload_hash;

  const std::string string_to_sign = "AWS4-HMAC-SHA256\n" + amz_date + "\n" + scope +
                                     "\n" + sha256_hex(canonical_request);

  const auto k_date = hmac_sha256_raw("AWS4" + c.sk, date_stamp);
  const auto k_region = hmac_sha256_raw(k_date, c.region);
  const auto k_service = hmac_sha256_raw(k_region, "s3");
  const auto k_signing = hmac_sha256_raw(k_service, "aws4_request");
  const std::string signature =
      hex_lower(hmac_sha256_raw(k_signing, string_to_sign));

  Assinatura a;
  a.amz_date = amz_date;
  a.authorization = "AWS4-HMAC-SHA256 Credential=" + c.ak + "/" + scope +
                    ", SignedHeaders=" + signed_headers + ", Signature=" + signature;
  return a;
}

// Executa o curl: corpo da resposta vai para `out_file`, status vem no stdout
// (`-w '%{http_code}'`). HTTP >= 400 (ou falha de transporte) -> die com o
// corpo da resposta truncado em ~200 chars. Retorna o status HTTP.
int http(const std::string& method, const std::string& url,
         const std::vector<std::pair<std::string, std::string>>& headers,
         const std::string& body, const std::string& out_file) {
  std::string body_file;
  std::string cmd = "curl -s --fail-with-body -o " + shell_quote(out_file) +
                    " -w '%{http_code}' -X " + method;
  for (const auto& [nome, valor] : headers) {
    cmd += " -H " + shell_quote(nome + ": " + valor);
  }
  if (method == "PUT") {
    char tmpl[] = "/tmp/tilt_s3_body_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd < 0) die("nao foi possivel criar arquivo temporario");
    std::ofstream out(tmpl, std::ios::trunc);
    out << body;
    out.close();
    ::close(fd);
    body_file = tmpl;
    cmd += " --data @" + body_file;
  }
  cmd += " " + shell_quote(url);

  std::string resp;
  {
    std::array<char, 4096> buf{};
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
      if (!body_file.empty()) ::unlink(body_file.c_str());
      die("nao foi possivel executar 'curl'");
    }
    std::size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) resp.append(buf.data(), n);
    const int rc = ::pclose(pipe);
    if (!body_file.empty()) ::unlink(body_file.c_str());
    if (rc != 0) {
      std::ifstream in(out_file);
      std::string corpo((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      die("requisicao falhou (curl codigo " + std::to_string(rc) +
          "): verifique endpoint/bucket/credenciais. Resposta: " + corpo.substr(0, 200));
    }
  }

  int status = 0;
  std::istringstream iss(resp);
  iss >> status;
  if (status >= 400) {
    std::ifstream in(out_file);
    std::string corpo((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    die("HTTP " + std::to_string(status) + ": " + corpo.substr(0, 200));
  }
  return status;
}

std::string exec(const Credenciais& c, const std::string& method, const Objeto& o,
                 const std::string& body) {
  const std::string host = endpoint_host(c.endpoint);
  const std::string canonical_uri = "/" + o.bucket + "/" + uri_encode_segmentos(o.chave);
  const std::string url = endpoint_base(c.endpoint) + canonical_uri;
  const std::string payload_hash = sha256_hex(body);

  const Assinatura a = assinar(c, method, canonical_uri, host, payload_hash);
  std::vector<std::pair<std::string, std::string>> headers = {
      {"Host", host},
      {"X-Amz-Content-Sha256", payload_hash},
      {"X-Amz-Date", a.amz_date},
      {"Authorization", a.authorization},
  };
  if (!c.token.empty()) headers.emplace_back("X-Amz-Security-Token", c.token);
  if (method == "PUT") headers.emplace_back("Content-Type", "application/octet-stream");

  char tmpl[] = "/tmp/tilt_s3_resp_XXXXXX";
  const int fd = ::mkstemp(tmpl);
  if (fd < 0) die("nao foi possivel criar arquivo temporario");
  ::close(fd);
  const std::string out_file = tmpl;
  http(method, url, headers, body, out_file);

  std::ifstream in(out_file, std::ios::binary);
  std::string corpo((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ::unlink(out_file.c_str());
  return corpo;
}

}  // namespace

std::string s3_get(const std::string& url) {
  const Objeto o = parse_url(url);
  const Credenciais c = credenciais();
  return exec(c, "GET", o, "");
}

void s3_put(const std::string& url, const std::string& body) {
  const Objeto o = parse_url(url);
  const Credenciais c = credenciais();
  exec(c, "PUT", o, body);
}

}  // namespace tilt::rt
