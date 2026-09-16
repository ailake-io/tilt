#!/usr/bin/env sh
# Teste do pool de conexoes (sql_pool.*) sem servidor de banco: compila o
# driver unitario standalone (so sql_pool.cpp, sem dependencias) e roda.
# Cobre reuso (hit), troca de conexao morta (stale), descarte de BEGIN/SET,
# discard() explicito, concorrencia 8x200 e bypass via TILT_SQL_POOL=0.
set -eu

SRC="${0%/*}/../src"
UNIT="${0%/*}/sql_pool_unit.cpp"

command -v g++ >/dev/null 2>&1 || {
  echo "g++ ausente; pulando o teste sql_pool"
  exit 0
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

g++ -std=c++20 -Wall -Wextra -Werror -I "$SRC" "$UNIT" "$SRC/runtime/sql_pool.cpp" \
  -o "$tmp/sql_pool_unit" || {
  echo "falha ao compilar o driver do pool"
  exit 1
}

"$tmp/sql_pool_unit" || {
  echo "falha no unit do pool"
  exit 1
}

# Bypass: sem nenhum hit com TILT_SQL_POOL=0.
hits=$(TILT_SQL_POOL=0 TILT_SQL_POOL_DEBUG=1 "$tmp/sql_pool_unit" 2>&1 | grep -c "hit" || true)
[ "$hits" = "0" ] || {
  echo "bypass com TILT_SQL_POOL=0 ainda deu hit"
  exit 1
}

# Reuso observavel com debug ligado (log vai para o stderr).
TILT_SQL_POOL_DEBUG=1 "$tmp/sql_pool_unit" 2>&1 | grep -q "hit" || {
  echo "sem hit com pool ligado"
  exit 1
}

echo "sql_pool_test ok"
