#pragma once

#include <string>

#include "runtime/value.hpp"

namespace tilt::rt {

// Delta Lake de 1a passada sobre diretorio local:
// - escrita cria <dir>/part-*.parquet + <dir>/_delta_log/000...00.json
//   (protocol + metaData + add), sobrescrevendo a tabela existente;
// - append (delta_append) cria uma nova versao no log com commitInfo + add,
//   sem apagar parquet nem log antigos. O commit grava o JSONL num arquivo
//   temporario do mesmo diretorio e fecha + rename() para o nome final
//   (rename atomico no mesmo filesystem); crash antes do rename deixa so um
//   parquet orfao, ignorado pela leitura. Pressupoe um unico escritor —
//   sem locks nem optimistic concurrency (1a passada);
// - leitura aplica o log em ordem de versao (add/remove) e concatena os
//   parquet listados. Sem transacoes concorrentes nem partições.
void delta_write(const std::string& dir, const Value& tabela);
void delta_append(const std::string& dir, const Value& tabela);
Value delta_read(const std::string& dir);

}  // namespace tilt::rt
