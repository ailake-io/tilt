#!/usr/bin/env sh
# Integration test for the S3 connector (`ler_s3`/`escrever_s3`): spins up a
# mock S3 server in python3 (http.server) that validates the AWS SigV4
# signature (recomputed with hashlib/hmac) on every request, then runs
# `tilt executar` on a roundtrip fixture (PUT + GET) and checks the printed
# content plus the mock's request log (exactly 1 PUT + 1 GET, valid signature).
set -eu

BIN="$1"
PORT_BASE="${TILT_TEST_PORT:-8631}"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste S3"
  exit 0
}
command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste S3"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# --- mock S3 com validacao SigV4 (query string faz parte da assinatura) ---------
python3 - "$PORT_BASE" "$tmp/porta" "$tmp/log" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import hashlib
import hmac
import http.server
import sys
from urllib.parse import parse_qsl, quote

port_base = int(sys.argv[1])
port_file = sys.argv[2]
log_path = sys.argv[3]

SECRET = "chave-de-teste-1234567890abcdef"
REGION = "us-east-1"
SERVICE = "s3"

store = {}
log = open(log_path, "a", encoding="utf-8")


def canonical_query(qs):
    # Mesma regra do cliente SigV4: pares ordenados por chave encoded,
    # "chave=valor" ambos encoded (RFC 3986 unreserved), '&' separador;
    # valor vazio vira so a chave encoded.
    if not qs:
        return ""
    enc = [(quote(k, safe="-_.~"), quote(v, safe="-_.~"))
           for k, v in parse_qsl(qs, keep_blank_values=True)]
    enc.sort()
    return "&".join(k + "=" + v if v else k for k, v in enc)


def sig_ok(method, path, qs, headers, payload):
    auth = headers.get("Authorization", "")
    parts = {}
    for item in auth[len("AWS4-HMAC-SHA256 "):].split(","):
        k, _, v = item.strip().partition("=")
        parts[k] = v
    cred = parts.get("Credential", "")
    ak, scope = cred.split("/", 1)
    date_stamp, region, service, terminal = scope.split("/")
    signed = parts.get("SignedHeaders", "").split(";")
    amz_date = headers.get("X-Amz-Date", "")
    payload_hash = headers.get("X-Amz-Content-Sha256", "")

    if hashlib.sha256(payload).hexdigest() != payload_hash:
        return False, "payload-hash"

    canonical_headers = ""
    for h in signed:
        valor = headers.get(h) if headers.get(h) is not None else headers.get(h.lower())
        if h == "host":
            valor = headers["Host"]
        if valor is None:
            return False, "header-ausente:" + h
        canonical_headers += h + ":" + valor.strip() + "\n"
    canonical = (method + "\n" + path + "\n" + canonical_query(qs) + "\n" +
                 canonical_headers + "\n" + ";".join(signed) + "\n" + payload_hash)
    string_to_sign = ("AWS4-HMAC-SHA256\n" + amz_date + "\n" + scope + "\n" +
                      hashlib.sha256(canonical.encode()).hexdigest())
    k = hmac.new(("AWS4" + SECRET).encode(), date_stamp.encode(), hashlib.sha256).digest()
    k = hmac.new(k, region.encode(), hashlib.sha256).digest()
    k = hmac.new(k, service.encode(), hashlib.sha256).digest()
    k = hmac.new(k, terminal.encode(), hashlib.sha256).digest()
    esperado = hmac.new(k, string_to_sign.encode(), hashlib.sha256).hexdigest()
    return hmac.compare_digest(esperado, parts.get("Signature", "")), "assinatura"


def xml_esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
             .replace('"', "&quot;").replace("'", "&#39;"))


