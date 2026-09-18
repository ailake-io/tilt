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

"$BIN" servir "$FIXTURE" --porta "$PORTA" --requisicoes 5 >"$tmp/serv.log" 2>&1 &
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
for rota in ('POST /eco', 'GET /falha'):
    lat = m['por_rota'][rota]['latencia_us']
    assert lat['total'] >= 1, (rota, lat)
    assert lat['max'] >= 1, (rota, lat)
    assert 0 < lat['media'] <= lat['max'], (rota, lat)
print('metricas validadas')
" || fail=1

prom=$(curl -s "http://127.0.0.1:$PORTA/metricas?formato=prometheus")
printf '%s\n' "$prom"
echo "$prom" | grep -q '^# TYPE tilt_http_requests_total counter$' || {
  echo "prometheus sem tipo de requisicoes"; fail=1; }
echo "$prom" | grep -q 'tilt_http_requests_total{service="Ops"} 2' || {
  echo "prometheus sem total de requisicoes"; fail=1; }
echo "$prom" | grep -q 'tilt_http_request_duration_microseconds_total{service="Ops",method="GET",route="/falha"}' || {
  echo "prometheus sem latencia por rota"; fail=1; }

grep -q '"trace_id":"req-' "$tmp/serv.log" || {
  echo "log sem trace_id estruturado"; fail=1; }
grep -q '"metodo":"POST","rota":"/eco","status":200' "$tmp/serv.log" || {
  echo "log sem evento JSON de /eco"; fail=1; }
grep -q '"metodo":"GET","rota":"/falha","status":500' "$tmp/serv.log" || {
  echo "log sem evento JSON de /falha"; fail=1; }

wait "$srv" 2>/dev/null || true

[ "$fail" = 0 ] && echo "servico_ops_test ok"
exit "$fail"
