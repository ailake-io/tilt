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
// - particoes hive-style: com `part_cols` nao vazio, os parquet sao
//   gravados em <dir>/<c1>=<v1>/<c2>=<v2>/part-NNNNN.parquet SEM as colunas
//   de particao (padrao Delta), o add carrega partitionValues com todas as
//   colunas e o metaData registra partitionColumns na mesma ordem. Nulo ou
//   '/' no valor da particao -> erro claro (sem __HIVE_DEFAULT_PARTITION__
//   nem escaping). O append herda as colunas da tabela existente (ordem
//   importa); `part_cols` explicito e divergente -> erro. A leitura reidrata
//   as colunas de particao a partir de partitionValues, convertendo para o
//   tipo declarado no schemaString (falha de conversao mantem texto);
// - pruning (fase 26): delta_read aceita `onde` (mapa coluna -> valor).
//   Predicados em colunas de particao podam arquivos inteiros pelo log
//   (partitionValues); predicados em outras colunas viram filtro de linha
//   aplicado apos a reidratacao. Igualdade apenas.
// - evolucao de schema (fase 27): o append aceita colunas a mais que o
//   schema atual (todas as colunas antigas presentes, ordem livre, resolucao
//   por nome). Coluna nova entra como nullable no fim do schemaString e o
//   commit carrega um metaData novo; arquivos antigos ficam sem a coluna e a
//   leitura (union-by-name) projeta nulo nas linhas deles. Remover coluna ou
//   mudar o tipo de uma existente -> erro claro ("evolucao de schema
//   suporta apenas adicao de colunas").
// - leitura aplica o log em ordem de versao (add/remove) e concatena os
//   parquet listados. Sem transacoes concorrentes.
void delta_write(const std::string& dir, const Value& tabela,
                 const std::vector<std::string>& part_cols = {});
void delta_append(const std::string& dir, const Value& tabela,
                  const std::vector<std::string>& part_cols_req = {});
// `onde` (opcional): mapa coluna -> valor. Igualdade; colunas de particao
// podam arquivos, o resto filtra linhas. Resultado pode ser tabela vazia.
Value delta_read(const std::string& dir, const Value* onde = nullptr);

}  // namespace tilt::rt
