#!/usr/bin/env sh
# Integration test do backend vetorial Pinecone do `indice` (REST/JSON puro
# sobre o cliente HTTP generico do runtime, sempre HTTPS): sobe um mock em
# python3 sobre TLS (cert auto-assinado gerado na hora com a CLI openssl —
# o `cryptography` nao e necessario; o curl do runtime confia no cert via
# env CURL_CA_BUNDLE) falando o data plane do Pinecone — POST /vectors/upsert
# ({namespace, vectors: [{id, values, metadata: {texto}}]}; cria o namespace
# implicitamente) e POST /query ({namespace, vector, topK} -> matches[] com
# id/score, similaridade de cosseno). Quando o header Api-Key chega errado
# (ou ausente), o mock responde 401 {"message": ...}; query em namespace
# desconhecido da 404 {"message": ...}. Rodadas: (1) PINECONE_API_KEY correta
# — insercao de 3 docs e busca top_k conferindo ids/ordem/scores (colisao do
# hash mock: "gato e cachorro" pontua 2/sqrt(5) = 0.894427 > "gato domestico"
# 1/sqrt(2) = 0.707107 > "engenharia de dados", fora do top 2) e o 404 de
# namespace desconhecido capturado pelo tilt; (2) chave errada — 401 com a
# mensagem do servidor; (3) PINECONE_API_KEY ausente — erro claro antes de
# tocar a rede (o log do mock nao ganha linhas).
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/pinecone_rag.tilt}"
PORT_BASE="${TILT_TEST_PORT:-8711}"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste pinecone"
  exit 0
}
command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste pinecone"
  exit 0
}
command -v openssl >/dev/null 2>&1 || {
  echo "openssl CLI ausente; pulando o teste pinecone"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# --- cert auto-assinado (SAN IP:127.0.0.1 — o fixture aponta p/ 127.0.0.1) ---
openssl req -x509 -newkey rsa:2048 -keyout "$tmp/key.pem" -out "$tmp/cert.pem" \
  -sha256 -days 1 -nodes -subj "/CN=localhost" \
  -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" >/dev/null 2>&1

# ------------------------------ mock Pinecone --------------------------------
python3 - "$PORT_BASE" "$tmp/porta" "$tmp/log" "$tmp/cert.pem" "$tmp/key.pem" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import http.server
import json
import math
import ssl
import socket as _socket
import sys

# HTTPServer.__init__ chama socket.getfqdn(host): no macOS a resolucao DNS
# reversa pode travar no runner (mesma observacao do mock S3/Elasticsearch).
_socket.getfqdn = lambda host="": "localhost"

port_base = int(sys.argv[1])
port_file = sys.argv[2]
log_path = sys.argv[3]
cert_path = sys.argv[4]
key_path = sys.argv[5]

ESPERADO = "segredo-pinecone"
namespaces = {}  # ns -> {id: {"texto": str, "vector": [float]}}
log = open(log_path, "a", encoding="utf-8")


class MockPinecone(http.server.BaseHTTPRequestHandler):
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
        if self.headers.get("Api-Key") == ESPERADO:
            return True
        self._enviar({"message": "chave de API invalida", "status": 401}, 401)
        return False

    def _servir(self, metodo):
        log.write("%s %s\n" % (metodo, self.path))
        log.flush()
        if not self._auth_ok():
            return
        path = self.path.split("?", 1)[0].strip("/")
        partes = [p for p in path.split("/") if p]
        try:
            if not partes and metodo == "GET":  # readiness
                return self._enviar({"status": "ok"})
            if partes == ["vectors", "upsert"] and metodo == "POST":
                corpo = self._corpo()
                ns = corpo.get("namespace", "")
                vetores = corpo.get("vectors")
                if not isinstance(vetores, list) or not vetores:
                    return self._enviar({"message": "upsert invalido: vectors ausente",
                                         "status": 400}, 400)
                alvo = namespaces.setdefault(ns, {})
                for v in vetores:
                    if not isinstance(v.get("id"), str) or \
                            not isinstance(v.get("values"), list) or not v["values"]:
                        return self._enviar({"message": "vector invalido: id/values ausente",
                                             "status": 400}, 400)
                    alvo[v["id"]] = {
                        "texto": (v.get("metadata") or {}).get("texto", ""),
                        "vector": v["values"],
                    }
                return self._enviar({"upsertedCount": len(vetores)})
            if partes == ["query"] and metodo == "POST":
                corpo = self._corpo()
                ns = corpo.get("namespace", "")
                vetor = corpo.get("vector")
                if ns not in namespaces:
                    return self._enviar({"message": "namespace desconhecido: " + ns,
                                         "status": 404}, 404)
                if not isinstance(vetor, list) or not vetor:
                    return self._enviar({"message": "query invalida: vector ausente",
                                         "status": 400}, 400)
                top_k = int(corpo.get("topK", 10))
                norm = math.sqrt(sum(x * x for x in vetor)) or 1.0
                achados = []
                for oid, obj in namespaces[ns].items():
                    ov = obj["vector"]
                    onorm = math.sqrt(sum(x * x for x in ov)) or 1.0
                    cos = sum(a * b for a, b in zip(vetor, ov)) / (norm * onorm)
                    achados.append((oid, cos))  # similaridade de cosseno
                achados.sort(key=lambda par: (-par[1], par[0]))
                return self._enviar({
                    "results": [],
                    "matches": [
                        {"id": oid, "score": score,
                         "metadata": {"texto": namespaces[ns][oid]["texto"]}}
                        for oid, score in achados[:top_k]
                    ],
                    "namespace": ns,
                })
            return self._enviar({"message": "endpoint nao suportado pelo mock: %s /%s"
                                 % (metodo, "/".join(partes)), "status": 400}, 400)
        except (json.JSONDecodeError, ValueError, TypeError) as e:
            return self._enviar({"message": "corpo invalido: %s" % e, "status": 400}, 400)

    do_GET = lambda self: self._servir("GET")
    do_POST = lambda self: self._servir("POST")


ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert_path, key_path)
for porta in range(port_base, port_base + 20):
    try:
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", porta), MockPinecone)
    except OSError:
        continue
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
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
[ -s "$tmp/porta" ] || { echo "mock pinecone nao iniciou"; cat "$tmp/mock_out"; exit 1; }
PORTA=$(cat "$tmp/porta")
for _ in $(seq 1 50); do
  curl -s --cacert "$tmp/cert.pem" --max-time 2 "https://127.0.0.1:$PORTA/" 2>/dev/null \
    | grep -q '"status"' && break
  sleep 0.1
