#include "runtime/s3.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "runtime/compat.hpp"
#include "runtime/http_client.hpp"
#include "runtime/sha256.hpp"

namespace tilt::rt {

namespace {

[[noreturn]] void die(const std::string& m) { throw std::runtime_error("s3: " + m); }

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
                   const std::string& host, const std::string& payload_hash,
                   const std::vector<std::pair<std::string, std::string>>& extras) {
  const std::time_t agora = std::time(nullptr);
  const std::tm tm_utc = tilt_gmtime(agora);
  char data_buf[9];
  std::strftime(data_buf, sizeof data_buf, "%Y%m%d", &tm_utc);
  const std::string date_stamp = data_buf;
  char amz_buf[17];
  std::strftime(amz_buf, sizeof amz_buf, "%Y%m%dT%H%M%SZ", &tm_utc);
  const std::string amz_date = amz_buf;

  const std::string scope = date_stamp + "/" + c.region + "/s3/aws4_request";

  // Headers assinados em ordem lexicografica: host, x-amz-content-sha256,
  // x-amz-date, extras (ex.: x-amz-copy-source) e x-amz-security-token.
  std::vector<std::pair<std::string, std::string>> canon = {
      {"host", host},
      {"x-amz-content-sha256", payload_hash},
      {"x-amz-date", amz_date},
  };
  canon.insert(canon.end(), extras.begin(), extras.end());
  if (!c.token.empty()) canon.emplace_back("x-amz-security-token", c.token);
  std::sort(canon.begin(), canon.end());

  std::string canonical_headers;
  std::string signed_headers;
  for (std::size_t i = 0; i < canon.size(); ++i) {
    canonical_headers += canon[i].first + ":" + canon[i].second + "\n";
    if (i) signed_headers += ";";
    signed_headers += canon[i].first;
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

}  // namespace

// Nucleo comum: monta URI + query canonical, assina SigV4 e executa o HTTP
// via cliente generico do runtime (http_client). Retorna (status, corpo). Com
// falhar=false nao die em HTTP >= 400 (o caller inspeciona o status, ex.:
// DELETE 404 -> "objeto nao encontrado"). `extra_headers` entra assinada
// (nome em caixa baixa) e e enviada como "X-Amz-..." (ex.: x-amz-copy-source);
// `resp_headers`, quando dado, recebe os headers da resposta em caixa baixa
// (ex.: etag do UploadPart).
std::pair<int, std::string> s3_request(
    const std::string& method, const std::string& bucket, const std::string& key,
    const std::vector<std::pair<std::string, std::string>>& query_map,
    const std::string& body, bool falhar = true,
    const std::vector<std::pair<std::string, std::string>>& extra_headers = {},
    std::vector<std::pair<std::string, std::string>>* resp_headers = nullptr) {
  const Credenciais c = credenciais();
  const std::string host = endpoint_host(c.endpoint);
  const std::string canonical_uri =
      key.empty() ? "/" + bucket : "/" + bucket + "/" + uri_encode_segmentos(key);
  const std::string query = canonical_query(query_map);
  std::string url = endpoint_base(c.endpoint) + canonical_uri;
  if (!query.empty()) url += "?" + query;
  const std::string payload_hash = sha256_hex(body);

  const Assinatura a =
      assinar(c, method, canonical_uri, query, host, payload_hash, extra_headers);
  std::vector<std::pair<std::string, std::string>> headers = {
      {"Host", host},
      {"X-Amz-Content-Sha256", payload_hash},
      {"X-Amz-Date", a.amz_date},
      {"Authorization", a.authorization},
  };
  if (!c.token.empty()) headers.emplace_back("X-Amz-Security-Token", c.token);
  for (const auto& [nome, valor] : extra_headers) {
    std::string titulo = nome;
    titulo[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(titulo[0])));
    headers.emplace_back(titulo, valor);
  }
  if (method == "PUT" || method == "POST") {
    headers.emplace_back("Content-Type", "application/octet-stream");
  }

  // timeout 0 = sem --max-time (mesma linha de comando de antes da extracao
  // do http_client); falhar espelha o antigo --fail-with-body opcional.
  const HttpClientResponse r = http_request(method, url, headers, body, 0, falhar, resp_headers);
  if (!r.error.empty()) {
    die(r.error + ": verifique endpoint/bucket/credenciais. Resposta: " +
        r.body.substr(0, 200));
  }
  if (falhar && r.status >= 400) {
    die("HTTP " + std::to_string(r.status) + ": " + r.body.substr(0, 200));
  }
  return {r.status, r.body};
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

void s3_copiar(const std::string& url_origem, const std::string& url_destino) {
  const Objeto origem = parse_url(url_origem);
  if (origem.chave.empty()) die("esperado 's3://bucket/chave' na origem");
  const Objeto destino = parse_url(url_destino);
  if (destino.chave.empty()) die("esperado 's3://bucket/chave' no destino");
  // x-amz-copy-source: "/bucket/chave" com a chave URI-encoded na integra.
  const std::string fonte =
      "/" + origem.bucket + "/" + uri_encode(origem.chave);
  s3_request("PUT", destino.bucket, destino.chave, {}, "", true,
             {{"x-amz-copy-source", fonte}});
}

std::vector<std::pair<std::string, std::string>> s3_cabecalho(
    const std::string& url) {
  const Objeto o = parse_url(url);
  if (o.chave.empty()) die("esperado 's3://bucket/chave'");
  std::vector<std::pair<std::string, std::string>> headers;
  // HEAD: falhar=false porque o tratamento de status e proprio (404 claro) e
  // nao ha corpo de erro para anexar a mensagem.
  const int status =
      s3_request("HEAD", o.bucket, o.chave, {}, "", false, {}, &headers).first;
  if (status == 404) die("objeto nao encontrado: " + o.chave);
  if (status != 200) die("head falhou com HTTP " + std::to_string(status));
  std::vector<std::pair<std::string, std::string>> meta;
  for (const auto& [nome, valor] : headers) {
    if (nome == "content-length" || nome == "content-type" || nome == "etag" ||
        nome == "last-modified" || nome.rfind("x-amz-meta-", 0) == 0) {
      meta.emplace_back(nome, valor);
    }
  }
  return meta;
}

std::string s3_multipart_iniciar(const std::string& url) {
  const Objeto o = parse_url(url);
  if (o.chave.empty()) die("esperado 's3://bucket/chave'");
  // Query "uploads" sem valor: POST ?uploads.
  const std::string xml =
      s3_request("POST", o.bucket, o.chave, {{"uploads", ""}}, "").second;
  const std::size_t b = xml.find("<UploadId>");
  const std::size_t e = xml.find("</UploadId>");
  if (b == std::string::npos || e == std::string::npos || e < b) {
    die("resposta sem <UploadId>: " + xml.substr(0, 200));
  }
  return xml.substr(b + 10, e - b - 10);
}

std::string s3_multipart_parte(const std::string& url,
                               const std::string& upload_id, int numero,
                               const std::string& dados) {
  const Objeto o = parse_url(url);
  if (o.chave.empty()) die("esperado 's3://bucket/chave'");
  if (numero < 1 || numero > 10000) {
    die("numero da parte deve estar entre 1 e 10000 (recebido " +
        std::to_string(numero) + ")");
  }
  std::vector<std::pair<std::string, std::string>> headers;
  s3_request("PUT", o.bucket, o.chave,
             {{"partNumber", std::to_string(numero)}, {"uploadId", upload_id}},
             dados, true, {}, &headers);
  for (const auto& [nome, valor] : headers) {
    if (nome == "etag") return valor;
  }
  die("resposta sem ETag da parte");
}

std::string s3_multipart_concluir(
    const std::string& url, const std::string& upload_id,
    const std::vector<std::pair<int, std::string>>& partes) {
  const Objeto o = parse_url(url);
  if (o.chave.empty()) die("esperado 's3://bucket/chave'");
  if (partes.empty()) die("lista de partes nao pode ser vazia");
  std::string xml = "<CompleteMultipartUpload>";
  int anterior = 0;
  for (const auto& [numero, etag] : partes) {
    if (numero < 1 || numero > 10000) {
      die("numero da parte deve estar entre 1 e 10000 (recebido " +
          std::to_string(numero) + ")");
    }
    if (numero <= anterior) {
      die("partes devem estar em ordem crescente, sem duplicatas (recebida " +
          std::to_string(numero) + " apos " + std::to_string(anterior) + ")");
    }
    anterior = numero;
    xml += "<Part><PartNumber>" + std::to_string(numero) +
           "</PartNumber><ETag>" + etag + "</ETag></Part>";
  }
  xml += "</CompleteMultipartUpload>";
  const std::string corpo =
      s3_request("POST", o.bucket, o.chave, {{"uploadId", upload_id}}, xml).second;
  const std::size_t b = corpo.find("<ETag>");
  const std::size_t e = corpo.find("</ETag>");
  if (b == std::string::npos || e == std::string::npos || e < b) {
    die("resposta sem <ETag>: " + corpo.substr(0, 200));
  }
  return corpo.substr(b + 6, e - b - 6);
}

void s3_multipart_abortar(const std::string& url, const std::string& upload_id) {
  const Objeto o = parse_url(url);
  if (o.chave.empty()) die("esperado 's3://bucket/chave'");
  const int status = s3_request("DELETE", o.bucket, o.chave,
                                {{"uploadId", upload_id}}, "", false)
                         .first;
  if (status != 204 && status != 200) {
    die("abort falhou com HTTP " + std::to_string(status));
  }
}

}  // namespace tilt::rt