class MockS3(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _responder(self, code, body=b""):
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _handle(self, method):
        length = int(self.headers.get("Content-Length", 0))
        payload = self.rfile.read(length) if length else b""
        path, _, qs = self.path.partition("?")
        ok, motivo = sig_ok(method, path, qs, self.headers, payload)
        if not ok:
            log.write("%s %s sig=INVALIDA %s\n" % (method, self.path, motivo))
            log.flush()
            self._responder(403, b'{"message":"assinatura invalida"}')
            return
        params = dict(parse_qsl(qs, keep_blank_values=True))
        if method == "PUT":
            store[path] = payload
            log.write("PUT %s sig=ok\n" % path)
            log.flush()
            self._responder(200, b"")
        elif method == "DELETE":
            if path in store:
                del store[path]
                self._responder(204, b"")
            else:
                self._responder(404, b'{"message":"nao encontrado"}')
            log.write("DELETE %s sig=ok\n" % path)
            log.flush()
        elif "list-type" in params:
            prefix = params.get("prefix", "")
            maxk = int(params.get("max-keys", "1000"))
            base = path + "/"  # path == "/bucket"
            chaves = sorted(k for k in store if k.startswith(base + prefix))
            chaves = [k[len(base):] for k in chaves][:maxk]
            corpos = "".join("<Contents><Key>%s</Key></Contents>" % xml_esc(k)
                             for k in chaves)
            xml = ('<?xml version="1.0" encoding="UTF-8"?>'
                   '<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">'
                   "<Name>%s</Name><Prefix>%s</Prefix><KeyCount>%d</KeyCount>"
                   "%s</ListBucketResult>" %
                   (xml_esc(path.strip("/")), xml_esc(prefix), len(chaves), corpos))
            log.write("LIST prefix=%s max=%d sig=ok\n" % (prefix, maxk))
            log.flush()
            self._responder(200, xml.encode())
        else:
            log.write("GET %s sig=ok\n" % path)
            log.flush()
            if path in store:
                self._responder(200, store[path])
            else:
                self._responder(404, b'{"message":"nao encontrado"}')

    def do_PUT(self):
        self._handle("PUT")

    def do_GET(self):
        self._handle("GET")

    def do_DELETE(self):
        self._handle("DELETE")


for porta in range(port_base, port_base + 20):
    try:
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", porta), MockS3)
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
[ -s "$tmp/porta" ] || { echo "mock S3 nao iniciou"; cat "$tmp/mock_out"; exit 1; }
PORTA=$(cat "$tmp/porta")

# --- roundtrip via tilt executar -------------------------------------------------
env_s3() {
  env \
    AWS_ACCESS_KEY_ID=AKIAEXEMPLO1234567 \
    AWS_SECRET_ACCESS_KEY=chave-de-teste-1234567890abcdef \
    AWS_SESSION_TOKEN= \
    AWS_REGION=us-east-1 \
    S3_ENDPOINT="http://127.0.0.1:$PORTA" \
    "$@"
}

out=$(env_s3 "$BIN" executar "${2:-${0%/*}/fixtures/s3_roundtrip.tilt}")
out_ops=$(env_s3 "$BIN" executar "${3:-${0%/*}/fixtures/s3_ops.tilt}")

kill "$mock_pid" 2>/dev/null || true
mock_pid=""

fail=0
echo "$out" | grep -q "ola s3" || { echo "saida sem 'ola s3': $out"; fail=1; }

# listar_s3/apagar_s3: 2 listagens com prefixo (2 chaves, depois 1) + 1 delete
echo "$out_ops" | grep -q "^== pipeline ops ==$" || { echo "ops sem cabecalho: $out_ops"; fail=1; }
linha1=$(printf '%s\n' "$out_ops" | sed -n 2p)
linha2=$(printf '%s\n' "$out_ops" | sed -n 3p)
linha3=$(printf '%s\n' "$out_ops" | sed -n 4p)
linha4=$(printf '%s\n' "$out_ops" | sed -n 5p)
[ "$linha1" = "relatorios/a.txt" ] || { echo "ops linha 1 inesperada: $linha1"; fail=1; }
[ "$linha2" = "relatorios/b.txt" ] || { echo "ops linha 2 inesperada: $linha2"; fail=1; }
[ "$linha3" = "relatorios/b.txt" ] || { echo "ops linha 3 inesperada (pos-delete): $linha3"; fail=1; }
[ -z "$linha4" ] || { echo "ops com saida extra: $out_ops"; fail=1; }

puts=$(grep -c "^PUT .* sig=ok$" "$tmp/log" || true)
gets=$(grep -c "^GET .* sig=ok$" "$tmp/log" || true)
[ "$puts" = "4" ] || { echo "esperado 4 PUT com assinatura ok, obtido $puts"; cat "$tmp/log"; fail=1; }
[ "$gets" = "1" ] || { echo "esperado 1 GET com assinatura ok, obtido $gets"; cat "$tmp/log"; fail=1; }
lists=$(grep -c "^LIST prefix=relatorios/ max=100 sig=ok$" "$tmp/log" || true)
[ "$lists" = "2" ] || { echo "esperado 2 LIST com prefixo 'relatorios/', obtido $lists"; cat "$tmp/log"; fail=1; }
dels=$(grep -c "^DELETE /bucket/relatorios/a\.txt sig=ok$" "$tmp/log" || true)
[ "$dels" = "1" ] || { echo "esperado 1 DELETE de relatorios/a.txt, obtido $dels"; cat "$tmp/log"; fail=1; }
if grep -q "sig=INVALIDA" "$tmp/log"; then
  echo "mock rejeitou assinatura:"; cat "$tmp/log"; fail=1
fi

[ "$fail" = 0 ] && echo "s3_test ok"
exit "$fail"
