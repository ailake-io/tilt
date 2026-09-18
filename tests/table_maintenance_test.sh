#!/usr/bin/env sh
# Vacuum conservador: remove somente Parquet órfão de Delta/Iceberg.
set -eu

BIN="$1"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/maintenance.tilt" <<'EOF'
pipeline manutencao:
  passos:
    - dados = [{ id: 1, nome: "ana" }]
    - escrever_delta dados, "delta"
    - escrever_delta dados, "delta"
    - removidos_delta = vacuum_delta "delta"
    - atuais_delta = ler_delta "delta"
    - imprimir "delta:", removidos_delta, tamanho(atuais_delta)
    - escrever_iceberg dados, "iceberg"
    - escrever_iceberg dados, "iceberg"
    - removidos_iceberg = vacuum_iceberg "iceberg"
    - atuais_iceberg = ler_iceberg "iceberg"
    - imprimir "iceberg:", removidos_iceberg, tamanho(atuais_iceberg)
EOF

out=$(cd "$tmp" && "$BIN" executar maintenance.tilt 2>&1)
printf '%s\n' "$out"
echo "$out" | grep -q "delta: 1 1" || {
  echo "vacuum Delta nao removeu exatamente um orfao"; exit 1; }
echo "$out" | grep -q "iceberg: 1 1" || {
  echo "vacuum Iceberg nao removeu exatamente um orfao"; exit 1; }

echo "table_maintenance: ok"
