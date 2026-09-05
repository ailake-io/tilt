#!/usr/bin/env sh
# Integration test for the MongoDB connector (`mongo_inserir`/`mongo_buscar`/
# `mongo_atualizar`/`mongo_deletar`/`mongo_criar_indice`): spins up a mock
# mongod in python3 (pure socketserver, implementing the same subset of the
# wire protocol: OP_MSG opcode 2013 with section kind 0, BSON com os mesmos
# tipos do cliente) and runs `tilt executar` em dois fixtures: o roundtrip
# (2 inserts na mesma colecao + 2 finds) e o CRUD (3 inserts, update $set,
# delete, createIndexes), checando as saidas impressas mais o log de
# requisicoes do mock (INSERT/FIND/UPDATE/DELETE/CREATEINDEXES).
set -eu

BIN="$1"
PORT_BASE="${TILT_TEST_PORT:-8651}"

command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste MongoDB"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# --- mock mongod ------------------------------------------------------------
python3 - "$PORT_BASE" "$tmp/porta" "$tmp/log" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import socket
import socketserver
import struct
import sys
import threading

port_base = int(sys.argv[1])
port_file = sys.argv[2]
log_path = sys.argv[3]

log = open(log_path, "a", encoding="utf-8")

# store[(db, colecao)] = [doc, ...]; doc = {campo: valor tagged}
store = {}
# indexes[(db, colecao, name)] = indice tagged (createIndexes e so registrado)
indexes = {}
req_id = 0


def p32(v):
    return struct.pack("<i", v)


def p64(v):
    return struct.pack("<q", v)


def pdbl(v):
    return struct.pack("<d", v)


def enc_cstr(s):
    return s.encode() + b"\x00"


def enc_str(s):
    b = s.encode()
    return p32(len(b) + 1) + b + b"\x00"


def enc_elem(name, tagged):
    t, v = tagged
    n = enc_cstr(name)
    if t == "double":
        return b"\x01" + n + pdbl(v)
    if t == "str":
        return b"\x02" + n + enc_str(v)
    if t == "doc":
        return b"\x03" + n + enc_doc(v)
    if t == "arr":
        return b"\x04" + n + enc_doc({str(i): e for i, e in enumerate(v)})
    if t == "oid":
        return b"\x07" + n + v
    if t == "bool":
        return b"\x08" + n + (b"\x01" if v else b"\x00")
    if t == "date":
        return b"\x09" + n + p64(v)
    if t == "null":
        return b"\x0a" + n
    if t == "i32":
        return b"\x10" + n + p32(v)
    if t == "i64":
        return b"\x12" + n + p64(v)
    raise ValueError("tag desconhecida: %r" % (t,))


def enc_doc(doc):
    body = b"".join(enc_elem(k, v) for k, v in doc.items())
    return p32(len(body) + 5) + body + b"\x00"


class ErroBSON(Exception):
    pass


def parse_val(t, data, pos):
    if t == 0x01:
        return ("double", struct.unpack_from("<d", data, pos)[0]), pos + 8
    if t == 0x02:
        (n,) = struct.unpack_from("<i", data, pos)
        pos += 4
        if n < 1 or pos + n > len(data):
            raise ErroBSON("string truncada")
        return ("str", data[pos:pos + n - 1].decode()), pos + n
    if t in (0x03, 0x04):
        doc, pos = parse_doc(data, pos)
        return ("doc", doc), pos
    if t == 0x07:
        if pos + 12 > len(data):
            raise ErroBSON("objectid truncado")
        return ("oid", data[pos:pos + 12]), pos + 12
    if t == 0x08:
        return ("bool", data[pos] != 0), pos + 1
    if t == 0x09:
        return ("date", struct.unpack_from("<q", data, pos)[0]), pos + 8
    if t == 0x0A:
        return ("null", None), pos
    if t == 0x10:
        return ("i32", struct.unpack_from("<i", data, pos)[0]), pos + 4
    if t == 0x12:
        return ("i64", struct.unpack_from("<q", data, pos)[0]), pos + 8
    raise ErroBSON("tipo bson nao suportado: 0x%02x" % t)


def parse_doc(data, pos):
    (length,) = struct.unpack_from("<i", data, pos)
    if length < 5 or pos + length > len(data):
        raise ErroBSON("documento com tamanho invalido")
    fim = pos + length
    pos += 4
    out = {}
    while pos < fim:
        t = data[pos]
        pos += 1
        if t == 0:
            if pos != fim:
                raise ErroBSON("documento com tamanho invalido")
            return out, fim
        nul = data.index(b"\x00", pos, fim)
        name = data[pos:nul].decode()
        pos = nul + 1
        out[name], pos = parse_val(t, data, pos)
    raise ErroBSON("documento com tamanho invalido")


