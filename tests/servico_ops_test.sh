#!/usr/bin/env sh
# /saude + /metricas no servir: sobe o fixture com --requisicoes, chama as
# rotas com curl e confere os JSONs (contagem por rota, erro 500 contado).
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/ops_servico.tilt}"
PORTA="8479"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste servico_ops"
  exit 0
}
command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste servico_ops"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$srv" 2>/dev/null || true; rm -rf "$tmp"' EXIT

"$BIN" servir "$FIXTURE" --porta "$PORTA" --requisicoes 4 >"$tmp/serv.log" 2>&1 &
srv=$!
sleep 2

fail=0
saude=$(curl -s "http://127.0.0.1:$PORTA/saude")
printf '%s\n' "$saude"
echo "$saude" | grep -q '"status": "ok"' || { echo "saude sem status ok"; fail=1; }
echo "$saude" | grep -q '"rotas": 2' || { echo "saude sem contar 2 rotas"; fail=1; }

eco=$(curl -s -X POST "http://127.0.0.1:$PORTA/eco" -H 'content-type: application/json' -d '{}')
echo "$eco" | grep -q '"ok": true' || { echo "eco sem ok:true"; fail=1; }

curl -s "http://127.0.0.1:$PORTA/falha" | grep -q '"erro"' || {
  echo "falha sem corpo de erro"; fail=1; }

met=$(curl -s "http://127.0.0.1:$PORTA/metricas")
printf '%s\n' "$met"
echo "$met" | python3 -c "
import json, sys
m = json.load(sys.stdin)
assert m['requisicoes'] == 2, m  # eco + falha (/saude e /metricas nao contam)
assert m['erros'] == 1, m
assert m['por_rota']['POST /eco']['total'] == 1, m
assert m['por_rota']['GET /falha']['erros'] == 1, m
print('metricas validadas')
" || fail=1

wait "$srv" 2>/dev/null || true

[ "$fail" = 0 ] && echo "servico_ops_test ok"
exit "$fail"
