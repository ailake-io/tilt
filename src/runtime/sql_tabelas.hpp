// SQL sobre tabelas tilt (`sql "select ..."` e `tabela.sql "..."`).
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "runtime/sql_params.hpp"
#include "runtime/value.hpp"

namespace tilt::rt {

// Roda `sql` sobre `tabelas` (nome, tabela) e devolve uma tabela. `motor`:
// "sqlite" (padrao; banco em memoria) ou "auto". Lanca std::runtime_error com
// mensagem acionavel (motor desconhecido, tabela vazia, SQL invalido...).
Value sql_tabelas(const std::string& motor, const std::string& sql,
                  const std::vector<std::pair<std::string, Value>>& tabelas,
                  const std::vector<SqlParam>& params);

}  // namespace tilt::rt
