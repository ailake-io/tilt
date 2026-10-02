#!/usr/bin/env sh
# Tool-calling nativo dos agentes (`protocolo: nativo`): um servidor HTTP falso
# em python3 finge os endpoints da Anthropic (POST /v1/messages, blocos
# tool_use/tool_result) e da OpenAI (POST /chat/completions, tool_calls e
# mensagens role=tool). Cada servidor pede uma ferramenta no 1o turno e responde
# no 2o; o teste confere o formato das duas requisicoes (tools/input_schema,
# tool_use e tool_result com o mesmo id) e a resposta do agente. Sem
# curl/python3, pula.
set -eu

BIN="$1"
PORT_BASE="${TILT_TEST_PORT:-8761}"

command -v curl >/dev/null 2>&1 || { echo "curl ausente; pulando agent_native"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "python3 ausente; pulando agent_native"; exit 0; }

tmp=$(mktemp -d)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT

python3 - "$PORT_BASE" "$tmp" <<'PYEOF' >"$tmp/mock_out" 2>&1 &
import http.server, json, socket as _socket, sys

_socket.getfqdn = lambda host="": "localhost"
port, tmp = int(sys.argv[1]), sys.argv[2]
contagem = {"anthropic": 0, "openai": 0}


class Mock(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):  # noqa: N802
        n = int(self.headers.get("Content-Length", 0))
        pedido = json.loads(self.rfile.read(n).decode("utf-8"))
        anthropic = self.path == "/v1/messages"
        chave = "anthropic" if anthropic else "openai"
        contagem[chave] += 1
        vez = contagem[chave]
        with open("%s/%s-%d.json" % (tmp, chave, vez), "w") as f:
            json.dump(pedido, f)
        if anthropic:
            if vez == 1:
                corpo = {"content": [
                    {"type": "text", "text": "vou consultar"},
                    {"type": "tool_use", "id": "toolu_1", "name": "somar_dois",
                     "input": {"a": 2, "b": 3}}],
                    "usage": {"input_tokens": 5, "output_tokens": 2}}
            else:
                corpo = {"content": [{"type": "text", "text": "a soma e 5"}],
                         "usage": {"input_tokens": 6, "output_tokens": 3}}
        else:
            if vez == 1:
                corpo = {"choices": [{"message": {"content": None, "tool_calls": [
                    {"id": "call_1", "type": "function",
                     "function": {"name": "somar_dois",
                                  "arguments": json.dumps({"a": 4, "b": 5})}}]}}],
                    "usage": {"prompt_tokens": 5, "completion_tokens": 2}}
            else:
                corpo = {"choices": [{"message": {"content": "a soma e 9"}}],
                         "usage": {"prompt_tokens": 6, "completion_tokens": 3}}
        raw = json.dumps(corpo).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), Mock)
open(tmp + "/porta", "w").write(str(port))
srv.serve_forever()
PYEOF
mock_pid=$!

for _ in $(seq 1 50); do [ -f "$tmp/porta" ] && break; sleep 0.1; done
[ -f "$tmp/porta" ] || { echo "mock nao subiu:"; cat "$tmp/mock_out"; exit 1; }

cat >"$tmp/prog.tilt" <<EOF2
llm claude:
  provedor: "anthropic"
  modelo: "claude-teste"
  base_url: "http://127.0.0.1:$PORT_BASE"
  chave: env "CHAVE"

llm gpt:
  provedor: "openai"
  modelo: "gpt-teste"
  base_url: "http://127.0.0.1:$PORT_BASE"
  chave: env "CHAVE"

ferramenta somar_dois:
  descricao: "Soma dois inteiros."
  entrada:
    a: inteiro
    b: inteiro
  executar:
    retornar a + b

agente A:
  llm: claude
  papel: "Voce soma."
  ferramentas: [somar_dois]
  protocolo: nativo
  max_passos: 3

agente B:
  llm: gpt
  papel: "Voce soma."
  ferramentas: [somar_dois]
  protocolo: nativo
  max_passos: 3

pipeline p:
  passos:
    - ra = A.responder "quanto e 2+3?"
    - imprimir ra.texto, ra.rastro[0].observacao
    - rb = B.responder "quanto e 4+5?"
    - imprimir rb.texto, rb.rastro[0].observacao
EOF2

out=$(cd "$tmp" && CHAVE=segredo "$BIN" executar prog.tilt 2>&1) || { echo "execucao falhou: $out"; exit 1; }
printf '%s\n' "$out"

fail=0
echo "$out" | grep -q "a soma e 5 5" || { echo "agente Anthropic sem a resposta esperada"; fail=1; }
echo "$out" | grep -q "a soma e 9 9" || { echo "agente OpenAI sem a resposta esperada"; fail=1; }

python3 - "$tmp" <<'PYEOF' || fail=1
import json, sys
t = sys.argv[1]
ok = True
def falha(msg):
    global ok
    print("FALHA:", msg)
    ok = False

# --- Anthropic
a1 = json.load(open(t + "/anthropic-1.json"))
a2 = json.load(open(t + "/anthropic-2.json"))
tools = a1.get("tools", [])
if not tools or tools[0].get("name") != "somar_dois":
    falha("anthropic: tools ausentes")
else:
    sch = tools[0].get("input_schema", {})
    if sch.get("type") != "object" or sch["properties"]["a"]["type"] != "integer":
        falha("anthropic: input_schema errado: %r" % sch)
    if sorted(sch.get("required", [])) != ["a", "b"]:
        falha("anthropic: required errado")
if a1["system"] != "Voce soma.":
    falha("anthropic: system")
m = a2["messages"]
if len(m) != 3:
    falha("anthropic: esperava 3 mensagens no 2o turno, veio %d" % len(m))
else:
    uso = [b for b in m[1]["content"] if b.get("type") == "tool_use"]
    res = m[2]["content"]
    if not uso or uso[0]["id"] != "toolu_1" or uso[0]["input"] != {"a": 2, "b": 3}:
        falha("anthropic: tool_use ausente/errado: %r" % m[1])
    if m[2]["role"] != "user" or res[0].get("type") != "tool_result" \
            or res[0].get("tool_use_id") != "toolu_1" or res[0].get("content") != "5":
        falha("anthropic: tool_result errado: %r" % m[2])

# --- OpenAI
o1 = json.load(open(t + "/openai-1.json"))
o2 = json.load(open(t + "/openai-2.json"))
tools = o1.get("tools", [])
if not tools or tools[0].get("type") != "function" \
        or tools[0]["function"]["name"] != "somar_dois" \
        or tools[0]["function"]["parameters"]["properties"]["b"]["type"] != "integer":
    falha("openai: tools errado: %r" % tools)
m = o2["messages"]
papeis = [x["role"] for x in m]
if papeis != ["system", "user", "assistant", "tool"]:
    falha("openai: papeis %r" % papeis)
else:
    chamadas = m[2].get("tool_calls", [])
    if not chamadas or chamadas[0]["id"] != "call_1" \
            or json.loads(chamadas[0]["function"]["arguments"]) != {"a": 4, "b": 5}:
        falha("openai: tool_calls errado: %r" % m[2])
    if m[3].get("tool_call_id") != "call_1" or m[3].get("content") != "9":
        falha("openai: mensagem tool errada: %r" % m[3])
sys.exit(0 if ok else 1)
PYEOF

[ "$fail" = 0 ] && echo "agent_native_test ok"
exit "$fail"
