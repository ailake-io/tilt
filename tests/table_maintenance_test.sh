#!/usr/bin/env sh
# Compactação e vacuum conservador de Delta/Iceberg.
set -eu

BIN="$1"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/maintenance.tilt" <<'EOF'
pipeline manutencao:
  passos:
    - dados = [{ id: 1, nome: "ana" }]
    - mais = [{ id: 2, nome: "bia" }]
    - escrever_delta dados, "delta"
    - anexar_delta mais, "delta"
    - otimizar_delta "delta"
    - removidos_delta = vacuum_delta "delta"
    - atuais_delta = ler_delta "delta"
    - imprimir "delta:", removidos_delta, tamanho(atuais_delta)
    - escrever_iceberg dados, "iceberg"
    - anexar_iceberg mais, "iceberg"
    - otimizar_iceberg "iceberg"
    - removidos_iceberg = vacuum_iceberg "iceberg"
    - atuais_iceberg = ler_iceberg "iceberg"
    - imprimir "iceberg:", removidos_iceberg, tamanho(atuais_iceberg)
    - part1 = [{ id: 1, estado: "sp" }]
    - part2 = [{ id: 2, estado: "sp" }]
    - escrever_delta part1, "delta_part", particionar_por: "estado"
    - anexar_delta part2, "delta_part"
    - otimizar_delta "delta_part"
    - removidos_part_delta = vacuum_delta "delta_part"
    - lido_part_delta = ler_delta "delta_part", onde: { estado: "sp" }
    - imprimir "delta-part:", removidos_part_delta, tamanho(lido_part_delta)
    - escrever_iceberg part1, "iceberg_part", particionar_por: ["estado"]
    - anexar_iceberg part2, "iceberg_part"
    - otimizar_iceberg "iceberg_part"
    - removidos_part_iceberg = vacuum_iceberg "iceberg_part"
    - lido_part_iceberg = ler_iceberg "iceberg_part", onde: { estado: "sp" }
    - imprimir "iceberg-part:", removidos_part_iceberg, tamanho(lido_part_iceberg)
    - zdados = [{ x: 1, y: 20, bucket: "a" }, { x: 0, y: 30, bucket: "a" }, { x: 1, y: 10, bucket: "a" }]
    - escrever_delta zdados, "delta_z", particionar_por: "bucket", z_order: ["x", "y"]
    - lido_z_delta = ler_delta "delta_z", onde: { bucket: "a" }
    - imprimir "delta-z:", tamanho(lido_z_delta), lido_z_delta[0].x, lido_z_delta[0].y
    - escrever_iceberg zdados, "iceberg_z", particionar_por: "bucket", z_order: ["x", "y"]
    - lido_z_iceberg = ler_iceberg "iceberg_z", onde: { bucket: "a" }
    - imprimir "iceberg-z:", tamanho(lido_z_iceberg), lido_z_iceberg[0].x, lido_z_iceberg[0].y
EOF

out=$(cd "$tmp" && "$BIN" executar maintenance.tilt 2>&1)
printf '%s\n' "$out"
echo "$out" | grep -q "delta: 2 2" || {
  echo "vacuum Delta nao removeu exatamente um orfao"; exit 1; }
echo "$out" | grep -q "iceberg: 2 2" || {
  echo "vacuum Iceberg nao removeu exatamente dois orfaos"; exit 1; }
echo "$out" | grep -q "delta-part: 2 2" || { echo "compactacao Delta perdeu particao"; exit 1; }
echo "$out" | grep -q "iceberg-part: 2 2" || { echo "compactacao Iceberg perdeu particao"; exit 1; }

echo "$out" | grep -q "delta-z: 3 0 30" || { echo "z-order Delta perdeu linhas"; exit 1; }
echo "$out" | grep -q "iceberg-z: 3 0 30" || { echo "z-order Iceberg perdeu linhas"; exit 1; }
echo "table_maintenance: ok"
