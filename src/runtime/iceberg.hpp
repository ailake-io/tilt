#pragma once

#include <string>

#include "runtime/value.hpp"

namespace tilt::rt {

// Iceberg de 1a passada, catalogo tipo Hadoop sobre diretorio local:
// - escrita cria <dir>/data/<uuid>.parquet (tabela sem particao) ou
//   <dir>/data/<col>=<valor>/00000-0-<uuid>.parquet com `part_col` (coluna
//   de particao fora do parquet, padrao Iceberg),
//   <dir>/metadata/<uuid>-m0.avro (manifest Avro OCF com o record
//   `partition` do data_file preenchido) e
//   <dir>/metadata/snap-<id>-0-<uuid>.avro (manifest list Avro OCF), e
//   commita <dir>/metadata/v<N>-<uuid>.metadata.json (format-version 2) com
//   partition-specs (identity, field-id 1000) + default-spec-id 0;
// - append (iceberg_append) valida schema identico, herda o partition spec
//   da tabela (erro se `part_col_req` diverge ou se a tabela nao e
//   particionada), grava novos data files, um novo manifest/manifest list e
//   commita v<N+1>-<uuid>.metadata.json com o novo snapshot (parent =
//   current-snapshot-id anterior). Commit atomico via temporario + rename()
//   no mesmo diretorio; pressupoe um unico escritor — sem locks nem
//   optimistic concurrency;
// - leitura resolve o snapshot atual percorrendo a cadeia de pais
//   (parent-snapshot-id) e coletando adds menos removes dos manifests,
//   concatenando os data files; em tabela particionada reidrata a coluna de
//   particao a partir dos records `partition` dos manifests, usando o tipo
//   do schema. Lanca std::runtime_error com mensagem acionavel em qualquer
//   limite.
void iceberg_write(const std::string& dir, const Value& tabela, const std::string& part_col);
void iceberg_append(const std::string& dir, const Value& tabela, const std::string& part_col_req);
Value iceberg_read(const std::string& dir);  // -> tabela (lista de mapas)

}  // namespace tilt::rt
