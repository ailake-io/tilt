#pragma once

#include <string>
#include <vector>

#include "runtime/value.hpp"

namespace tilt::rt {

// Parquet nativo, sem dependencias externas de link:
// - writer: um row group, encoding PLAIN, sem compressao; colunas sem nulos
//   sao REQUIRED (identico a antes) e colunas com nulos viram OPTIONAL, com
//   definition levels RLE (0 = nulo, 1 = definido) e valores nulos omitidos;
//   cada coluna leva o field_id (thrift SchemaElement[9]) — 1..N por padrao,
//   ou o vetor explicito em `field_ids` (mesma ordem das colunas), para
//   casar com os ids do schema Iceberg quando uma coluna fica fora do
//   arquivo (ex.: coluna de particao);
// - tipos: logico -> BOOLEAN, inteiro -> INT64, decimal -> DOUBLE,
//   texto -> BYTE_ARRAY (nulo e aceito em qualquer uma delas);
// - reader: le todos os row groups (concatena), campos REQUIRED e OPTIONAL
//   (definition levels RLE), paginas PLAIN e DICTIONARY (PLAIN_DICTIONARY /
//   RLE_DICTIONARY) e codec gzip/deflate (zlib via dlopen("libz.so.1")).
//   Snappy e demais codecs, DATA_PAGE_V2 e campos REPEATED falham com
//   mensagem clara em vez de lixo silencioso.
//
// Lanca std::runtime_error com mensagem acionavel em qualquer limite.

void parquet_write(const std::string& path, const Value& tabela,
                   const std::vector<int>* field_ids = nullptr);
Value parquet_read(const std::string& path);  // -> tabela (lista de mapas)

}  // namespace tilt::rt
