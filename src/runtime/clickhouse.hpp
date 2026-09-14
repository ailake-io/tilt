#pragma once

#include <string>
#include <utility>
#include <vector>

#include "runtime/sql_params.hpp"
#include "runtime/value.hpp"

namespace tilt::rt {

// Conector ClickHouse via HTTP nativo, sobre o cliente generico do runtime
// (http_request): subprocesso `curl`, zero dependencias de link. `url` segue
// o formato "clickhouse://[usuario[:senha]@]host[:porta][/banco]" (HTTP;
// porta default 8123; usuario default "default"). Sem userinfo na URL a
// autenticacao vem das env CLICKHOUSE_USER/CLICKHOUSE_PASSWORD (senha vazia
// quando ambas ausentes). A resposta e pedida em FORMAT JSONEachRow (anexado
// automaticamente quando o SQL nao traz um FORMAT proprio): cada linha
// NDJSON vira um mapa da tabela. Mapeamento de valores: Int*/UInt* ->
// inteiro, Float*/Decimal -> decimal, String/FixedString/Date/DateTime ->
// texto, Nullable -> nulo (JSON null ou o marcador "ᴺᵁᴸᴸ" de FORMATs
// TSV/CSV), demais tipos (Array/Tuple/Map/JSON aninhados) -> texto
// (serializacao JSON). Timeout de 60s (consultas analiticas).
//
// Lanca std::runtime_error com mensagem acionavel em qualquer falha:
// ClickHouse devolve HTTP >= 400 com o erro em texto puro no corpo
// ("clickhouse: <corpo truncado>").
Value clickhouse_query(const std::string& url, const std::string& sql);

// Idem, com `?` ligados como `{pN:Tipo}` (SELECT com params).
Value clickhouse_query_params(const std::string& url, const std::string& sql,
                              const std::vector<SqlParam>& params);

// Executa um comando SQL sem resultado (INSERT/DDL/ALTER...). Mesmo POST
// HTTP, sem FORMAT anexado; o corpo da resposta e ignorado. Erros como acima.
void clickhouse_exec(const std::string& url, const std::string& sql);

// Idem, com `?` ligados como query params `{pN:Tipo}` (Marco 3 / D1):
// inteiro->Int64, decimal->Float64, texto->String, logico->UInt8,
// nulo->NULL inline.
void clickhouse_exec_params(const std::string& url, const std::string& sql,
                            const std::vector<SqlParam>& params);

// Transacoes multi-comando nao existem no ClickHouse via HTTP: erro claro.
void clickhouse_transact(
    const std::string& url,
    const std::vector<std::pair<std::string, std::vector<SqlParam>>>& passos);

}  // namespace tilt::rt