done

run_fixture() {
  sed "s/@PORTA@/$PORTA/g" "$FIXTURE" >"$tmp/pinecone_rag.tilt"
  # CURL_CA_BUNDLE: o curl do runtime confia no cert auto-assinado do mock.
  env TILT_LLM=mock CURL_CA_BUNDLE="$tmp/cert.pem" ${1:+PINECONE_API_KEY="$1"} \
      "$BIN" executar "$tmp/pinecone_rag.tilt"
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
  # 404 de namespace desconhecido com a mensagem do servidor
  echo "$out" | grep -q "erro-404: pinecone: namespace desconhecido: fantasma" || {
    echo "saida sem o 404 capturado: $out"; fail=1;
  }
  return "$fail"
}

# 1) chave correta: insercao + busca conferindo ids/ordem/scores + 404
out=$(run_fixture segredo-pinecone)
check_output "$out" || exit 1

# 2) chave errada: o 401 do mock chega como excecao com a mensagem do servidor
if out2=$(run_fixture errada 2>&1); then
  echo "rodada com chave errada deveria falhar, mas passou: $out2"
  exit 1
fi
echo "$out2" | grep -q "pinecone: chave de API invalida" || {
  echo "saida da chave errada sem o motivo do 401: $out2"
  exit 1
}

# 3) PINECONE_API_KEY ausente: erro claro antes de tocar a rede (log parado)
antes=$(grep -c . "$tmp/log" 2>/dev/null || echo 0)
if out3=$(env -u PINECONE_API_KEY sh -c '
    sed "s/@PORTA@/$1/g" "$2" >"$3/pinecone_rag.tilt"
    env TILT_LLM=mock CURL_CA_BUNDLE="$3/cert.pem" "$4" executar "$3/pinecone_rag.tilt"
  ' sh "$PORTA" "$FIXTURE" "$tmp" "$BIN" 2>&1); then
  echo "rodada sem chave deveria falhar, mas passou: $out3"
  exit 1
fi
echo "$out3" | grep -q "pinecone: PINECONE_API_KEY nao definida" || {
  echo "saida sem a chave nao pede a env: $out3"
  exit 1
}
depois=$(grep -c . "$tmp/log" 2>/dev/null || echo 0)
[ "$depois" = "$antes" ] || {
  echo "rodada sem chave tocou na rede (log $antes -> $depois linhas)"
  exit 1
}

# conferencia independente via HTTP: os 3 vetores restam no namespace ns1
resp=$(curl -s --cacert "$tmp/cert.pem" -X POST -H "Content-Type: application/json" \
  -H "Api-Key: segredo-pinecone" \
  --data '{"namespace": "ns1", "vector": [1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1], "topK": 5}' \
  "https://127.0.0.1:$PORTA/query")
for id in a1 b1 c1; do
  echo "$resp" | grep -q "\"id\": \"$id\"" || {
    echo "vetor $id ausente no pinecone mock: $resp"; exit 1;
  }
done

echo "pinecone_test ok"
exit 0
