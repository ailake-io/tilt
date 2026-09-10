#!/usr/bin/env sh
# Integration test for the S3 connector (`ler_s3`/`escrever_s3`/`listar_s3`/
# `apagar_s3`/`copiar_s3`/`cabecalho_s3`/multipart): spins up a mock S3
# server in python3 (http.server) that validates the AWS SigV4 signature
# (recomputed with hashlib/hmac) on every request, then runs `tilt executar`
# on fixtures: roundtrip (PUT + GET), list/delete, copy/head/multipart (incl.
# validacao de erros de ordem/duplicata de partes) and checks the printed
# content plus the mock's request log (assinaturas ok, comandos esperados).
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
import re
import socket as _socket
import sys
from urllib.parse import parse_qsl, quote, unquote

# HTTPServer.__init__ chama socket.getfqdn(host): no macOS a resolucao DNS
# reversa pode travar no runner (timeout de varios segundos) e o mock nunca
# chega a escrever o arquivo de porta. O nome do servidor e irrelevante.
_socket.getfqdn = lambda host="": "localhost"

port_base = int(sys.argv[1])
port_file = sys.argv[2]
log_path = sys.argv[3]

SECRET = "chave-de-teste-1234567890abcdef"
REGION = "us-east-1"
SERVICE = "s3"

store = {}
content_types = {}  # path -> Content-Type do PUT (HeadObject devolve)
uploads = {}  # uploadId -> {"path": path, "partes": {n: bytes}}
mp_seq = [0]
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
            if "partNumber" in params:
                self._mp_parte(path, params, payload)
            elif self.headers.get("X-Amz-Copy-Source"):
                self._copiar(path, self.headers["X-Amz-Copy-Source"])
            else:
                store[path] = payload
                content_types[path] = self.headers.get(
                    "Content-Type", "application/octet-stream")
                log.write("PUT %s sig=ok\n" % path)
                log.flush()
                self._responder(200, b"")
        elif method == "POST":
            if "uploads" in params:
                self._mp_iniciar(path)
            elif "uploadId" in params:
                self._mp_concluir(path, params["uploadId"], payload)
            else:
                self._responder(404, b'{"message":"nao encontrado"}')
        elif method == "DELETE":
            if "uploadId" in params:
                self._mp_abortar(path, params["uploadId"])
            elif path in store:
                del store[path]
                self._responder(204, b"")
                log.write("DELETE %s sig=ok\n" % path)
                log.flush()
            else:
                self._responder(404, b'{"message":"nao encontrado"}')
        elif method == "HEAD":
            log.write("HEAD %s sig=ok\n" % path)
            log.flush()
            if path in store:
                corpo = store[path]
                self.send_response(200)
                self.send_header("Content-Length", str(len(corpo)))
                self.send_header("Content-Type", content_types.get(
                    path, "application/octet-stream"))
                self.send_header("ETag", '"%s"' % hashlib.md5(corpo).hexdigest())
                self.send_header("Last-Modified",
                                 "Wed, 01 Jan 2025 00:00:00 GMT")
                self.end_headers()
            else:
                self._responder(404, b"")
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

    def _copiar(self, path_destino, fonte):
        # x-amz-copy-source: "/bucket/chave" (chave URI-encoded).
        fonte_path = "/" + unquote(fonte.lstrip("/"))
        if fonte_path not in store:
            self._responder(404, b'{"message":"origem nao encontrada"}')
            return
        store[path_destino] = store[fonte_path]
        content_types[path_destino] = content_types.get(
            fonte_path, "application/octet-stream")
        log.write("COPY %s <- %s sig=ok\n" % (path_destino, fonte_path))
        log.flush()
        etag = hashlib.md5(store[path_destino]).hexdigest()
        xml = ('<?xml version="1.0" encoding="UTF-8"?>'
               '<CopyObjectResult '
               'xmlns="http://s3.amazonaws.com/doc/2006-03-01/">'
               "<ETag>\"%s\"</ETag></CopyObjectResult>" % etag)
        self._responder(200, xml.encode())

    def _mp_iniciar(self, path):
        mp_seq[0] += 1
        uid = "upload-%d" % mp_seq[0]
        uploads[uid] = {"path": path, "partes": {}}
        log.write("MP-INIT %s uid=%s sig=ok\n" % (path, uid))
        log.flush()
        xml = ('<?xml version="1.0" encoding="UTF-8"?>'
               '<InitiateMultipartUploadResult '
               'xmlns="http://s3.amazonaws.com/doc/2006-03-01/">'
               "<UploadId>%s</UploadId></InitiateMultipartUploadResult>" % uid)
        self._responder(200, xml.encode())

    def _mp_parte(self, path, params, payload):
        uid = params["uploadId"]
        numero = int(params["partNumber"])
        up = uploads.get(uid)
        if up is None:
            self._responder(404, b'{"message":"upload nao encontrado"}')
            return
        up["partes"][numero] = payload
        etag = '"parte-%s-%d"' % (hashlib.md5(payload).hexdigest()[:8], numero)
        log.write("MP-PART %s n=%d sig=ok\n" % (path, numero))
        log.flush()
        self.send_response(200)
        self.send_header("ETag", etag)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _mp_concluir(self, path, uid, payload):
        up = uploads.get(uid)
        if up is None:
            self._responder(404, b'{"message":"upload nao encontrado"}')
            return
        texto = payload.decode("utf-8")
        numeros = [int(n) for n in re.findall(r"<PartNumber>(\d+)</PartNumber>", texto)]
        faltando = [n for n in numeros if n not in up["partes"]]
        if faltando:
            self._responder(400, b'{"message":"partes ausentes"}')
            return
        conteudo = b"".join(up["partes"][n] for n in numeros)
        store[path] = conteudo
        etag = '"mp-%s"' % hashlib.md5(conteudo).hexdigest()
        log.write("MP-DONE %s partes=%s sig=ok\n" % (path, ",".join(map(str, numeros))))
        log.flush()
        del uploads[uid]
        xml = ('<?xml version="1.0" encoding="UTF-8"?>'
               '<CompleteMultipartUploadResult '
               'xmlns="http://s3.amazonaws.com/doc/2006-03-01/">'
               "<ETag>%s</ETag></CompleteMultipartUploadResult>" % etag)
        self._responder(200, xml.encode())

    def _mp_abortar(self, path, uid):
        if uid in uploads:
            del uploads[uid]
        log.write("MP-ABORT %s sig=ok\n" % path)
        log.flush()
        self._responder(204, b"")

    def do_PUT(self):
        self._handle("PUT")

    def do_POST(self):
        self._handle("POST")

    def do_HEAD(self):
        self._handle("HEAD")

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
out_extras=$(env_s3 "$BIN" executar "${4:-${0%/*}/fixtures/s3_extras.tilt}")

