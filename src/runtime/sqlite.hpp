#pragma once

#include <string>

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

}  // namespace tilt::rt
