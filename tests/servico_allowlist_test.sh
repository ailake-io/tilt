#!/usr/bin/env sh
# Garante que um servico HTTP executa apenas as ferramentas declaradas em
# ferramentas: e responde 500 de forma capturavel quando outra e chamada.
set -eu

BIN="$1"
FIXTURE="$2"
PORTA="8490"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste servico_allowlist"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$srv" 2>/dev/null || true; rm -rf "$tmp"' EXIT

"$BIN" servir "$FIXTURE" --porta "$PORTA" --requisicoes 3 >"$tmp/serv.log" 2>&1 &
srv=$!
for _ in $(seq 1 30); do
  curl -s "http://127.0.0.1:$PORTA/nao-existe" >/dev/null 2>&1 && break
  sleep 0.1
done

ok=$(curl -s -X POST "http://127.0.0.1:$PORTA/ok"   -H 'content-type: application/json' -d '{"msg":"ola"}')
echo "$ok" | grep -q '"resultado": "permitido: ola"' || {
  echo "ferramenta permitida nao executou: $ok"; exit 1;
}

negada=$(curl -s -X POST "http://127.0.0.1:$PORTA/negada"   -H 'content-type: application/json' -d '{"msg":"ola"}')
echo "$negada" | grep -q "ferramenta 'negada' nao permitida neste servico HTTP" || {
  echo "allowlist nao bloqueou ferramenta: $negada"; exit 1;
}

wait "$srv" 2>/dev/null || true
echo "servico_allowlist_test ok"
exit 0
