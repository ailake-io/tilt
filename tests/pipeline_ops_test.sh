#!/usr/bin/env sh
# Operacao de pipelines (hermetico, sem rede): backoff no ao_falhar,
# tempo_limite por passo e quarentena (dead-letter) no para cada.
set -eu

BIN="$1"
FIXDIR="${2:-${0%/*}/fixtures}"
case "$FIXDIR" in
  /*) ;;
  *) FIXDIR="$(pwd)/$FIXDIR" ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

# 1) backoff: repetir 2, espera 1s x2 -> ~3s com as duas esperas no log
inicio=$(date +%s)
out_b=$(cd "$tmp" && "$BIN" executar "$FIXDIR/ops_backoff.tilt" 2>&1 || true)
fim=$(date +%s)
printf '%s\n' "$out_b"
echo "$out_b" | grep -q "nova tentativa em 1s (2/3)" || {
  echo "backoff: sem espera de 1s na 1a retentativa"; fail=1; }
echo "$out_b" | grep -q "nova tentativa em 2s (3/3)" || {
  echo "backoff: sem espera de 2s na 2a retentativa"; fail=1; }
[ $((fim - inicio)) -ge 3 ] || { echo "backoff: rapido demais (sem espera?)"; fail=1; }

# 2) timeout: passo 2 (laco) excede 3s
out_t=$(cd "$tmp" && "$BIN" executar "$FIXDIR/ops_timeout.tilt" 2>&1 || true)
printf '%s\n' "$out_t"
echo "$out_t" | grep -q "excedeu tempo_limite de 3s" || {
  echo "timeout: sem erro de tempo_limite"; fail=1; }
echo "$out_t" | grep -q "terminou" && { echo "timeout: passo terminou (nao abortei)"; fail=1; }

# 3) quarentena: 3 linhas desviadas, pipeline continua
out_q=$(cd "$tmp" && "$BIN" executar "$FIXDIR/ops_quarentena.tilt" 2>&1)
printf '%s\n' "$out_q"
echo "$out_q" | grep -q "quarentena: 3 linha(s) desviadas para quarentena.jsonl" || {
  echo "quarentena: sem resumo das 3 linhas"; fail=1; }
echo "$out_q" | grep -q "^fim" || { echo "quarentena: pipeline nao continuou"; fail=1; }
[ -f "$tmp/quarentena.jsonl" ] || { echo "quarentena: arquivo nao criado"; fail=1; }
n=$(grep -c '"erro"' "$tmp/quarentena.jsonl")
[ "$n" = "3" ] || { echo "quarentena: esperado 3 registros, achou $n"; fail=1; }
grep -q '"v": 1' "$tmp/quarentena.jsonl" || { echo "quarentena: linha sem o mapa original"; fail=1; }

[ "$fail" = 0 ] && echo "pipeline_ops_test ok"
exit "$fail"
