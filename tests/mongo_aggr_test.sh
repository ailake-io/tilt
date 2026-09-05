#!/usr/bin/env sh
# Integration test do `mongo_agregar`: sobe um mock mongod em python3 (mesmo
# subconjunto do wire protocol de mongo_test.sh: OP_MSG opcode 2013 com
# section kind 0, BSON com os mesmos tipos do cliente) e roda `tilt executar`
# em fixtures/mongo_aggr.tilt, que insere 4 vendas e dispara 3 aggregates
# ($match+$group+$sort+$limit, $project com _id: 0, $group com $avg+$sort).
# O mock avalia o pipeline de verdade sobre os docs inseridos ($match com
# $gte/$eq/$in e $and/$or, $project de inclusao/exclusao, $group com _id por
# campo e $sum/$avg/$min/$max, $sort 1/-1, $limit/$skip) e devolve tudo no
# firstBatch (sem getMore). Checa as saidas impressas mais o log
# (INSERT/AGGREGATE).
set -eu

BIN="$1"
PORT_BASE="${TILT_TEST_PORT:-8671}"

command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste MongoDB aggregate"
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


# ---------------------------------------------------------- aggregate (mock)
#
# Avalia o pipeline sobre os docs em memoria, com o mesmo subconjunto que o
# cliente tilt documenta: $match (igualdade + $eq/$ne/$gt/$gte/$lt/$lte/$in,
# combinado por E, com $and/$or), $project (1/verdadeiro inclui, 0/falso
# exclui; sem _id implicito ao incluir), $group (_id por "$campo"; $sum com 1
# ou "$campo", $avg/$min/$max), $sort (1/-1), $limit/$skip. Etapa fora desse
# conjunto -> erro claro; resultado inteiro devolve i64, $avg devolve double.

def casa_op(valor, op, arg):
    if op == "$eq":
        return valor == arg
    if op == "$ne":
        return valor != arg
    if valor is None:
        return False
    if op == "$gt":
        return valor[1] > arg[1]
    if op == "$gte":
        return valor[1] >= arg[1]
    if op == "$lt":
        return valor[1] < arg[1]
    if op == "$lte":
        return valor[1] <= arg[1]
    if op == "$in":
        return valor in arg[1]
    raise ErroBSON("operador de match nao suportado pelo mock: " + op)


def casa_match(doc, filtro):
    for k, cond in filtro.items():
        if k == "$and":
            if not all(casa_match(doc, sub[1]) for sub in cond[1]):
                return False
        elif k == "$or":
            if not any(casa_match(doc, sub[1]) for sub in cond[1]):
                return False
        elif cond[0] == "doc":
            for op, arg in cond[1].items():
                if not casa_op(doc.get(k), op, arg):
                    return False
        else:
            if doc.get(k) != cond:
                return False
    return True


def agrega_project(docs, espec):
    verdade = (("i64", 1), ("i32", 1), ("bool", True))
    falsidade = (("i64", 0), ("i32", 0), ("bool", False))
    inclusoes = {k for k, v in espec.items() if v in verdade}
    exclusoes = {k for k, v in espec.items() if v in falsidade}
    saida = []
    for doc in docs:
        if inclusoes:
            novo = {}
            if "_id" not in exclusoes and "_id" in doc:
                novo["_id"] = doc["_id"]
            for k in inclusoes:
                if k in doc:
                    novo[k] = doc[k]
        else:
            novo = {k: v for k, v in doc.items() if k not in exclusoes}
        saida.append(novo)
    return saida


def agrega_grupo(docs, espec):
    id_spec = espec["_id"]
    grupos = {}
    for doc in docs:
        if id_spec[0] == "str" and id_spec[1].startswith("$"):
            chave_t = doc.get(id_spec[1][1:], ("null", None))
        else:
            chave_t = id_spec
        grupos.setdefault(repr(chave_t), (chave_t, []))[1].append(doc)
    saida = []
    for chave_t, docs_g in grupos.values():
        novo = {"_id": chave_t}
        for campo, espec_acc in espec.items():
            if campo == "_id":
                continue
            _t, opmap = espec_acc
            for op, arg in opmap.items():
                if arg[0] == "str" and arg[1].startswith("$"):
                    vals = [d.get(arg[1][1:], ("null", None)) for d in docs_g]
                else:
                    vals = [arg] * len(docs_g)
                nums = [v[1] for v in vals]
                inteiro = all(v[0] in ("i64", "i32") for v in vals)
                if op == "$sum":
                    r = sum(nums)
                elif op == "$avg":
                    r = sum(nums) / len(nums)
                elif op == "$min":
                    r = min(nums)
                elif op == "$max":
                    r = max(nums)
                else:
                    raise ErroBSON("acumulador nao suportado pelo mock: " + op)
                if op == "$avg" or not inteiro:
                    novo[campo] = ("double", float(r))
                else:
                    novo[campo] = ("i64", int(r))
        saida.append(novo)
    return saida


