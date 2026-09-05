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
// - append (iceberg_append) valida o schema por nome (todas as colunas do
//   metadata presentes, tipos em comum iguais, ordem livre) com evolucao
//   limitada de schema (fase 27): coluna nova entra required:false no fim do
//   schema com field-id novo (last-column-id + 1), o metadata versionado
//   ganha um schema-id novo mantendo o historico (ids antigos estaveis) e os
//   data files do append sao gravados com esses field-ids; arquivos antigos
//   ficam sem a coluna e a leitura projeta nulo (union-by-name). Remover
//   coluna ou mudar o tipo de uma existente -> erro claro. Herda o partition
//   spec da tabela (erro se `part_col_req` diverge ou se a tabela nao e
//   particionada), grava novos data files, um novo manifest/manifest list e
//   commita v<N+1>-<uuid>.metadata.json com o novo snapshot (parent =
//   current-snapshot-id anterior). Commit atomico via temporario + rename()
//   no mesmo diretorio; pressupoe um unico escritor — sem locks nem
//   optimistic concurrency;
// - leitura resolve o snapshot atual percorrendo a cadeia de pais
//   (parent-snapshot-id) e coletando adds menos removes dos manifests,
//   concatenando os data files; projeta cada arquivo no schema corrente por
//   nome/field-id (coluna ausente no arquivo -> nulo); em tabela particionada
//   reidrata a coluna de particao a partir dos records `partition` dos
//   manifests, usando o tipo do schema. Lanca std::runtime_error com mensagem
//   acionavel em qualquer limite.
void iceberg_write(const std::string& dir, const Value& tabela, const std::string& part_col);
void iceberg_append(const std::string& dir, const Value& tabela, const std::string& part_col_req);
Value iceberg_read(const std::string& dir);  // -> tabela (lista de mapas)

}  // namespace tilt::rt
