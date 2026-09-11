#pragma once

#include <string>

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

// Executa um comando SQL sem resultado (INSERT/UPDATE/DELETE/DDL). Aceita
// um unico comando por chamada. Lanca std::runtime_error com a mensagem do
// servidor em qualquer falha.
void mysql_exec(const std::string& url, const std::string& sql);

}  // namespace tilt::rt
