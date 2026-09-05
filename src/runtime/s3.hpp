#pragma once

#include <string>

namespace tilt::rt {

// Cliente S3 minimo: GET/PUT de objetos via REST + AWS SigV4 proprio
// (sha256.hpp) e HTTP pelo binario curl (::popen), mesmo padrao do qdrant.cpp.
// 1a passada: somente GET/PUT, query string vazia, payload inteiro em memoria.
//
// `url` tem o formato "s3://bucket/chave" (a chave pode conter '/').
// Configuracao por variaveis de ambiente:
//   AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY (obrigatorias, nao vazias),
//   AWS_SESSION_TOKEN (opcional), AWS_REGION (default "us-east-1") e
//   S3_ENDPOINT (default "https://s3.<region>.amazonaws.com"; use
//   "http://host:porta" para S3-compativel, ex.: MinIO).
std::string s3_get(const std::string& url);
void s3_put(const std::string& url, const std::string& body);

}  // namespace tilt::rt
