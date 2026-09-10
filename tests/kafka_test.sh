#!/usr/bin/env sh
# Integration test for the Kafka connector (`ler_kafka`/`escrever_kafka` and
# `fonte tipo: kafka`): spins up a mock Kafka 0.9-era broker in python3 (pure
# socketserver, implementing the wire protocol: metadata v0, produce v1,
# fetch v1, plus group coordination — find_coordinator v0, join_group v0,
# heartbeat v0, leave_group v0, sync_group v0, offset_fetch v0, offset_commit
# v1) and runs `tilt executar` on fixtures: kafka_roundtrip.tilt (2 produces +
# 1 fetch from the beginning) and kafka_grupo.tilt (consumer group with offset
# commit checkpoint + stateless `fonte tipo:` kafka), checking the printed
# messages (order and count), the empty second read of the group and the
# mock's request log. Then it produces into a 2-partition topic and runs two
# concurrent consumers in the same group (kafka_grupo2.tilt), asserting that
# the mock's roundrobin split the partitions between them (each consumer read
# only its own messages), with heartbeats and clean LeaveGroup in the log.
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
python3 - "$PORT_BASE" "$tmp/porta" "$tmp/log" g2 "vendas=2" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import socket
import socketserver
import struct
import sys
import threading
import zlib

port_base = int(sys.argv[1])
port_file = sys.argv[2]
log_path = sys.argv[3]
# grupos cujo join deve aguardar um 2o membro (ate 3s) para exercitar o
# rebalanceamento com 2 consumidores concorrentes; demais grupos respondem
# na hora com o membro unico.
grupos_multi = set(sys.argv[4].split(",")) if len(sys.argv) > 4 else set()
# declaracao explicita de particoes por topico ("vendas=2") — o metadata a
# usa como minimo, mesmo antes de qualquer produce naquela particao.
topicos_partes = {}
if len(sys.argv) > 5:
    for item in sys.argv[5].split(","):
        t, _, n = item.partition("=")
        if t and n:
            topicos_partes[t] = int(n)

log = open(log_path, "a", encoding="utf-8")

# store[(topico, particao)] = [valor_bytes, ...]  (offset = indice)
store = {}
# membros[gid] = {member_id: [topicos do join]}; geracoes[gid] = geracao atual;
# assignments[gid] = {member_id: [particoes]} calculado roundrobin no join.
membros = {}
geracoes = {}
assignments = {}
offsets = {}  # (gid, topico, part) -> offset commitado
lock = threading.Condition()
contador_membro = [0]


def parts_do_topico(t):
    ps = sorted(p for (tt, p) in store if tt == t)
    minimo = topicos_partes.get(t, 1)
    if len(ps) < minimo:
        ps = list(range(minimo))
    return ps if ps else [0]


def assignment_roundrobin(gid):
    """Distribui as particoes dos topicos (ordenados) entre os membros
    (ordenados por member_id), uma particao por membro por vez."""
    out = {}
    ids = sorted(membros.get(gid, {}))
    if not ids:
        return out
    topicos = set()
    for ts in membros[gid].values():
        topicos.update(ts)
    i = 0
    for t in sorted(topicos):
        for p in parts_do_topico(t):
            out.setdefault(ids[i % len(ids)], []).append(p)
            i += 1
    return out


def member_metadata_bytes(topicos):
    out = p16(0) + p32(len(topicos))
    for t in topicos:
        out += pstr(t)
    return out + p32(-1)  # user_data NULL


def parse_assignment_bytes(a):
    """MemberAssignment v0 -> lista de (topico, [particoes])."""
    apos = 2  # version int16
    nt = struct.unpack_from(">i", a, apos)[0]
    apos += 4
    out = []
    for _ in range(nt):
        t, apos = rd_str(a, apos)
        np_ = struct.unpack_from(">i", a, apos)[0]
        apos += 4
        parts = []
        for _ in range(np_):
            (p,) = struct.unpack_from(">i", a, apos)
            apos += 4
            parts.append(p)
        out.append((t, parts))
    return out


