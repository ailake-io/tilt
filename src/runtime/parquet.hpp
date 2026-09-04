#pragma once

#include <string>

#include "runtime/value.hpp"

namespace tilt::rt {

// Parquet de 1a passada, sem dependencias externas:
// - writer: um row group, encoding PLAIN, sem compressao, colunas REQUIRED;
// - tipos: logico -> BOOLEAN, inteiro -> INT64, decimal -> DOUBLE,
//   texto -> BYTE_ARRAY;
// - reader: le o mesmo subconjunto (arquivos comprimidos/optional/repetidos
//   falham com mensagem clara em vez de lixo silencioso).
//
// Lanca std::runtime_error com mensagem acionavel em qualquer limite.

void parquet_write(const std::string& path, const Value& tabela);
Value parquet_read(const std::string& path);  // -> tabela (lista de mapas)

}  // namespace tilt::rt
