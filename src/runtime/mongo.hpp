#pragma once

#include <cstdint>
#include <string>

#include "runtime/value.hpp"

namespace tilt::rt {

// Cliente MongoDB nativo (sem dependencias): BSON proprio (serializer +
// parser) e wire protocol OP_MSG (opcode 2013) sobre socket POSIX, no estilo
// de redis.cpp/kafka.cpp (read_full/write_full, timeout de 5s). Zero
// checksum e zero OP_COMPRESSED; document sequences (kind 1) sao puladas na
// leitura. Handshake com {isMaster: 1} no connect.
//
// O servidor vem da env `MONGO_URL`
// (default "mongodb://127.0.0.1:27017"); o path opcional da URL e o banco
// default (mongodb://host:porta/banco). Sem path e sem opcao `banco:`,
// inserir/buscar falham com erro acionavel antes de tocar a rede.

// Insere `doc` (deve ser mapa; senao "mongo: inserir espera um mapa") na
// colecao. Gera `_id` ObjectId quando ausente. Comando {insert, $db,
// documents}; ok:0 na resposta vira excecao com o errmsg do servidor.
void mongo_inserir(const std::string& colecao, const Value& doc, const std::string& banco);

// Busca na colecao devolvendo lista de mapas (max 100 por padrao). `filtro`
// deve ser mapa de igualdade exata campo a campo (top-level, combinado por
// E) — filtros vazios retornam tudo. Comando {find, $db, filter, limit,
// batchSize}; a resposta vem em {cursor: {firstBatch: [...]}}. ObjectId do
// servidor vira texto hex de 24 chars.
Value mongo_buscar(const std::string& colecao, const Value& filtro, std::int64_t max,
                   const std::string& banco);

}  // namespace tilt::rt
