#pragma once

#include <string>

#include "runtime/value.hpp"

namespace tilt::rt {

// Cliente Redis nativo via protocolo RESP (sockets POSIX, sem dependencias).
// Limitacoes da 1a passada: um comando por conexao (abre, envia, le
// resposta, fecha). Timeout de 5s por operacao.
//
// `url` tem o formato "redis://host:porta" (padrao localhost:6379), com
// extensoes opcionais: userinfo ":senha@" para AUTH ("redis://:senha@host"),
// path numerico para SELECT ("redis://host:6379/2") e o esquema "rediss://"
// para TLS (OpenSSL carregado via dlopen; ver runtime/tls.hpp, incluindo a
// env TILT_TLS_SKIP_VERIFY para certificados auto-assinados em testes). As
// opcoes em `RedisOpts` (quando informadas) vencem o que vier na URL.

// Opcoes de conexao: `auth` vazio = sem AUTH; `db` < 0 = sem SELECT;
// `tls` = true liga TLS mesmo com URL "redis://" (rediss:// tambem liga).
struct RedisOpts {
  std::string auth;
  int db = -1;
  bool tls = false;
};

// GET <chave>. Texto cru retorna como Texto; se comecar com '{' ou '[',
// tenta json_parse e devolve Mapa/Lista. Chave inexistente -> excecao.
Value redis_get(const std::string& url, const std::string& chave,
                const RedisOpts& opts = {});

// SET <chave> <valor>: texto/logico/inteiro/decimal gravados como string do
// valor; mapa/lista serializados com JSON compacto inline.
void redis_set(const std::string& url, const std::string& chave, const Value& valor,
               const RedisOpts& opts = {});

}  // namespace tilt::rt
