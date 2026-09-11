#pragma once

#include <string>

#include "runtime/value.hpp"

namespace tilt::rt {

// Conector Elasticsearch/OpenSearch via REST/JSON puro, sobre o cliente
// generico do runtime (http_request): subprocesso `curl`, zero dependencias
// de link. `url` segue o formato
// "elasticsearch://[usuario[:senha]@]host[:porta][/indice]" (o esquema
// "opensearch://" tambem vale, mesma porta default 9200). Sem userinfo na URL
// a autenticacao vem das env ELASTIC_USER/ELASTIC_PASSWORD; sem nenhuma das
// duas, as requisicoes sao enviadas sem header de autenticacao. Com usuario,
// o header "Authorization: Basic <base64(user:senha)>" e enviado — mesmo com
// senha vazia. Timeout de 30s.
//
// es_query executa POST /<indice>/_search com o corpo DSL (texto JSON passado
// direto; mapa e serializado com json_dump) e devolve um mapa:
//   { total: <inteiro>, hits: <tabela>, agregacoes?: <mapa> }
// Cada linha de hits traz "_id" mais os campos de "_source" achatados um
// nivel (objetos aninhados continuam mapas). "total" vem de hits.total
// (`.value` no ES 7+; numero puro no ES 6). "agregacoes" so aparece quando a
// resposta traz "aggregations". Sem indice na URL, a busca e em todos os
// indices (POST /_search).
//
// es_exec e a escape hatch generica: qualquer metodo (GET/POST/PUT/PATCH/
// DELETE/HEAD) em qualquer caminho — relativo ao indice da URL quando ele
// existe (ex.: URL ".../meuindice" + "/_doc/1" -> /meuindice/_doc/1), senao
// relativo a raiz (ex.: "/meuindice", "/meuindice/_delete_by_query?pretty",
// "/_cat/indices"). Corpo: texto enviado direto; demais valores serializados
// com json_dump; omitido/nulo = sem corpo. A resposta e parseada como JSON;
// corpo vazio -> nulo e corpo nao-JSON -> texto cru (endpoints como _cat
// devolvem TSV).
//
// Ambas lancam std::runtime_error ("elasticsearch: <motivo>") em falha de
// transporte, HTTP >= 400 (o motivo vem de `error.reason` do JSON de erro do
// servidor, com fallback para o corpo truncado) ou resposta invalida.
Value es_query(const std::string& url, const Value& dsl);
Value es_exec(const std::string& url, const std::string& metodo, const std::string& caminho,
              const Value& corpo = Value::nulo());

}  // namespace tilt::rt
