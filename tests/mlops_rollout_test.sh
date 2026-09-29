#!/usr/bin/env sh
# Valida limite por rota e metadado de variante no rollout local.
set -eu
BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/mlops_rollout.tilt}"
PORTA="8481"
command -v curl >/dev/null 2>&1 || { echo "curl ausente; pulando mlops_rollout"; exit 0; }
tmp=$(mktemp -d)
srv=""
trap 'if [ -n "$srv" ]; then kill "$srv" 2>/dev/null || true; fi; rm -rf "$tmp"' EXIT
"$BIN" servir "$FIXTURE" --porta "$PORTA" --requisicoes 2 >"$tmp/serv.log" 2>&1 &
srv=$!
sleep 2
first=$(curl -sS -D "$tmp/first.headers" -o "$tmp/first.body" -X POST "http://127.0.0.1:$PORTA/predizer" -H 'content-type: application/json' -d '{}')
second=$(curl -sS -D "$tmp/second.headers" -o "$tmp/second.body" -w '%{http_code}' -X POST "http://127.0.0.1:$PORTA/predizer" -H 'content-type: application/json' -d '{}')
grep -q 'HTTP/.* 200' "$tmp/first.headers" || { cat "$tmp/serv.log"; echo "primeira chamada não retornou 200"; exit 1; }
grep -qi '^X-Tilt-Variant: estavel' "$tmp/first.headers" || { cat "$tmp/first.headers"; echo "variante ausente"; exit 1; }
grep -q '"variante": "estavel"' "$tmp/first.body" || { cat "$tmp/first.body"; echo "corpo sem variante"; exit 1; }
[ "$second" = 429 ] || { cat "$tmp/second.body"; echo "limite não retornou 429: $second"; exit 1; }
grep -qi '^Retry-After: 60' "$tmp/second.headers" || { cat "$tmp/second.headers"; echo "Retry-After ausente"; exit 1; }
wait "$srv" 2>/dev/null || true
echo "mlops_rollout_test ok"
