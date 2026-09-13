#!/usr/bin/env sh
# Backends distribuidos do checkpoint da janela (Marco 1 / A2):
# TILT_CHECKPOINT_DIR=s3://bucket/prefixo e kafka:<topico>, com dois
# processos `tilt executar --agendar` seguidos — o segundo nao reprocessa.
# Mocks minimos em python3 (S3: PUT/GET path-style; Kafka: metadata v0,
# InitProducerId, Produce v3, Fetch v1). Pula sem python3/curl.
set -eu

BIN="$1"
FIXTURE_DIR="$2"

command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste checkpoint_backend"
  exit 0
}
command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste checkpoint_backend"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$mock_s3" "$mock_k" 2>/dev/null || true; rm -rf "$tmp"' EXIT
cp "$FIXTURE_DIR/dados.csv" "$FIXTURE_DIR/input.tilt" "$tmp/"

# --- mock S3 minimo (PUT/GET, 404 NoSuchKey) ------------------------------------
python3 - "$tmp/s3porta" "$tmp/s3log" <<'PYEOF' >"$tmp/s3out" 2>&1 &
import http.server
import socket as _socket
import sys

_socket.getfqdn = lambda host="": "localhost"
port_file, log_path = sys.argv[1], sys.argv[2]
store = {}
log = open(log_path, "a", encoding="utf-8")


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _path(self):
        p = self.path.split("?", 1)[0]
        return p

    def do_PUT(self):
        n = int(self.headers.get("Content-Length", 0))
        store[self._path()] = self.rfile.read(n)
        log.write("PUT %s %d\n" % (self._path(), n))
        log.flush()
        body = b""
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.send_header("ETag", '"x"')
        self.end_headers()

    def do_GET(self):
        p = self._path()
        if p in store:
            b = store[p]
            log.write("GET %s 200\n" % p)
            log.flush()
            self.send_response(200)
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
        else:
            log.write("GET %s 404\n" % p)
            log.flush()
            b = b'<?xml version="1.0"?><Error><Code>NoSuchKey</Code></Error>'
            self.send_response(404)
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)


srv = None
for porta in range(8651, 8671):
    try:
        srv = http.server.HTTPServer(("127.0.0.1", porta), H)
        break
    except OSError:
        continue
if srv is None:
    sys.exit("sem porta p/ mock S3")
open(port_file, "w").write(str(porta))
srv.serve_forever()
PYEOF
mock_s3=$!

# --- mock Kafka minimo (metadata, InitPID, produce v3, fetch v1) -----------------
python3 - "$tmp/kporta" "$tmp/klog" <<'PYEOF' >"$tmp/kout" 2>&1 &
import socketserver
import struct
import sys

port_file, log_path = sys.argv[1], sys.argv[2]
store = {}
log = open(log_path, "a", encoding="utf-8")


def p16(v):
    return struct.pack(">h", v)


def p32(v):
    return struct.pack(">i", v)


def p64(v):
    return struct.pack(">q", v)


def pstr(s):
    b = s.encode()
    return p16(len(b)) + b


def rf(conn, n):
    b = b""
    while len(b) < n:
        d = conn.recv(n - len(b))
        if not d:
            return None
        b += d
    return b


def rd_uvarint(body, pos):
    v = 0
    shift = 0
    while True:
        b = body[pos]
        pos += 1
        v |= (b & 0x7F) << shift
        if not (b & 0x80):
            return v, pos
        shift += 7


