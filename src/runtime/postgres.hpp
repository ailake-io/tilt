#pragma once

#include <string>
#include <utility>
#include <vector>

#include "runtime/sql_params.hpp"
#include "runtime/value.hpp"

namespace tilt::rt {

// Conector Postgres via dlopen("libpq.so.5") — zero dependencias de link.
// `url` e uma connection string libpq (ex.: "host=localhost port=5432
// dbname=teste user=thiago"). Aceita apenas consultas SELECT (PGRES_TUPLES_OK);
// outros comandos falham com mensagem clara. Mapeamento de tipos por OID:
// bool(16) -> logico, int8/int2/int4(20/21/23) -> inteiro,
// float4/float8(700/701) -> decimal, numeric(1700) -> decimal via strtod,
// demais OIDs -> texto; SQL NULL -> nulo.
// Retorna uma tabela (lista de mapas; chaves = nomes de coluna).
//
// Lanca std::runtime_error com mensagem acionavel em qualquer falha.
Value postgres_query(const std::string& url, const std::string& sql);

// Executa SQL que nao retorna linhas (DDL/DML: CREATE/INSERT/UPDATE...).
// Nao verifica o tipo de comando; erro do servidor vira excecao com a
// mensagem do Postgres. Para SELECT use postgres_query().
void postgres_exec(const std::string& url, const std::string& sql);

// Idem, com `?` posicionais ligados em texto via PQexecParams (Marco 3 / D1).
void postgres_exec_params(const std::string& url, const std::string& sql,
                          const std::vector<SqlParam>& params);

// Transacao numa unica conexao (Marco 3 / D1): BEGIN, passos, COMMIT;
// falha faz ROLLBACK e relanca com o indice do passo.
void postgres_transact(const std::string& url,
                       const std::vector<std::pair<std::string, std::vector<SqlParam>>>& passos);

}  // namespace tilt::rt
