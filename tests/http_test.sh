#!/usr/bin/env sh
# Integration test for `tilt servir`: start the server, drive it with curl,
# check the responses and the request log, and confirm a clean exit.
set -eu

BIN="$1"
FIXTURE="$2"
PORT="${TILT_TEST_PORT:-8611}"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste HTTP"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$srv_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

"$BIN" servir "$FIXTURE" --porta "$PORT" --requisicoes 3 >"$tmp/log" 2>&1 &
srv_pid=$!

# wait for the listen line
for _ in $(seq 1 50); do
  grep -q "escutando" "$tmp/log" 2>/dev/null && break
  sleep 0.1
done

r1=$(curl -s -X POST "localhost:$PORT/eco" -d '{"msg":"oi"}')
r2=$(curl -s "localhost:$PORT/saude")
r3=$(curl -s -o /dev/null -w '%{http_code}' "localhost:$PORT/nao-existe")

wait "$srv_pid"
srv_pid=""

fail=0
echo "$r1" | grep -q '"recebido": "oi"' || { echo "POST /eco inesperado: $r1"; fail=1; }
echo "$r2" | grep -q '"ok": true' || { echo "GET /saude inesperado: $r2"; fail=1; }
[ "$r3" = "404" ] || { echo "esperado 404, obtido $r3"; fail=1; }
grep -q "POST /eco -> 200" "$tmp/log" || { echo "log sem 'POST /eco -> 200'"; cat "$tmp/log"; fail=1; }
grep -q "GET /nao-existe -> 404" "$tmp/log" || { echo "log sem 404"; cat "$tmp/log"; fail=1; }

[ "$fail" = 0 ] && echo "http_test ok"
exit "$fail"
