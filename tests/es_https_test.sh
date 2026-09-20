#!/usr/bin/env sh
# Elasticsearch/OpenSearch sobre HTTPS (esquemas elasticsearch+https:// e
# opensearch+https://): um servidor HTTPS em python3 com certificado
# autoassinado (gerado com a CLI openssl) responde GET / e POST /<indice>/_search;
# o curl confia no certificado via CURL_CA_BUNDLE. Sem curl/python3/openssl, pula.
set -eu

BIN="$1"
PORT="${TILT_TEST_PORT:-8791}"

for cmd in curl python3 openssl; do
  command -v "$cmd" >/dev/null 2>&1 || { echo "$cmd ausente; pulando es_https"; exit 0; }
done

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

openssl req -x509 -newkey rsa:2048 -nodes -keyout "$tmp/key.pem" -out "$tmp/cert.pem" \
  -days 2 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
  >/dev/null 2>&1 || { echo "openssl nao gerou o certificado; pulando es_https"; exit 0; }

python3 - "$PORT" "$tmp" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import http.server, json, socket as _socket, ssl, sys

_socket.getfqdn = lambda host="": "localhost"
port, tmp = int(sys.argv[1]), sys.argv[2]


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _json(self, obj):
        raw = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):  # noqa: N802
        self._json({"name": "no-https", "version": {"number": "8.0.0"}})

    def do_POST(self):  # noqa: N802
        n = int(self.headers.get("Content-Length", 0))
        self.rfile.read(n)
        self._json({"hits": {"total": {"value": 1},
                             "hits": [{"_id": "1", "_source": {"titulo": "seguro"}}]}})


srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(tmp + "/cert.pem", tmp + "/key.pem")
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
open(tmp + "/porta", "w").write(str(port))
srv.serve_forever()
PYEOF
mock_pid=$!
for _ in $(seq 1 50); do [ -f "$tmp/porta" ] && break; sleep 0.1; done
[ -f "$tmp/porta" ] || { echo "mock https nao subiu:"; cat "$tmp/mock_out"; exit 1; }

cat >"$tmp/prog.tilt" <<EOF2
pipeline p:
  passos:
    - r = es_buscar "elasticsearch+https://127.0.0.1:$PORT/artigos", "{\"query\": {\"match_all\": {}}}"
    - imprimir "es:", r.total, r.hits[0].titulo
    - o = es_buscar "opensearch+https://127.0.0.1:$PORT/artigos", "{\"query\": {\"match_all\": {}}}"
    - imprimir "os:", o.total
    - info = es_executar "elasticsearch+https://127.0.0.1:$PORT", "GET", "/"
    - imprimir "info:", info.name
EOF2

fail=0
out=$(cd "$tmp" && CURL_CA_BUNDLE="$tmp/cert.pem" "$BIN" executar prog.tilt 2>&1) || {
  echo "execucao falhou: $out"; exit 1; }
printf '%s\n' "$out"
for esperado in "es: 1 seguro" "os: 1" "info: no-https"; do
  echo "$out" | grep -qF "$esperado" || { echo "FALHA: sem '$esperado'"; fail=1; }
done

# sem confiar no certificado, o TLS falha (nao cai para http nem ignora o erro)
if out2=$(cd "$tmp" && "$BIN" executar prog.tilt 2>&1); then
  echo "FALHA: certificado autoassinado deveria ser recusado"; fail=1
fi

[ "$fail" = 0 ] && echo "es_https_test ok"
exit "$fail"