def recv_full(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def parse_op_msg(payload):
    # requestID, responseTo, opCode, flagBits, sections...
    (_req, _resp_to, opcode, _flags) = struct.unpack_from("<iiii", payload, 0)
    if opcode != 2013:
        raise ErroBSON("opcode inesperado: %d" % opcode)
    pos = 16
    while pos < len(payload):
        kind = payload[pos]
        pos += 1
        if kind == 0:
            return parse_doc(payload, pos)[0]
        if kind == 1:
            (slen,) = struct.unpack_from("<i", payload, pos)
            pos += slen
        else:
            raise ErroBSON("section kind nao suportada: %d" % kind)
    raise ErroBSON("requisicao sem section kind 0")


def op_msg_response(req_id, doc):
    body = struct.pack("<iiii", req_id, req_id, 2013, 0)
    body += b"\x00" + enc_doc(doc)
    return p32(len(body) + 4) + body


def como_doc(tagged_doc):
    # tagged ("doc", {...}) -> dict puro de tagged (para o store)
    return dict(tagged_doc)


class Mongod(socketserver.BaseRequestHandler):
    def handle(self):
        conn = self.request
        while True:
            hdr = recv_full(conn, 4)
            if hdr is None:
                return
            (tamanho,) = struct.unpack("<i", hdr)
            if tamanho < 16 or tamanho > 64 * 1024 * 1024:
                return
            payload = recv_full(conn, tamanho - 4)
            if payload is None:
                return
            try:
                resp = self.despachar(parse_op_msg(payload))
            except ErroBSON as e:
                resp = op_msg_response(req_id, {"ok": ("double", 0.0),
                                                "errmsg": ("str", str(e))})
            conn.sendall(resp)

    def despachar(self, cmd):
        global req_id
        req_id += 1
        if "isMaster" in cmd or "hello" in cmd:
            return op_msg_response(req_id, {"ok": ("double", 1.0),
                                            "ismaster": ("bool", True)})
        if "insert" in cmd:
            _tag, colecao = cmd["insert"]
            _tag, db = cmd["$db"]
            _tag, docs = cmd["documents"]
            fila = store.setdefault((db, colecao), [])
            for _tag, doc in docs.values():
                fila.append(como_doc(doc))
            log.write("INSERT %s.%s\n" % (db, colecao))
            log.flush()
            return op_msg_response(req_id, {"ok": ("double", 1.0), "n": ("i32", 1)})
        if "find" in cmd:
            _tag, colecao = cmd["find"]
            _tag, db = cmd["$db"]
            filtro = cmd.get("filter", ("doc", {}))[1]
            achados = []
            for doc in store.get((db, colecao), []):
                if all(doc.get(k) == v for k, v in filtro.items()):
                    achados.append(("doc", doc))
            limite = cmd.get("limit", ("i64", 0))[1]
            if limite > 0:
                achados = achados[:limite]
            log.write("FIND %s.%s n=%d\n" % (db, colecao, len(achados)))
            log.flush()
            return op_msg_response(req_id, {
                "ok": ("double", 1.0),
                "cursor": ("doc", {"firstBatch": ("arr", achados), "id": ("i64", 0)}),
            })
        if "update" in cmd:
            _tag, colecao = cmd["update"]
            _tag, db = cmd["$db"]
            fila = store.setdefault((db, colecao), [])
            n = 0
            n_modified = 0
            for _tag, upd in cmd["updates"][1].values():
                filtro = upd.get("q", ("doc", {}))[1]
                _tag, u = upd["u"]
                multi = upd.get("multi", ("bool", False))[1]
                set_ops = u.get("$set", ("doc", {}))[1]
                for doc in fila:
                    if all(doc.get(k) == v for k, v in filtro.items()):
                        n += 1
                        for k, v in set_ops.items():
                            doc[k] = v
                        n_modified += 1
                        if not multi:
                            break
            log.write("UPDATE %s.%s n=%d nModified=%d\n" % (db, colecao, n, n_modified))
            log.flush()
            return op_msg_response(req_id, {"ok": ("double", 1.0),
                                            "n": ("i32", n),
                                            "nModified": ("i32", n_modified)})
        if "delete" in cmd:
            _tag, colecao = cmd["delete"]
            _tag, db = cmd["$db"]
            fila = store.setdefault((db, colecao), [])
            n = 0
            for _tag, dl in cmd["deletes"][1].values():
                filtro = dl.get("q", ("doc", {}))[1]
                restantes = []
                for doc in fila:
                    if all(doc.get(k) == v for k, v in filtro.items()):
                        n += 1
                    else:
                        restantes.append(doc)
                fila[:] = restantes
            log.write("DELETE %s.%s n=%d\n" % (db, colecao, n))
            log.flush()
            return op_msg_response(req_id, {"ok": ("double", 1.0), "n": ("i32", n)})
        if "createIndexes" in cmd:
            _tag, colecao = cmd["createIndexes"]
            _tag, db = cmd["$db"]
            for _tag, idx in cmd["indexes"][1].values():
                indexes[(db, colecao, idx["name"][1])] = idx
            nomes = ",".join(idx["name"][1] for _tag, idx in cmd["indexes"][1].values())
            log.write("CREATEINDEXES %s.%s %s\n" % (db, colecao, nomes))
            log.flush()
            return op_msg_response(req_id, {"ok": ("double", 1.0),
                                            "createdCollectionAutomatically": ("bool", False),
                                            "numIndexesBefore": ("i32", 1),
                                            "numIndexesAfter": ("i32", 2)})
        return op_msg_response(req_id, {"ok": ("double", 0.0),
                                        "errmsg": ("str", "comando desconhecido")})


class Servidor(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


srv = None
for porta in range(port_base, port_base + 20):
    try:
        srv = Servidor(("127.0.0.1", porta), Mongod)
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
[ -s "$tmp/porta" ] || { echo "mock MongoDB nao iniciou"; cat "$tmp/mock_out"; exit 1; }
PORTA=$(cat "$tmp/porta")

# --- roundtrip via tilt executar ---------------------------------------------
out=$(
  env MONGO_URL="mongodb://127.0.0.1:$PORTA/loja" \
    "$BIN" executar "${2:-${0%/*}/fixtures/mongo_roundtrip.tilt}"
)

cp "$tmp/log" "$tmp/log_roundtrip"

# --- CRUD (update/delete/createIndexes) em banco separado ---------------------
out2=$(
  env MONGO_URL="mongodb://127.0.0.1:$PORTA/lojadb" \
    "$BIN" executar "${0%/*}/fixtures/mongo_crud.tilt"
)

kill "$mock_pid" 2>/dev/null || true
mock_pid=""

fail=0
echo "$out" | grep -q "^1$" || { echo "saida sem tamanho 1 (filtro ana): $out"; fail=1; }
echo "$out" | grep -q "^200$" || { echo "saida sem valor 200: $out"; fail=1; }
echo "$out" | grep -q "^2$" || { echo "saida sem tamanho 2 (sem filtro): $out"; fail=1; }

inserts=$(grep -c "^INSERT " "$tmp/log_roundtrip" || true)
finds=$(grep -c "^FIND " "$tmp/log_roundtrip" || true)
[ "$inserts" = "2" ] || { echo "esperado 2 INSERT, obtido $inserts"; cat "$tmp/log_roundtrip"; fail=1; }
[ "$finds" = "2" ] || { echo "esperado 2 FIND, obtido $finds"; cat "$tmp/log_roundtrip"; fail=1; }
grep -q "^INSERT loja.pedidos" "$tmp/log_roundtrip" || { echo "log sem INSERT loja.pedidos"; cat "$tmp/log_roundtrip"; fail=1; }
grep -q "^FIND loja.pedidos n=1" "$tmp/log_roundtrip" || { echo "log sem FIND loja.pedidos n=1"; cat "$tmp/log_roundtrip"; fail=1; }
grep -q "^FIND loja.pedidos n=2" "$tmp/log_roundtrip" || { echo "log sem FIND loja.pedidos n=2"; cat "$tmp/log_roundtrip"; fail=1; }

# --- CRUD: saida (nModified=1, valor 999, n=1, total 2, name cliente_1) -------
echo "$out2" | grep -q "^1$" || { echo "saida crud sem nModified 1: $out2"; fail=1; }
echo "$out2" | grep -q "^999$" || { echo "saida crud sem valor 999: $out2"; fail=1; }
echo "$out2" | grep -q "^cliente_1$" || { echo "saida crud sem name cliente_1: $out2"; fail=1; }

inserts2=$(grep -c "^INSERT " "$tmp/log" || true)
finds2=$(grep -c "^FIND " "$tmp/log" || true)
updates=$(grep -c "^UPDATE " "$tmp/log" || true)
deletes=$(grep -c "^DELETE " "$tmp/log" || true)
idxs=$(grep -c "^CREATEINDEXES " "$tmp/log" || true)
[ "$inserts2" = "5" ] || { echo "esperado 5 INSERT no total, obtido $inserts2"; cat "$tmp/log"; fail=1; }
[ "$finds2" = "4" ] || { echo "esperado 4 FIND no total, obtido $finds2"; cat "$tmp/log"; fail=1; }
[ "$updates" = "1" ] || { echo "esperado 1 UPDATE, obtido $updates"; cat "$tmp/log"; fail=1; }
[ "$deletes" = "1" ] || { echo "esperado 1 DELETE, obtido $deletes"; cat "$tmp/log"; fail=1; }
[ "$idxs" = "1" ] || { echo "esperado 1 CREATEINDEXES, obtido $idxs"; cat "$tmp/log"; fail=1; }
grep -q "^UPDATE lojadb.pedidos n=1 nModified=1" "$tmp/log" || { echo "log sem UPDATE lojadb.pedidos n=1 nModified=1"; cat "$tmp/log"; fail=1; }
grep -q "^DELETE lojadb.pedidos n=1" "$tmp/log" || { echo "log sem DELETE lojadb.pedidos n=1"; cat "$tmp/log"; fail=1; }
grep -q "^CREATEINDEXES lojadb.pedidos cliente_1" "$tmp/log" || { echo "log sem CREATEINDEXES lojadb.pedidos cliente_1"; cat "$tmp/log"; fail=1; }
grep -q "^FIND lojadb.pedidos n=2" "$tmp/log" || { echo "log sem FIND lojadb.pedidos n=2"; cat "$tmp/log"; fail=1; }

[ "$fail" = 0 ] && echo "mongo_test ok"
exit "$fail"
