#!/usr/bin/env sh
# Integration test for the Iceberg REST catalog (fase 29): spins up a mock
# Iceberg REST server in python3 (http.server) implementing the subset the
# tilt client speaks — createTable / loadTable / transactions (requirements +
# updates applied over an in-memory TableMetadata, materialized as
# <location>/metadata/v<N>.metadata.json) —, then runs `tilt executar` on
# fixtures (write+append+read roundtrip, append with schema evolution, and
# overwrite of an existing table) with ICEBERG_CATALOG=rest + ICEBERG_URI,
# checks the output, the mock's request log and two clear errors (missing
# ICEBERG_URI, table not found). Finally validates the server-produced
# metadata with pyiceberg (StaticTable) when it is installed.
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/iceberg_roundtrip.tilt}"
FIX_EVO="${3:-${0%/*}/fixtures/iceberg_rest_evolucao.tilt}"
FIX_FANTASMA="${4:-${0%/*}/fixtures/iceberg_rest_fantasma.tilt}"
FIX_SOBRESCRETA="${5:-${0%/*}/fixtures/iceberg_rest_sobrescrita.tilt}"
case "$FIXTURE" in
  /*) ;;
  *) FIXTURE="$(pwd)/$FIXTURE" ;;
esac
case "$FIX_EVO" in
  /*) ;;
  *) FIX_EVO="$(pwd)/$FIX_EVO" ;;
esac
case "$FIX_FANTASMA" in
  /*) ;;
  *) FIX_FANTASMA="$(pwd)/$FIX_FANTASMA" ;;
esac
case "$FIX_SOBRESCRETA" in
  /*) ;;
  *) FIX_SOBRESCRETA="$(pwd)/$FIX_SOBRESCRETA" ;;
esac
PORT_BASE="${TILT_TEST_PORT:-8681}"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste iceberg_rest"
  exit 0
}
command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste iceberg_rest"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# --- mock do Iceberg REST catalog ----------------------------------------------
python3 - "$PORT_BASE" "$tmp/porta" "$tmp/log" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import http.server
import json
import os
import sys
import time
import uuid

port_base = int(sys.argv[1])
port_file = sys.argv[2]
log_path = sys.argv[3]

log = open(log_path, "a", encoding="utf-8")
tables = {}  # nome -> estado (metadata em memoria + versao + commitada)


class Erro(Exception):
    def __init__(self, message, tipo="IllegalArgumentException", code="400"):
        super().__init__(message)
        self.tipo = tipo
        self.code = code


def agora_ms():
    return int(time.time() * 1000)


def sem_file(p):
    return p[7:] if p.startswith("file://") else p


def gravar_metadata(t):
    caminho = sem_file(t["metadata_location"])
    os.makedirs(os.path.dirname(caminho), exist_ok=True)
    with open(caminho, "w", encoding="utf-8") as f:
        json.dump(t["md"], f, indent=2)


def materializar(t):
    if not os.path.exists(sem_file(t["metadata_location"])):
        gravar_metadata(t)


def nova_metadata(location, schema, spec, properties):
    schema = dict(schema)
    schema["schema-id"] = 0
    last_column_id = max([f.get("id", 0) for f in schema.get("fields", [])] + [0])
    spec = dict(spec) if spec else {"spec-id": 0, "fields": []}
    spec["spec-id"] = 0
    return {
        "format-version": 1,
        "table-uuid": str(uuid.uuid4()),
        "location": location,
        "last-sequence-number": 0,
        "last-updated-ms": agora_ms(),
        "last-column-id": last_column_id,
        "schemas": [schema],
        "current-schema-id": 0,
        "partition-specs": [spec],
        "default-spec-id": 0,
        "properties": dict(properties or {}),
        "current-snapshot-id": -1,
        "snapshots": [],
        "snapshot-log": [],
        "metadata-log": [],
        "sort-orders": [{"order-id": 0, "fields": []}],
        "default-sort-order-id": 0,
        "refs": {},
    }


def aplicar_updates(t, updates):
    md = t["md"]
    for u in updates:
        a = u.get("action")
        if a == "assign-uuid":
            md["table-uuid"] = u["uuid"]
        elif a == "upgrade-format-version":
            md["format-version"] = u["format-version"]
        elif a == "add-schema":
            s = u["schema"]
            s["schema-id"] = s.get("schema-id", 0)
            if all(x["schema-id"] != s["schema-id"] for x in md["schemas"]):
                md["schemas"].append(s)
            if "last-column-id" in u:
                md["last-column-id"] = u["last-column-id"]
        elif a == "set-current-schema":
            md["current-schema-id"] = u["schema-id"]
        elif a == "add-partition-spec":
            sp = u["spec"]
            sp["spec-id"] = sp.get("spec-id", 0)
            if all(x["spec-id"] != sp["spec-id"] for x in md["partition-specs"]):
                md["partition-specs"].append(sp)
        elif a == "set-default-spec":
            md["default-spec-id"] = u["spec-id"]
        elif a == "set-location":
            md["location"] = u["location"]
        elif a == "set-properties":
            md["properties"].update(u.get("properties", {}))
        elif a == "add-snapshot":
            snap = u["snapshot"]
            md["snapshots"] = [s for s in md["snapshots"]
                               if s["snapshot-id"] != snap["snapshot-id"]]
            md["snapshots"].append(snap)
            md["snapshot-log"].append({"timestamp-ms": snap["timestamp-ms"],
                                       "snapshot-id": snap["snapshot-id"]})
        elif a == "set-snapshot-ref":
            md["refs"][u["ref-name"]] = {"snapshot-id": u["snapshot-id"],
                                         "type": u.get("type", "branch")}
            md["current-snapshot-id"] = u["snapshot-id"]
        elif a == "remove-snapshot-ref":
            md["refs"].pop(u["ref-name"], None)
        elif a == "remove-snapshots":
            ids = set(u["snapshot-ids"])
            md["snapshots"] = [s for s in md["snapshots"]
                               if s["snapshot-id"] not in ids]
            md["snapshot-log"] = [e for e in md["snapshot-log"]
                                  if e["snapshot-id"] not in ids]
        else:
            raise Erro("update nao suportado: " + str(a))


def checar_requirements(t, reqs):
    md = t["md"]
    for r in reqs:
        tp = r.get("type")
        if tp == "assert-create":
            if t["commitada"]:
                raise Erro("assert-create em tabela ja commitada",
                           "CommitFailedException", "409")
        elif tp == "assert-current-snapshot-id":
            if md["current-snapshot-id"] != r.get("snapshot-id"):
                raise Erro("assert-current-snapshot-id: esperado %r, obtido %r" %
                           (r.get("snapshot-id"), md["current-snapshot-id"]),
                           "CommitFailedException", "409")
        elif tp == "assert-table-uuid":
            if md["table-uuid"] != r.get("uuid"):
                raise Erro("assert-table-uuid falhou", "CommitFailedException", "409")
        else:
            raise Erro("requirement nao suportado: " + str(tp))


class MockRest(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _responder(self, code, body=b""):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _erro(self, e):
        self._responder(int(e.code), json.dumps(
            {"error": {"message": str(e), "type": e.tipo, "code": e.code}}))

    def _body_json(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b""
        try:
            return json.loads(raw or b"{}")
        except ValueError as e:
            raise Erro("corpo invalido: " + str(e))

    def _load_table(self, nome, status):
        t = tables[nome]
        materializar(t)
        log.write("LOAD %s %d\n" % (nome, status))
        log.flush()
        self._responder(status, json.dumps(
            {"metadata-location": t["metadata_location"],
             "metadata": t["md"], "config": {}}))

    def do_GET(self):
        prefix = "/v1/namespaces/default/tables/"
        if not self.path.startswith(prefix):
            self._responder(404, '{"error":{"message":"rota nao encontrada","type":"NotFoundException","code":"404"}}')
            return
        nome = self.path[len(prefix):]
        if nome in tables:
            self._load_table(nome, 200)
        else:
            log.write("LOAD %s 404\n" % nome)
            log.flush()
            self._responder(404, json.dumps(
                {"error": {"message": "tabela nao encontrada: " + nome,
                           "type": "NoSuchTableException", "code": "404"}}))

    def do_POST(self):
        prefix = "/v1/namespaces/default/tables"
        if self.path == prefix:
            self._create_table()
            return
        if self.path.startswith(prefix + "/") and self.path.endswith("/transactions"):
            nome = self.path[len(prefix) + 1:-len("/transactions")]
            self._commit(nome)
            return
        self._responder(404, '{"error":{"message":"rota nao encontrada","type":"NotFoundException","code":"404"}}')

    def _create_table(self):
        try:
            corpo = self._body_json()
            nome = corpo["name"]
            location = corpo["location"]
            if nome in tables:
                raise Erro("tabela ja existe: " + nome, "AlreadyExistsException", "409")
            md = nova_metadata(location, corpo.get("schema", {"type": "struct", "fields": []}),
                               corpo.get("partition-spec"), corpo.get("properties"))
            tables[nome] = {"md": md, "versao": 0, "commitada": False,
                            "metadata_location": location + "/metadata/v0.metadata.json"}
            gravar_metadata(tables[nome])
            log.write("CREATE %s\n" % nome)
            log.flush()
            self._load_table(nome, 200)
        except (Erro, KeyError) as e:
            if isinstance(e, KeyError):
                e = Erro("campo ausente: " + str(e))
            self._erro(e)

    def _commit(self, nome):
        try:
            if nome not in tables:
                raise Erro("tabela nao encontrada: " + nome, "NoSuchTableException", "404")
            corpo = self._body_json()
            t = tables[nome]
            checar_requirements(t, corpo.get("requirements", []))
            aplicar_updates(t, corpo.get("updates", []))
            t["versao"] += 1
            t["commitada"] = True
            t["md"]["last-updated-ms"] = agora_ms()
            t["metadata_location"] = (t["md"]["location"] +
                                      "/metadata/v%d.metadata.json" % t["versao"])
            gravar_metadata(t)
            reqs = ",".join(r.get("type", "?") for r in corpo.get("requirements", []))
            log.write("COMMIT %s reqs=%s\n" % (nome, reqs))
            log.flush()
            self._load_table(nome, 200)
        except Erro as e:
            self._erro(e)


for porta in range(port_base, port_base + 20):
    try:
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", porta), MockRest)
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
[ -s "$tmp/porta" ] || { echo "mock REST nao iniciou"; cat "$tmp/mock_out"; exit 1; }
PORTA=$(cat "$tmp/porta")

env_rest() {
  env ICEBERG_CATALOG=rest ICEBERG_URI="http://127.0.0.1:$PORTA" "$@"
}

# --- write + append + read via REST -------------------------------------------
cd "$tmp"
out=$(env_rest "$BIN" executar "$FIXTURE")
out_evo=$(env_rest "$BIN" executar "$FIX_EVO")
out_sob=$(env_rest "$BIN" executar "$FIX_SOBRESCRETA")

# --- erros claros ----------------------------------------------------------------
fail=0

echo "$out" | grep -q "^3$" || { echo "esperado '3' (linhas lidas): $out"; fail=1; }
echo "$out" | grep -q "ana 40 carla" || { echo "saida inesperada: $out"; fail=1; }
echo "$out_evo" | grep -q "^3$" || { echo "evolucao: esperado '3': $out_evo"; fail=1; }
echo "$out_evo" | grep -q "ana 40 carla" || { echo "evolucao: saida inesperada: $out_evo"; fail=1; }
echo "$out_sob" | grep -q "^2$" || { echo "sobrescrita: esperado '2': $out_sob"; fail=1; }
echo "$out_sob" | grep -q "duda 70" || { echo "sobrescrita: saida inesperada: $out_sob"; fail=1; }

# ler tabela inexistente -> nao zero + mensagem clara
if fantasma=$(env_rest "$BIN" executar "$FIX_FANTASMA" 2>&1); then
  echo "ler tabela inexistente deveria falhar: $fantasma"; fail=1
else
  echo "$fantasma" | grep -q "tabela_fantasma" || { echo "erro sem nome da tabela: $fantasma"; fail=1; }
  echo "$fantasma" | grep -q "nao existe no catalogo REST" || { echo "erro inesperado: $fantasma"; fail=1; }
fi

# ICEBERG_URI ausente com ICEBERG_CATALOG=rest
if sem_uri=$(env ICEBERG_CATALOG=rest "$BIN" executar "$FIXTURE" 2>&1); then
  echo "rodar sem ICEBERG_URI deveria falhar: $sem_uri"; fail=1
else
  echo "$sem_uri" | grep -q "ICEBERG_URI" || { echo "erro sem mencionar ICEBERG_URI: $sem_uri"; fail=1; }
fi

# --- log do mock -------------------------------------------------------------------
creates=$(grep -c "^CREATE tabela_iceberg$" "$tmp/log" || true)
[ "$creates" = "1" ] || { echo "esperado 1 CREATE, obtido $creates"; cat "$tmp/log"; fail=1; }
creates_evo=$(grep -c "^CREATE tabela_evolucao$" "$tmp/log" || true)
[ "$creates_evo" = "1" ] || { echo "esperado 1 CREATE (evolucao), obtido $creates_evo"; cat "$tmp/log"; fail=1; }

commits=$(grep -c "^COMMIT tabela_iceberg reqs=assert-current-snapshot-id$" "$tmp/log" || true)
[ "$commits" = "2" ] || { echo "esperados 2 COMMITs, obtidos $commits"; cat "$tmp/log"; fail=1; }
commits_evo=$(grep -c "^COMMIT tabela_evolucao reqs=assert-current-snapshot-id$" "$tmp/log" || true)
[ "$commits_evo" = "2" ] || { echo "esperados 2 COMMITs (evolucao), obtidos $commits_evo"; cat "$tmp/log"; fail=1; }
commits_sob=$(grep -c "^COMMIT tabela_sobrescrita reqs=assert-current-snapshot-id$" "$tmp/log" || true)
[ "$commits_sob" = "3" ] || { echo "esperados 3 COMMITs (sobrescrita: write+append+overwrite), obtidos $commits_sob"; cat "$tmp/log"; fail=1; }
creates_sob=$(grep -c "^CREATE tabela_sobrescrita$" "$tmp/log" || true)
[ "$creates_sob" = "1" ] || { echo "esperado 1 CREATE (sobrescrita), obtido $creates_sob"; cat "$tmp/log"; fail=1; }

loads_404=$(grep -c "^LOAD tabela_iceberg 404$" "$tmp/log" || true)
[ "$loads_404" = "1" ] || { echo "esperado 1 LOAD 404, obtido $loads_404"; cat "$tmp/log"; fail=1; }
loads_200=$(grep -c "^LOAD tabela_iceberg 200$" "$tmp/log" || true)
[ "$loads_200" -ge "4" ] || { echo "esperados >=4 LOAD 200, obtidos $loads_200"; cat "$tmp/log"; fail=1; }
if grep -q "tabela_fantasma 200" "$tmp/log"; then
  echo "fantasma nao deveria existir no catalogo"; fail=1
fi

# --- validacao pyiceberg (opt-in por disponibilidade) -------------------------------
kill "$mock_pid" 2>/dev/null || true
mock_pid=""

python3 - "$tmp/tabela_iceberg" "$tmp/tabela_evolucao" "$tmp/tabela_sobrescrita" <<'PYEOF'
import glob
import json
import os
import re
import sys


class Erro(Exception):
    pass


try:
    from pyiceberg.table import StaticTable
except ImportError:
    print("pyiceberg ausente; validacao estatica pulada")
    sys.exit(0)

# (linhas lidas no scan, snapshots no metadata final do "servidor")
esperados = {sys.argv[1]: (3, 2), sys.argv[2]: (3, 2), sys.argv[3]: (2, 1)}
for tab in sys.argv[1:]:
    metas = [m for m in glob.glob(os.path.join(tab, "metadata", "v*.metadata.json"))
             if re.search(r"/v\d+\.metadata\.json$", m)]
    if not metas:
        raise Erro("metadata do servidor (v<N>.metadata.json) ausente em " + tab)
    meta = sorted(metas)[-1]
    tabela = StaticTable.from_metadata(meta)
    plan = tabela.scan().to_arrow()
    n, snaps = esperados[tab]
    if plan.num_rows != n:
        raise Erro("esperado %d linhas em %s, lidas %d" % (n, tab, plan.num_rows))
    md = json.load(open(meta))
    if len(md["snapshots"]) != snaps:
        raise Erro("esperados %d snapshots em %s, obtidos %d" % (snaps, tab, len(md["snapshots"])))
    if md["current-snapshot-id"] != md["snapshots"][-1]["snapshot-id"]:
        raise Erro("current-snapshot-id nao aponta o ultimo snapshot em " + tab)
    print("pyiceberg ok: %s -> %d linhas, %d snapshot(s) (%s)" %
          (os.path.basename(tab), plan.num_rows, snaps, meta))
PYEOF

[ "$fail" = 0 ] && echo "iceberg_rest_test ok"
exit "$fail"
