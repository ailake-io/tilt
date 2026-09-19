#!/usr/bin/env sh
# Verifica que uma rota pode declarar uma versao (v1/v2), que o servidor
# acrescenta o prefixo no caminho e que rotas legadas continuam funcionando.
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/servico_versao.tilt}"
PORTA="8491"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste servico_versao"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$srv" 2>/dev/null || true; rm -rf "$tmp"' EXIT

"$BIN" servir "$FIXTURE" --porta "$PORTA" --requisicoes 5 >"$tmp/serv.log" 2>&1 &
srv=$!
for _ in $(seq 1 30); do
  curl -s "http://127.0.0.1:$PORTA/nao-existe" >/dev/null 2>&1 && break
  sleep 0.1
done

v1=$(curl -s "http://127.0.0.1:$PORTA/v1/ping")
echo "$v1" | grep -q '"versao": "v1"' || {
  echo "rota v1 nao respondeu: $v1"; exit 1;
}

v2=$(curl -s "http://127.0.0.1:$PORTA/v2/ping")
echo "$v2" | grep -q '"versao": "v2"' || {
  echo "rota v2 nao respondeu: $v2"; exit 1;
}

legado=$(curl -s "http://127.0.0.1:$PORTA/legado")
echo "$legado" | grep -q '"versao": "legado"' || {
  echo "rota sem versao deixou de responder: $legado"; exit 1;
}

curl -s -o "$tmp/sem-prefixo" "http://127.0.0.1:$PORTA/ping"
grep -q 'rota nao encontrada' "$tmp/sem-prefixo" || {
  echo "rota versionada respondeu sem prefixo"; exit 1;
}

wait "$srv" 2>/dev/null || true
echo "servico_versao_test ok"
