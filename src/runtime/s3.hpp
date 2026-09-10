#pragma once

#include <string>
#include <utility>
#include <vector>

namespace tilt::rt {

// Cliente S3 minimo: GET/PUT/LIST/DELETE de objetos via REST + AWS SigV4
// proprio (sha256.hpp) e HTTP pelo binario curl (tilt_popen; _popen no
// Windows, mesmo padrao do qdrant.cpp). Query string suportada na assinatura
// (list-type/prefix/max-keys do LIST, uploadId/partNumber do multipart);
// payload inteiro em memoria. Cobre tambem CopyObject, HeadObject e upload
// multipart (iniciar/parte/concluir/abortar).
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
// CopyObject: PUT no destino com x-amz-copy-source assinado; o bucket da
// origem pode diferir do destino.
void s3_copiar(const std::string& url_origem, const std::string& url_destino);
// HeadObject: devolve os metadados do objeto como pares (nome em caixa
// baixa): content-length, content-type, etag, last-modified e x-amz-meta-*;
// 404 -> "s3: objeto nao encontrado: <chave>".
std::vector<std::pair<std::string, std::string>> s3_cabecalho(
    const std::string& url);
// Multipart upload: iniciar devolve o uploadId (POST ?uploads); parte faz o
// PUT de uma parte numerada (1..10000) e devolve o ETag da parte; concluir
// envia o CompleteMultipartUpload (partes nao vazias, em ordem crescente, sem
// duplicatas) e devolve o ETag final; abortar descarta as partes.
std::string s3_multipart_iniciar(const std::string& url);
std::string s3_multipart_parte(const std::string& url,
                               const std::string& upload_id, int numero,
                               const std::string& dados);
std::string s3_multipart_concluir(
    const std::string& url, const std::string& upload_id,
    const std::vector<std::pair<int, std::string>>& partes);
void s3_multipart_abortar(const std::string& url, const std::string& upload_id);

}  // namespace tilt::rt
