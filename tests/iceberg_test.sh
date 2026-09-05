#!/usr/bin/env sh
# Integration test for the Iceberg connector (`escrever_iceberg`/`anexar_iceberg`/
# `ler_iceberg`): runs a write+append+read roundtrip with `tilt executar`, then a
# python3 verifier inspects the table directory — parses the metadata JSON
# (format-version 2, snapshot chain with parent, snapshot log) and the Avro OCF
# files (manifest list + manifest) to check the final snapshot references the
# appended data file with the right record_count.
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/iceberg_roundtrip.tilt}"
case "$FIXTURE" in
  /*) ;;
  *) FIXTURE="$(pwd)/$FIXTURE" ;;
esac

command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste iceberg"
  exit 0
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cd "$tmp"
out=$("$BIN" executar "$FIXTURE")

echo "$out" | grep -q "^3$" || { echo "esperado '3' (linhas lidas): $out"; exit 1; }
echo "$out" | grep -q "ana 40 carla" || { echo "saida inesperada: $out"; exit 1; }

TAB="$tmp/tabela_iceberg"
[ -d "$TAB/metadata" ] || { echo "diretorio metadata ausente"; exit 1; }

python3 - "$TAB" <<'PYEOF'
import glob
import json
import os
import sys

tab = sys.argv[1]


class Erro(Exception):
    pass


class Dec:
    """Decoder binario Avro (varint/zigzag + primitivos)."""

    def __init__(self, b):
        self.b = b
        self.p = 0

    def long(self):
        v = s = 0
        while True:
            if self.p >= len(self.b):
                raise Erro("varint truncado")
            x = self.b[self.p]
            self.p += 1
            v |= (x & 0x7F) << s
            if not (x & 0x80):
                break
            s += 7
        return (v >> 1) ^ -(v & 1)

    def string(self):
        n = self.long()
        if n < 0 or self.p + n > len(self.b):
            raise Erro("string truncada")
        s = self.b[self.p : self.p + n]
        self.p += n
        return s

    def boolean(self):
        if self.p >= len(self.b):
            raise Erro("boolean truncado")
        x = self.b[self.p]
        self.p += 1
        return bool(x)

    def raw(self, n):
        if self.p + n > len(self.b):
            raise Erro("bytes truncados")
        s = self.b[self.p : self.p + n]
        self.p += n
        return s


def decode(d, schema):
    """Decodifica um registro conforme o schema JSON do header."""
    if isinstance(schema, list):  # uniao
        idx = d.long()
        if idx == 0:
            return None
        return decode(d, schema[idx])
    if isinstance(schema, str):
        if schema == "null":
            return None
        if schema == "boolean":
            return d.boolean()
        if schema in ("int", "long"):
            return d.long()
        if schema == "string":
            return d.string().decode("utf-8")
        if schema == "bytes":
            return d.string()
        raise Erro("tipo avro nao suportado: " + schema)
    t = schema["type"]
    if t == "record":
        return {f["name"]: decode(d, f["type"]) for f in schema["fields"]}
    if t == "array":
        out = []
        while True:
            n = d.long()
            if n == 0:
                break
            if n < 0:
                d.long()
                n = -n
            for _ in range(n):
                out.append(decode(d, schema["items"]))
        return out
    if t == "map":
        out = {}
        while True:
            n = d.long()
            if n == 0:
                break
            if n < 0:
                d.long()
                n = -n
            for _ in range(n):
                k = d.string().decode("utf-8")
                out[k] = decode(d, schema["values"])
        return out
    return decode(d, t)


def ocf(path):
    """Le um Avro OCF: header (metadata map + sync) e todos os blocos."""
    b = open(path, "rb").read()
    if b[:4] != b"Obj\x01":
        raise Erro(path + ": magic Obj\\x01 ausente")
    d = Dec(b)
    d.p = 4
    meta = {}
    while True:
        n = d.long()
        if n == 0:
            break
        if n < 0:
            d.long()
            n = -n
        for _ in range(n):
            k = d.string().decode("utf-8")
            meta[k] = d.string()
    sync = d.raw(16)
    if meta.get("avro.codec", b"null") != b"null":
        raise Erro("codec avro nao suportado: " + meta["avro.codec"].decode())
    if "avro.schema" not in meta:
        raise Erro("avro.schema ausente no header")
    schema = json.loads(meta["avro.schema"])
    out = []
    while d.p < len(b):
        count = d.long()
        size = d.long()
        blk = Dec(d.raw(size))
        for _ in range(count):
            out.append(decode(blk, schema))
        if d.raw(16) != sync:
            raise Erro("sync marker invalido em " + path)
        if blk.p != size:
            raise Erro("registro maior que o bloco em " + path)
    return schema, out


def sem_file(p):
    return p[7:] if p.startswith("file://") else p


# (a) metadata: format-version 2, 2 snapshots, append com parent na cadeia ----
mds = sorted(glob.glob(os.path.join(tab, "metadata", "v*.metadata.json")))
if len(mds) != 2:
    raise Erro("esperados 2 metadata files (v0 e v1), obtidos %d" % len(mds))
md = json.load(open(mds[-1]))
if md["format-version"] != 2:
    raise Erro("format-version != 2")
snaps = md["snapshots"]
if len(snaps) != 2:
    raise Erro("esperados 2 snapshots, obtidos %d" % len(snaps))
if snaps[0].get("parent-snapshot-id") is not None:
    raise Erro("snapshot inicial nao deve ter parent")
cur_id = md["current-snapshot-id"]
if snaps[1]["snapshot-id"] != cur_id:
    raise Erro("current-snapshot-id nao e o ultimo snapshot")
if snaps[1].get("parent-snapshot-id") != snaps[0]["snapshot-id"]:
    raise Erro("append sem parent = snapshot anterior")
if snaps[0]["summary"]["operation"] != "overwrite" or snaps[1]["summary"]["operation"] != "append":
    raise Erro("operations incorretas (esperado overwrite + append)")
log = md["snapshot-log"]
if len(log) != 2 or log[0]["snapshot-id"] != snaps[0]["snapshot-id"] or log[1]["snapshot-id"] != cur_id:
    raise Erro("snapshot-log deve listar os 2 snapshots em ordem")
fields = [f["name"] for f in md["schemas"][0]["fields"]]
if fields != ["nome", "idade", "nota"]:
    raise Erro("schema inesperado: " + repr(fields))

# (b) manifest list + manifest do snapshot final: ADD do arquivo do append ----
_, mlist = ocf(sem_file(snaps[1]["manifest-list"]))
if len(mlist) != 1:
    raise Erro("manifest list do append deve ter 1 registro")
m0 = mlist[0]
if m0["added_files_count"] != 1 or m0["added_snapshot_id"] != cur_id:
    raise Erro("manifest list: contagens/snapshot incorretos")
mpath = sem_file(m0["manifest_path"])
if not os.path.exists(mpath):
    raise Erro("manifest ausente: " + mpath)
if os.path.getsize(mpath) != m0["manifest_length"]:
    raise Erro("manifest_length diverge do tamanho real")

schema, entries = ocf(mpath)
if len(entries) != 1:
    raise Erro("manifest do append deve ter 1 entrada")
e = entries[0]
df = e["data_file"]
if e["status"] != 1:
    raise Erro("entrada do append deve ter status 1 (ADDED)")
if e["snapshot_id"] != cur_id:
    raise Erro("snapshot_id da entrada != snapshot corrente")
if df["record_count"] != 1:
    raise Erro("record_count do append deve ser 1, obtido %r" % df["record_count"])
if df["file_format"] != "PARQUET" or df["content"] != 0:
    raise Erro("data_file: format/content incorretos")
dpath = sem_file(df["file_path"])
if not os.path.exists(dpath):
    raise Erro("data file ausente: " + dpath)
if os.path.getsize(dpath) != df["file_size_in_bytes"]:
    raise Erro("file_size_in_bytes diverge do tamanho real")
if not dpath.endswith(".parquet") or "/data/" not in dpath:
    raise Erro("data file fora de data/: " + dpath)

# (c) manifest do snapshot inicial: ainda referencia o 1o data file (2 linhas) -
_, mlist0 = ocf(sem_file(snaps[0]["manifest-list"]))
_, entries0 = ocf(sem_file(mlist0[0]["manifest_path"]))
if len(entries0) != 1 or entries0[0]["status"] != 1:
    raise Erro("manifest inicial: esperada 1 entrada ADDED")
if entries0[0]["data_file"]["record_count"] != 2:
    raise Erro("record_count inicial deve ser 2")
if sem_file(entries0[0]["data_file"]["file_path"]) == dpath:
    raise Erro("snapshots inicial e final apontam o mesmo data file")

print("iceberg verifier ok")
PYEOF

echo "iceberg_test ok"
