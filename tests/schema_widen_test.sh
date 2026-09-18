#!/usr/bin/env sh
# Widening de schema (int->long, float->double) no anexar_*:
# tabelas externas com tipos estreitos aceitam appends tilt (largos),
# com promocao registrada no log (Delta) / novo schema-id com field-ids
# estaveis (Iceberg). Precisa de python3 + pyarrow (Delta) e/ou pyiceberg
# (Iceberg); pula a parte sem a lib.
set -eu

BIN="$1"

command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste schema_widen"
  exit 0
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

# --- Delta: parquet INT32 + log integer, append long -> promove a long -----
if python3 -c "import pyarrow" 2>/dev/null; then
  mkdir -p "$tmp/dwid/_delta_log"
  python3 - "$tmp/dwid" <<'PYEOF'
import json, sys
import pyarrow as pa, pyarrow.parquet as pq
d = sys.argv[1]
t = pa.table({"nome": ["ana"], "idade": pa.array([30], type=pa.int32())})
pq.write_table(t, d + "/part-0.parquet")
schema = {"type": "struct", "fields": [
  {"name": "nome", "type": "string", "nullable": True, "metadata": {}},
  {"name": "idade", "type": "integer", "nullable": True, "metadata": {}}]}
log = open(d + "/_delta_log/00000000000000000000.json", "w")
log.write(json.dumps({"protocol": {"minReaderVersion": 1, "minWriterVersion": 2}}) + "\n")
log.write(json.dumps({"metaData": {"id": "ext-w", "format": {"provider": "parquet", "options": {}},
  "schemaString": json.dumps(schema), "partitionColumns": [], "configuration": {},
  "createdTime": 1}}) + "\n")
log.write(json.dumps({"add": {"path": "part-0.parquet", "partitionValues": {}, "size": 100,
  "modificationTime": 2, "dataChange": True}}) + "\n")
PYEOF
  cat >"$tmp/dw.tilt" <<'TILT'
pipeline p:
  passos:
    - t2 = [{ nome: "bia", idade: 5 }]
    - anexar_delta t2, "dwid"
    - r = ler_delta "dwid"
    - imprimir tamanho r
TILT
  out=$(cd "$tmp" && "$BIN" executar dw.tilt) || { echo "anexar com widen falhou: $out"; fail=1; }
  echo "$out" | grep -q "^2$" || { echo "leitura pos-widen sem 2 linhas: $out"; fail=1; }
  python3 - "$tmp/dwid" <<'PYEOF' || fail=1
import glob, json, sys
d = sys.argv[1]
v1 = sorted(glob.glob(d + "/_delta_log/*.json"))[-1]
sch = None
for l in open(v1):
    m = json.loads(l)
    if "metaData" in m:
        sch = json.loads(m["metaData"]["schemaString"])
tys = {f["name"]: f["type"] for f in sch["fields"]}
assert tys.get("idade") == "long", tys
import pyarrow.dataset as ds
rows = ds.dataset(d, format="parquet", partitioning="hive").to_table().to_pylist()
assert len(rows) == 2, rows
print("delta widen ok")
PYEOF
  # Negativo: texto x long continua erro claro.
  cat >"$tmp/dwneg.tilt" <<'TILT'
pipeline p:
  passos:
    - t2 = [{ nome: 42, idade: 5 }]
    - anexar_delta t2, "dwid"
TILT
  if (cd "$tmp" && "$BIN" executar dwneg.tilt 2>&1); then
    echo "divergencia texto/long devia falhar"; fail=1
  else
    (cd "$tmp" && "$BIN" executar dwneg.tilt 2>&1) | grep -q "tipo divergente" || {
      echo "erro sem mensagem de divergencia"; fail=1; }
  fi
else
  echo "pyarrow ausente; parte Delta pulada"
fi

# --- Iceberg: metadata int + append long -> schema-id novo, id estavel -----
if python3 -c "import pyiceberg.table" 2>/dev/null; then
  cat >"$tmp/iw.tilt" <<'TILT'
pipeline p:
  passos:
    - t = [{ nome: "ana", qtd: 3 }]
    - escrever_iceberg t, "itab"
TILT
  (cd "$tmp" && "$BIN" executar iw.tilt >/dev/null) || { echo "escrever_iceberg falhou"; fail=1; }
  python3 - "$tmp/itab" <<'PYEOF'
import glob, json, sys
d = sys.argv[1]
metas = sorted(glob.glob(d + "/metadata/v*.metadata.json"))
m = json.load(open(metas[-1]))
for f in m["schemas"][0]["fields"]:
    if f["name"] == "qtd":
        f["type"] = "int"
json.dump(m, open(metas[-1], "w"))
PYEOF
  cat >"$tmp/iw2.tilt" <<'TILT'
pipeline p:
  passos:
    - t2 = [{ nome: "bia", qtd: 5 }]
    - anexar_iceberg t2, "itab"
    - r = ler_iceberg "itab"
    - imprimir tamanho r
TILT
  out=$(cd "$tmp" && "$BIN" executar iw2.tilt) || { echo "anexar iceberg com widen falhou: $out"; fail=1; }
  echo "$out" | grep -q "^2$" || { echo "leitura iceberg pos-widen sem 2 linhas: $out"; fail=1; }
  python3 - "$tmp/itab" <<'PYEOF' || fail=1
import glob, sys
from pyiceberg.table import StaticTable
d = sys.argv[1]
metas = sorted(glob.glob(d + "/metadata/v*.metadata.json"))
t = StaticTable.from_metadata(metas[-1])
sch = {s.schema_id: [(f.field_id, f.name, str(f.field_type)) for f in s.fields]
       for s in t.schemas().values()}
old = dict((n, (i, ty)) for i, n, ty in sch[0])
new = dict((n, (i, ty)) for i, n, ty in sch[max(sch)])
assert old["qtd"][0] == new["qtd"][0] == 2, sch
assert old["qtd"][1] == "int" and new["qtd"][1] == "long", sch
rows = t.scan().to_arrow().to_pylist()
assert len(rows) == 2, rows
print("iceberg widen ok")
PYEOF
else
  echo "pyiceberg ausente; parte Iceberg pulada"
fi

[ "$fail" = 0 ] && echo "schema_widen_test ok"
exit "$fail"
