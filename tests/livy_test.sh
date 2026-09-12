#!/usr/bin/env sh
# Spark via Apache Livy (backend remoto do `fonte tipo: spark` e dos builtins
# spark_sql/spark_executar): sobe um mock em python3 (http.server) falando o
# subset REST do Livy — POST /sessions ({kind, conf} -> {id}; o mock nasce
# "idle" direto), GET /sessions (lista {sessions: [{id, kind, state}]}) para o
# cliente reusar a sessao idle (o tilt NUNCA fecha a sessao: fica no pool do
# Livy), POST /sessions/{id}/statements ({code} -> statement "running") e
# GET /sessions/{id}/statements/{st} ("available" no 1o poll, com
# output.data["text/plain"] simulando toJSON.collectAsList().toString(); SQL
# com "tabela_inexistente" vira statement em "error" com ename/evalue). O
# fixture (fixtures/livy_spark.tilt) cobre: fonte spark com SELECT simples
# conferindo a tabela, fonte spark com agregacao, spark_sql com agregacao +
# conf de sessao, spark_executar com codigo verbatim (saida em texto) e SQL
# invalido capturavel com tentar/capturar. Validacao contra um Livy real
# exigiria subir o Livy na mao num container Spark (imagem apache/livy nao e
# oficial); mock basta, no padrao dos testes de integracao do projeto.
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/livy_spark.tilt}"
PORT_BASE="${TILT_TEST_PORT:-8731}"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste livy"
  exit 0
}
command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste livy"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

# ------------------------------- mock Livy -----------------------------------
python3 - "$PORT_BASE" "$tmp/porta" "$tmp/livy_state.json" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import http.server
import json
import re
import socket as _socket
import sys

# HTTPServer.__init__ chama socket.getfqdn(host): no macOS a resolucao DNS
# reversa pode travar no runner (mesma observacao do mock S3/Elasticsearch).
_socket.getfqdn = lambda host="": "localhost"

port_base = int(sys.argv[1])
port_file = sys.argv[2]
state_file = sys.argv[3]

sessoes = {}      # id -> {"kind": str, "state": str, "conf": dict}
proxima_sessao = 0
statements = {}   # (sessao, id) -> {"state": str, "output": dict}
proximo_statement = 0


def salvar_estado():
    with open(state_file, "w", encoding="utf-8") as f:
        json.dump({"sessoes": sessoes, "criadas": criadas}, f)


criadas = []  # conf de cada POST /sessions (para o teste conferir conf/reuso)


def saida_para(code):
    # spark_sql: o cliente envolve o SQL em spark.sql("""...""")...
    if "spark.sql(" in code:
        partes = code.split('"""')
        sql = partes[1] if len(partes) >= 3 else ""
        if "tabela_inexistente" in sql:
            return {"status": "error", "ename": "AnalysisException",
                    "evalue": "Table or view not found: tabela_inexistente",
                    "traceback": []}
        if "group by" in sql.lower():
            linhas = [{"regiao": "sul", "total": 2},
                      {"regiao": "norte", "total": 1}]
        elif "count(*)" in sql or "count(1)" in sql:
            linhas = [{"total": 3}]
        else:
            linhas = [{"id": 1, "nome": "ana"}, {"id": 2, "nome": "bruno"}]
        return {"status": "ok", "execution_count": 1,
                "data": {"text/plain": json.dumps(linhas)}}
    # spark_executar: codigo verbatim (REPL Scala do mock)
    if "1 + 1" in code:
        texto = "res0: Int = 2"
    elif "spark.range" in code:
        texto = "res1: Long = 10"
    else:
        texto = "res2: Unit = ()"
    return {"status": "ok", "execution_count": 1,
            "data": {"text/plain": texto}}


