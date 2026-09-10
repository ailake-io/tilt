#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "runtime/value.hpp"

namespace tilt::rt {

// Cliente Kafka nativo via wire protocol 0.9-era (sockets POSIX, sem
// dependencias): MetadataRequest (api 3, v0), ProduceRequest (api 0, v1) e
// FetchRequest (api 1, v1), com CRC32-IEEE proprio para o message set.
// Consumer groups com rebalanceamento real: FindCoordinator (api 10, v0),
// JoinGroup (api 11, v0), Heartbeat (api 12, v0), LeaveGroup (api 13, v0),
// SyncGroup (api 14, v0), OffsetFetch (api 9, v0) e OffsetCommit (api 8, v1).
// O lider do grupo calcula o assignment "roundrobin" das particoes e o
// distribui no SyncGroup; um heartbeat por thread (a cada 3s) mantem a
// membrasia e erros de rebalance (RebalanceInProgress/IllegalGeneration/
// UnknownMemberId) disparam rejoin com retomada do offset commitado. Limitacoes:
// deteccao de entrada/saida de membros so e revalidada no proximo heartbeat
// ou na proxima chamada, sem SASL; TLS via OpenSSL carregado em runtime
// (dlopen) quando `tls` = true — ver runtime/tls.hpp, incluindo a env
// TILT_TLS_SKIP_VERIFY para certificados auto-assinados em testes. Timeout
// de 5s por operacao.
//
// O broker vem de `broker` quando nao vazio; caso contrario da env
// `KAFKA_BOOTSTRAP` (default "127.0.0.1:9092").

// Produz `valor` bruto (ja serializado pelo chamador) no topico/particao.
// required_acks=1; error_code != 0 na resposta vira excecao com o nome do
// erro. Retorna o offset atribuido (nao usado pelo builtin, util p/ testes).
std::int64_t kafka_produzir(const std::string& topico, const std::string& valor,
                            std::int32_t particao, bool tls = false);

// Le do lider da particao 0 de `topico` e devolve a lista de valores (texto)
// na ordem do log. `do_fim` = true faz fetch a partir do high watermark
// (offset -1, "latest"); caso contrario le do earliest. `max` limita o numero
// de mensagens retornadas.

Value kafka_ler(const std::string& topico, bool do_fim, std::int64_t max,
                const std::string& broker = "", bool tls = false);

// Consome `topico` como membro de `grupo` com coordenacao completa:
// FindCoordinator -> JoinGroup -> Heartbeat inicial -> SyncGroup (assignment
// "roundrobin" calculado pelo lider) -> OffsetFetch (offsets commitados) ->
// Fetch a partir do offset commitado (ou earliest) -> OffsetCommit por
// particao -> LeaveGroup limpo, com heartbeat periodico (3s) em thread e
// rejoin (ate 3x) se rebalance ocorrer durante o consumo. Devolve pares
// (particao, payload) na ordem consumida; com N membros no grupo as
// particoes sao divididas entre eles. `broker` vazio usa `KAFKA_BOOTSTRAP`.
// Erros fatais de grupo viram excecao com o nome e o codigo do erro.
std::vector<std::pair<int, std::string>> kafka_consume_group(const std::string& broker,
                                                             const std::string& grupo,
                                                             const std::string& topico,
                                                             int max_msgs, bool tls = false);

}  // namespace tilt::rt
