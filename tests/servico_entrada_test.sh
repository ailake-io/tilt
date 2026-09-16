#!/usr/bin/env sh
# Validacao de `entrada:` contra `tipo` no servir: corpo ok, preenchimento
# de padrao (`campo: Tipo = padrao`), campo ausente sem padrao (400) e tipo
# errado (400). Sobe o fixture com --requisicoes e chama as rotas com curl.
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/servico_entrada.tilt}"
PORTA="8487"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste servico_entrada"
  exit 0
}
command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste servico_entrada"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$srv" 2>/dev/null || true; rm -rf "$tmp"' EXIT

"$BIN" servir "$FIXTURE" --porta "$PORTA" --requisicoes 4 >"$tmp/serv.log" 2>&1 &
srv=$!
sleep 2

fail=0
base="http://127.0.0.1:$PORTA/pedidos"

ok=$(curl -s -X POST "$base" -H 'content-type: application/json' -d '{"nome":"ana","qtd":2}')
echo "$ok" | grep -q '"nome": "ana"' || { echo "ok sem nome ana: $ok"; fail=1; }
echo "$ok" | grep -q '"qtd": 2' || { echo "ok sem qtd 2: $ok"; fail=1; }

padrao=$(curl -s -X POST "$base" -H 'content-type: application/json' -d '{"qtd":3}')
echo "$padrao" | grep -q '"nome": "anon"' || { echo "padrao nao preencheu nome: $padrao"; fail=1; }

ausente=$(curl -s -X POST "$base" -H 'content-type: application/json' -d '{"nome":"ana"}')
echo "$ausente" | grep -q "campo 'qtd' ausente" || { echo "ausente sem 400 claro: $ausente"; fail=1; }

tipo=$(curl -s -X POST "$base" -H 'content-type: application/json' -d '{"nome":"ana","qtd":"muita"}')
echo "$tipo" | grep -q "campo 'qtd' deve ser inteiro" || { echo "tipo errado sem 400 claro: $tipo"; fail=1; }

wait "$srv" 2>/dev/null || true

[ "$fail" = 0 ] && echo "servico_entrada_test ok"
exit "$fail"
