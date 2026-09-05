#pragma once

#include <string>

#include "runtime/value.hpp"

namespace tilt::rt {

// Iceberg de 1a passada, catalogo tipo Hadoop sobre diretorio local:
// - escrita cria <dir>/data/<uuid>.parquet (mesmo perfil do parquet nativo),
//   <dir>/metadata/<uuid>-m0.avro (manifest Avro OCF) e
//   <dir>/metadata/snap-<id>-0-<uuid>.avro (manifest list Avro OCF), e
//   commita <dir>/metadata/0-<uuid>.metadata.json (format-version 2);
// - append (iceberg_append) valida schema identico, grava um novo data file,
//   um novo manifest/manifest list e commita v<N+1>-<uuid>.metadata.json com
//   o novo snapshot (parent = current-snapshot-id anterior). Commit atomico
//   via temporario + rename() no mesmo diretorio; pressupoe um unico
//   escritor — sem locks nem optimistic concurrency;
// - leitura resolve o snapshot atual percorrendo a cadeia de pais
//   (parent-snapshot-id) e coletando adds menos removes dos manifests,
//   concatenando os data files com validacao de schema entre arquivos.
// Lanca std::runtime_error com mensagem acionavel em qualquer limite.
void iceberg_write(const std::string& dir, const Value& tabela);
void iceberg_append(const std::string& dir, const Value& tabela);
Value iceberg_read(const std::string& dir);  // -> tabela (lista de mapas)

}  // namespace tilt::rt
