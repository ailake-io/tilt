#!/usr/bin/env sh
# Integration test for `tilt servir`: start the server, drive it with curl,
# check the responses and the request log, and confirm a clean exit.
# Also covers keep-alive (two requests on one connection) and concurrency
# (parallel clients on separate connections).
set -eu

BIN="$1"
FIXTURE="$2"
PORT="${TILT_TEST_PORT:-8611}"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste HTTP"
  exit 0
}

wait_listen() {
  for _ in $(seq 1 50); do
    grep -q "escutando" "$1" 2>/dev/null && return 0
    sleep 0.1
  done
  echo "servidor nao iniciou; log:"; cat "$1"; return 1
}

# --- 1) fluxo basico: 3 requisicoes em conexoes separadas -------------------
tmp=$(mktemp -d)
trap 'kill "$srv_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

"$BIN" servir "$FIXTURE" --porta "$PORT" --requisicoes 3 >"$tmp/log" 2>&1 &
srv_pid=$!
wait_listen "$tmp/log"

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

# --- 2) keep-alive: duas requisicoes na MESMA conexao (uma so delas close) --
if command -v python3 >/dev/null 2>&1; then
  ka_port=$((PORT + 1))
  "$BIN" servir "$FIXTURE" --porta "$ka_port" --requisicoes 2 >"$tmp/log_ka" 2>&1 &
  srv_pid=$!
  wait_listen "$tmp/log_ka"

  n=$(python3 - "$ka_port" <<'PYEOF'
import socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=5)
# manda as duas de uma vez: tambem exercita o pipeline do buffer da conexao
s.sendall(b"GET /saude HTTP/1.1\r\nHost: x\r\n\r\n"
          b"GET /saude HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
data = b""
while True:
    chunk = s.recv(4096)
    if not chunk:
        break
    data += chunk
print(data.count(b"HTTP/1.1 200 OK"))
PYEOF
)
  wait "$srv_pid"
  srv_pid=""
  [ "$n" = "2" ] || { echo "keep-alive: esperado 2 respostas 200, obtido $n"; fail=1; }
else
  echo "python3 ausente; pulando o teste de keep-alive"
fi

# --- 3) concorrencia: 8 clientes em paralelo em conexoes separadas ----------
cx_port=$((PORT + 2))
"$BIN" servir "$FIXTURE" --porta "$cx_port" --requisicoes 8 >"$tmp/log_cx" 2>&1 &
srv_pid=$!
wait_listen "$tmp/log_cx"

i=1
while [ "$i" -le 8 ]; do
  curl -s -o "$tmp/cx_$i" "localhost:$cx_port/saude" &
  i=$((i + 1))
done
wait
wait "$srv_pid"
srv_pid=""

i=1
while [ "$i" -le 8 ]; do
  grep -q '"ok": true' "$tmp/cx_$i" || { echo "cliente $i sem resposta ok"; fail=1; }
  i=$((i + 1))
done

[ "$fail" = 0 ] && echo "http_test ok"
exit "$fail"
