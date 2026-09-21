#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "runtime/value.hpp"

namespace tilt::rt {

// Parquet nativo, sem dependencias externas de link:
// - writer: um row group, encoding PLAIN; colunas sem nulos sao REQUIRED e
//   colunas com nulos viram OPTIONAL, com definition levels RLE (valores
//   nulos omitidos); paginas DATA_PAGE v1 por padrao ou v2 com
//   `paginas_v2`; compressao gzip (padrao) ou snappy literal-only; strings
//   levam anotacao UTF8; listas de escalares viram campos REPEATED com
//   anotacao LIST (3-level padrao); cada folha leva um field_id
//   (thrift SchemaElement[9]) — sequencial por folha, ou o vetor explicito em
//   `field_ids` (um por coluna top-level; structs com field-ids explicitos
//   ainda nao suportados), para casar com os ids do schema
//   Iceberg quando uma coluna fica fora do arquivo (ex.: coluna de
//   particao);
// - tipos: logico -> BOOLEAN, inteiro -> INT64, decimal -> DOUBLE,
//   texto -> BYTE_ARRAY (UTF8), lista de escalares -> REPEATED + LIST,
//   mapa -> STRUCT (grupo sem anotacao, recursivo: escalares, listas de
//   escalares e structs aninhados; struct nulo por linha vira grupo OPTIONAL,
//   chave ausente vira campo OPTIONAL); estreitamento opt-in via
//   ParquetWriteOpts.tipos ("col" ou "struct.campo" -> "int32"/"float", com
//   anotacao INTEGER no INT32); listas aninhadas (list<list<...>>), listas
//   de structs e elementos nulos em lista falham com erro claro;
//   structs com `field_ids` explicitos (caminho Iceberg) ainda nao suportados;
// - reader: le todos os row groups (concatena), campos REQUIRED, OPTIONAL e
//   REPEATED (definition/repetition levels RLE), structs aninhados (grupos
//   sem anotacao LIST; struct OPTIONAL definido com todos os campos nulos
//   distingue-se do struct nulo pelos definition levels), MAP<string,*>
//   (partitionValues de checkpoints), fisicos INT32/INT64/FLOAT/DOUBLE/
//   BOOLEAN/BYTE_ARRAY/FIXED_LEN_BYTE_ARRAY/INT96, logicos UTF8/STRING/
//   INTEGER/DATE/TIME/TIMESTAMP/DECIMAL (data/hora/timestamp -> texto ISO,
//   decimal -> decimal), paginas v1 e v2, PLAIN e
//   DICTIONARY (PLAIN_DICTIONARY/RLE_DICTIONARY) e codecs gzip/deflate
//   (zlib via dlopen("libz.so.1")), snappy (codec proprio) e zstd via dlopen
//   (libzstd, sem dependencia de link).
//
// Lanca std::runtime_error com mensagem acionavel em qualquer limite.

// Codecs (mesmos valores do enum parquet): 0 = sem compressao,
// 1 = snappy, 2 = gzip, 6 = zstd.
struct ParquetWriteOpts {
  int codec = 2;          // gzip por padrao
  bool paginas_v2 = false;  // DATA_PAGE v1 por padrao
  // Estreitamento fisico opt-in (Marco 1 / B1): caminho pontilhado da folha
  // ("col" ou "struct.campo") -> "int32" (de inteiro, com checagem de
  // alcance) ou "float" (de decimal). Sem entrada: INT64/DOUBLE de sempre.
  std::map<std::string, std::string> tipos;
  // Dictionary encoding automatico quando ha repeticao (Marco 2 / B4);
  // `dicionario: falso` volta ao PLAIN puro.
  bool dicionario = true;
  // Modular Encryption (AES_GCM_V1): segredo local derivado por SHA-256.
  // O leitor usa TILT_PARQUET_KEY, evitando persistir a chave no arquivo.
  std::string chave;
  // AWS KMS: KeyId para GenerateDataKey (AES_256); o arquivo persiste somente
  // o KeyId resolvido e CiphertextBlob, nunca a data key em claro.
  std::string chave_kms;
};

void parquet_write(const std::string& path, const Value& tabela,
                   const std::vector<int>* field_ids = nullptr,
                   const ParquetWriteOpts& opts = {});
Value parquet_read(const std::string& path);  // -> tabela (lista de mapas)

// Streaming por row group (treino em arquivos grandes): abre uma vez e
// decodifica grupo a grupo, sem materializar o arquivo todo.
struct ParquetEstado;  // opaco (definido em parquet.cpp)
struct ParquetFluxo {
  std::string caminho;
  std::vector<std::string> colunas;  // nomes top-level, em ordem
  std::vector<std::int64_t> linhas_por_grupo;
  std::int64_t grupos = 0;
  std::int64_t linhas = 0;
  std::shared_ptr<ParquetEstado> estado;
};
ParquetFluxo parquet_abrir_fluxo(const std::string& path);  // lanca em erro
Value parquet_ler_grupo_fluxo(ParquetFluxo& fx, std::int64_t grupo);  // tabela; lanca em erro

}  // namespace tilt::rt