# casos de erro: concluir com partes fora de ordem / duplicada devem falhar
erro_ordem_status=0
out_erro_ordem=$(env_s3 "$BIN" executar "${5:-${0%/*}/fixtures/s3_extras_erro_ordem.tilt}" 2>&1) ||
  erro_ordem_status=1
erro_dup_status=0
out_erro_dup=$(env_s3 "$BIN" executar "${6:-${0%/*}/fixtures/s3_extras_erro_dup.tilt}" 2>&1) ||
  erro_dup_status=1

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
[ "$puts" = "5" ] || { echo "esperado 5 PUT com assinatura ok, obtido $puts"; cat "$tmp/log"; fail=1; }
[ "$gets" = "3" ] || { echo "esperado 3 GET com assinatura ok, obtido $gets"; cat "$tmp/log"; fail=1; }
lists=$(grep -c "^LIST prefix=relatorios/ max=100 sig=ok$" "$tmp/log" || true)
[ "$lists" = "2" ] || { echo "esperado 2 LIST com prefixo 'relatorios/', obtido $lists"; cat "$tmp/log"; fail=1; }
dels=$(grep -c "^DELETE /bucket/relatorios/a\.txt sig=ok$" "$tmp/log" || true)
[ "$dels" = "1" ] || { echo "esperado 1 DELETE de relatorios/a.txt, obtido $dels"; cat "$tmp/log"; fail=1; }

# --- extras: copiar/cabecalho/multipart (fixture s3_extras.tilt) --------------
cabec_extras=$(printf '%s\n' "$out_extras" | sed -n 1p)
[ "$cabec_extras" = "== pipeline extras ==" ] || { echo "extras sem cabecalho: $out_extras"; fail=1; }
# copiar: conteudo identico no destino
copia=$(printf '%s\n' "$out_extras" | sed -n 2p)
[ "$copia" = "conteudo-original" ] || { echo "copia com conteudo inesperado: $copia"; fail=1; }
# cabecalho: content-length (17 = len('conteudo-original')), content-type, etag
tam_esperado=$(printf '%s' "conteudo-original" | wc -c | tr -d ' ')
tam_lido=$(printf '%s\n' "$out_extras" | sed -n 3p)
[ "$tam_lido" = "$tam_esperado" ] || { echo "content-length: esperado $tam_esperado, obtido $tam_lido"; fail=1; }
ct_lido=$(printf '%s\n' "$out_extras" | sed -n 4p)
[ "$ct_lido" = "application/octet-stream" ] || { echo "content-type inesperado: $ct_lido"; fail=1; }
etag_lido=$(printf '%s\n' "$out_extras" | sed -n 5p)
printf '%s' "$etag_lido" | grep -qE '^"[0-9a-f]{32}"$' ||
  { echo "etag inesperado: $etag_lido"; fail=1; }
