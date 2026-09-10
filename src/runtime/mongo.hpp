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
// default (mongodb://host:porta/banco). O esquema "mongodb+srv://" liga TLS
// (OpenSSL via dlopen; ver runtime/tls.hpp, incluindo a env
// TILT_TLS_SKIP_VERIFY para certificados auto-assinados em testes) — sem
// lookup DNS SRV nesta fase, o host e usado como em mongodb://. Sem path e
// sem opcao `banco:`, inserir/buscar falham com erro acionavel antes de
// tocar a rede.

// Insere `doc` (deve ser mapa; senao "mongo: inserir espera um mapa") na
// colecao. Gera `_id` ObjectId quando ausente. Comando {insert, $db,
// documents}; ok:0 na resposta vira excecao com o errmsg do servidor.
void mongo_inserir(const std::string& colecao, const Value& doc, const std::string& banco);

// Busca na colecao devolvendo lista de mapas (max 100 por padrao). `filtro`
// deve ser mapa de igualdade exata campo a campo (top-level, combinado por
// E) — filtros vazios retornam tudo. Comando {find, $db, filter, limit,
// batchSize, projection?}; se o cursor do servidor vier com id != 0, itera
// getMore ({getMore, $db, collection, batchSize?}) acumulando nextBatch ate
// o cursor fechar (limite de 10000 getMore por seguranca). `somente` (lista
// de textos nao vazios) vira projection whitelist {campo: 1, ...}; `lote`
// (> 0) e o batchSize solicitado ao servidor. ObjectId do servidor vira
// texto hex de 24 chars.
Value mongo_buscar(const std::string& colecao, const Value& filtro, std::int64_t max,
                   const std::string& banco, const Value& somente, std::int64_t lote);

// Atualiza documentos que casam com `filtro` (mesma igualdade top-level de
// buscar) aplicando `mudancas`. Operadores suportados: `$set` e `$inc` (mapa
// {campo: valor, ...} cada um), podendo ser combinados no mesmo update;
// outro operador -> erro claro. `multi` falso (default) atualiza so o
// primeiro que casa; verdadeiro atualiza todos.
// Comando {update, $db, updates: [{q, u, multi}]}; ok:0 vira excecao com o
// errmsg. Devolve nModified como inteiro.
std::int64_t mongo_atualizar(const std::string& colecao, const Value& filtro,
                             const Value& mudancas, bool multi, const std::string& banco);

// Remove os documentos que casam com `filtro` (igualdade top-level).
// Comando {delete, $db, deletes: [{q, limit: 0}]} (limit 0 = todos que
// casam; 1a passada sem limit 1). Devolve n (deletados) como inteiro.
std::int64_t mongo_deletar(const std::string& colecao, const Value& filtro,
                           const std::string& banco);

// Cria indice ascendente (1) nos `campos` (lista de textos nao vazia) da
// colecao. Comando {createIndexes, $db, indexes: [{key: {a: 1, ...},
// name: "a_1_..."}]}; o name e gerado dos campos. Devolve o name (texto).
std::string mongo_criar_indice(const std::string& colecao, const Value& campos,
                               const std::string& banco);

// Agrega na colecao com o `pipeline` (lista de mapas, traduzidos 1:1 para
// BSON: $match/$project/$group/$sort/$limit/$skip e operadores como $gte,
// $sum, $avg passam como chaves normais). Comando {aggregate, $db, pipeline,
// cursor: {}}; ok:0 vira excecao com o errmsg. Devolve o cursor.firstBatch
// como lista de mapas (mesma conversao BSON->tilt de buscar). Sem getMore
// nesta fase: se o cursor do servidor tiver id != 0, apenas o firstBatch e
// devolvido — use $limit/$skip para caber no primeiro batch.
Value mongo_agregar(const std::string& colecao, const Value& etapas,
                    const std::string& banco);

}  // namespace tilt::rt
