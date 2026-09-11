#!/usr/bin/env sh
# Integration test do backend vetorial Weaviate do `indice` (REST/JSON puro
# sobre o cliente HTTP generico do runtime): sobe um mock em python3
# (http.server) falando o subset do Weaviate — GET /v1/schema (lista de
# classes), GET /v1/schema/<classe> (404 {"error": [{"message": ...}]} quando
# a classe nao existe), POST /v1/schema (cria a classe; exige vectorizer
# "none" e a propriedade "texto"), PUT /v1/objects/<classe>/<id> ({texto,
# vector}) e POST /v1/graphql (nearVector {vector} + limit -> _additional
# {id, distance}, ordenado por distancia de cosseno). Quando um header
# Authorization chega, o mock exige "Bearer segredo-weaviate" (401 com a
# mensagem do servidor caso contrario); sem o header, a requisicao e anonima.
# O fixture (fixtures/weaviate_rag.tilt) roda com TILT_LLM=mock: embeddings
# deterministicos (bag-of-tokens). Colisao do hash mock: em "gato e cachorro"
# um token cai na mesma dimensao de "gato", entao b1 pontua 2/sqrt(5) =
# 0.894427 > a1 (1/sqrt(2) = 0.707107) > c1 (sem token em comum). O teste
# confere ids/ordem/scores do top_k e o erro GraphQL de classe desconhecida
# propagado pelo tilt.
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/weaviate_rag.tilt}"
PORT_BASE="${TILT_TEST_PORT:-8701}"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste weaviate"
  exit 0
}
command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste weaviate"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# ------------------------------ mock Weaviate --------------------------------
python3 - "$PORT_BASE" "$tmp/porta" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import http.server
import json
import math
import re
import socket as _socket
import sys

# HTTPServer.__init__ chama socket.getfqdn(host): no macOS a resolucao DNS
# reversa pode travar no runner (mesma observacao do mock S3/Elasticsearch).
_socket.getfqdn = lambda host="": "localhost"

port_base = int(sys.argv[1])
port_file = sys.argv[2]

ESPERADO = "Bearer segredo-weaviate"
classes = {}  # nome -> {id: {"texto": str, "vector": [float]}}


def erro(message, status):
    return {"error": [{"message": message}]}, status


class MockWeaviate(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _enviar(self, obj, status=200):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _corpo(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n)) if n else {}

    def _auth_ok(self):
        auth = self.headers.get("Authorization")
        if auth is None or auth == ESPERADO:
            return True  # servidor de teste tambem aceita requisicao anonima
        self._enviar(*erro("chave de API invalida", 401))
        return False

    def _servir(self, metodo):
        if not self._auth_ok():
            return
        path = self.path.split("?", 1)[0].strip("/")
        partes = [p for p in path.split("/") if p]
        try:
            if partes == ["v1", "schema"] and metodo == "GET":
                return self._enviar({"classes": [
                    {"class": nome, "vectorizer": "none",
                     "properties": [{"name": "texto", "dataType": ["text"]}]}
                    for nome in sorted(classes)]})
            if partes == ["v1", "schema"] and metodo == "POST":
                corpo = self._corpo()
                if not corpo.get("class") or corpo.get("vectorizer") != "none":
                    return self._enviar(*erro(
                        "schema invalido: vectorizer deve ser 'none'", 422))
                props = [p.get("name") for p in corpo.get("properties", [])
                         if isinstance(p, dict)]
                if "texto" not in props:
                    return self._enviar(*erro(
                        "schema invalido: propriedade 'texto' ausente", 422))
                classes.setdefault(corpo["class"], {})
                return self._enviar({"class": corpo["class"]})
            if len(partes) == 3 and partes[:2] == ["v1", "schema"] and metodo == "GET":
                if partes[2] not in classes:
                    return self._enviar(*erro("classe nao encontrada: " + partes[2], 404))
                return self._enviar({"class": partes[2], "vectorizer": "none"})
            if len(partes) == 4 and partes[:2] == ["v1", "objects"] and metodo == "PUT":
                nome, oid = partes[2], partes[3]
                if nome not in classes:
                    return self._enviar(*erro("classe nao encontrada: " + nome, 404))
                corpo = self._corpo()
                if not isinstance(corpo.get("vector"), list) or not corpo["vector"]:
                    return self._enviar(*erro("objeto invalido: vector ausente", 422))
                classes[nome][oid] = {
                    "texto": (corpo.get("properties") or {}).get("texto", ""),
                    "vector": corpo["vector"],
                }
                return self._enviar({"class": nome, "id": oid})
            if partes == ["v1", "graphql"] and metodo == "POST":
                return self._graphql(self._corpo().get("query", ""))
            return self._enviar(*erro(
                "endpoint nao suportado pelo mock: %s /%s"
                % (metodo, "/".join(partes)), 400))
        except (json.JSONDecodeError, ValueError) as e:
            return self._enviar(*erro("corpo invalido: %s" % e, 400))

    def _graphql(self, query):
        m = re.search(
            r"\{\s*Get\s*\{\s*(\w+)\s*\(\s*nearVector\s*:\s*\{\s*vector\s*:\s*"
            r"\[([0-9.,eE+-]*)\]\s*\}\s*,\s*limit\s*:\s*(\d+)", query)
        if not m:
            return self._enviar(
                {"errors": [{"message": "query GraphQL nao suportada pelo mock"}]})
        nome = m.group(1)
        vetor = [float(x) for x in m.group(2).split(",") if x]
        limite = int(m.group(3))
        if nome not in classes:
            return self._enviar({"errors": [{"message": "classe desconhecida: " + nome}]})
        norm = math.sqrt(sum(x * x for x in vetor)) or 1.0
        achados = []
        for oid, obj in classes[nome].items():
            ov = obj["vector"]
            onorm = math.sqrt(sum(x * x for x in ov)) or 1.0
            cos = sum(a * b for a, b in zip(vetor, ov)) / (norm * onorm)
            achados.append((oid, 1.0 - cos))  # distancia de cosseno
        achados.sort(key=lambda par: (par[1], par[0]))
        return self._enviar({"data": {"Get": {nome: [
            {"_additional": {"id": oid, "distance": dist}}
            for oid, dist in achados[:limite]]}}})

    do_GET = lambda self: self._servir("GET")
    do_POST = lambda self: self._servir("POST")
    do_PUT = lambda self: self._servir("PUT")


