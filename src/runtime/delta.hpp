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
// - particoes hive-style (fase 25): com `part_col` nao vazio, os parquet sao
//   gravados em <dir>/<col>=<valor>/part-NNNNN.parquet SEM a coluna de
//   particao (padrao Delta), o add carrega partitionValues e o metaData
//   registra partitionColumns. Nulo ou '/' no valor da particao -> erro claro
//   (fase 25 nao suporta __HIVE_DEFAULT_PARTITION__ nem escaping). O append
//   herda a particao da tabela existente; `part_col` explicito e divergente
//   -> erro. A leitura reidrata a coluna de particao a partir de
//   partitionValues, convertendo para o tipo declarado no schemaString
//   (falha de conversao mantem texto). Sem filtro por diretorio de particao
//   (le tudo e reidrata — predicados em `onde` ficam para fase futura);
// - leitura aplica o log em ordem de versao (add/remove) e concatena os
//   parquet listados. Sem transacoes concorrentes.
void delta_write(const std::string& dir, const Value& tabela, const std::string& part_col = "");
void delta_append(const std::string& dir, const Value& tabela, const std::string& part_col = "");
Value delta_read(const std::string& dir);

}  // namespace tilt::rt
