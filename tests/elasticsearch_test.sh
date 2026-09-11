#!/usr/bin/env sh
# Integration test do conector Elasticsearch/OpenSearch (REST/JSON puro sobre
# o cliente HTTP generico do runtime): sobe um mock em python3 (http.server)
# falando o subset REST do ES 8 — PUT /<indice> (cria/recria o indice),
# PUT|POST /<indice>/_doc[/id] (indexa; POST gera _id sequencial),
# GET|POST /<indice>/_search (match_all, match/term simples e agregacao
# terms), POST /<indice>/_delete_by_query, GET /_cat/indices (responde texto
# puro, exercita o fallback de json invalido do es_executar), cluster info em
# GET / e erro padrao do ES 8 {"error": {"type":..., "reason":...},
# "status": N} para indice inexistente (404) e credencial errada (401).
# Quando um header Authorization chega, o mock valida o par elastic:senha —
# assim as rodadas com userinfo na URL (elastic:senha@) e com env
# ELASTIC_USER/ELASTIC_PASSWORD so passam se o Basic Auth estiver correto, e
# a rodada com senha errada tem o 401 (error.reason) propagado pelo tilt.
# O proprio tilt precisa do binario `curl` (o cliente HTTP e subprocesso curl).
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/elasticsearch_roundtrip.tilt}"
PORT_BASE="${TILT_TEST_PORT:-8681}"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste elasticsearch"
  exit 0
}
command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste elasticsearch"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# ------------------------------ mock Elasticsearch ----------------------------
python3 - "$PORT_BASE" "$tmp/porta" "$tmp/log" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import base64
import http.server
import json
import socket as _socket
import sys

# HTTPServer.__init__ chama socket.getfqdn(host): no macOS a resolucao DNS
# reversa pode travar no runner (mesma observacao do mock S3).
_socket.getfqdn = lambda host="": "localhost"

port_base = int(sys.argv[1])
port_file = sys.argv[2]
log_path = sys.argv[3]

ESPERADO = "Basic " + base64.b64encode(b"elastic:senha").decode()
indices = {}  # nome -> {"docs": {id: dict}, "seq": int}
log = open(log_path, "a", encoding="utf-8")


def selecionar(docs, query):
    # Subconjunto proposital: match_all, match (case-insensitive) e term.
    if not isinstance(query, dict):
        return list(docs.items())
    if "match_all" in query:
        return list(docs.items())
    for operador in ("match", "term"):
        if operador in query and isinstance(query[operador], dict):
            campo, valor = next(iter(query[operador].items()))
            valor = str(valor).lower()
            return [(i, d) for i, d in docs.items()
                    if str(d.get(campo, "")).lower() == valor]
    return list(docs.items())