for porta in range(port_base, port_base + 20):
    try:
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", porta), MockWeaviate)
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
[ -s "$tmp/porta" ] || { echo "mock weaviate nao iniciou"; cat "$tmp/mock_out"; exit 1; }
PORTA=$(cat "$tmp/porta")
for _ in $(seq 1 50); do
  curl -s --max-time 2 "http://127.0.0.1:$PORTA/v1/schema" 2>/dev/null | grep -q '"classes"' && break
  sleep 0.1
done

run_fixture() {
  sed "s/@PORTA@/$PORTA/g" "$FIXTURE" >"$tmp/weaviate_rag.tilt"
  env TILT_LLM=mock ${1:+WEAVIATE_API_KEY="$1"} "$BIN" executar "$tmp/weaviate_rag.tilt"
}

check_output() {
  out="$1"
  fail=0
  # insercao dos 3 documentos
  echo "$out" | grep -q "inseridos: 3" || {
    echo "saida sem 'inseridos: 3': $out"; fail=1;
  }
  # top_k: b1 (colisao no mock -> 2/sqrt(5)) antes de a1 (1/sqrt(2)); c1 fora
  linhas=$(echo "$out" | grep "^achado:" || true)
  [ "$(printf '%s\n' "$linhas" | grep -c .)" = "2" ] || {
    echo "esperados 2 achados: $out"; fail=1;
  }
  printf '%s\n' "$linhas" | sed -n 1p | grep -q "achado: b1 0.894" || {
    echo "primeiro achado nao e 'b1 0.894': $out"; fail=1;
  }
  printf '%s\n' "$linhas" | sed -n 2p | grep -q "achado: a1 0.707" || {
    echo "segundo achado nao e 'a1 0.707': $out"; fail=1;
  }
  echo "$linhas" | grep -q "c1" && {
    echo "c1 nao deveria estar no top 2: $out"; fail=1;
  }
  echo "$out" | grep -q "top1: b1" || {
    echo "saida sem 'top1: b1': $out"; fail=1;
  }
  # erro GraphQL (status 200, errors[0].message) propagado pelo tilt
  echo "$out" | grep -q "erro-graphql: weaviate: classe desconhecida: Espectro" || {
    echo "saida sem o erro GraphQL capturado: $out"; fail=1;
  }
  return "$fail"
}

# 1) sem autenticacao: sem header Authorization -> anonimo
out=$(run_fixture)
check_output "$out" || exit 1

# 2) env WEAVIATE_API_KEY correta: Bearer passa no mock (classe ja existe:
#    exercita o caminho GET /v1/schema/<classe> 200 do ensure)
out2=$(run_fixture segredo-weaviate)
check_output "$out2" || exit 1

# 3) chave errada: o 401 do mock chega como excecao com a mensagem do servidor
if out3=$(run_fixture errada 2>&1); then
  echo "rodada com chave errada deveria falhar, mas passou: $out3"
  exit 1
fi
echo "$out3" | grep -q "weaviate: chave de API invalida" || {
  echo "saida da chave errada sem o motivo do 401: $out3"
  exit 1
}

# conferencia independente via HTTP: os 3 objetos restam na classe Documentos
resp=$(curl -s -X POST -H "Content-Type: application/json" \
  --data '{"query": "{ Get { Documentos(nearVector: {vector: [1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]}, limit: 5) { _additional { id } } } }"}' \
  "http://127.0.0.1:$PORTA/v1/graphql")
for id in a1 b1 c1; do
  echo "$resp" | grep -q "\"id\": \"$id\"" || {
    echo "objeto $id ausente no weaviate mock: $resp"; exit 1;
  }
done

echo "weaviate_test ok"
exit 0
