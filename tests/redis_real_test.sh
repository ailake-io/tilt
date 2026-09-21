#!/usr/bin/env sh
# Redis real (7): SET/GET/INCR, nil -> nulo, ler/escrever_redis, streams pelo
# redis_executar (XADD/XLEN), pipeline com redis_lote, erro -ERR capturavel e
# hashes. O mock RESP so cobre um subconjunto. Sem TILT_REDIS_REAL_URL (ex.:
# redis://127.0.0.1:6379/0), pula.
set -eu

BIN="$1"
[ -n "${TILT_REDIS_REAL_URL:-}" ] || {
  echo "TILT_REDIS_REAL_URL nao definida; pulando o teste redis_real"
  exit 0
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
p="tiltreal$$"

cat >"$tmp/prog.tilt" <<EOF2
pipeline redis_real:
  passos:
    - url = env "TILT_REDIS_REAL_URL"
    - imprimir "set:", redis_executar(url, "SET", "$p-k", "valor")
    - imprimir "get:", redis_executar(url, "GET", "$p-k")
    - imprimir "incr:", redis_executar(url, "INCR", "$p-c")
    - imprimir "nil:", redis_executar(url, "GET", "$p-nao-existe")
    - escrever_redis url, "$p-x", "valor-a"
    - imprimir "ler:", ler_redis(url, "$p-x")
    - redis_executar url, "XADD", "$p-fluxo", "*", "campo", "v1"
    - redis_executar url, "XADD", "$p-fluxo", "*", "campo", "v2"
    - imprimir "xlen:", redis_executar(url, "XLEN", "$p-fluxo")
    - lote = redis_lote url, [["RPUSH", "$p-fila", "a"], ["RPUSH", "$p-fila", "b"], ["LRANGE", "$p-fila", 0, -1]]
    - imprimir "lote:", lote
    - tentar:
        redis_executar url, "COMANDO_INVALIDO"
    capturar erro:
      imprimir "erro:", erro
    - redis_executar url, "HSET", "$p-h", "a", "1", "b", "2"
    - imprimir "hgetall:", redis_executar(url, "HGETALL", "$p-h")
    - redis_executar url, "DEL", "$p-k", "$p-c", "$p-x", "$p-fluxo", "$p-fila", "$p-h"
EOF2

out=$("$BIN" executar "$tmp/prog.tilt" 2>&1) || { echo "execucao falhou: $out"; exit 1; }
printf '%s\n' "$out"

fail=0
for esperado in "set: OK" "get: valor" "incr: 1" "nil: nulo" "ler: valor-a" "xlen: 2" \
                "lote: [1, 2, [a, b]]" "erro: redis: ERR unknown command" "hgetall: [a, 1, b, 2]"; do
  echo "$out" | grep -qF "$esperado" || { echo "FALHA: sem '$esperado'"; fail=1; }
done
[ "$fail" = 0 ] && echo "redis_real_test ok"
exit "$fail"
