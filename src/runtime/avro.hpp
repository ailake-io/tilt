#pragma once

#include <cstdint>
#include <string>

#include "runtime/value.hpp"

namespace tilt::rt {

// Serializa um valor no envelope Avro binario do Confluent Schema Registry:
// magic byte 0 + schema id big-endian (int32) + payload Avro.
std::string avro_confluent_encode(const std::string& schema_json, std::int32_t schema_id,
                                  const Value& value);

struct AvroConfluentValue {
  std::int32_t schema_id = 0;
  Value value;
};

AvroConfluentValue avro_confluent_decode(const std::string& schema_json,
                                         const std::string& payload);

// Operacoes REST minimas do Schema Registry compatível com a API Confluent.
// `base_url` deve apontar para a raiz, por exemplo http://localhost:8081.
std::string avro_schema_registry_get(const std::string& base_url, std::int32_t schema_id);
std::int32_t avro_schema_registry_register(const std::string& base_url,
                                           const std::string& subject,
                                           const std::string& schema_json);

}  // namespace tilt::rt
