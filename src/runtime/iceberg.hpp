#pragma once

#include <string>
#include <vector>

#include "runtime/value.hpp"

namespace tilt::rt {

// Iceberg de 1a passada, catalogo tipo Hadoop sobre diretorio local:
// - escrita cria <dir>/data/<uuid>.parquet (tabela sem particao) ou
//   <dir>/data/<c1>=<v1>/<c2>=<v2>/00000-0-<uuid>.parquet com `part_cols`
//   (colunas de particao fora do parquet, padrao Iceberg),
//   <dir>/metadata/<uuid>-m0.avro (manifest Avro OCF com o record
//   `partition` do data_file preenchido) e
//   <dir>/metadata/snap-<id>-0-<uuid>.avro (manifest list Avro OCF), e
//   commita <dir>/metadata/v<N>-<uuid>.metadata.json (format-version 2) com
//   partition-specs (identity, field-ids 1000+) + default-spec-id 0;
// - append (iceberg_append) valida o schema por nome (todas as colunas do
//   metadata presentes, tipos em comum iguais, ordem livre) com evolucao
//   limitada de schema (fase 27): coluna nova entra required:false no fim do
//   schema com field-id novo (last-column-id + 1), o metadata versionado
//   ganha um schema-id novo mantendo o historico (ids antigos estaveis) e os
//   data files do append sao gravados com esses field-ids; arquivos antigos
//   ficam sem a coluna e a leitura projeta nulo (union-by-name). Remover
//   coluna ou mudar o tipo de uma existente -> erro claro. Herda o partition
//   spec da tabela (erro se `part_cols_req` diverge ou se a tabela nao e
//   particionada), grava novos data files, um novo manifest/manifest list e
//   commita v<N+1>-<uuid>.metadata.json com o novo snapshot (parent =
//   current-snapshot-id anterior). Commit atomico via temporario + rename()
//   no mesmo diretorio; pressupoe um unico escritor — sem locks nem
//   optimistic concurrency;
// - leitura resolve o snapshot atual percorrendo a cadeia de pais
//   (parent-snapshot-id) e coletando adds menos removes dos manifests,
//   concatenando os data files; projeta cada arquivo no schema corrente por
//   nome/field-id (coluna ausente no arquivo -> nulo); em tabela particionada
//   reidrata as colunas de particao a partir dos records `partition` dos
//   manifests, usando o tipo do schema. Lanca std::runtime_error com mensagem
//   acionavel em qualquer limite;
// - pruning: iceberg_read aceita `onde` (mapa coluna -> valor, igualdade).
//   Predicados em colunas de particao podam data files inteiros comparando
//   contra os records `partition` dos manifests; predicados em outras colunas
//   viram filtro de linha aplicado apos a reidratacao. Sem match, retorna
//   tabela vazia.
// - REST catalog (fase 29, opt-in via ICEBERG_CATALOG=rest + ICEBERG_URI):
//   as tres funcoes passam a operar via Iceberg REST Open API (subconjunto:
//   loadTable/createTable/transactions no namespace "default", prefixo v1).
//   O argumento `dir` continua sendo a location local (enviada como file://
//   no createTable) e o nome da tabela no catalogo e o basename dele; o tilt
//   segue gravando data files/manifests/metadata localmente e commita as
//   locations no catalogo. Subconjunto de updates: upgrade-format-version,
//   set-location, set-properties, add-snapshot, set-snapshot-ref,
//   add-schema/set-current-schema (evolucao e sobrescrita),
//   remove-snapshot-ref/remove-snapshots (sobrescrita). Sobrescrita de
//   tabela existente mantem o partition spec (divergencia -> erro claro).
//   Sem as env vars o comportamento e o HadoopCatalog local, byte a byte.
void iceberg_write(const std::string& dir, const Value& tabela,
                   const std::vector<std::string>& part_cols = {});
void iceberg_append(const std::string& dir, const Value& tabela,
                    const std::vector<std::string>& part_cols_req = {});
// `onde` (opcional): mapa coluna -> valor. Igualdade; colunas de particao
// podam data files, o resto filtra linhas. Resultado pode ser tabela vazia.
Value iceberg_read(const std::string& dir, const Value* onde = nullptr);  // -> tabela (lista de mapas)

}  // namespace tilt::rt
