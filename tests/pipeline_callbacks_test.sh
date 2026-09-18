#!/usr/bin/env sh
# Callbacks de sucesso/falha recebem contexto do pipeline.
set -eu

BIN="$1"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/callbacks.tilt" <<'EOF'
pipeline sucesso:
  sla: "1h"
  passos:
    - imprimir "corpo-ok"
  on_success:
    - imprimir "callback-sucesso", status, pipeline, tentativas

pipeline falha:
  sla: "1h"
  ao_falhar: repetir 1, jitter: "0s"
  passos:
    - x = ler_csv "arquivo-inexistente.csv"
  on_failure:
    - imprimir "callback-falha", status, pipeline, tentativas
EOF

out=$(cd "$tmp" && "$BIN" executar callbacks.tilt 2>&1 || true)
printf '%s\n' "$out"
echo "$out" | grep -q "callback-sucesso sucesso sucesso 1" || {
  echo "callback de sucesso sem contexto"; exit 1; }
echo "$out" | grep -q "callback-falha falha falha 2" || {
  echo "callback de falha sem contexto"; exit 1; }

json_out=$(cd "$tmp" && TILT_PIPELINE_LOG_JSON=1 "$BIN" executar callbacks.tilt 2>&1 || true)
echo "$json_out" | grep -q '\{"evento":"pipeline","nome":"sucesso","status":"sucesso"' || {
  echo "log JSON sem contexto de sucesso"; exit 1; }
echo "$json_out" | grep -q '\{"evento":"pipeline","nome":"falha","status":"falha"' || {
  echo "log JSON sem contexto de falha"; exit 1; }

echo "pipeline_callbacks: ok"