# multipart: conteudo concatenado das 2 partes + etag final
grande=$(printf '%s\n' "$out_extras" | sed -n 6p)
[ "$grande" = "parte-A-parte-B" ] || { echo "multipart com conteudo inesperado: $grande"; fail=1; }
etag_mp=$(printf '%s\n' "$out_extras" | sed -n 7p)
printf '%s' "$etag_mp" | grep -qE '^"mp-[0-9a-f]{32}"$' ||
  { echo "etag final inesperado: $etag_mp"; fail=1; }
extra_linha=$(printf '%s\n' "$out_extras" | sed -n 8p)
[ -z "$extra_linha" ] || { echo "extras com saida extra: $out_extras"; fail=1; }

# erros de validacao das partes: fora de ordem e duplicada
[ "$erro_ordem_status" = "1" ] ||
  { echo "esperada falha em partes fora de ordem, obteve sucesso: $out_erro_ordem"; fail=1; }
echo "$out_erro_ordem" | grep -q "ordem crescente" ||
  { echo "erro de ordem sem mensagem clara: $out_erro_ordem"; fail=1; }
[ "$erro_dup_status" = "1" ] ||
  { echo "esperada falha em parte duplicada, obteve sucesso: $out_erro_dup"; fail=1; }
echo "$out_erro_dup" | grep -q "ordem crescente" ||
  { echo "erro de duplicata sem mensagem clara: $out_erro_dup"; fail=1; }

# comandos no log do mock
copys=$(grep -c "^COPY /bucket/copia\.txt <- /bucket/origem\.txt sig=ok$" "$tmp/log" || true)
[ "$copys" = "1" ] || { echo "esperado 1 COPY, obtido $copys"; cat "$tmp/log"; fail=1; }
heads=$(grep -c "^HEAD /bucket/origem\.txt sig=ok$" "$tmp/log" || true)
[ "$heads" = "1" ] || { echo "esperado 1 HEAD, obtido $heads"; cat "$tmp/log"; fail=1; }
mpinit_grande=$(grep -c "^MP-INIT /bucket/grande\.bin uid=upload-[0-9]* sig=ok$" "$tmp/log" || true)
[ "$mpinit_grande" = "1" ] || { echo "esperado 1 MP-INIT de grande.bin, obtido $mpinit_grande"; cat "$tmp/log"; fail=1; }
mpinit_abort=$(grep -c "^MP-INIT /bucket/abortado\.bin uid=upload-[0-9]* sig=ok$" "$tmp/log" || true)
[ "$mpinit_abort" = "1" ] || { echo "esperado 1 MP-INIT de abortado.bin, obtido $mpinit_abort"; cat "$tmp/log"; fail=1; }
mpp1=$(grep -c "^MP-PART /bucket/grande\.bin n=1 sig=ok$" "$tmp/log" || true)
mpp2=$(grep -c "^MP-PART /bucket/grande\.bin n=2 sig=ok$" "$tmp/log" || true)
[ "$mpp1" = "1" ] || { echo "esperada 1 parte n=1 de grande.bin, obtido $mpp1"; cat "$tmp/log"; fail=1; }
[ "$mpp2" = "1" ] || { echo "esperada 1 parte n=2 de grande.bin, obtido $mpp2"; cat "$tmp/log"; fail=1; }
mpdone=$(grep -c "^MP-DONE /bucket/grande\.bin partes=1,2 sig=ok$" "$tmp/log" || true)
[ "$mpdone" = "1" ] || { echo "esperado 1 MP-DONE de grande.bin, obtido $mpdone"; cat "$tmp/log"; fail=1; }
mpabort=$(grep -c "^MP-ABORT /bucket/abortado\.bin sig=ok$" "$tmp/log" || true)
[ "$mpabort" = "1" ] || { echo "esperado 1 MP-ABORT de abortado.bin, obtido $mpabort"; cat "$tmp/log"; fail=1; }
if grep -q "sig=INVALIDA" "$tmp/log"; then
  echo "mock rejeitou assinatura:"; cat "$tmp/log"; fail=1
fi

[ "$fail" = 0 ] && echo "s3_test ok"
exit "$fail"