class MockLivy(http.server.BaseHTTPRequestHandler):
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
        self._enviar({"msg": message}, status)

    def _sessao_resumida(self, sid, s):
        return {"id": sid, "kind": s["kind"], "state": s["state"],
                "appId": "mock-app-%d" % sid}

    def _servir(self, metodo):
        global proxima_sessao, proximo_statement
        path = self.path.split("?", 1)[0].strip("/")
        try:
            if path == "sessions" and metodo == "GET":
                return self._enviar({"from": 0, "total": len(sessoes),
                                     "sessions": [self._sessao_resumida(sid, s)
                                                  for sid, s in sessoes.items()]})
            if path == "sessions" and metodo == "POST":
                corpo = self._corpo()
                kind = corpo.get("kind", "spark")
                if kind not in ("spark", "pyspark"):
                    return self._falhar(400, "kind invalido: %s" % kind)
                sid = proxima_sessao
                proxima_sessao += 1
                sessoes[sid] = {"kind": kind, "state": "idle",
                                "conf": corpo.get("conf") or {}}
                criadas.append({"kind": kind, "conf": corpo.get("conf")})
                salvar_estado()
                return self._enviar(self._sessao_resumida(sid, sessoes[sid]),
                                    status=201)
            m = re.match(r"^sessions/(\d+)$", path)
            if m and metodo == "GET":
                sid = int(m.group(1))
                if sid not in sessoes:
                    return self._falhar(404, "sessao desconhecida: %d" % sid)
                return self._enviar(self._sessao_resumida(sid, sessoes[sid]))
            m = re.match(r"^sessions/(\d+)/statements$", path)
            if m and metodo == "POST":
                sid = int(m.group(1))
                if sid not in sessoes:
                    return self._falhar(404, "sessao desconhecida: %d" % sid)
                if sessoes[sid]["state"] != "idle":
                    return self._falhar(400, "sessao %d nao esta idle" % sid)
                corpo = self._corpo()
                st = proximo_statement
                proximo_statement += 1
                statements[(sid, st)] = {"state": "running",
                                         "output": saida_para(corpo.get("code", ""))}
                return self._enviar({"id": st, "state": "running"}, status=201)
            m = re.match(r"^sessions/(\d+)/statements/(\d+)$", path)
            if m and metodo == "GET":
                chave = (int(m.group(1)), int(m.group(2)))
                if chave[0] not in sessoes or chave not in statements:
                    return self._falhar(404, "statement desconhecido")
                st = statements[chave]
                # 1o poll: running -> available/error (exercita o polling)
                saida = st["output"]
                if saida.get("status") == "error":
                    st["state"] = "error"
                else:
                    st["state"] = "available"
                return self._enviar({"id": chave[1], "state": st["state"],
                                     "output": saida})
            return self._falhar(400, "endpoint nao suportado pelo mock: %s /%s"
                                % (metodo, path))
        except (json.JSONDecodeError, ValueError, TypeError) as e:
            return self._falhar(400, "corpo invalido: %s" % e)

    do_GET = lambda self: self._servir("GET")
    do_POST = lambda self: self._servir("POST")


for porta in range(port_base, port_base + 20):
    try:
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", porta), MockLivy)
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
[ -s "$tmp/porta" ] || { echo "mock livy nao iniciou"; cat "$tmp/mock_out"; exit 1; }
PORTA=$(cat "$tmp/porta")
for _ in $(seq 1 50); do
  curl -s --max-time 2 "http://127.0.0.1:$PORTA/sessions" 2>/dev/null \
    | grep -q '"sessions"' && break
  sleep 0.1
done

run_fixture() {
  sed "s/@PORTA@/$PORTA/g" "$FIXTURE" >"$tmp/livy_spark.tilt"
  "$BIN" executar "$tmp/livy_spark.tilt"
}

check_output() {
  out="$1"
  fail=0
  confere() {
    printf '%s\n' "$out" | grep -qF "$1" || { echo "saida sem '$1'"; fail=1; }
  }
  # fonte spark: SELECT simples -> tabela
  confere "linhas: 2"
  confere "linha: 1 ana"
  confere "linha: 2 bruno"
  # fonte spark com agregacao
  confere "regiao: sul 2"
  confere "regiao: norte 1"
  # spark_sql com agregacao
  confere "total: 3"
  # spark_executar: codigo verbatim, saida em texto
  confere "executar: res0: Int = 2"
  # statement em error: ename/evalue do Livy, capturavel
  confere "erro-sql: livy: AnalysisException: Table or view not found: tabela_inexistente"
  return "$fail"
}

# 1) fixture: sessao nova (kind spark, conf repassada) e todos os statements
out=$(run_fixture)
printf '%s\n' "$out"
check_output "$out" || exit 1

# conf chegou ao POST /sessions e o tilt reusou a sessao idle ao longo do
# fixture (1 unica sessao criada para 4 fontes/builtins)
python3 - "$tmp/livy_state.json" <<'PYEOF' || exit 1
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    estado = json.load(f)
criadas = estado["criadas"]
assert len(criadas) == 1, "esperada 1 sessao criada no 1o run, %d" % len(criadas)
assert criadas[0]["kind"] == "spark", "kind diverge: %r" % criadas[0]
assert criadas[0]["conf"].get("spark.master") == "local[2]", \
    "conf nao repassada: %r" % criadas[0]["conf"]
assert len(estado["sessoes"]) == 1, "sessoes: %r" % estado["sessoes"]
print("mock: 1 sessao criada (kind spark, conf ok), reusada por todos os statements")
PYEOF

# 2) 2a execucao: o mock ainda tem a sessao idle -> nenhuma sessao nova
out2=$(run_fixture)
check_output "$out2" || exit 1
python3 - "$tmp/livy_state.json" <<'PYEOF' || exit 1
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    estado = json.load(f)
assert len(estado["criadas"]) == 1, \
    "2o run deveria reusar a sessao idle (criadas: %d)" % len(estado["criadas"])
print("mock: 2o run reusou a sessao aberta (0 novas sessoes)")
PYEOF

echo "livy_test ok"
exit 0
