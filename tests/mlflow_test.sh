#!/usr/bin/env sh
# Teste deterministico do registrar_em: mlflow:// contra um tracking server fake.
set -eu

BIN="$1"
PORT_BASE="${TILT_TEST_PORT:-8671}"
command -v curl >/dev/null 2>&1 || { echo "curl ausente; pulando mlflow"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "python3 ausente; pulando mlflow"; exit 0; }

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

python3 - "$PORT_BASE" "$tmp/porta" "$tmp/log" <<'PYEOF' >"$tmp/mock.out" 2>&1 &
import http.server
import json
import socket as _socket
import sys
from urllib.parse import urlparse

_socket.getfqdn = lambda host="": "localhost"
base = int(sys.argv[1])
port_file = sys.argv[2]
log_path = sys.argv[3]
log = open(log_path, "a", encoding="utf-8")

class MlflowMock(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, code, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def record(self, method, payload=None):
        entry = {"method": method, "path": self.path,
                 "authorization": self.headers.get("Authorization", ""),
                 "workspace": self.headers.get("X-MLFLOW-WORKSPACE", ""),
                 "body": payload}
        log.write(json.dumps(entry, sort_keys=True) + "\n")
        log.flush()

    def do_GET(self):
        self.record("GET")
        if urlparse(self.path).path.endswith("/experiments/get-by-name"):
            self.reply(404, {"error_code": "RESOURCE_DOES_NOT_EXIST"})
        else:
            self.reply(404, {"error_code": "NOT_FOUND"})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length).decode("utf-8") if length else "{}"
        payload = json.loads(raw or "{}")
        self.record("POST", payload)
        path = urlparse(self.path).path
        if path.endswith("/experiments/create"):
            self.reply(200, {"experiment_id": "17"})
        elif path.endswith("/runs/create"):
            self.reply(200, {"run": {"info": {"run_id": "run-1"}}})
        elif path.endswith("/runs/log-batch") or path.endswith("/runs/update"):
            self.reply(200, {})
        else:
            self.reply(404, {"error_code": "NOT_FOUND"})

for port in range(base, base + 20):
    try:
        server = http.server.ThreadingHTTPServer(("127.0.0.1", port), MlflowMock)
        break
    except OSError:
        continue
else:
    raise SystemExit("nenhuma porta livre")

with open(port_file, "w", encoding="utf-8") as out:
    out.write(str(port))
server.serve_forever()
PYEOF
mock_pid=$!

for _ in $(seq 1 50); do
  [ -s "$tmp/porta" ] && break
  sleep 0.1
done
[ -s "$tmp/porta" ] || { cat "$tmp/mock.out"; exit 1; }
port=$(cat "$tmp/porta")

cat >"$tmp/input.tilt" <<EOF
experimento demo:
  dados: [
    { x: 0, y: 0 },
    { x: 1, y: 1 },
    { x: 2, y: 2 },
    { x: 3, y: 3 }
  ]
  alvo: "y"
  atributos: [x]
  modelo: regressao_linear
  metricas: [rmse]
  registrar_em: "mlflow://127.0.0.1:$port/exp-demo"
EOF

out=$(cd "$tmp" && MLFLOW_TRACKING_TOKEN=token123 MLFLOW_WORKSPACE=workspace   "$BIN" executar input.tilt)

cat >"$tmp/eval.tilt" <<EOF
avaliacao qualidade:
  dados:
    - { v: 1, esperado: 1 }
    - { v: 2, esperado: 2 }
  executar:
    - retornar caso.v
  registrar_em: "mlflow://127.0.0.1:$port/exp-eval"
EOF

eval_out=$(cd "$tmp" && MLFLOW_TRACKING_TOKEN=token123 MLFLOW_WORKSPACE=workspace \
  "$BIN" executar eval.tilt)
echo "$eval_out" | grep -q "run enviado ao MLflow: run-1" || {
  echo "run de avaliacao MLflow ausente:"; echo "$eval_out"; exit 1; }
[ ! -f "$tmp/avaliacao_qualidade_run.json" ] || {
  echo "JSON local de avaliacao foi criado"; exit 1; }

echo "$out" | grep -q "run enviado ao MLflow: run-1" || {
  echo "run MLflow ausente:"; echo "$out"; exit 1; }
[ ! -f "$tmp/experimento_demo_run.json" ] || {
  echo "JSON local legado foi criado"; exit 1; }

grep -q '"authorization": "Bearer token123"' "$tmp/log" || {
  echo "token nao chegou ao MLflow"; cat "$tmp/log"; exit 1; }
grep -q '"workspace": "workspace"' "$tmp/log" || {
  echo "workspace nao chegou ao MLflow"; cat "$tmp/log"; exit 1; }
grep -q 'experiments/create' "$tmp/log" || {
  echo "experimento nao foi criado"; cat "$tmp/log"; exit 1; }
grep -q '"experiment_id": "17"' "$tmp/log" || {
  echo "run sem experiment_id"; cat "$tmp/log"; exit 1; }
grep -q 'runs/log-batch' "$tmp/log" || {
  echo "metricas nao foram enviadas em batch"; cat "$tmp/log"; exit 1; }
grep -q '"key": "teste_rmse"' "$tmp/log" || {
  echo "rmse nao foi enviada"; cat "$tmp/log"; exit 1; }
grep -q '"key": "tilt.linhas"' "$tmp/log" || {
  echo "parametros nao foram enviados"; cat "$tmp/log"; exit 1; }
grep -q 'runs/update' "$tmp/log" || {
  echo "run nao foi finalizado"; cat "$tmp/log"; exit 1; }

echo "mlflow: ok"
