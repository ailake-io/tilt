#!/usr/bin/env sh
# Integration test for the Kafka connector (`ler_kafka`/`escrever_kafka`):
# spins up a mock Kafka 0.9-era broker in python3 (pure socketserver,
# implementing the same subset of the wire protocol: metadata v0, produce v1,
# fetch v1) and runs `tilt executar` on a roundtrip fixture (2 produces on
# the same partition + 1 fetch from the beginning), checking the printed
# messages (order and count) plus the mock's request log (2 PRODUCE + 1 FETCH).
set -eu

BIN="$1"
PORT_BASE="${TILT_TEST_PORT:-8641}"

command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste Kafka"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# --- mock broker 0.9-era -------------------------------------------------------
python3 - "$PORT_BASE" "$tmp/porta" "$tmp/log" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import socket
import socketserver
import struct
import sys
import zlib

port_base = int(sys.argv[1])
port_file = sys.argv[2]
log_path = sys.argv[3]

log = open(log_path, "a", encoding="utf-8")

# store[(topico, particao)] = [valor_bytes, ...]  (offset = indice)
store = {}


def p8(v):
    return struct.pack(">b", v)


def p16(v):
    return struct.pack(">h", v)


def p32(v):
    return struct.pack(">i", v)


def p64(v):
    return struct.pack(">q", v)


def pstr(s):
    b = s.encode()
    return p16(len(b)) + b


def pbytes(b):
    return p32(len(b)) + b


def recv_full(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def parse_messages(data):
    msgs = []
    pos = 0
    while pos + 12 <= len(data):
        (_offset, size) = struct.unpack_from(">qi", data, pos)
        pos += 12
        msg = data[pos:pos + size]
        pos += size
        (_crc, magic, _attr) = struct.unpack_from(">Ibb", msg, 0)
        if magic == 1:  # v1 tem timestamp
            klen = struct.unpack_from(">i", msg, 14)[0]
            vpos = 18 + (0 if klen < 0 else klen)
        else:
            klen = struct.unpack_from(">i", msg, 6)[0]
            vpos = 10 + (0 if klen < 0 else klen)
        vlen = struct.unpack_from(">i", msg, vpos)[0]
        msgs.append(msg[vpos + 4:vpos + 4 + vlen] if vlen >= 0 else b"")
    return msgs


def encode_message(valor, offset):
    body = p8(0) + p8(0) + p32(-1) + pbytes(valor)  # magic, attr, key NULL, value
    msg = struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF) + body
    return p64(offset) + p32(len(msg)) + msg


