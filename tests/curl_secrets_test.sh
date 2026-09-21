#!/usr/bin/env sh
# Segredos fora do argv do curl: chave de API (LLM) e userinfo da URL (HTTP
# generico) vao para um arquivo `-K` 0600, nunca para a linha de comando, que
# qualquer usuario da maquina le em ps//proc. Um `curl` falso no PATH registra
# o argv e copia o arquivo -K; o teste confere que o segredo esta so no -K e
# que o arquivo temporario some depois da chamada.
set -eu

BIN="$1"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/bin"

cat >"$tmp/bin/curl" <<'FAKE'
#!/bin/sh
# curl falso: registra argv, copia o arquivo -K e responde como um servidor.
echo "$*" >>"$LOG/argv"
out=""
while [ $# -gt 0 ]; do
  case "$1" in
    -K) cp "$2" "$LOG/cfg"; echo "$2" >"$LOG/cfg_path"; shift 2 ;;
    -D) : >"$2"; shift 2 ;;
    -o) out="$2"; shift 2 ;;
    *) shift ;;
  esac
done
if [ -n "$out" ]; then
  printf '{"ok":1}' >"$out"
  printf '200'
else
  printf '{"content":[{"type":"text","text":"ola"}],"usage":{"input_tokens":1,"output_tokens":1}}\n200'
fi
FAKE
chmod +x "$tmp/bin/curl"

cat >"$tmp/prog.tilt" <<'EOF2'
llm g:
  provedor: "anthropic"
  modelo: "claude-teste"
  chave: env "SEGREDO_LLM"

pipeline p:
  passos:
    - r = perguntar g, usuario: "oi"
    - imprimir r.texto
    - j = http_get_json "https://usuario:SENHA_URL@exemplo.test/api"
    - imprimir j.ok
EOF2

export LOG="$tmp"
out=$(cd "$tmp" && PATH="$tmp/bin:$PATH" SEGREDO_LLM="SEGREDO_LLM_XYZ" "$BIN" executar prog.tilt 2>&1) || {
  echo "execucao falhou: $out"; exit 1; }
printf '%s\n' "$out"

fail=0
echo "$out" | grep -q "ola" || { echo "resposta do LLM ausente"; fail=1; }
[ -s "$tmp/argv" ] || { echo "curl falso nao foi chamado"; fail=1; }

# 1) nenhum segredo no argv
if grep -q "SEGREDO_LLM_XYZ\|SENHA_URL" "$tmp/argv"; then
  echo "segredo vazou para o argv do curl:"; cat "$tmp/argv"; fail=1
fi
# 2) os segredos estao no arquivo -K (a ultima copia e a da chamada HTTP; o
#    arquivo da chamada LLM foi sobrescrito, entao confere a chave no argv log
#    da 1a chamada via -K de novo abaixo)
grep -q 'usuario:SENHA_URL@exemplo.test' "$tmp/cfg" || {
  echo "URL com userinfo ausente do arquivo -K"; fail=1; }
# 3) o arquivo -K nao sobrevive a chamada
p=$(cat "$tmp/cfg_path")
[ ! -e "$p" ] || { echo "arquivo -K nao foi removido: $p"; fail=1; }

# 4) chave do LLM no -K (roda so o LLM)
cat >"$tmp/llm.tilt" <<'EOF2'
llm g:
  provedor: "anthropic"
  modelo: "claude-teste"
  chave: env "SEGREDO_LLM"

pipeline p:
  passos:
    - r = perguntar g, usuario: "oi"
    - imprimir r.texto
EOF2
rm -f "$tmp/argv" "$tmp/cfg"
(cd "$tmp" && PATH="$tmp/bin:$PATH" SEGREDO_LLM="SEGREDO_LLM_XYZ" "$BIN" executar llm.tilt >/dev/null 2>&1) || true
grep -qi 'x-api-key: SEGREDO_LLM_XYZ' "$tmp/cfg" || {
  echo "x-api-key ausente do arquivo -K"; fail=1; }
grep -q "SEGREDO_LLM_XYZ" "$tmp/argv" && { echo "chave do LLM no argv"; fail=1; }

[ "$fail" = 0 ] && echo "curl_secrets_test ok"
exit "$fail"
