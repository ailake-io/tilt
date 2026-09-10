#pragma once

#include <string>
#include <vector>

#include "runtime/value.hpp"

namespace tilt::rt {

// Cliente Redis nativo via protocolo RESP (sockets POSIX, sem dependencias).
// Timeout de 5s por operacao. `ler_redis`/`escrever_redis` usam uma conexao
// por comando; `redis_executar` idem; `redis_lote` envia varios comandos em
// pipeline numa unica conexao.
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

// Converte um argumento de comando para a string enviada ao Redis:
// texto cru; logico/inteiro/decimal como em redis_set. Demais tipos ->
// excecao "redis: tipo '<t>' nao suportado em argumento de comando redis".
std::string redis_arg_para_texto(const Value& v);

// Executa um comando RESP arbitrario (`comando` = [nome, arg1, ...]) e
// converte a resposta: simple string/bulk -> texto; integer -> inteiro;
// array -> lista recursiva (itens nil -> nulo); nil -> nulo; error ->
// excecao "redis: <msg>". `comando` nao pode ser vazio nem ter strings vazias.
Value redis_executar(const std::string& url, const std::vector<std::string>& comando,
                     const RedisOpts& opts = {});

// Pipeline: envia todos os comandos numa unica conexao (sem ler entre eles)
// e so entao le as N respostas na ordem, devolvendo a lista de valores (mesma
// conversao de redis_executar). Limite de seguranca: 10000 comandos.
Value redis_lote(const std::string& url,
                 const std::vector<std::vector<std::string>>& comandos,
                 const RedisOpts& opts = {});

}  // namespace tilt::rt
