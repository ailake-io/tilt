#pragma once

#include <string>
#include <utility>
#include <vector>

#include "runtime/sql_params.hpp"
#include "runtime/value.hpp"

namespace tilt::rt {

// Conector SQLite via dlopen("libsqlite3.so.0") — zero dependencias de link.
// Aceita apenas consultas que retornam linhas (SELECT/PRAGMA/WITH); INSERT,
// UPDATE, DDL etc. falham com mensagem clara. Mapeamento de tipos:
// INTEGER -> inteiro, FLOAT -> decimal, TEXT -> texto, NULL -> nulo,
// BLOB -> texto com bytes em hexadecimal (ex.: "0xDEADBEEF").
// Retorna uma tabela (lista de mapas; chaves = nomes de coluna).
//
// Lanca std::runtime_error com mensagem acionavel em qualquer falha.
Value sqlite_query(const std::string& db_path, const std::string& sql);

// Idem, com `?` posicionais ligados por tipo (SELECT com params).
Value sqlite_query_params(const std::string& db_path, const std::string& sql,
                          const std::vector<SqlParam>& params);

// Executa um comando SQL sem resultado (INSERT/UPDATE/DELETE/DDL). Cria o
// arquivo do banco quando nao existe. Aceita um unico comando por chamada.
// Lanca std::runtime_error com a mensagem do SQLite em qualquer falha.
void sqlite_exec(const std::string& db_path, const std::string& sql);

// Idem, com `?` posicionais ligados por tipo (Marco 3 / D1). A contagem tem
// que bater (erro claro); tipos: inteiro, decimal, texto, logico, nulo.
void sqlite_exec_params(const std::string& db_path, const std::string& sql,
                        const std::vector<SqlParam>& params);

// Transacao numa unica conexao (Marco 3 / D1): BEGIN, passos em ordem,
// COMMIT; qualquer falha faz ROLLBACK e relanca com o indice do passo
// ("passo N: ..."). Lista vazia = BEGIN+COMMIT imediatos.
void sqlite_transact(const std::string& db_path,
                     const std::vector<std::pair<std::string, std::vector<SqlParam>>>& passos);

// SQL sobre tabelas tilt: abre um banco em memoria, carrega cada `(nome, tabela)`
// (colunas = chaves; INTEGER/REAL/TEXT inferidos das linhas; logico vira 0/1;
// lista/mapa vira texto JSON) e roda `sql` (com `?` ligados a `params`). Devolve
// uma tabela. Lanca std::runtime_error.
Value sqlite_consulta_tabelas(const std::string& sql,
                              const std::vector<std::pair<std::string, Value>>& tabelas,
                              const std::vector<SqlParam>& params);

}  // namespace tilt::rt
