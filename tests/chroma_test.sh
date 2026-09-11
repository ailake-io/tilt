#!/usr/bin/env sh
# Integration test do backend vetorial Chroma do `indice` (REST/JSON puro
# sobre o cliente HTTP generico do runtime, HTTP puro, sem auth): sobe um
# mock em python3 (http.server) falando o subset do Chroma — POST
# /api/v1/collections ({name, get_or_create} -> {id}; valida o nome contra
# ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ e devolve 400 {"error": ...} se invalido),
# POST /api/v1/collections/{id}/add ({ids, embeddings, metadatas, documents})
# e POST /api/v1/collections/{id}/query ({query_embeddings, n_results,
# include} -> {ids: [[...]], distances: [[...]]}, cosseno real com
# distance = 1 - cosseno). O fixture (fixtures/chroma_rag.tilt) roda com
# TILT_LLM=mock: embeddings deterministicos (bag-of-tokens). Colisao do hash
# mock: em "gato e cachorro" um token cai na mesma dimensao de "gato", entao
# b1 pontua 2/sqrt(5) = 0.894427 > a1 (1/sqrt(2) = 0.707107) > c1 (sem token
# em comum, fora do top 2). A rodada 2 reexecuta o fixture com a colecao ja
# existente (get_or_create devolve o id gravado) e o 400 de nome invalido e
# capturado pelo tilt. Sem mock de embeddings externo.
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/chroma_rag.tilt}"
PORT_BASE="${TILT_TEST_PORT:-8721}"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste chroma"
  exit 0
}
command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste chroma"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# ------------------------------- mock Chroma ---------------------------------
python3 - "$PORT_BASE" "$tmp/porta" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import http.server
import json
import math
import re
import socket as _socket
import sys
import uuid

# HTTPServer.__init__ chama socket.getfqdn(host): no macOS a resolucao DNS
# reversa pode travar no runner (mesma observacao do mock S3/Elasticsearch).
_socket.getfqdn = lambda host="": "localhost"

port_base = int(sys.argv[1])
port_file = sys.argv[2]

NOME_OK = re.compile(r"^[a-zA-Z0-9][a-zA-Z0-9._-]*$")
colecoes = {}  # id -> {"nome": str, "docs": {id: {"texto": str, "vetor": [float]}}}
por_nome = {}  # nome -> id


class MockChroma(http.server.BaseHTTPRequestHandler):
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

    def _falhar(self, status, message):
        self._enviar({"error": message}, status)

    def _servir(self, metodo):
        path = self.path.split("?", 1)[0].strip("/")
        partes = [p for p in path.split("/") if p]
        try:
            if partes == ["api", "v1", "collections"] and metodo == "POST":
                corpo = self._corpo()
                nome = corpo.get("name", "")
                if not isinstance(nome, str) or not NOME_OK.match(nome):
                    return self._falhar(400, "nome de colecao invalido: %s" % nome)
                if nome not in por_nome:
                    cid = str(uuid.uuid4())
                    por_nome[nome] = cid
                    colecoes[cid] = {"nome": nome, "docs": {}}
                return self._enviar({"id": por_nome[nome], "name": nome,
                                     "metadata": None})
            m = re.match(r"^api/v1/collections/([^/]+)/(add|query)$", path)
            if m and metodo == "POST":
                cid, acao = m.group(1), m.group(2)
                if cid not in colecoes:
                    return self._falhar(404, "colecao desconhecida: " + cid)
                col = colecoes[cid]
                corpo = self._corpo()
                if acao == "add":
                    ids = corpo.get("ids")
                    emb = corpo.get("embeddings")
                    metas = corpo.get("metadatas") or []
                    docs = corpo.get("documents") or []
                    if not isinstance(ids, list) or not isinstance(emb, list) or \
                            len(ids) != len(emb):
                        return self._falhar(400, "add invalido: ids/embeddings "
                                                 "ausentes ou de tamanhos diferentes")
                    for i, oid in enumerate(ids):
                        if not isinstance(emb[i], list) or not emb[i]:
                            return self._falhar(400, "embedding invalido para o id "
                                                     "%s" % oid)
                        col["docs"][oid] = {
                            "texto": (metas[i].get("texto") if i < len(metas)
                                      and isinstance(metas[i], dict) else
                                      (docs[i] if i < len(docs) else "")),
                            "vetor": emb[i],
                        }
                    return self._enviar(True)
                # query: distance = 1 - cosseno (hnsw:space cosine)
                consultas = corpo.get("query_embeddings")
                if not isinstance(consultas, list) or not consultas:
                    return self._falhar(400, "query invalida: query_embeddings ausente")
                n_results = int(corpo.get("n_results", 10))
                ids_l, dists_l = [], []
                for vetor in consultas:
                    if not isinstance(vetor, list) or not vetor:
                        return self._falhar(400, "query invalida: embedding vazio")
                    norm = math.sqrt(sum(x * x for x in vetor)) or 1.0
                    achados = []
                    for oid, obj in col["docs"].items():
                        ov = obj["vetor"]
                        onorm = math.sqrt(sum(x * x for x in ov)) or 1.0
                        cos = sum(a * b for a, b in zip(vetor, ov)) / (norm * onorm)
                        achados.append((oid, 1.0 - cos))
                    achados.sort(key=lambda par: (par[1], par[0]))
                    ids_l.append([oid for oid, _ in achados[:n_results]])
                    dists_l.append([d for _, d in achados[:n_results]])
                return self._enviar({"ids": ids_l, "distances": dists_l,
                                     "metadatas": [[] for _ in consultas],
                                     "documents": [[] for _ in consultas],
                                     "embeddings": None})
            return self._falhar(400, "endpoint nao suportado pelo mock: %s /%s"
                                % (metodo, "/".join(partes)))
        except (json.JSONDecodeError, ValueError, TypeError) as e:
            return self._falhar(400, "corpo invalido: %s" % e)

    do_GET = lambda self: self._servir("GET")
    do_POST = lambda self: self._servir("POST")


