#pragma once

#include <string>
#include <vector>

namespace tilt::rt {

// Cliente S3 minimo: GET/PUT/LIST/DELETE de objetos via REST + AWS SigV4
// proprio (sha256.hpp) e HTTP pelo binario curl (::popen), mesmo padrao do
// qdrant.cpp. Query string suportada na assinatura (list-type/prefix/max-keys
// do LIST); payload inteiro em memoria.
//
// `url` tem o formato "s3://bucket/chave" (a chave pode conter '/');
// `bucket_url` e "s3://bucket" (sem chave).
// Configuracao por variaveis de ambiente:
//   AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY (obrigatorias, nao vazias),
//   AWS_SESSION_TOKEN (opcional), AWS_REGION (default "us-east-1") e
//   S3_ENDPOINT (default "https://s3.<region>.amazonaws.com"; use
//   "http://host:porta" para S3-compativel, ex.: MinIO).
std::string s3_get(const std::string& url);
void s3_put(const std::string& url, const std::string& body);
// Lista as chaves do bucket com ListObjectsV2 ({list-type: 2, prefix, max-keys}).
std::vector<std::string> s3_list(const std::string& bucket_url,
                                 const std::string& prefixo, int max);
// DELETE da chave; 204/200 ok, 404 -> "s3: objeto nao encontrado: <chave>".
void s3_delete(const std::string& url);

}  // namespace tilt::rt
