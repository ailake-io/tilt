#!/usr/bin/env sh
# Integration test do conector DuckDB (libduckdb.so carregada via dlopen em
# runtime): roda fixtures/duckdb_roundtrip.tilt contra um arquivo temporario,
# criando a tabela via executar_sql "duckdb://...", inserindo, atualizando,
# lendo de volta via fonte/ler (decimal, boolean como inteiro, nulo) e
# capturando os erros do DuckDB (PK duplicada e SQL invalido). Se a libduckdb
# nao estiver instalada (ldconfig, LD_LIBRARY_PATH ou caminhos comuns), pula
# com mensagem no padrao dos outros testes.
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/duckdb_roundtrip.tilt}"

duckdb_lib_found() {
  if ldconfig -p 2>/dev/null | grep -q "libduckdb\.so"; then
    return 0
  fi
  # shellcheck disable=SC2046
  for f in $(echo "${LD_LIBRARY_PATH:-}" | tr ':' ' ') /usr/lib /usr/local/lib \
           /usr/lib/x86_64-linux-gnu /opt/duckdb; do
    [ -n "$f" ] || continue
    for g in "$f"/libduckdb.so "$f"/libduckdb.so.* "$f"/libduckdb.dylib "$f"/libduckdb.*.dylib; do
      [ -e "$g" ] && return 0
    done
  done
  return 1
}

duckdb_lib_found || {
  echo "libduckdb nao encontrada (instale o pacote duckdb); pulando o teste duckdb"
  exit 0
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
DB="$tmp/teste.db"
export DUCKDB_DB="$DB"
export DUCKDB_URL="duckdb://$DB"

out=$("$BIN" executar "$FIXTURE")

fail=0
# (a) INSERT + UPDATE refletidos no SELECT de volta; boolean vira inteiro
#     (1/0) e NULL vira nulo
echo "$out" | grep -q "linha: 1 ana 10.5 1 nulo" || {
  echo "saida sem 'linha: 1 ana 10.5 1 nulo': $out"; fail=1;
}
echo "$out" | grep -q "linha: 2 bruno 99.9 0 ok" || {
  echo "saida sem 'linha: 2 bruno 99.9 0 ok': $out"; fail=1;
}
# (b) erro de constraint do DuckDB capturado com a mensagem do servidor
echo "$out" | grep -q "erro-constraint:.*primary key" || {
  echo "saida sem erro de constraint capturado: $out"; fail=1;
}
# (c) SQL invalido capturado com a mensagem do parser
echo "$out" | grep -q "erro-sql:.*syntax error" || {
  echo "saida sem erro de SQL invalido capturado: $out"; fail=1;
}

[ "$fail" = 0 ] && echo "duckdb_test ok"
exit "$fail"
