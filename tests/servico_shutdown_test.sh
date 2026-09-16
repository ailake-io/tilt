#!/usr/bin/env sh
# Graceful shutdown: SIGTERM apos request drena e encerra; request em voo
# (rota lenta) ainda recebe a resposta antes de o processo sair.
set -eu

BIN="$1"
FIX="${2:-${0%/*}/fixtures/ops_servico.tilt}"
LENTO="${3:-${0%/*}/fixtures/servico_lento.tilt}"
P1="8483"
P2="8484"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste servico_shutdown"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$s1" 2>/dev/null || true; kill "$s2" 2>/dev/null || true; rm -rf "$tmp"' EXIT

espera_sair() {
  # espera_sair <pid> <timeout-s>: 0 se saiu, 1 se continua vivo (e mata -9).
  pid="$1"
  max="$2"
  i=0
  while kill -0 "$pid" 2>/dev/null; do
    i=$((i + 1))
    if [ "$i" -ge "$max" ]; then
      kill -9 "$pid" 2>/dev/null || true
      return 1
    fi
    sleep 1
  done
  return 0
}

fail=0

# 1) Basico: atende, recebe SIGTERM, encerra.
"$BIN" servir "$FIX" --porta "$P1" --requisicoes 0 >"$tmp/s1.log" 2>&1 &
s1=$!
sleep 2
curl -s "http://127.0.0.1:$P1/saude" | grep -q '"status": "ok"' || {
  echo "saude antes do TERM falhou"; fail=1; }
kill -TERM "$s1"
espera_sair "$s1" 10 || { echo "servidor nao encerrou via SIGTERM"; fail=1; }

# 2) Em voo: request lento + SIGTERM no meio; resposta chega e sai.
"$BIN" servir "$LENTO" --porta "$P2" --threads 1 --requisicoes 0 >"$tmp/s2.log" 2>&1 &
s2=$!
sleep 2
curl -s --max-time 25 "http://127.0.0.1:$P2/lento" >"$tmp/inflight.out" 2>&1 &
sleep 1
kill -TERM "$s2"
espera_sair "$s2" 20 || { echo "servidor nao encerrou apos drenar"; fail=1; }
wait 2>/dev/null || true
grep -q '"n": 50000' "$tmp/inflight.out" || {
  echo "resposta em voo nao chegou:"; cat "$tmp/inflight.out"; fail=1; }

[ "$fail" = 0 ] && echo "servico_shutdown_test ok"
exit "$fail"
