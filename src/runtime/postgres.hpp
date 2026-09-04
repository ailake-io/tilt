#pragma once

#include <string>

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

}  // namespace tilt::rt