def rd_str(body, pos):
    n = struct.unpack_from(">h", body, pos)[0]
    pos += 2
    if n < 0:
        return "", pos
    return body[pos:pos + n].decode(), pos + n


def rd_bytes(body, pos):
    n = struct.unpack_from(">i", body, pos)[0]
    pos += 4
    if n < 0:
        return b"", pos
    return body[pos:pos + n], pos + n


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
        if api == 10:
            return p32(corr) + self.find_coordinator(payload[pos:])
        if api == 11:
            return p32(corr) + self.join_group(payload[pos:])
        if api == 12:
            return p32(corr) + self.heartbeat(payload[pos:])
        if api == 13:
            return p32(corr) + self.leave_group(payload[pos:])
        if api == 14:
            return p32(corr) + self.sync_group(payload[pos:])
        if api == 9:
            return p32(corr) + self.offset_fetch(payload[pos:])
        if api == 8:
            return p32(corr) + self.offset_commit(payload[pos:])
        return p32(corr)  # api desconhecida: so o correlation_id

    # --- coordenacao de consumer groups (0.9-era) -------------------------
    def find_coordinator(self, body):
        gid, _pos = rd_str(body, 0)
        porta = self.server.server_address[1]
        log.write("FINDCOORDINATOR %s\n" % gid)
        log.flush()
        return p16(0) + p32(0) + pstr("127.0.0.1") + p32(porta)  # si mesmo

    def join_group(self, body):
        gid, pos = rd_str(body, 0)
        pos += 4  # session_timeout
        _member, pos = rd_str(body, pos)
        _ptype, pos = rd_str(body, pos)
        nproto = struct.unpack_from(">i", body, pos)[0]
        pos += 4
        topicos = []
        for _ in range(nproto):
            _nome, pos = rd_str(body, pos)
            meta, pos = rd_bytes(body, pos)
            mpos = 2  # pula version int16
            nt = struct.unpack_from(">i", meta, mpos)[0]
            mpos += 4
            for _ in range(nt):
                t, mpos = rd_str(meta, mpos)
                topicos.append(t)
        with lock:
            contador_membro[0] += 1
            mid = "m-%d" % contador_membro[0]
            membros.setdefault(gid, {})[mid] = topicos
            geracoes[gid] = geracoes.get(gid, 0) + 1
            gen = geracoes[gid]
            lock.notify_all()
            if gid in grupos_multi and len(membros[gid]) == 1:
                # primeiro membro de um grupo multi: aguarda o par (ex.: 2o
                # consumidor tilt concorrente) ou segue sozinho no timeout.
                lock.wait(timeout=3.0)
            assignments[gid] = assignment_roundrobin(gid)
            lider = sorted(membros[gid])[0]
            lista = [(m, membros[gid][m]) for m in sorted(membros[gid])] if mid == lider else []
        log.write("JOINGROUP %s\n" % gid)
        log.flush()
        # error 0, generation, protocolo escolhido, lider, este member_id,
        # [members] (so o lider recebe a lista)
        out = p16(0) + p32(gen) + pstr("roundrobin") + pstr(lider) + pstr(mid)
        out += p32(len(lista))
        for m, ts in lista:
            out += pstr(m) + pbytes(member_metadata_bytes(ts))
        return out

    def heartbeat(self, body):
        gid, _pos = rd_str(body, 0)
        log.write("HEARTBEAT %s\n" % gid)
        log.flush()
        return p16(0)

    def leave_group(self, body):
        gid, pos = rd_str(body, 0)
        mid, _pos = rd_str(body, pos)
        with lock:
            if gid in membros and mid in membros[gid]:
                del membros[gid][mid]
                geracoes[gid] = geracoes.get(gid, 0) + 1
                if membros[gid]:
                    assignments[gid] = assignment_roundrobin(gid)
                else:
                    membros.pop(gid, None)
                    assignments.pop(gid, None)
            lock.notify_all()
        log.write("LEAVEGROUP %s\n" % gid)
        log.flush()
        return p16(0)

    def sync_group(self, body):
        gid, pos = rd_str(body, 0)
        pos += 4  # generation
        member, pos = rd_str(body, pos)
        na = struct.unpack_from(">i", body, pos)[0]
        pos += 4
        lider_assign = {}
        for _ in range(na):
            m, pos = rd_str(body, pos)
            a, pos = rd_bytes(body, pos)
            lider_assign[m] = parse_assignment_bytes(a)
        with lock:
            if lider_assign:  # assignment enviado pelo lider no SyncGroup
                assignments[gid] = {m: parts for m, tp in lider_assign.items()
                                    for _t, parts in tp}
            parts = assignments.get(gid, {}).get(member, [])
            ts = membros.get(gid, {}).get(member, [])
            log.write("SYNCGROUP %s\n" % gid)
            log.write("SYNCASSIGN %s %s %s\n" % (gid, member, ",".join(str(p) for p in parts)))
            log.flush()
            assignment = p16(0) + p32(len(ts))
            for t in ts:
                assignment += pstr(t) + p32(len(parts))
                for p in parts:
                    assignment += p32(p)
            assignment += p32(-1)  # user_data NULL
        return p16(0) + pbytes(assignment)

    def offset_fetch(self, body):
        gid, pos = rd_str(body, 0)
        nt = struct.unpack_from(">i", body, pos)[0]
        pos += 4
        topicos = []
        for _ in range(nt):
            t, pos = rd_str(body, pos)
            np_ = struct.unpack_from(">i", body, pos)[0]
            pos += 4
            partes = []
            for _ in range(np_):
                (part,) = struct.unpack_from(">i", body, pos)
                pos += 4
                partes.append(part)
            topicos.append((t, partes))
        log.write("OFFSETFETCH %s\n" % gid)
        log.flush()
        out = p32(len(topicos))
        for t, partes in topicos:
            out += pstr(t) + p32(len(partes))
            for part in partes:
                off = offsets.get((gid, t, part), -1)
                out += p32(part) + p64(off) + pstr("") + p16(0)
        return out

    def offset_commit(self, body):
        gid, pos = rd_str(body, 0)
        pos += 4  # generation
        _member, pos = rd_str(body, pos)
        nt = struct.unpack_from(">i", body, pos)[0]
        pos += 4
        topicos = []
        for _ in range(nt):
            t, pos = rd_str(body, pos)
            np_ = struct.unpack_from(">i", body, pos)[0]
            pos += 4
            partes = []
            for _ in range(np_):
                (part, off, _ts) = struct.unpack_from(">iqi", body, pos)
                pos += 20
                _meta, pos = rd_str(body, pos)
                offsets[(gid, t, part)] = off
                partes.append(part)
                log.write("OFFSETCOMMIT %s %s %d %d\n" % (gid, t, part, off))
                log.flush()
            topicos.append((t, partes))
        out = p32(len(topicos))
        for t, partes in topicos:
            out += pstr(t) + p32(len(partes))
            for part in partes:
                out += p32(part) + p16(0)
        return out

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
            ps = parts_do_topico(t)
            out += pstr(t) + p32(len(ps))
            for p in ps:
                out += p16(0) + p32(p) + p32(0)  # erro, partition_id, leader=0
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
fail=0

