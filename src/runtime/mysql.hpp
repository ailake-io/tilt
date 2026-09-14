#pragma once

#include <string>
#include <utility>
#include <vector>

#include "runtime/sql_params.hpp"
#include "runtime/value.hpp"

namespace tilt::rt {

// Conector MySQL/MariaDB via dlopen("libmariadb.so.3" / "libmysqlclient.so*")
// — zero dependencias de link. `url` segue o formato
// "mysql://usuario:senha@host:porta/banco" (porta default 3306; o esquema
// "mariadb://" tambem e aceito). Aceita apenas consultas que retornam
// linhas (SELECT/WITH/SHOW/DESCRIBE/EXPLAIN); INSERT, UPDATE, DDL etc.
// falham com mensagem clara. Mapeamento de tipos (enum_field_types):
// TINYINT/SMALLINT/INT/MEDIUMINT/BIGINT/YEAR -> inteiro,
// DECIMAL/NEWDECIMAL/FLOAT/DOUBLE -> decimal, demais tipos -> texto,
// SQL NULL -> nulo. Retorna uma tabela (lista de mapas; chaves = nomes de
// coluna).
//
// Lanca std::runtime_error com mensagem acionavel em qualquer falha.
Value mysql_query(const std::string& url, const std::string& sql);

// Idem, com `?` interpolados apos escape pela conexao (SELECT com params).
Value mysql_query_params(const std::string& url, const std::string& sql,
                         const std::vector<SqlParam>& params);

// Executa um comando SQL sem resultado (INSERT/UPDATE/DELETE/DDL). Aceita
// um unico comando por chamada. Lanca std::runtime_error com a mensagem do
// servidor em qualquer falha.
void mysql_exec(const std::string& url, const std::string& sql);

// Idem, com `?` interpolados apos escape pela conexao (Marco 3 / D1):
// texto com mysql_real_escape_string (charset da conexao), numeros crus,
// Nulo como NULL. Prepared server-side (mysql_stmt_*) fica para quando
// houver cobertura com servidor (o layout de MYSQL_BIND difere entre
// MySQL/MariaDB).
void mysql_exec_params(const std::string& url, const std::string& sql,
                       const std::vector<SqlParam>& params);

// Transacao numa unica conexao (Marco 3 / D1): START TRANSACTION, passos,
// COMMIT; falha faz ROLLBACK e relanca com o indice do passo.
void mysql_transact(const std::string& url,
                    const std::vector<std::pair<std::string, std::vector<SqlParam>>>& passos);

}  // namespace tilt::rt
