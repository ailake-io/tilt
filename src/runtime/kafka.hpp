#pragma once

#include <cstdint>
#include <string>

#include "runtime/value.hpp"

namespace tilt::rt {

// Cliente Kafka nativo via wire protocol 0.9-era (sockets POSIX, sem
// dependencias): MetadataRequest (api 3, v0), ProduceRequest (api 0, v1) e
// FetchRequest (api 1, v1), com CRC32-IEEE proprio para o message set.
// Limitacoes da 1a passada: sem consumer groups / offset commit (stateless —
// desde "inicio" rele do earliest toda vez), sem SASL/TLS (plain), um broker
// lider por chamada. Timeout de 5s por operacao.
//
// O broker vem da env `KAFKA_BOOTSTRAP` (default "127.0.0.1:9092").

// Produz `valor` bruto (ja serializado pelo chamador) no topico/particao.
// required_acks=1; error_code != 0 na resposta vira excecao com o nome do
// erro. Retorna o offset atribuido (nao usado pelo builtin, util p/ testes).
std::int64_t kafka_produzir(const std::string& topico, const std::string& valor,
                            std::int32_t particao);

// Le do lider da particao 0 de `topico` e devolve a lista de valores (texto)
// na ordem do log. `do_fim` = true faz fetch a partir do high watermark
// (offset -1, "latest"); caso contrario le do earliest. `max` limita o numero
// de mensagens retornadas.

Value kafka_ler(const std::string& topico, bool do_fim, std::int64_t max);

}  // namespace tilt::rt