def chave_ord(t):
    if t[0] in ("i64", "i32", "double"):
        return (0, t[1])
    if t[0] == "str":
        return (1, t[1])
    if t[0] == "null":
        return (2, None)
    return (3, repr(t))


def agrega_sort(docs, espec):
    for campo in reversed(list(espec.keys())):
        direcao = espec[campo][1]
        docs.sort(key=lambda d: chave_ord(d.get(campo, ("null", None))),
                  reverse=(direcao < 0))
    return docs


def agrega_pipeline(docs, pipeline):
    for etapa in pipeline:
        for op, espec in etapa.items():
            _tag, e = espec
            if op == "$match":
                docs = [d for d in docs if casa_match(d, e)]
            elif op == "$project":
                docs = agrega_project(docs, e)
            elif op == "$group":
                docs = agrega_grupo(docs, e)
            elif op == "$sort":
                docs = agrega_sort(docs, e)
            elif op == "$limit":
                docs = docs[:espec[1]]
            elif op == "$skip":
                docs = docs[espec[1]:]
            else:
                raise ErroBSON("etapa nao suportada pelo mock: " + op)
    return docs


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
                fila.append(dict(doc))
            log.write("INSERT %s.%s\n" % (db, colecao))
            log.flush()
            return op_msg_response(req_id, {"ok": ("double", 1.0), "n": ("i32", 1)})
        if "aggregate" in cmd:
            _tag, colecao = cmd["aggregate"]
            _tag, db = cmd["$db"]
            # pipeline vem como documento-array BSON: {"0": ("doc", etapa), ...}
            _tag, pipeline_doc = cmd["pipeline"]
            pipeline = [e for _t, e in pipeline_doc.values()]
            docs = [dict(d) for d in store.get((db, colecao), [])]
            docs = agrega_pipeline(docs, pipeline)
            log.write("AGGREGATE %s.%s n=%d\n" % (db, colecao, len(docs)))
            log.flush()
            return op_msg_response(req_id, {
                "ok": ("double", 1.0),
                "cursor": ("doc", {"firstBatch": ("arr", [("doc", d) for d in docs]),
                                   "id": ("i64", 0)}),
            })
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

# --- aggregate via tilt executar ---------------------------------------------
out=$(
  env MONGO_URL="mongodb://127.0.0.1:$PORTA/loja" \
    "$BIN" executar "${2:-${0%/*}/fixtures/mongo_aggr.tilt}"
)

kill "$mock_pid" 2>/dev/null || true
mock_pid=""

fail=0
# $match $gte 100 -> ana 400 / bob 200; $sort total -1 + $limit 1 -> so ana
echo "$out" | grep -q "^1$" || { echo "saida sem tamanho 1 (r1): $out"; fail=1; }
echo "$out" | grep -q "^ana$" || { echo "saida sem _id ana (r1): $out"; fail=1; }
echo "$out" | grep -q "^400$" || { echo "saida sem total 400 (r1): $out"; fail=1; }
# $project cliente/valor sem _id: 4 docs, primeiro ana/100
echo "$out" | grep -q "^4$" || { echo "saida sem tamanho 4 (r2): $out"; fail=1; }
echo "$out" | grep -q "^100$" || { echo "saida sem valor 100 (r2): $out"; fail=1; }
# $avg por cat: a -> 200, b -> 125
echo "$out" | grep -q "^2$" || { echo "saida sem tamanho 2 (r3): $out"; fail=1; }
echo "$out" | grep -q "^a$" || { echo "saida sem _id a (r3): $out"; fail=1; }
echo "$out" | grep -q "^b$" || { echo "saida sem _id b (r3): $out"; fail=1; }
echo "$out" | grep -q "^125$" || { echo "saida sem media 125 (r3): $out"; fail=1; }

inserts=$(grep -c "^INSERT " "$tmp/log" || true)
aggrs=$(grep -c "^AGGREGATE " "$tmp/log" || true)
[ "$inserts" = "4" ] || { echo "esperado 4 INSERT, obtido $inserts"; cat "$tmp/log"; fail=1; }
[ "$aggrs" = "3" ] || { echo "esperado 3 AGGREGATE, obtido $aggrs"; cat "$tmp/log"; fail=1; }
grep -q "^INSERT loja.vendas" "$tmp/log" || { echo "log sem INSERT loja.vendas"; cat "$tmp/log"; fail=1; }
grep -q "^AGGREGATE loja.vendas n=1" "$tmp/log" || { echo "log sem AGGREGATE loja.vendas n=1"; cat "$tmp/log"; fail=1; }
grep -q "^AGGREGATE loja.vendas n=4" "$tmp/log" || { echo "log sem AGGREGATE loja.vendas n=4"; cat "$tmp/log"; fail=1; }
grep -q "^AGGREGATE loja.vendas n=2" "$tmp/log" || { echo "log sem AGGREGATE loja.vendas n=2"; cat "$tmp/log"; fail=1; }

[ "$fail" = 0 ] && echo "mongo_aggr_test ok"
exit "$fail"
