#!/usr/bin/env sh
# Integration test for parallel route execution in `tilt servir`: a CPU-burn
# route must run concurrently on the worker pool. Baseline: 4 sequential
# requests on a serial server (~4x the single-request time, curl startup
# included). Parallel: the same 4 requests fired concurrently on a 4-thread
# pool, which must finish well under the serial baseline. Also checks all
# responses are correct.
set -eu

BIN="$1"
FIXTURE="$2"
PORT="${TILT_TEST_PORT:-8621}"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste de paralelismo"
  exit 0
}

# O pool de rotas so existe no caminho epoll (Linux); em outras plataformas
# o fallback bloqueante e serial por construcao.
uname -s | grep -q Linux || {
  echo "nao-Linux: servico e serial; pulando o teste de paralelismo"
  exit 0
}

# Num nucleo so nao ha speedup possivel; o teste exigiria o impossivel.
nproc 2>/dev/null | grep -qv '^1$' || {
  echo "1 nucleo: sem speedup possivel; pulando o teste de paralelismo"
  exit 0
}

wait_listen() {
  for _ in $(seq 1 50); do
    grep -q "escutando" "$1" 2>/dev/null && return 0
    sleep 0.1
  done
  echo "servidor nao iniciou; log:"; cat "$1"; return 1
}

check_resp() {
  grep -q '"n": 50000' "$1" || {
    echo "resposta incorreta em $1:"; cat "$1"; return 1
  }
}

tmp=$(mktemp -d)
trap 'kill "$srv_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

fail=0

# --- baseline serial: 4 requests em sequencia (--threads 1) ------------------
"$BIN" servir "$FIXTURE" --porta "$PORT" --requisicoes 4 --threads 1 >"$tmp/log_s" 2>&1 &
srv_pid=$!
wait_listen "$tmp/log_s"

t0=$(date +%s%N)
curl -s -m 120 -o "$tmp/seq_1" "localhost:$PORT/lento"
curl -s -m 120 -o "$tmp/seq_2" "localhost:$PORT/lento"
curl -s -m 120 -o "$tmp/seq_3" "localhost:$PORT/lento"
curl -s -m 120 -o "$tmp/seq_4" "localhost:$PORT/lento"
t1=$(date +%s%N)

# O servidor ja encerrou ao esgotar a cota (4 despachos); o `wait` so confere.
wait "$srv_pid" || true
srv_pid=""
TSEQ=$(( (t1 - t0) / 1000000 ))
echo "Tseq (4 sequenciais, --threads 1) = ${TSEQ} ms"

i=1
while [ "$i" -le 4 ]; do
  check_resp "$tmp/seq_$i" || fail=1
  i=$((i + 1))
done

# --- paralelo: os mesmos 4 requests concorrentes num pool de 4 ---------------
"$BIN" servir "$FIXTURE" --porta $((PORT + 1)) --requisicoes 4 --threads 4 >"$tmp/log_p" 2>&1 &
srv_pid=$!
wait_listen "$tmp/log_p"

t0=$(date +%s%N)
i=1
pids=""
while [ "$i" -le 4 ]; do
  curl -s -m 120 -o "$tmp/par_$i" "localhost:$((PORT + 1))/lento" &
  pids="$pids $!"
  i=$((i + 1))
done
# Esperar os curls explicitamente (bare `wait` quebra no bash 3.2 do macOS)
# e depois o servidor, que so encerra ao esgotar a cota --requisicoes 4;
# assim nenhum dos dois fica pendurado.
wait $pids 2>/dev/null || true
wait "$srv_pid"
t1=$(date +%s%N)
srv_pid=""
TPAR=$(( (t1 - t0) / 1000000 ))
echo "Tpar (4 concorrentes, --threads 4) = ${TPAR} ms"

i=1
while [ "$i" -le 4 ]; do
  check_resp "$tmp/par_$i" || fail=1
  i=$((i + 1))
done

# Sem pool, Tpar ~= Tseq. Com pool, espera-se speedup claro; o limiar de
# 3/4 absorve maquinas lentas/compartilhadas e curls de startup seriado.
if [ "$TPAR" -ge $((TSEQ * 3 / 4)) ]; then
  echo "sem paralelismo: Tpar=${TPAR} ms deveria ser bem menor que Tseq=${TSEQ} ms"
  fail=1
fi

[ "$fail" = 0 ] && echo "par_test ok"
exit "$fail"
