#include "runtime/s3.hpp"

#include <algorithm>
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

#include "runtime/compat.hpp"
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

// Aceita "s3://bucket", "s3://bucket/" ou "s3://bucket/chave" (a chave pode
// conter '/'); chave vazia = operacao no bucket (ex.: LIST).
Objeto parse_url(const std::string& url) {
  const std::string prefix = "s3://";
  if (url.rfind(prefix, 0) != 0) {
    die("esperado 's3://bucket/chave'");
  }
  const std::string rest = url.substr(prefix.size());
  const std::size_t slash = rest.find('/');
  if (slash == 0) {
    die("esperado 's3://bucket/chave'");
  }
  Objeto o;
  o.bucket = slash == std::string::npos ? rest : rest.substr(0, slash);
  o.chave = slash == std::string::npos ? "" : rest.substr(slash + 1);
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

bool unreserved(unsigned char c) {
  return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

// Codifica cada segmento (entre '/') preservando os caracteres unreserved do
// SigV4 (RFC 3986): letras, digitos, '-', '_', '.', '~'. Espaco vira %20 etc.
std::string uri_encode_segmentos(const std::string& path) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : path) {
    if (c == '/') {
      out += '/';
    } else if (unreserved(c)) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[(c >> 4) & 0x0F];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

// URI-encoding integral (RFC 3986 unreserved, sem preservar '/') para chaves e
// valores da query string — usado tanto no CanonicalRequest quanto na URL.
std::string uri_encode(const std::string& s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (unreserved(c)) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[(c >> 4) & 0x0F];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

// CanonicalQueryString do SigV4: pares ordenados lexicograficamente por chave
// encoded, "chave=valor" (ambos encoded), separados por '&'; valor vazio vira
// so a chave encoded.
std::string canonical_query(
    const std::vector<std::pair<std::string, std::string>>& query) {
  std::vector<std::pair<std::string, std::string>> enc;
  enc.reserve(query.size());
  for (const auto& [k, v] : query) {
    enc.emplace_back(uri_encode(k), uri_encode(v));
  }
  std::sort(enc.begin(), enc.end());
  std::string out;
  for (std::size_t i = 0; i < enc.size(); ++i) {
    if (i) out += '&';
    out += enc[i].first;
    if (!enc[i].second.empty()) out += "=" + enc[i].second;
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

// AWS SigV4 (service "s3"), com CanonicalQueryString generica.
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
                   const std::string& canonical_uri, const std::string& query,
                   const std::string& host, const std::string& payload_hash) {
  const std::time_t agora = std::time(nullptr);
  const std::tm tm_utc = tilt_gmtime(agora);
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

  const std::string canonical_request = method + "\n" + canonical_uri + "\n" +
                                        query + "\n" + canonical_headers + "\n" +
                                        signed_headers + "\n" + payload_hash;

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
// (`-w '%{http_code}'`). Com falhar=true (default), HTTP >= 400 (ou falha de
// transporte) -> die com o corpo da resposta truncado em ~200 chars. Retorna
// o status HTTP.
int http(const std::string& method, const std::string& url,
         const std::vector<std::pair<std::string, std::string>>& headers,
         const std::string& body, const std::string& out_file, bool falhar) {
  std::string body_file;
  std::string cmd = "curl -s ";
  if (falhar) cmd += "--fail-with-body ";
  cmd += "-o " + shell_quote(out_file) + " -w '%{http_code}' -X " + method;
  for (const auto& [nome, valor] : headers) {
    cmd += " -H " + shell_quote(nome + ": " + valor);
  }
  if (method == "PUT") {
    std::string body_path;
    const int fd = tilt_tempfile("s3_body", body_path);
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
      std::ifstream in(out_file);
      std::string corpo((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      die("requisicao falhou (curl codigo " + std::to_string(rc) +
          "): verifique endpoint/bucket/credenciais. Resposta: " + corpo.substr(0, 200));
    }
  }

  int status = 0;
  std::istringstream iss(resp);
  iss >> status;
  if (falhar && status >= 400) {
    std::ifstream in(out_file);
    std::string corpo((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    die("HTTP " + std::to_string(status) + ": " + corpo.substr(0, 200));
  }
  return status;
}

}  // namespace

// Nucleo comum: monta URI + query canonical, assina SigV4 e executa o HTTP.
// Retorna (status, corpo). Com falhar=false nao die em HTTP >= 400 (o caller
// inspeciona o status, ex.: DELETE 404 -> "objeto nao encontrado").
std::pair<int, std::string> s3_request(
    const std::string& method, const std::string& bucket, const std::string& key,
    const std::vector<std::pair<std::string, std::string>>& query_map,
    const std::string& body, bool falhar = true) {
  const Credenciais c = credenciais();
  const std::string host = endpoint_host(c.endpoint);
  const std::string canonical_uri =
      key.empty() ? "/" + bucket : "/" + bucket + "/" + uri_encode_segmentos(key);
  const std::string query = canonical_query(query_map);
  std::string url = endpoint_base(c.endpoint) + canonical_uri;
  if (!query.empty()) url += "?" + query;
  const std::string payload_hash = sha256_hex(body);

  const Assinatura a = assinar(c, method, canonical_uri, query, host, payload_hash);
  std::vector<std::pair<std::string, std::string>> headers = {
      {"Host", host},
      {"X-Amz-Content-Sha256", payload_hash},
      {"X-Amz-Date", a.amz_date},
      {"Authorization", a.authorization},
  };
  if (!c.token.empty()) headers.emplace_back("X-Amz-Security-Token", c.token);
  if (method == "PUT") headers.emplace_back("Content-Type", "application/octet-stream");

  std::string out_file;
  const int fd = tilt_tempfile("s3_resp", out_file);
  if (fd < 0) die("nao foi possivel criar arquivo temporario");
  tilt_close_file(fd);
  const int status = http(method, url, headers, body, out_file, falhar);

  std::ifstream in(out_file, std::ios::binary);
  std::string corpo((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::remove(out_file.c_str());
  return {status, corpo};
}

namespace {

// Decodifica as entidades XML que o S3 escapa em <Key>: &amp; &lt; &gt;
// &quot; &#39;.
std::string decodificar_xml(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '&') {
      if (s.compare(i, 5, "&amp;") == 0) {
        out += '&';
        i += 5;
      } else if (s.compare(i, 4, "&lt;") == 0) {
        out += '<';
        i += 4;
      } else if (s.compare(i, 4, "&gt;") == 0) {
        out += '>';
        i += 4;
      } else if (s.compare(i, 6, "&quot;") == 0) {
        out += '"';
        i += 6;
      } else if (s.compare(i, 5, "&#39;") == 0) {
        out += '\'';
        i += 5;
      } else {
        out += s[i];
        ++i;
      }
    } else {
      out += s[i];
      ++i;
    }
  }
  return out;
}

}  // namespace

std::string s3_get(const std::string& url) {
  const Objeto o = parse_url(url);
  if (o.chave.empty()) die("esperado 's3://bucket/chave'");
  return s3_request("GET", o.bucket, o.chave, {}, "").second;
}

void s3_put(const std::string& url, const std::string& body) {
  const Objeto o = parse_url(url);
  if (o.chave.empty()) die("esperado 's3://bucket/chave'");
  s3_request("PUT", o.bucket, o.chave, {}, body);
}

std::vector<std::string> s3_list(const std::string& bucket_url,
                                 const std::string& prefixo, int max) {
  const Objeto o = parse_url(bucket_url);
  if (max <= 0) die("max deve ser positivo");
  const auto r = s3_request("GET", o.bucket, "",
                            {{"list-type", "2"},
                             {"prefix", prefixo},
                             {"max-keys", std::to_string(max)}},
                            "");
  std::vector<std::string> chaves;
  const std::string& xml = r.second;
  std::size_t pos = 0;
  while ((pos = xml.find("<Key>", pos)) != std::string::npos) {
    pos += 5;
    const std::size_t fim = xml.find("</Key>", pos);
    if (fim == std::string::npos) break;
    chaves.push_back(decodificar_xml(xml.substr(pos, fim - pos)));
    pos = fim + 6;
  }
  return chaves;
}

void s3_delete(const std::string& url) {
  const Objeto o = parse_url(url);
  if (o.chave.empty()) die("esperado 's3://bucket/chave'");
  const int status = s3_request("DELETE", o.bucket, o.chave, {}, "", false).first;
  if (status == 404) die("objeto nao encontrado: " + o.chave);
  if (status != 204 && status != 200) {
    die("delete falhou com HTTP " + std::to_string(status));
  }
}

}  // namespace tilt::rt
