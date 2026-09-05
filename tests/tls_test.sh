#!/usr/bin/env sh
# Integration test for the TLS layer (src/runtime/tls.*) through the Redis
# connector: spins up a mock Redis server speaking RESP over TLS (python3 ssl
# module, self-signed certificate generated at test time with the openssl
# CLI — the `cryptography` package is not required) and runs `tilt executar`
# on fixtures/redis_tls.tilt, checking:
#   1. with TILT_TLS_SKIP_VERIFY=1, both activation paths work end-to-end:
#      rediss:// URL and {tls: verdadeiro} over a plain redis:// URL;
#   2. without the env, the self-signed certificate is rejected and the error
#      message mentions TILT_TLS_SKIP_VERIFY;
#   3. the mock's request log shows the RESP commands arrived.
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/redis_tls.tilt}"
PORT_BASE="${TILT_TEST_PORT:-8691}"

command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste TLS"
  exit 0
}
command -v openssl >/dev/null 2>&1 || {
  echo "openssl CLI ausente; pulando o teste TLS"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# --- cert auto-assinado (CN=localhost) ---------------------------------------
openssl req -x509 -newkey rsa:2048 -keyout "$tmp/key.pem" -out "$tmp/cert.pem" \
  -sha256 -days 1 -nodes -subj "/CN=localhost" >/dev/null 2>&1

# --- mock RESP sobre TLS ------------------------------------------------------
python3 - "$PORT_BASE" "$tmp/porta" "$tmp/log" "$tmp/cert.pem" "$tmp/key.pem" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import socketserver
import ssl
import sys

port_base = int(sys.argv[1])
port_file = sys.argv[2]
log_path = sys.argv[3]
cert_path = sys.argv[4]
key_path = sys.argv[5]

log = open(log_path, "a", encoding="utf-8")

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert_path, key_path)

store = {}


class RespReader:
    def __init__(self, conn):
        self.conn = conn
        self.buf = b""

    def read_line(self):
        while b"\r\n" not in self.buf:
            chunk = self.conn.recv(4096)
            if not chunk:
                return None
            self.buf += chunk
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def read_n(self, n):
        while len(self.buf) < n:
            chunk = self.conn.recv(4096)
            if not chunk:
                return None
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def read_command(self):
        line = self.read_line()
        if line is None or not line.startswith(b"*"):
            return None
        nargs = int(line[1:])
        args = []
        for _ in range(nargs):
            hdr = self.read_line()
            if hdr is None or not hdr.startswith(b"$"):
                return None
            n = int(hdr[1:])
            data = self.read_n(n)
            if data is None or self.read_n(2) != b"\r\n":
                return None
            args.append(data.decode())
        return args


class RedisTlsMock(socketserver.BaseRequestHandler):
    def handle(self):
        conn = ctx.wrap_socket(self.request, server_side=True)
        reader = RespReader(conn)
        while True:
            args = reader.read_command()
            if args is None:
                return
            cmd = args[0].upper()
            if cmd == "PING":
                conn.sendall(b"+PONG\r\n")
            elif cmd == "GET":
                log.write("GET %s\n" % args[1])
                log.flush()
                valor = store.get(args[1])
                if valor is None:
                    conn.sendall(b"$-1\r\n")
                else:
                    b = valor.encode()
                    conn.sendall(b"$%d\r\n" % len(b) + b + b"\r\n")
            elif cmd == "SET":
                store[args[1]] = args[2]
                log.write("SET %s\n" % args[1])
                log.flush()
                conn.sendall(b"+OK\r\n")
            else:
                conn.sendall(b"-ERR unknown command\r\n")


class Servidor(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


srv = None
for porta in range(port_base, port_base + 20):
    try:
        srv = Servidor(("127.0.0.1", porta), RedisTlsMock)
        break
    except OSError:
        continue
if srv is None:
    sys.exit("nenhuma porta livre a partir de %d" % port_base)

with open(port_file, "w", encoding="utf-8") as f:
    f.write(str(porta))
srv.serve_forever()
PYEOF
mock_pid=$!

for _ in $(seq 1 50); do
  [ -s "$tmp/porta" ] && break
  sleep 0.1
done
[ -s "$tmp/porta" ] || { echo "mock Redis-TLS nao iniciou"; cat "$tmp/mock_out"; exit 1; }
PORTA=$(cat "$tmp/porta")

fail=0

# --- (1) sem TILT_TLS_SKIP_VERIFY: cert auto-assinado rejeitado com dica ------
if err=$(env REDIS_URL="rediss://127.0.0.1:$PORTA" \
             REDIS_URL2="redis://127.0.0.1:$PORTA" \
             "$BIN" executar "$FIXTURE" 2>&1); then
  echo "execucao sem TILT_TLS_SKIP_VERIFY deveria falhar (cert auto-assinado)"
  fail=1
else
  echo "$err" | grep -q "TILT_TLS_SKIP_VERIFY" || {
    echo "erro sem mencao a TILT_TLS_SKIP_VERIFY: $err"; fail=1;
  }
fi

# --- (2) com TILT_TLS_SKIP_VERIFY=1: roundtrip rediss:// e {tls: verdadeiro} --
out=$(
  env TILT_TLS_SKIP_VERIFY=1 \
    REDIS_URL="rediss://127.0.0.1:$PORTA" \
    REDIS_URL2="redis://127.0.0.1:$PORTA" \
    "$BIN" executar "$FIXTURE"
)

echo "$out" | grep -q "valor-tls" || {
  echo "saida sem 'valor-tls' (rediss://): $out"; fail=1;
}
echo "$out" | grep -q "valor-op" || {
  echo "saida sem 'valor-op' ({tls: verdadeiro}): $out"; fail=1;
}

kill "$mock_pid" 2>/dev/null || true
mock_pid=""

# --- (3) log do mock: os quatro comandos chegaram -----------------------------
check_log() {
  esperado="$1"; shift
  n=$(grep -c "^$esperado\$" "$tmp/log" || true)
  [ "$n" = "$1" ] || { echo "esperado $1 x '$esperado', obtido $n"; cat "$tmp/log"; fail=1; }
}
check_log "SET chave-tls" 1
check_log "GET chave-tls" 1
check_log "SET chave-op" 1
check_log "GET chave-op" 1

[ "$fail" = 0 ] && echo "tls_test ok"
exit "$fail"