# --- roundtrip via tilt executar -------------------------------------------------
out=$(
  env KAFKA_BOOTSTRAP="127.0.0.1:$PORTA" \
    "$BIN" executar "${2:-${0%/*}/fixtures/kafka_roundtrip.tilt}"
)

# --- consumer group + fonte kafka (fixture kafka_grupo.tilt) ---------------------
out2=$(
  env KAFKA_BOOTSTRAP="127.0.0.1:$PORTA" \
    "$BIN" executar "${0%/*}/fixtures/kafka_grupo.tilt"
)

# --- 2 consumidores no mesmo grupo (rebalanceamento roundrobin) -------------------
# 4 produces no topico "vendas" (2 particoes): v-1a/v-1b na p0, v-2a/v-2b na p1.
env KAFKA_BOOTSTRAP="127.0.0.1:$PORTA" \
  "$BIN" executar "${0%/*}/fixtures/kafka_produz_vendas.tilt" >"$tmp/out3" 2>&1 || {
  echo "produz_vendas falhou:"; cat "$tmp/out3"; fail=1;
}

env KAFKA_BOOTSTRAP="127.0.0.1:$PORTA" \
  "$BIN" executar "${0%/*}/fixtures/kafka_grupo2.tilt" >"$tmp/outA" 2>&1 &