class MockES(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _falhar(self, status, tipo, reason):
        corpo = {"error": {"type": tipo, "reason": reason,
                           "root_cause": [{"type": tipo, "reason": reason}]},
                 "status": status}
        self._enviar(corpo, status)

    def _enviar(self, obj, status=200, content_type="application/json"):
        data = (json.dumps(obj) if content_type == "application/json"
                else str(obj)).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _corpo(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n)) if n else {}

    def _auth_ok(self):
        auth = self.headers.get("Authorization")
        if auth is None:
            return True  # servidor de teste tambem aceita requisicao anonima
        if auth == ESPERADO:
            return True
        self._falhar(401, "security_exception",
                     "nome de usuario ou senha invalidos")
        return False

    def _partes(self):
        path = self.path.split("?", 1)[0].strip("/")
        return [p for p in path.split("/") if p]

    def _servir(self, metodo):
        if not self._auth_ok():
            return
        log.write("%s %s\n" % (metodo, self.path))
        log.flush()
        partes = self._partes()
        try:
            if not partes:  # GET / -> info do cluster (readiness)
                return self._enviar({"name": "mock-es",
                                     "cluster_name": "tilt-test",
                                     "version": {"number": "8.11.0",
                                                 "tagline": "You Know, for Search"}})
            if partes == ["_cat", "indices"] and metodo == "GET":
                linhas = "".join("%s\n" % nome for nome in sorted(indices))
                return self._enviar(linhas, content_type="text/plain")
            if len(partes) == 1 and metodo == "PUT":  # cria/recria indice
                indices[partes[0]] = {"docs": {}, "seq": 0}
                return self._enviar({"acknowledged": True, "index": partes[0]})
            if len(partes) == 1 and metodo == "DELETE":
                if partes[0] not in indices:
                    return self._falhar(404, "index_not_found_exception",
                                        "no such index [%s]" % partes[0])
                del indices[partes[0]]
                return self._enviar({"acknowledged": True})
            indice = partes[0] if partes else ""
            if indice not in indices:
                return self._falhar(404, "index_not_found_exception",
                                    "no such index [%s]" % indice)
            idx = indices[indice]
            if len(partes) >= 2 and partes[1] == "_doc" and metodo in ("PUT", "POST"):
                doc_id = partes[2] if len(partes) > 2 else str(idx["seq"] + 1)
                novo = doc_id not in idx["docs"]
                idx["docs"][doc_id] = self._corpo()
                idx["seq"] += 1
                return self._enviar({"_index": indice, "_id": doc_id,
                                     "_version": 1 if novo else 2,
                                     "result": "created" if novo else "updated"})
            if len(partes) == 2 and partes[1] == "_search":
                body = self._corpo() if metodo == "POST" else {}
                achados = selecionar(idx["docs"], body.get("query"))
                hits = [{"_index": indice, "_id": i, "_score": 1.0,
                         "_source": d} for i, d in achados]
                resp = {"took": 1, "timed_out": False,
                        "_shards": {"total": 1, "successful": 1, "skipped": 0,
                                    "failed": 0},
                        "hits": {"total": {"value": len(hits), "relation": "eq"},
                                 "max_score": 1.0,
                                 "hits": [] if body.get("size") == 0 else hits}}
                aggs = body.get("aggs") or body.get("aggregations")
                if isinstance(aggs, dict):
                    resp["aggregations"] = {}
                    for nome, spec in aggs.items():
                        terms = (spec or {}).get("terms") if isinstance(spec, dict) else None
                        campo = (terms or {}).get("field")
                        buckets = {}
                        for _, d in achados:
                            chave = d.get(campo, "")
                            buckets[chave] = buckets.get(chave, 0) + 1
                        resp["aggregations"][nome] = {
                            "buckets": [{"key": k, "doc_count": c}
                                        for k, c in sorted(buckets.items())]}
                return self._enviar(resp)
            if len(partes) == 2 and partes[1] == "_delete_by_query" and metodo == "POST":
                alvos = [i for i, _ in selecionar(idx["docs"],
                                                  self._corpo().get("query"))]
                for i in alvos:
                    del idx["docs"][i]
                return self._enviar({"took": 1, "timed_out": False,
                                     "total": len(alvos), "deleted": len(alvos),
                                     "batches": 1, "version_conflicts": 0,
                                     "noops": 0, "retries": {"bulk": 0, "search": 0},
                                     "throttled_millis": 0,
                                     "requests_per_second": -1.0,
                                     "throttled_until_millis": 0,
                                     "failures": []})
            return self._falhar(400, "illegal_argument_exception",
                                "endpoint nao suportado pelo mock: %s /%s"
                                % (metodo, "/".join(partes)))
        except (json.JSONDecodeError, ValueError) as e:
            return self._falhar(400, "parse_exception",
                                "corpo invalido: %s" % e)

    do_GET = lambda self: self._servir("GET")
    do_POST = lambda self: self._servir("POST")
    do_PUT = lambda self: self._servir("PUT")
    do_DELETE = lambda self: self._servir("DELETE")


for porta in range(port_base, port_base + 20):
    try:
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", porta), MockES)
    except OSError:
        continue
    with open(port_file, "w", encoding="utf-8") as f:
        f.write(str(porta))
    srv.serve_forever()
    break
else:
    sys.exit("nenhuma porta livre a partir de %d" % port_base)
PYEOF
mock_pid=$!

for _ in $(seq 1 50); do
  [ -s "$tmp/porta" ] && break
  sleep 0.1
done
[ -s "$tmp/porta" ] || { echo "mock elasticsearch nao iniciou"; cat "$tmp/mock_out"; exit 1; }
PORTA=$(cat "$tmp/porta")
for _ in $(seq 1 50); do
  curl -s --max-time 2 "http://127.0.0.1:$PORTA/" 2>/dev/null | grep -q tagline && break
  sleep 0.1
