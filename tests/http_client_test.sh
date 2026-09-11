#!/usr/bin/env sh
# Integration test for the generic HTTP client exposed to the language
# (builtins http_get_json/http_post_json and stdlib rede.tilt): spins up a
# mock HTTP server in python3 (http.server) that echoes method, custom headers
# and JSON body, then runs `tilt executar` on fixtures covering GET/POST with
# headers, JSON roundtrip, a 404 captured with tentar/capturar, invalid-URL
# validation and the rede.tilt roundtrip.
set -eu

BIN="$1"
PORT_BASE="${TILT_TEST_PORT:-8651}"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste http_client"
  exit 0
}
command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste http_client"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# --- mock HTTP generico ------------------------------------------------------
python3 - "$PORT_BASE" "$tmp/porta" "$tmp/log" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import http.server
import json
import socket as _socket
import sys

# HTTPServer.__init__ chama socket.getfqdn(host): no macOS a resolucao DNS
# reversa pode travar no runner (timeout de varios segundos) e o mock nunca
# chega a escrever o arquivo de porta. O nome do servidor e irrelevante.
_socket.getfqdn = lambda host="": "localhost"

port_base = int(sys.argv[1])
port_file = sys.argv[2]
log_path = sys.argv[3]
log = open(log_path, "a", encoding="utf-8")


class MockHttp(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _responder(self, code, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _registrar(self, metodo):
        tok = self.headers.get("X-Token", "-")
        log.write("%s %s x-token=%s\n" % (metodo, self.path, tok))
        if metodo == "POST":
            ct = self.headers.get("Content-Type", "-")
            log.write("CT %s\n" % ("ok" if ct == "application/json" else "ruim:" + ct))
        log.flush()

    def do_GET(self):
        self._registrar("GET")
        if self.path == "/status":
            self._responder(200, {"status": "ok", "token": self.headers.get("X-Token", "-")})
        else:
            self._responder(404, {"erro": "nao encontrado"})

    def do_POST(self):
        self._registrar("POST")
        length = int(self.headers.get("Content-Length", 0))
        payload = json.loads(self.rfile.read(length).decode("utf-8") or "{}")
        self._responder(200, {"recebido": payload,
                              "ct": self.headers.get("Content-Type", "-")})


for porta in range(port_base, port_base + 20):
    try:
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", porta), MockHttp)
        break
    except OSError:
        continue
else:
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
[ -s "$tmp/porta" ] || { echo "mock HTTP nao iniciou"; cat "$tmp/mock_out"; exit 1; }
PORTA=$(cat "$tmp/porta")

# --- builtins http_get_json/http_post_json ------------------------------------
out=$(env TILT_HTTP_BASE="http://127.0.0.1:$PORTA" \
  "$BIN" executar "${2:-${0%/*}/fixtures/http_generico.tilt}")

# --- roundtrip stdlib rede.tilt ------------------------------------------------
out_rede=$(env TILT_HTTP_BASE="http://127.0.0.1:$PORTA" \
  "$BIN" executar "${3:-${0%/*}/fixtures/rede_roundtrip.tilt}")

kill "$mock_pid" 2>/dev/null || true
mock_pid=""

fail=0

# fixture http_generico: GET com header custom, POST com corpo JSON, 404
# capturado, URL invalida capturada
cab=$(printf '%s\n' "$out" | sed -n 1p)
[ "$cab" = "== http ==" ] || { echo "http sem cabecalho: $out"; fail=1; }
linha2=$(printf '%s\n' "$out" | sed -n 2p)
[ "$linha2" = "ok segredo123" ] || { echo "GET com header inesperado: $linha2"; fail=1; }
linha3=$(printf '%s\n' "$out" | sed -n 3p)
[ "$linha3" = "tilt 42 application/json" ] ||
  { echo "POST roundtrip inesperado: $linha3"; fail=1; }
linha4=$(printf '%s\n' "$out" | sed -n 4p)
[ "$linha4" = "404 capturado" ] || { echo "404 nao capturado: $linha4"; fail=1; }
linha5=$(printf '%s\n' "$out" | sed -n 5p)
[ "$linha5" = "url invalida capturada" ] ||
  { echo "URL invalida nao capturada: $linha5"; fail=1; }
linha6=$(printf '%s\n' "$out" | sed -n 6p)
[ -z "$linha6" ] || { echo "http com saida extra: $out"; fail=1; }

# fixture rede_roundtrip: get_json/post_json com a mesma saida espelhada
cab_rede=$(printf '%s\n' "$out_rede" | sed -n 1p)
[ "$cab_rede" = "== rede ==" ] || { echo "rede sem cabecalho: $out_rede"; fail=1; }
rl2=$(printf '%s\n' "$out_rede" | sed -n 2p)
[ "$rl2" = "ok abc" ] || { echo "rede.get_json inesperado: $rl2"; fail=1; }
rl3=$(printf '%s\n' "$out_rede" | sed -n 3p)
[ "$rl3" = "verdadeiro tilt application/json" ] ||
  { echo "rede.post_json inesperado: $rl3"; fail=1; }
rl4=$(printf '%s\n' "$out_rede" | sed -n 4p)
[ -z "$rl4" ] || { echo "rede com saida extra: $out_rede"; fail=1; }

# --- log do mock ----------------------------------------------------------------
get_status=$(grep -c "^GET /status x-token=segredo123$" "$tmp/log" || true)
[ "$get_status" = "1" ] || { echo "esperado 1 GET /status com x-token, obtido $get_status"; cat "$tmp/log"; fail=1; }
get_falta=$(grep -c "^GET /falta x-token=-$" "$tmp/log" || true)
[ "$get_falta" = "1" ] || { echo "esperado 1 GET /falta, obtido $get_falta"; cat "$tmp/log"; fail=1; }
post_echo=$(grep -c "^POST /echo x-token=segredo123$" "$tmp/log" || true)
[ "$post_echo" = "1" ] || { echo "esperado 1 POST /echo com x-token, obtido $post_echo"; cat "$tmp/log"; fail=1; }
ct_ok=$(grep -c "^CT ok$" "$tmp/log" || true)
[ "$ct_ok" = "2" ] || { echo "esperado 2 POST com Content-Type json, obtido $ct_ok"; cat "$tmp/log"; fail=1; }
if grep -q "^CT ruim" "$tmp/log"; then
  echo "mock recebeu Content-Type errado:"; cat "$tmp/log"; fail=1
fi

[ "$fail" = 0 ] && echo "http_client_test ok"
exit "$fail"