for porta in range(port_base, port_base + 20):
    try:
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", porta), MockChroma)
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
[ -s "$tmp/porta" ] || { echo "mock chroma nao iniciou"; cat "$tmp/mock_out"; exit 1; }
PORTA=$(cat "$tmp/porta")
for _ in $(seq 1 50); do
  curl -s --max-time 2 -X POST -H "Content-Type: application/json" \
    --data '{"name": "readiness", "get_or_create": true}' \
    "http://127.0.0.1:$PORTA/api/v1/collections" 2>/dev/null | grep -q '"id"' && break
  sleep 0.1
done

run_fixture() {
  sed "s/@PORTA@/$PORTA/g" "$FIXTURE" >"$tmp/chroma_rag.tilt"
  env TILT_LLM=mock "$BIN" executar "$tmp/chroma_rag.tilt"
}

check_output() {
  out="$1"
  fail=0
  # insercao dos 3 documentos (colecao criada automaticamente no 1o acesso)
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
  # 400 de nome de colecao invalido com a mensagem do servidor
  echo "$out" | grep -q "erro-nome: chroma: nome de colecao invalido: Nome Invalido" || {
    echo "saida sem o 400 capturado: $out"; fail=1;
  }
  return "$fail"
}

# 1) colecao nova: get_or_create cria, add indexa e query confere ids/scores
out=$(run_fixture)
check_output "$out" || exit 1

# 2) colecao ja existe: get_or_create devolve o id gravado; ids sobrescritos
out2=$(run_fixture)
check_output "$out2" || exit 1

# conferencia independente via HTTP: os 3 documentos restam na colecao docs
cid=$(curl -s -X POST -H "Content-Type: application/json" \
  --data '{"name": "docs", "get_or_create": true}' \
  "http://127.0.0.1:$PORTA/api/v1/collections" | sed 's/.*"id": *"\([^"]*\)".*/\1/')
resp=$(curl -s -X POST -H "Content-Type: application/json" \
  --data '{"query_embeddings": [[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]], "n_results": 5}' \
  "http://127.0.0.1:$PORTA/api/v1/collections/$cid/query")
for id in a1 b1 c1; do
  echo "$resp" | grep -q "\"$id\"" || {
    echo "documento $id ausente no chroma mock: $resp"; exit 1;
  }
done

echo "chroma_test ok"
exit 0