pidA=$!
env KAFKA_BOOTSTRAP="127.0.0.1:$PORTA" \
  "$BIN" executar "${0%/*}/fixtures/kafka_grupo2.tilt" >"$tmp/outB" 2>&1 &
pidB=$!
okA=0
wait "$pidA" || okA=1
okB=0
wait "$pidB" || okB=1
[ "$okA" = 0 ] || { echo "consumidor A falhou:"; cat "$tmp/outA"; fail=1; }
[ "$okB" = 0 ] || { echo "consumidor B falhou:"; cat "$tmp/outB"; fail=1; }

# cada consumidor leu so a sua particao (junto de mensagens disjunto por
# particao) e, juntos, cobriram as duas
pA="?"
if grep -q "v-1a" "$tmp/outA" && ! grep -q "v-2a" "$tmp/outA"; then
  grep -q "v-1b" "$tmp/outA" || { echo "consumidor A (p0) sem v-1b:"; cat "$tmp/outA"; fail=1; }
  pA=0
elif grep -q "v-2a" "$tmp/outA" && ! grep -q "v-1a" "$tmp/outA"; then
  grep -q "v-2b" "$tmp/outA" || { echo "consumidor A (p1) sem v-2b:"; cat "$tmp/outA"; fail=1; }
  pA=1
fi
pB="?"
if grep -q "v-1a" "$tmp/outB" && ! grep -q "v-2a" "$tmp/outB"; then
  grep -q "v-1b" "$tmp/outB" || { echo "consumidor B (p0) sem v-1b:"; cat "$tmp/outB"; fail=1; }
  pB=0
elif grep -q "v-2a" "$tmp/outB" && ! grep -q "v-1a" "$tmp/outB"; then
  grep -q "v-2b" "$tmp/outB" || { echo "consumidor B (p1) sem v-2b:"; cat "$tmp/outB"; fail=1; }
  pB=1
fi
{ [ "$pA" != "?" ] && [ "$pB" != "?" ] && [ "$pA" != "$pB" ]; } || {
  echo "particoes nao divididas entre os consumidores (A=$pA B=$pB):"
  echo "--- A:"; cat "$tmp/outA"; echo "--- B:"; cat "$tmp/outB"
  echo "--- log:"; cat "$tmp/log"; fail=1;
}

kill "$mock_pid" 2>/dev/null || true
mock_pid=""

echo "$out" | grep -q "msg-1" || { echo "saida sem 'msg-1': $out"; fail=1; }
echo "$out" | grep -q "msg-2" || { echo "saida sem 'msg-2': $out"; fail=1; }
# ordem: msg-1 deve aparecer antes de msg-2
antes=$(echo "$out" | grep -n "msg-1" | head -1 | cut -d: -f1)
depois=$(echo "$out" | grep -n "msg-2" | head -1 | cut -d: -f1)
[ -n "$antes" ] && [ -n "$depois" ] && [ "$antes" -lt "$depois" ] || {
  echo "ordem inesperada (msg-1 linha $antes, msg-2 linha $depois): $out"; fail=1;
}
echo "$out" | grep -q "^2$" || { echo "saida sem tamanho 2: $out"; fail=1; }