class Broker(socketserver.BaseRequestHandler):
    def handle(self):
        conn = self.request
        while True:
            hdr = recv_full(conn, 4)
            if hdr is None:
                return
            (tamanho,) = struct.unpack(">i", hdr)
            if tamanho < 10 or tamanho > 64 * 1024 * 1024:
                return
            payload = recv_full(conn, tamanho)
            if payload is None:
                return
            resp = self.despachar(payload)
            conn.sendall(p32(len(resp)) + resp)

    def despachar(self, payload):
        (api, _vers, corr) = struct.unpack_from(">hhi", payload, 0)
        pos = 8
        clen = struct.unpack_from(">h", payload, pos)[0]
        pos += 2 + clen  # pula client_id
        if api == 3:
            return p32(corr) + self.metadata(payload[pos:])
        if api == 0:
            return p32(corr) + self.produce(payload[pos:])
        if api == 1:
            return p32(corr) + self.fetch(payload[pos:])
        return p32(corr)  # api desconhecida: so o correlation_id

    def metadata(self, body):
        porta = self.server.server_address[1]
        nt = struct.unpack_from(">i", body, 0)[0]
        pos = 4
        topicos = []
        for _ in range(nt):
            tlen = struct.unpack_from(">h", body, pos)[0]
            pos += 2
            topicos.append(body[pos:pos + tlen].decode())
            pos += tlen
        out = p32(1) + p32(0) + pstr("127.0.0.1") + p32(porta)  # brokers: si mesmo
        out += p32(len(topicos))
        for t in topicos:
            out += pstr(t) + p32(1)  # 1 particao
            out += p16(0) + p32(0) + p32(0)  # erro, partition_id, leader=0
            out += p32(1) + p32(0)  # [replicas]
            out += p32(1) + p32(0)  # [isr]
        return out

    def produce(self, body):
        pos = 6  # required_acks(int16) + timeout(int32)
        nt = struct.unpack_from(">i", body, pos)[0]
        pos += 4
        respostas = []
        for _ in range(nt):
            tlen = struct.unpack_from(">h", body, pos)[0]
            pos += 2
            topico = body[pos:pos + tlen].decode()
            pos += tlen
            np_ = struct.unpack_from(">i", body, pos)[0]
            pos += 4
            partes = []
            for _ in range(np_):
                (particao, mssz) = struct.unpack_from(">ii", body, pos)
                pos += 8
                ms = body[pos:pos + mssz]
                pos += mssz
                fila = store.setdefault((topico, particao), [])
                base = len(fila)
                for valor in parse_messages(ms):
                    fila.append(valor)
                log.write("PRODUCE %s %d %d\n" % (topico, particao, len(fila) - base))
                log.flush()
                partes.append((particao, 0, base))  # erro 0, offset base
            respostas.append((topico, partes))
        out = p32(len(respostas))
        for topico, partes in respostas:
            out += pstr(topico) + p32(len(partes))
            for particao, erro, offset in partes:
                out += p32(particao) + p16(erro) + p64(offset)
        return out

    def fetch(self, body):
        pos = 12  # replica_id + max_wait + min_bytes
        nt = struct.unpack_from(">i", body, pos)[0]
        pos += 4
        respostas = []
        for _ in range(nt):
            tlen = struct.unpack_from(">h", body, pos)[0]
            pos += 2
            topico = body[pos:pos + tlen].decode()
            pos += tlen
            np_ = struct.unpack_from(">i", body, pos)[0]
            pos += 4
            partes = []
            for _ in range(np_):
                (particao, offset, _maxb) = struct.unpack_from(">iqi", body, pos)
                pos += 16
                fila = store.get((topico, particao), [])
                hw = len(fila)
                if offset == -1:  # "latest": so mensagens novas
                    inicio = hw
                    erro = 0
                elif offset > hw:
                    inicio = hw
                    erro = 1  # OffsetOutOfRange
                else:
                    inicio = int(offset)
                    erro = 0
                ms = b"".join(encode_message(v, o) for o, v in enumerate(fila[inicio:], start=inicio))
                log.write("FETCH %s %d %d\n" % (topico, particao, offset))
                log.flush()
                partes.append((particao, erro, hw, ms))
            respostas.append((topico, partes))
        out = p32(len(respostas))
        for topico, partes in respostas:
            out += pstr(topico) + p32(len(partes))
            for particao, erro, hw, ms in partes:
                out += p32(particao) + p16(erro) + p64(hw) + p32(len(ms)) + ms
        return out


class Servidor(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


srv = None
for porta in range(port_base, port_base + 20):
    try:
        srv = Servidor(("127.0.0.1", porta), Broker)
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
[ -s "$tmp/porta" ] || { echo "mock Kafka nao iniciou"; cat "$tmp/mock_out"; exit 1; }
PORTA=$(cat "$tmp/porta")

# --- roundtrip via tilt executar -------------------------------------------------
out=$(
  env KAFKA_BOOTSTRAP="127.0.0.1:$PORTA" \
    "$BIN" executar "${2:-${0%/*}/fixtures/kafka_roundtrip.tilt}"
)

kill "$mock_pid" 2>/dev/null || true
mock_pid=""

fail=0
echo "$out" | grep -q "msg-1" || { echo "saida sem 'msg-1': $out"; fail=1; }
echo "$out" | grep -q "msg-2" || { echo "saida sem 'msg-2': $out"; fail=1; }
# ordem: msg-1 deve aparecer antes de msg-2
antes=$(echo "$out" | grep -n "msg-1" | head -1 | cut -d: -f1)
depois=$(echo "$out" | grep -n "msg-2" | head -1 | cut -d: -f1)
[ -n "$antes" ] && [ -n "$depois" ] && [ "$antes" -lt "$depois" ] || {
  echo "ordem inesperada (msg-1 linha $antes, msg-2 linha $depois): $out"; fail=1;
}
echo "$out" | grep -q "^2$" || { echo "saida sem tamanho 2: $out"; fail=1; }

produces=$(grep -c "^PRODUCE " "$tmp/log" || true)
fetches=$(grep -c "^FETCH " "$tmp/log" || true)
[ "$produces" = "2" ] || { echo "esperado 2 PRODUCE, obtido $produces"; cat "$tmp/log"; fail=1; }
[ "$fetches" = "1" ] || { echo "esperado 1 FETCH, obtido $fetches"; cat "$tmp/log"; fail=1; }

[ "$fail" = 0 ] && echo "kafka_test ok"
exit "$fail"
