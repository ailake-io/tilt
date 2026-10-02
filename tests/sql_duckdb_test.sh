#!/usr/bin/env sh
# `sql` com motor duckdb: tabelas tilt via appender, parametros e SQL que le CSV/
# Parquet direto (motor "auto" escolhe o DuckDB por conta do nome do arquivo).
# Pula (exit 0) sem libduckdb: defina TILT_DUCKDB_DIR com a pasta da libduckdb.so.
set -eu

BIN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
if [ -n "${TILT_DUCKDB_DIR:-}" ]; then
  export LD_LIBRARY_PATH="$TILT_DUCKDB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
elif ! ldconfig -p 2>/dev/null | grep -q libduckdb; then
  echo "pulado: sem libduckdb (defina TILT_DUCKDB_DIR)"; exit 0
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"
printf 'regiao,valor\nsul,10\nnorte,20\nsul,5\n' > v.csv
cat > p.tilt <<'TILTEOF'
pipeline p:
  passos:
    - vendas = [{ regiao: "sul", valor: 30, taxa: 0.5 }, { regiao: "norte", valor: 120, taxa: 1.5 }, { regiao: "sul", valor: 5, taxa: 0.25 }]
    - r = sql "select regiao, sum(valor) as total, count(*) as n from vendas group by regiao order by regiao", motor: "duckdb"
    - imprimir r[0].regiao, r[0].total, r[0].n, r[1].total
    - a = sql "select * from vendas where valor >= ? order by valor", [30], motor: "duckdb"
    - imprimir tamanho(a), a[0].valor
    - c = sql "select regiao, sum(valor) as total from 'v.csv' group by regiao order by regiao"
    - imprimir c[0].regiao, c[0].total, c[1].regiao, c[1].total
    - escrever_parquet c, "s.parquet"
    - d = sql "select count(*) as n from 's.parquet'"
    - imprimir d[0].n
TILTEOF
out=$("$BIN" executar p.tilt 2>&1) || { echo "FALHA: $out"; exit 1; }
esperado="== pipeline p ==
norte 120 1 35
2 30
norte 20 sul 15
2"
[ "$out" = "$esperado" ] || { echo "FALHA: saida inesperada:"; echo "$out"; exit 1; }
echo "sql duckdb ok"