# (b) 1a leitura do grupo devolve as 3 mensagens, em ordem (1 linha: lista)
echo "$out2" | grep -q "^\[c-1, c-2, c-3\]$" || {
  echo "grupo: esperado '[c-1, c-2, c-3]': $out2"; fail=1;
}
# (c) 2a leitura vem vazia (offsets commitados): tamanho 0 impresso
echo "$out2" | grep -q "^0$" || { echo "grupo: 2a leitura deveria vir vazia (sem '0'): $out2"; fail=1; }
# (d) fonte kafka sem grupo le as 2 mensagens do outro topico
echo "$out2" | grep -q "k-1" || { echo "fonte kafka: saida sem 'k-1': $out2"; fail=1; }
echo "$out2" | grep -q "k-2" || { echo "fonte kafka: saida sem 'k-2': $out2"; fail=1; }

produces=$(grep -c "^PRODUCE " "$tmp/log" || true)
fetches=$(grep -c "^FETCH " "$tmp/log" || true)
# 7 produces dos fixtures originais + 4 do cenario de 2 particoes
[ "$produces" = "11" ] || { echo "esperado 11 PRODUCE, obtido $produces"; cat "$tmp/log"; fail=1; }
# 4 fetches dos fixtures originais + 1 por consumidor (cada um so busca a sua
# particao) no cenario de rebalanceamento
[ "$fetches" = "6" ] || { echo "esperado 6 FETCH, obtido $fetches"; cat "$tmp/log"; fail=1; }

# coordenacao do consumer group g1: 2 chamadas no fixture kafka_grupo
for ev in JOINGROUP SYNCGROUP OFFSETFETCH OFFSETCOMMIT LEAVEGROUP; do
  n=$(grep -c "^$ev g1" "$tmp/log" || true)
  [ "$n" = "2" ] || { echo "esperado 2 $ev g1, obtido $n"; cat "$tmp/log"; fail=1; }
done
n=$(grep -c "^HEARTBEAT g1" "$tmp/log" || true)
[ "$n" -ge 2 ] || { echo "esperado >= 2 HEARTBEAT g1, obtido $n"; cat "$tmp/log"; fail=1; }
# o offset commitado na 1a chamada deve ser 3 (proximo apos as 3 mensagens)
grep -q "^OFFSETCOMMIT g1 compras 0 3$" "$tmp/log" || {
  echo "OFFSETCOMMIT esperado 'g1 compras 0 3' ausente"; cat "$tmp/log"; fail=1;
}

# coordenacao do grupo g2 (2 consumidores concorrentes): join/sync/commit/
# leave por consumidor, com assignment disjunto cobrindo as duas particoes
for ev in JOINGROUP SYNCGROUP OFFSETFETCH OFFSETCOMMIT LEAVEGROUP; do
  n=$(grep -c "^$ev g2" "$tmp/log" || true)
  [ "$n" = "2" ] || { echo "esperado 2 $ev g2, obtido $n"; cat "$tmp/log"; fail=1; }
done
n=$(grep -c "^HEARTBEAT g2" "$tmp/log" || true)
[ "$n" -ge 2 ] || { echo "esperado >= 2 HEARTBEAT g2, obtido $n"; cat "$tmp/log"; fail=1; }
a0=$(grep -c "^SYNCASSIGN g2 m-[0-9]* 0$" "$tmp/log" || true)
a1=$(grep -c "^SYNCASSIGN g2 m-[0-9]* 1$" "$tmp/log" || true)
{ [ "$a0" = "1" ] && [ "$a1" = "1" ]; } || {
  echo "esperado assignment disjunto {0,1} em g2 (p0=$a0 p1=$a1)"; cat "$tmp/log"; fail=1;
}
grep -q "^OFFSETCOMMIT g2 vendas 0 2$" "$tmp/log" || {
  echo "OFFSETCOMMIT esperado 'g2 vendas 0 2' ausente"; cat "$tmp/log"; fail=1;
}
grep -q "^OFFSETCOMMIT g2 vendas 1 2$" "$tmp/log" || {
  echo "OFFSETCOMMIT esperado 'g2 vendas 1 2' ausente"; cat "$tmp/log"; fail=1;
}

[ "$fail" = 0 ] && echo "kafka_test ok"
exit "$fail"
