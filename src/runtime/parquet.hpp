#pragma once

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
//   chave ausente vira campo OPTIONAL); listas aninhadas (list<list<...>>),
//   listas de structs e elementos nulos em lista falham com erro claro;
//   structs com `field_ids` explicitos (caminho Iceberg) ainda nao suportados;
// - reader: le todos os row groups (concatena), campos REQUIRED, OPTIONAL e
//   REPEATED (definition/repetition levels RLE), structs aninhados (grupos
//   sem anotacao LIST; struct OPTIONAL definido com todos os campos nulos
//   distingue-se do struct nulo pelos definition levels), paginas v1 e v2, PLAIN e
//   DICTIONARY (PLAIN_DICTIONARY/RLE_DICTIONARY) e codecs gzip/deflate
//   (zlib via dlopen("libz.so.1")) e snappy (codec proprio, sem dlopen).
//
// Lanca std::runtime_error com mensagem acionavel em qualquer limite.

// Codecs (mesmos valores do enum parquet): 0 = sem compressao,
// 1 = snappy, 2 = gzip.
struct ParquetWriteOpts {
  int codec = 2;          // gzip por padrao
  bool paginas_v2 = false;  // DATA_PAGE v1 por padrao
};

void parquet_write(const std::string& path, const Value& tabela,
                   const std::vector<int>* field_ids = nullptr,
                   const ParquetWriteOpts& opts = {});
Value parquet_read(const std::string& path);  // -> tabela (lista de mapas)

}  // namespace tilt::rt
