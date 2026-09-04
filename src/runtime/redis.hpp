#pragma once

#include <string>

#include "runtime/value.hpp"

namespace tilt::rt {

// Cliente Redis nativo via protocolo RESP (sockets POSIX, sem dependencias).
// Limitacoes da 1a passada: sem TLS, sem AUTH, sem selecao de db, um comando
// por conexao (abre, envia, le resposta, fecha). Timeout de 5s por operacao.
//
// `url` tem o formato "redis://host:porta" (padrao localhost:6379).

// GET <chave>. Texto cru retorna como Texto; se comecar com '{' ou '[',
// tenta json_parse e devolve Mapa/Lista. Chave inexistente -> excecao.
Value redis_get(const std::string& url, const std::string& chave);

// SET <chave> <valor>: texto/logico/inteiro/decimal gravados como string do
// valor; mapa/lista serializados com JSON compacto inline.
void redis_set(const std::string& url, const std::string& chave, const Value& valor);

}  // namespace tilt::rt
