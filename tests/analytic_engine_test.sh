#!/usr/bin/env sh
set -eu

BIN="$1"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/pipeline.tilt" <<EOF
pipeline analitico:
  paralelo: verdadeiro
  passos:
    - csv = ler_csv "$tmp/dados.csv", colunar: verdadeiro, lazy: verdadeiro
    - base = [{ grupo: "a", valor: 2 }, { grupo: "a", valor: 3 }]
    - escrever_parquet base, "$tmp/dados.parquet"
    - parquet = ler_parquet "$tmp/dados.parquet", colunar: verdadeiro, lazy: verdadeiro
    - grupos = base.agrupar_por "grupo", { total: somar "valor", n: contar }
    - imprimir tamanho(csv), tamanho(parquet), grupos[0].total, grupos[0].n
EOF
printf 'nome,valor\nana,1\nbruno,2\n' >"$tmp/dados.csv"
out=$(TILT_ANALYTIC_ENGINE=native "$BIN" executar "$tmp/pipeline.tilt")
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -q '2 2 5 2'
echo "analytic_engine_test ok"
