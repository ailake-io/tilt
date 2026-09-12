#!/usr/bin/env sh
# Integration test for `tilt servir-catalogo` (fase 30): the fixture writes two
# Iceberg tables into a temp dir (plain write+append -> v1.metadata.json, and a
# table partitioned by estado), the catalog server is started in background and
# a python3 (urllib) client exercises the read-only Iceberg REST subset:
# config/namespaces/tables listings, loadTable (metadata-location http, snapshot
# manifest-lists rewritten to this server's URLs), byte-identical downloads of
# the metadata.json and of a manifest .avro via those URLs, path traversal
# (../, symlink out of root) -> 403/404, unknown table -> 404
# NoSuchTableException and read-only writes (createTable/commit/HEAD) -> 501.
# Server is killed in the trap.
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/iceberg_catalogo.tilt}"
case "$FIXTURE" in
  /*) ;;
  *) FIXTURE="$(pwd)/$FIXTURE" ;;
esac
PORT_BASE="${TILT_TEST_PORT:-8791}"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste iceberg_catalog"
  exit 0
}
command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste iceberg_catalog"
  exit 0
}

tmp=$(mktemp -d)
cd "$tmp"

out=$("$BIN" executar "$FIXTURE")
printf '%s\n' "$out"
echo "$out" | grep -q "catalogo pronto" || {
  echo "fixture nao produziu o esperado: $out"
  exit 1
}

# sobe o servidor na primeira porta livre a partir de PORT_BASE. O readiness
# exige o "escutando" no log (prova que E o nosso servidor que respondeu —
# a linha sai apos o bind, antes do accept loop) + /v1/config OK.
PORTA=""
srv=""
for p in $(seq "$PORT_BASE" $((PORT_BASE + 19))); do
  "$BIN" servir-catalogo "$tmp" --porta "$p" >catalogo.log 2>&1 &
  srv=$!
  for _ in $(seq 1 50); do
    if ! kill -0 "$srv" 2>/dev/null; then break; fi  # morreu (bind) -> proxima
    if grep -q "escutando" catalogo.log 2>/dev/null &&
       curl -sf "http://127.0.0.1:$p/v1/config" >/dev/null 2>&1; then
      PORTA=$p
      break 2
    fi
    sleep 0.1
  done
  kill "$srv" 2>/dev/null || true
  wait "$srv" 2>/dev/null || true
  srv=""
done
[ -n "$PORTA" ] || {
  echo "servir-catalogo nao iniciou a partir da porta $PORT_BASE"
  cat catalogo.log
  exit 1
}
trap 'kill "$srv" 2>/dev/null || true; rm -rf "$tmp"' EXIT

grep -q "default.tabela_iceberg" catalogo.log || {
  echo "startup nao listou as tabelas servidas:"
  cat catalogo.log
  exit 1
}
grep -q "default.vendas_part" catalogo.log || {
  echo "startup nao listou a tabela particionada:"
  cat catalogo.log
  exit 1
}

BASE="http://127.0.0.1:$PORTA/v1"

python3 - "$BASE" "$tmp" <<'PYEOF'
import glob
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

base = sys.argv[1]
root = sys.argv[2]


def falha(msg):
    raise SystemExit("FALHA: " + msg)


def get(url, expect=(200,)):
    try:
        with urllib.request.urlopen(url) as r:
            status, body = r.status, r.read()
    except urllib.error.HTTPError as e:
        status, body = e.code, e.read()
    if status not in expect:
        falha("GET %s -> %d (esperado %s): %s" % (url, status, expect, body[:300]))
    return body


def local_da_url(url):
    # http://host/v1/files/<rel-ao-root, segmentos escapados> -> path local
    path = urllib.parse.urlparse(url).path
    prefix = "/v1/files/"
    if not path.startswith(prefix):
        falha("URL nao e /v1/files/<rel>: %r" % url)
    local = os.path.join(root, urllib.parse.unquote(path[len(prefix):]))
    if not local.startswith(root + os.sep):
        falha("URL fora do root: %r -> %r" % (url, local))
    return local


def confere_bytes(url, path):
    body = get(url)
    with open(path, "rb") as f:
        local = f.read()
    if body != local:
        falha("bytes divergem: %s (%d B) vs %s (%d B)" % (url, len(body), path, len(local)))
    return body


# --- config (com e sem query string, como o cliente REST do Spark) ----------
cfg = json.loads(get(base + "/config"))
if "defaults" not in cfg or "overrides" not in cfg:
    falha("/config sem defaults/overrides: %r" % cfg)
get(base + "/config?warehouse=/x&prefix=v1")

# --- namespaces --------------------------------------------------------------
ns = json.loads(get(base + "/namespaces"))
if ["default"] not in ns.get("namespaces", []):
    falha("/namespaces sem [default]: %r" % ns)
nsd = json.loads(get(base + "/namespaces/default"))
if nsd.get("namespace") != ["default"]:
    falha("/namespaces/default diverge: %r" % nsd)

# --- listagem de tabelas -----------------------------------------------------
ids = json.loads(get(base + "/namespaces/default/tables"))["identifiers"]
nomes = sorted(i["name"] for i in ids)
if nomes != ["tabela_iceberg", "vendas_part"]:
    falha("tabelas listadas divergem: %r" % nomes)

# --- loadTable: metadata-location http + manifest-lists reescritas -----------
load = json.loads(get(base + "/namespaces/default/tables/tabela_iceberg"))
ml = load.get("metadata-location", "")
if not (ml.startswith("http://") and "/v1/files/" in ml):
    falha("metadata-location nao e URL http deste servidor: %r" % ml)
md = load.get("metadata")
if not isinstance(md, dict) or md.get("format-version") != 2:
    falha("metadata ausente/format-version diverge: %r" % (md or {})[:200])
snaps = md.get("snapshots", [])
if len(snaps) != 2:
    falha("esperados 2 snapshots (write+append), obtidos %d" % len(snaps))

for s in snaps:
    loc = s.get("manifest-list", "")
    if not loc.startswith("http://"):
        falha("manifest-list nao reescrita para http: %r" % loc)
    confere_bytes(loc, local_da_url(loc))  # manifest list servido byte a byte

# metadata-location baixa o v<N>.metadata.json mais recente, byte a byte
meta_path = local_da_url(ml)
if not re.search(r"/v\d+\.metadata\.json$", meta_path):
    falha("metadata-location nao aponta v<N>.metadata.json: %r" % meta_path)
if not meta_path.endswith("/v1.metadata.json"):
    falha("esperado o metadata mais recente v1 (write+append): %r" % meta_path)
confere_bytes(ml, meta_path)

# um manifest (.avro) tambem e servido byte a byte — tanto pela URL path-style
# gerada pelo servidor quanto pela forma ?path=<abs>
manifest = sorted(glob.glob(os.path.join(root, "tabela_iceberg", "metadata", "*-m0.avro")))[0]
rel = os.path.relpath(manifest, root)
confere_bytes(base + "/files/" + urllib.parse.quote(rel, safe="/"), manifest)
confere_bytes(base + "/files?path=" + urllib.parse.quote(manifest, safe=""), manifest)
# HEAD no arquivo -> 200
req = urllib.request.Request(base + "/files/" + urllib.parse.quote(rel, safe="/"),
                             method="HEAD")
with urllib.request.urlopen(req) as r:
    if r.status != 200:
        falha("HEAD arquivo -> %d" % r.status)

# loadTable da tabela particionada mantem o partition spec
load2 = json.loads(get(base + "/namespaces/default/tables/vendas_part"))
campos = load2["metadata"]["partition-specs"][0]["fields"]
if not campos or campos[0].get("source-id") != 1:
    falha("partition spec da tabela particionada diverge: %r" % campos)

# --- tabela inexistente -> 404 NoSuchTableException ---------------------------
body = get(base + "/namespaces/default/tables/tabela_fantasma", expect=(404,)).decode()
if "NoSuchTableException" not in body or "tabela_fantasma" not in body:
    falha("404 sem NoSuchTableException: %r" % body)

# --- path traversal -> 403/404 (forma ?path= e forma path-style) ---------------
alvos = [
    "/etc/passwd",
    root + "/../etc/passwd",
    root + "/tabela_iceberg/metadata/../../etc/passwd",
    "file:///etc/passwd",
]
fora = os.path.join(os.path.dirname(root), "fora_catalogo_%d.txt" % os.getpid())
with open(fora, "w") as f:
    f.write("segredo")
link = os.path.join(root, "link_fora")
os.symlink(fora, link)
alvos.append(link)
try:
    for alvo in alvos:
        urls = [base + "/files?path=" + urllib.parse.quote(alvo, safe="")]
        if alvo.startswith(root + "/"):
            rel_trav = os.path.relpath(alvo, root)
            urls.append(base + "/files/" + urllib.parse.quote(rel_trav, safe="/"))
        for url in urls:
            try:
                with urllib.request.urlopen(url) as r:
                    falha("traversal nao bloqueado: %s -> %d" % (url, r.status))
            except urllib.error.HTTPError as e:
                if e.code not in (403, 404):
                    falha("traversal com status inesperado: %s -> %d" % (url, e.code))
finally:
    os.unlink(link)
    os.unlink(fora)

# arquivo inexistente dentro do root -> 404
get(base + "/files?path=" + urllib.parse.quote(root + "/nada.parquet", safe=""),
    expect=(404,))
get(base + "/files/" + urllib.parse.quote("nada.parquet", safe="/"), expect=(404,))

# --- catalogo read-only: escritas -> 501 ---------------------------------------
def metodo(url, m):
    req = urllib.request.Request(url, data=b"{}" if m in ("POST", "PUT") else None, method=m)
    try:
        with urllib.request.urlopen(req) as r:
            falha("%s %s -> %d, esperava 501" % (m, url, r.status))
    except urllib.error.HTTPError as e:
        if e.code != 501:
            falha("%s %s -> %d, esperava 501" % (m, url, e.code))
        return e.read().decode()

body = metodo(base + "/namespaces/default/tables", "POST")
if "read-only" not in body:
    falha("501 sem mensagem read-only: %r" % body)
body = metodo(base + "/namespaces/default/tables/tabela_iceberg/transactions", "POST")
if "read-only" not in body:
    falha("501 (transactions) sem mensagem read-only: %r" % body)
metodo(base + "/namespaces/default/tables/tabela_iceberg", "HEAD")
metodo(base + "/namespaces/default/tables/tabela_iceberg", "DELETE")

# rota fora do prefixo -> 404
get(base + "inexistente", expect=(404,))
get(base.replace("/v1", "/v2") + "/namespaces", expect=(404,))

print("cliente REST ok: config, namespaces, loadTable, bytes, traversal, read-only")
PYEOF

echo "iceberg_catalog_test ok"
