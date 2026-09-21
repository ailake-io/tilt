#pragma once

#include <string>
#include <utility>
#include <vector>

#include "runtime/sql_params.hpp"
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

// Idem, com `?` posicionais ligados por tipo via prepared statements
// (SELECT com params).
Value duckdb_query_params(const std::string& db_path, const std::string& sql,
                          const std::vector<SqlParam>& params);

// Executa um comando SQL sem resultado (INSERT/UPDATE/DELETE/DDL). Cria o
// arquivo do banco quando nao existe. Aceita um unico comando por chamada.
// Lanca std::runtime_error com a mensagem do DuckDB em qualquer falha.
void duckdb_exec(const std::string& db_path, const std::string& sql);

// Idem, com `?` posicionais ligados por tipo via prepared statements
// (Marco 3 / D1).
void duckdb_exec_params(const std::string& db_path, const std::string& sql,
                        const std::vector<SqlParam>& params);

// Transacao numa unica conexao (Marco 3 / D1): BEGIN, passos, COMMIT;
// falha faz ROLLBACK e relanca com o indice do passo.
void duckdb_transact(const std::string& db_path,
                     const std::vector<std::pair<std::string, std::vector<SqlParam>>>& passos);

// SQL sobre tabelas tilt no DuckDB (banco em memoria; carga por appender). Igual a
// sqlite_consulta_tabelas, mas o SQL tambem pode ler arquivos direto
// (`select ... from 'vendas.parquet'`, `read_csv_auto('x.csv')`).
Value duckdb_consulta_tabelas(const std::string& sql,
                              const std::vector<std::pair<std::string, Value>>& tabelas,
                              const std::vector<SqlParam>& params);

}  // namespace tilt::rt
