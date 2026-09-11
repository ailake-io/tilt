#pragma once

#include <string>
#include <utility>
#include <vector>

#include "runtime/value.hpp"

namespace tilt::rt {

// Cliente HTTP generico do runtime. Mesmo padrao dos conectores
// (s3/llm/qdrant/iceberg): subprocesso `curl`, sem sockets proprios. O corpo
// da resposta vai para arquivo temporario (-o), o status vem no stdout via
// -w '%{http_code}' e, quando solicitado, os headers da resposta para outro
// arquivo temporario (-D), no formato "Nome: valor" por linha.

struct HttpClientResponse {
  int status = 0;      // codigo HTTP; 0 = falha de transporte/execucao
  std::string body;    // corpo da resposta (em >= 400, o corpo do erro)
  std::string error;   // vazio = ok; senao, falha de transporte/execucao do curl
};

// `metodo`: GET/POST/PUT/PATCH/DELETE (HEAD sai como `curl -I`). O corpo e
// enviado via arquivo temporario (--data @arquivo) nos metodos com payload
// (PUT/POST/PATCH). `timeout_s` <= 0 desliga --max-time. `falhar` adiciona
// --fail-with-body (HTTP >= 400 vira error — comportamento historico do s3 e
// do qdrant). `resp_headers`, quando dado, recebe os headers da resposta com
// nomes em caixa baixa (ex.: etag do S3).
HttpClientResponse http_request(
    const std::string& metodo, const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers = {},
    const std::string& body = "", int timeout_s = 30, bool falhar = false,
    std::vector<std::pair<std::string, std::string>>* resp_headers = nullptr);

// GET/POST JSON de alto nivel sobre http_request. Lancam std::runtime_error
// ("http: ...") em falha de transporte, status >= 400 (com o corpo truncado
// em ~200 chars) ou corpo invalido como JSON. A URL deve comecar com
// "http://" ou "https://". http_post_json serializa o corpo com json_dump e
// envia Content-Type: application/json (salvo se o chamador ja passou um
// Content-Type nos headers); a resposta e parseada com json_parse.
Value http_get_json(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers = {},
    int timeout_s = 30);
Value http_post_json(
    const std::string& url, const Value& corpo,
    const std::vector<std::pair<std::string, std::string>>& headers = {},
    int timeout_s = 30);

}  // namespace tilt::rt