done

run_fixture() {
  env ELASTIC_URL="$1" ELASTIC_BASE="$2" ${3:+ELASTIC_USER="$3"} \
      ${4:+ELASTIC_PASSWORD="$4"} "$BIN" executar "$FIXTURE"
}

check_output() {
  out="$1"
  fail=0
  # (a) escape hatch: criacao do indice e indexacao com _id proprio
  echo "$out" | grep -q "criado: verdadeiro" || {
    echo "saida sem 'criado: verdadeiro': $out"; fail=1;
  }
  echo "$out" | grep -q "doc1: created 1" || {
    echo "saida sem 'doc1: created 1': $out"; fail=1;
  }
  # (b) fonte/ler com match_all: total + hits achatados (_id + _source)
  echo "$out" | grep -q "total: 2" || {
    echo "saida sem 'total: 2': $out"; fail=1;
  }
  echo "$out" | grep -q "hit: 1 ana 10" || {
    echo "saida sem 'hit: 1 ana 10': $out"; fail=1;
  }
  echo "$out" | grep -q "hit: 2 bruno 5" || {
    echo "saida sem 'hit: 2 bruno 5': $out"; fail=1;
  }
  # (c) es_buscar com agregacao terms (DSL em mapa)
  echo "$out" | grep -q "aggs-total: 2" || {
    echo "saida sem 'aggs-total: 2': $out"; fail=1;
  }
  echo "$out" | grep -q "bucket: ana 1" || {
    echo "saida sem 'bucket: ana 1': $out"; fail=1;
  }
  echo "$out" | grep -q "bucket: bruno 1" || {
    echo "saida sem 'bucket: bruno 1': $out"; fail=1;
  }
  # (d) es_buscar com DSL em texto e filtro match
  echo "$out" | grep -q "match-total: 1" || {
    echo "saida sem 'match-total: 1': $out"; fail=1;
  }
  # (e) erro do servidor (404) com error.reason
  echo "$out" | grep -q "erro-404: elasticsearch: no such index \[inexistente\]" || {
    echo "saida sem erro 404 capturado: $out"; fail=1;
  }
  # (f) delete by query + endpoint de texto puro (_cat)
  echo "$out" | grep -q "deletados: 1" || {
    echo "saida sem 'deletados: 1': $out"; fail=1;
  }
  echo "$out" | grep -q "cat: artigos" || {
    echo "saida sem 'cat: artigos': $out"; fail=1;
  }
  return "$fail"
}

BASE="elasticsearch://127.0.0.1:$PORTA"

# 1) sem autenticacao: URL sem userinfo, sem env -> sem header Authorization
out=$(run_fixture "$BASE/artigos" "$BASE")
check_output "$out" || exit 1

# 2) userinfo na URL (elastic:senha@): o mock so aceita o Basic Auth correto
out2=$(run_fixture "elasticsearch://elastic:senha@127.0.0.1:$PORTA/artigos" \
                   "elasticsearch://elastic:senha@127.0.0.1:$PORTA")
check_output "$out2" || exit 1

# 3) fallback de env: URL sem userinfo + ELASTIC_USER/ELASTIC_PASSWORD
out3=$(run_fixture "$BASE/artigos" "$BASE" elastic senha)
check_output "$out3" || exit 1

# 4) senha errada: o 401 do mock chega como excecao com o error.reason
if out4=$(run_fixture "elasticsearch://elastic:errada@127.0.0.1:$PORTA/artigos" \
                      "elasticsearch://elastic:errada@127.0.0.1:$PORTA" 2>&1); then
  echo "rodada com senha errada deveria falhar, mas passou: $out4"
  exit 1
fi
echo "$out4" | grep -q "elasticsearch: nome de usuario ou senha invalidos" || {
  echo "saida da senha errada sem o motivo do 401: $out4"
  exit 1
}

# conferencia independente via HTTP: o indice restou com 1 documento (ana)
cnt=$(curl -s -X POST -H "Content-Type: application/json" \
      --data '{"query": {"match_all": {}}}' \
      "http://127.0.0.1:$PORTA/artigos/_search")
echo "$cnt" | grep -q '"value": 1' || {
  echo "esperado total 1 em artigos apos os testes, obtido: $cnt"; exit 1;
}

echo "elasticsearch_test ok"
exit 0
