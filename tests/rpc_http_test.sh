#!/usr/bin/env sh
# `tilt rpc --porta`: as chamadas do JSON-lines por HTTP (GET /funcoes, /saude,
# POST /chamar, /lote, /pipeline) com codigos 200/400/404. Pula sem curl/python3.
set -eu

BIN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
EX="$(cd "$2" && pwd)"
for c in curl python3; do
  command -v "$c" >/dev/null 2>&1 || { echo "pulado: sem $c"; exit 0; }
done
tmp=$(mktemp -d)
pid=""
trap '[ -z "$pid" ] || kill "$pid" 2>/dev/null || true; rm -rf "$tmp"' EXIT
fail=0
porta=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
"$BIN" rpc "$EX/vendas.tilt" --porta "$porta" >"$tmp/log" 2>&1 &
pid=$!
n=0
until curl -s -o /dev/null "http://127.0.0.1:$porta/saude"; do
  n=$((n + 1)); [ "$n" -lt 30 ] || { echo "FALHA: servidor nao subiu: $(cat "$tmp/log")"; exit 1; }
  sleep 1
done

req() { # req <metodo> <caminho> [corpo] -> "<codigo> <corpo>"
  curl -s -w ' %{http_code}' -X "$1" "http://127.0.0.1:$porta$2" ${3:+-d "$3"}
}
ck() { # ck <esperado> <obtido>
  [ "$1" = "$2" ] || { echo "FALHA: esperado [$1] obtido [$2]"; fail=1; }
}
ck '{"ok":true} 200' "$(req GET /saude)"
req GET /funcoes | grep -qF '"nome":"classificar"' || { echo "FALHA: /funcoes"; fail=1; }
ck '{"ok":true,"resultado":"alto"} 200' "$(req POST /chamar/classificar '[120]')"
ck '{"ok":true,"resultado":"medio"} 200' "$(req POST /chamar/classificar '{"args":[60]}')"
ck '{"ok":true,"resultado":[{"valor":30},{"valor":120}]} 200' \
  "$(req POST /chamar/so_altos '{"args":[[{"valor":30},{"valor":120}]],"nomeados":{"minimo":20}}')"
ck '{"ok":true,"resultado":["baixo","alto"]} 200' "$(req POST /lote/classificar '[[1],[200]]')"
req POST /pipeline/demo | grep -qF 'pipeline demo rodou' || { echo "FALHA: /pipeline"; fail=1; }
ck "{\"ok\":false,\"erro\":\"funcao 'nada' nao existe\"} 404" "$(req POST /chamar/nada '[]')"
req POST /chamar/falhar | grep -q ' 400$' || { echo "FALHA: erro de execucao devia ser 400"; fail=1; }
req POST /chamar/classificar 'isto nao e json' | grep -q ' 400$' || { echo "FALHA: corpo invalido"; fail=1; }
req GET /nada | grep -q ' 404$' || { echo "FALHA: rota desconhecida"; fail=1; }

[ "$fail" = 0 ] && echo "rpc http ok"
exit "$fail"
