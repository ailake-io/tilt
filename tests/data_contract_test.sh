#!/bin/sh
set -eu

tilt=$1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/tilt-data-contract.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/dados.csv" <<'CSV'
id,valor,quando
1,1.5,2024-01-01T10:00:00-03:00
2,2.0,2024-01-02T10:00:00-03:00
3,3.25,2024-01-03T10:00:00-03:00
CSV
cat >"$tmp/contrato.tilt" <<'TILT'
pipeline contrato:
  passos:
    - tabela = ler_csv "dados.csv", inferir: verdadeiro, amostra: 2, destino_fuso: "UTC"
    - schema = inferir_schema tabela
    - validacao = validar_schema tabela, schema
    - perfil = perfil tabela, amostra: 2
    - nova = [{ id: 4, valor: 4.0, extra: "novo" }]
    - evoluido = evoluir_schema schema, nova
    - imprimir tabela[0].quando, validacao.ok, perfil.amostra, evoluido.versao
TILT

output=$(cd "$tmp" && "$tilt" executar contrato.tilt)
printf '%s\n' "$output" | grep -q '2024-01-01T13:00:00'
printf '%s\n' "$output" | grep -q 'verdadeiro 2 2'
echo "data_contract ok"
