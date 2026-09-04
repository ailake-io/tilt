#pragma once

#include <string>

#include "runtime/value.hpp"

namespace tilt::rt {

// Delta Lake de 1a passada sobre diretorio local:
// - escrita cria <dir>/part-*.parquet + <dir>/_delta_log/000...00.json
//   (protocol + metaData + add), sobrescrevendo a tabela existente;
// - leitura aplica o log em ordem de versao (add/remove) e concatena os
//   parquet listados. Sem transacoes concorrentes nem partições.
void delta_write(const std::string& dir, const Value& tabela);
Value delta_read(const std::string& dir);

}  // namespace tilt::rt