class H(socketserver.BaseRequestHandler):
    def handle(self):
        c = self.request
        while True:
            h = rf(c, 4)
            if not h:
                return
            (t,) = struct.unpack(">i", h)
            p = rf(c, t)
            if not p:
                return
            (api, _ver, corr) = struct.unpack_from(">hhi", p, 0)
            cp = 8 + 2 + struct.unpack_from(">h", p, 8)[0]
            if api == 3:
                # metadata v0: [topicos] -> 1 broker (si mesmo) + 1 particao
                # por topico pedido
                q = cp
                nt = struct.unpack_from(">i", p, q)[0]
                q += 4
                tops = []
                for _ in range(nt):
                    tl = struct.unpack_from(">h", p, q)[0]
                    q += 2
                    tops.append(p[q:q + tl].decode())
                    q += tl
                resp = p32(1) + p32(0) + pstr("127.0.0.1") + p32(
                    self.server.server_address[1])
                resp += p32(len(tops))
                for topico in tops:
                    resp += pstr(topico) + p32(1) + p16(0) + p32(0) + p32(0)
                    resp += p32(1) + p32(0) + p32(1) + p32(0)
                c.sendall(p32(4 + len(resp)) + p32(corr) + resp)
            elif api == 22:
                log.write("INITPID\n")
                log.flush()
                c.sendall(p32(4 + 4 + 2 + 8 + 2) + p32(corr) + p32(0) + p16(0) +
                          p64(5) + p16(0))
            elif api == 0:
                # produce v3: pula transactional/acks/timeout, 1 topico/1 particao
                q = cp + 2 + 2 + 4 + 4
                tlen = struct.unpack_from(">h", p, q)[0]
                q += 2
                topico = p[q:q + tlen].decode()
                q += tlen + 4
                (part, mlen) = struct.unpack_from(">ii", p, q)
                q += 8
                batch = p[q:q + mlen]
                # cabecalho do RecordBatch ate producerId/epoch/seq
                (pid, _ep, seq) = struct.unpack_from(">qhi", batch, 8 + 4 + 4 + 1 + 4 + 2 + 4 + 8 + 8)
                bp = 8 + 4 + 4 + 1 + 4 + 2 + 4 + 8 + 8 + 8 + 2 + 4 + 4
                vals = []
                while bp < len(batch):
                    _rl, bp = rd_uvarint(batch, bp)
                    bp += 1
                    _, bp = rd_uvarint(batch, bp)
                    _, bp = rd_uvarint(batch, bp)
                    kl, bp = rd_uvarint(batch, bp)
                    # zigzag decode
                    kl = (kl >> 1) ^ -(kl & 1)
                    if kl >= 0:
                        bp += kl
                    vl, bp = rd_uvarint(batch, bp)
                    vl = (vl >> 1) ^ -(vl & 1)
                    vals.append(batch[bp:bp + vl])
                    bp += vl
                    _, bp = rd_uvarint(batch, bp)
                fila = store.setdefault((topico, part), [])
                base = len(fila)
                fila.extend(vals)
                log.write("KPRODUCE %s %d %d seq=%d pid=%d\n" % (topico, part, len(vals), seq,
                                                                 pid))
                log.flush()
                r = p32(0) + p32(1) + pstr(topico) + p32(1) + p32(part) + p16(0)
                r += p64(base) + p64(-1) + p64(0)
                c.sendall(p32(4 + len(r)) + p32(corr) + r)
            elif api == 1:
                q = cp + 4 + 4 + 4
                nt = struct.unpack_from(">i", p, q)[0]
                q += 4
                tlen = struct.unpack_from(">h", p, q)[0]
                q += 2
                topico = p[q:q + tlen].decode()
                q += tlen + 4
                (part, _off, _mb) = struct.unpack_from(">iqi", p, q)
                fila = store.get((topico, part), [])
                ms = b""
                for i, v in enumerate(fila):
                    import zlib
                    body = b"\x00\x00" + p32(-1) + p32(len(v)) + v
                    ms += p64(i) + p32(4 + len(body)) + struct.pack(
                        ">I", zlib.crc32(body) & 0xFFFFFFFF) + body
                log.write("KFETCH %s %d %d\n" % (topico, part, len(fila)))
                log.flush()
                r = p32(1) + pstr(topico) + p32(1) + p32(part) + p16(0) + p64(len(fila))
                r += p32(len(ms)) + ms
                c.sendall(p32(4 + len(r)) + p32(corr) + r)
            else:
                return


srv = None
for porta in range(19651, 19671):
    try:
        srv = socketserver.TCPServer(("127.0.0.1", porta), H)
        srv.allow_reuse_address = True
        break
    except OSError:
        continue
if srv is None:
    sys.exit("sem porta p/ mock Kafka")
open(port_file, "w").write(str(porta))
srv.serve_forever()
PYEOF
mock_k=$!

for _ in $(seq 1 50); do
  [ -s "$tmp/s3porta" ] && [ -s "$tmp/kporta" ] && break
  sleep 0.1
done
[ -s "$tmp/s3porta" ] || { echo "mock S3 nao iniciou"; cat "$tmp/s3out"; exit 1; }
[ -s "$tmp/kporta" ] || { echo "mock Kafka nao iniciou"; cat "$tmp/kout"; exit 1; }
S3PORTA=$(cat "$tmp/s3porta")
KPORTA=$(cat "$tmp/kporta")
fail=0

export AWS_ACCESS_KEY_ID=test AWS_SECRET_ACCESS_KEY=test
export S3_ENDPOINT="http://127.0.0.1:$S3PORTA" S3_REGION=us-east-1

run() { # $1 = MAX, resto = env extra
  max="$1"; shift
  ( cd "$tmp" && env "$@" TILT_AGORA=2026-01-05T02:50:00 TILT_AGENDAR_MAX="$max" \
    "$BIN" executar --agendar input.tilt )
}

# --- S3 -------------------------------------------------------------------------
out1=$(run 2 "TILT_CHECKPOINT_DIR=s3://bkt/cp")
echo "$out1" | grep -q "lote: 2 1 2" || { echo "S3 run1 sem lote"; echo "$out1"; fail=1; }
grep -q "PUT /bkt/cp/dados.csv.tilt-offset" "$tmp/s3log" || {
  echo "S3 sem PUT do offset:"; cat "$tmp/s3log"; fail=1; }
[ ! -f "$tmp/dados.csv.tilt-offset" ] || { echo "offset vazou p/ local (S3)"; fail=1; }

out2=$(run 4 "TILT_CHECKPOINT_DIR=s3://bkt/cp")
echo "$out2" | grep -q "janela nao fechou" || {
  echo "S3 run2 reprocessou:"; echo "$out2"; fail=1; }

# --- Kafka ----------------------------------------------------------------------
out3=$(run 2 "KAFKA_BOOTSTRAP=127.0.0.1:$KPORTA" "TILT_CHECKPOINT_DIR=kafka:cp-topic")
echo "$out3" | grep -q "lote: 2 1 2" || { echo "Kafka run1 sem lote"; echo "$out3"; fail=1; }
grep -q "KPRODUCE cp-topic 0 .* seq=0" "$tmp/klog" || {
  echo "Kafka sem produce v3 do checkpoint:"; cat "$tmp/klog"; fail=1; }
[ ! -f "$tmp/dados.csv.tilt-offset" ] || { echo "offset vazou p/ local (Kafka)"; fail=1; }

out4=$(run 4 "KAFKA_BOOTSTRAP=127.0.0.1:$KPORTA" "TILT_CHECKPOINT_DIR=kafka:cp-topic")
echo "$out4" | grep -q "janela nao fechou" || {
  echo "Kafka run2 reprocessou:"; echo "$out4"; fail=1; }

[ "$fail" = 0 ] && echo "checkpoint_backend: ok"
exit "$fail"
