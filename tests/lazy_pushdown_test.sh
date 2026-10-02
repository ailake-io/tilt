#!/usr/bin/env sh
set -eu

bin=$1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/dados.csv" <<'EOF'
id,grupo,valor
1,a,10
2,b,20
3,a,30
4,b,40
5,a,50
EOF

cat >"$tmp/lazy.tilt" <<EOF
fonte sql:
  tipo: sqlite
  caminho: "$tmp/sqlite.db"
  consulta: "select id, grupo, valor from dados"
  lazy: verdadeiro

pipeline lazy_pushdown:
  passos:
    - csv = ler_csv "$tmp/dados.csv", colunar: verdadeiro, lazy: verdadeiro
    - filtrado = csv.filtrar linha.id >= 2 e linha.id <= 4
    - projetado = filtrado.selecionar "grupo"
    - limitado = projetado.limite 2
    - escrever_json limitado, "$tmp/csv.json"
    - base = [{ id: 1, grupo: "a" }, { id: 2, grupo: "b" }, { id: 3, grupo: "a" }]
    - escrever_parquet base, "$tmp/dados.parquet", row_group: 1
    - parquet = ler_parquet "$tmp/dados.parquet", colunar: verdadeiro, lazy: verdadeiro
    - pfiltrado = parquet.filtrar linha.id >= 2
    - pprojetado = pfiltrado.selecionar "grupo"
    - escrever_json pprojetado, "$tmp/parquet.json"
    - plimitado = parquet.limite 1
    - escrever_json plimitado, "$tmp/parquet_limit.json"
    - url = "sqlite://$tmp/sqlite.db"
    - executar_sql url, "create table dados (id integer, grupo text, valor integer)"
    - executar_sql url, "insert into dados values (1, 'a', 10), (2, 'b', 20), (3, 'a', 30)"
    - remoto = ler sql
    - remoto = remoto.filtrar linha.id >= 2 e linha.id <= 3
    - remoto = remoto.selecionar "grupo"
    - remoto = remoto.limite 1
    - escrever_json remoto, "$tmp/sql.json"
EOF

"$bin" executar "$tmp/lazy.tilt" >/dev/null
grep -q '"grupo": "b"' "$tmp/csv.json"
grep -q '"grupo": "a"' "$tmp/csv.json"
grep -q '"grupo": "b"' "$tmp/parquet.json"
grep -q '"grupo": "a"' "$tmp/parquet.json"
test "$(grep -c '"grupo"' "$tmp/csv.json")" -eq 2
test "$(grep -c '"grupo"' "$tmp/parquet.json")" -eq 2
test "$(grep -c '"id"' "$tmp/parquet_limit.json")" -eq 1
grep -q '"grupo": "b"' "$tmp/sql.json"
test "$(grep -c '"grupo"' "$tmp/sql.json")" -eq 1
echo "lazy_pushdown_test ok"
