#!/usr/bin/env sh
# Robustez do cliente LLM (retry/backoff, fallback, teto de tokens, timeout):
# sobe um mock HTTP em python3 que finge dois endpoints —
# POST /v1/messages (shape Anthropic) e POST /chat/completions (shape
# OpenAI) — com o comportamento roteado pelo "model" do corpo:
#   instavel -> 429, 429, 200 (usage 7+3)
#   quebrado -> sempre 500
#   ok       -> sempre 200 (usage 10+5)
#   lento    -> dorme 5s e devolve 200
# O fixture (fixtures/llm_robusto.tilt) aponta cada 'llm' para o mock via
# base_url (substituicao @PORTA@) e cobre: retry com tokens no mapa de
# resposta, fallback via reserva:, teto_tokens barrando a 2a chamada e
# tempo_limite abortando chamada lenta. Sem curl/python3, pula.
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/llm_robusto.tilt}"
PORT_BASE="${TILT_TEST_PORT:-8741}"

command -v curl >/dev/null 2>&1 || {
  echo "curl ausente; pulando o teste llm_robusto"
  exit 0
}
command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste llm_robusto"
  exit 0
}

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

python3 - "$PORT_BASE" "$tmp/porta" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import http.server
import json
import socket as _socket
import sys
import time

_socket.getfqdn = lambda host="": "localhost"

port_base = int(sys.argv[1])
port_file = sys.argv[2]
chamadas = {}


class MockLLM(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):  # noqa: N802
        n = int(self.headers.get("Content-Length", 0))
        corpo = self.rfile.read(n).decode("utf-8", "replace")
        try:
            modelo = json.loads(corpo).get("model", "")
        except Exception:
            modelo = ""
        chamadas[modelo] = chamadas.get(modelo, 0) + 1
        vez = chamadas[modelo]

        if modelo == "lento":
            time.sleep(5)
            self._ok("devagar mas cheguei", {"prompt_tokens": 1, "completion_tokens": 1},
                     aberto=True)
        elif modelo == "quebrado":
            self._erro(500, {"error": "quebrou"})
        elif modelo == "instavel" and vez <= 2:
            self._erro(429, {"error": {"message": "limite"}})
        elif self.path == "/v1/messages":
            self._ok("resposta instavel",
                     {"input_tokens": 7, "output_tokens": 3}, aberto=False)
        else:
            self._ok("resposta " + modelo,
                     {"prompt_tokens": 10, "completion_tokens": 5}, aberto=True)

    def _ok(self, texto, uso, aberto):
        if aberto:
            corpo = {"choices": [{"message": {"content": texto}}], "usage": uso}
        else:
            corpo = {"content": [{"text": texto}], "usage": uso}
        raw = json.dumps(corpo).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def _erro(self, codigo, corpo):
        raw = json.dumps(corpo).encode("utf-8")
        self.send_response(codigo)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


for porta in range(port_base, port_base + 20):
    try:
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", porta), MockLLM)
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
[ -s "$tmp/porta" ] || { echo "mock llm nao iniciou"; cat "$tmp/mock_out"; exit 1; }
PORTA=$(cat "$tmp/porta")
for _ in $(seq 1 50); do
  curl -s --max-time 2 -X POST "http://127.0.0.1:$PORTA/chat/completions" \
    -d '{"model":"ping"}' 2>/dev/null | grep -q "resposta" && break
  sleep 0.1
done

sed "s/@PORTA@/$PORTA/g" "$FIXTURE" >"$tmp/llm_robusto.tilt"
out=$("$BIN" executar "$tmp/llm_robusto.tilt")
printf '%s\n' "$out"

fail=0
confere() {
  printf '%s\n' "$out" | grep -qF "$1" || { echo "saida sem '$1'"; fail=1; }
}
# retry: 429,429,200 (anthropic) com tokens no mapa
confere "texto: resposta instavel"
confere "tokens: 7 3"
confere "modelo: instavel"
# fallback: reserva 'ok' responde (shape openai)
confere "texto: resposta ok"
confere "modelo: ok"
# teto: 1a usa 15, 2a barra
confere "uso1: 15"
confere "teto:"
echo "$out" | grep "teto:" | grep -q "teto_tokens" || {
  echo "teto sem mencionar teto_tokens"; fail=1; }
# timeout: 2 tentativas de 2s
confere "timeout:"
echo "$out" | grep "timeout:" | grep -q "2 tentativa" || {
  echo "timeout sem mencionar tentativas"; fail=1; }

[ "$fail" = 0 ] && echo "llm_robusto_test ok"
exit "$fail"
