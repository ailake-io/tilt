#pragma once

#include <string>

#include "runtime/value.hpp"

namespace tilt::rt {

// Conector DuckDB via dlopen("libduckdb.so") — zero dependencias de link.
// `db_path` e o arquivo do banco (":memory:" abre um banco em memoria).
// Aceita apenas consultas que retornam
// linhas (SELECT/PRAGMA/WITH); INSERT, UPDATE, DDL etc. falham com mensagem
// clara. Mapeamento de tipos: BOOLEAN/TINYINT/SMALLINT/INTEGER/BIGINT ->
// inteiro, FLOAT/DOUBLE -> decimal, VARCHAR e demais -> texto (via
// duckdb_value_varchar), NULL -> nulo.
// Retorna uma tabela (lista de mapas; chaves = nomes de coluna).
//
// Lanca std::runtime_error com mensagem acionavel em qualquer falha.
Value duckdb_query(const std::string& db_path, const std::string& sql);

// Executa um comando SQL sem resultado (INSERT/UPDATE/DELETE/DDL). Cria o
// arquivo do banco quando nao existe. Aceita um unico comando por chamada.
// Lanca std::runtime_error com a mensagem do DuckDB em qualquer falha.
void duckdb_exec(const std::string& db_path, const std::string& sql);

}  // namespace tilt::rt
