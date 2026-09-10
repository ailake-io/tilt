#!/usr/bin/env sh
# Integration test do builtin executar_sql com Postgres real: sobe um cluster
# temporario com initdb (auth trust, porta TILT_TEST_PORT ou 8661, socket unix
# no dir tmp para nao depender de /var/run/postgresql), roda
# fixtures/pg_roundtrip.tilt (CREATE TABLE + INSERT + UPDATE via executar_sql,
# SELECT de volta via fonte/ler e INSERT com erro de constraint capturado),
# valida a saida e confere com psql que a tabela existe. Derruba o cluster no
# trap EXIT. Se initdb/postgres nao existirem no PATH nem em
# /usr/lib/postgresql/*/bin, pula com mensagem (padrao dos outros testes).
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/pg_roundtrip.tilt}"
PORT_BASE="${TILT_TEST_PORT:-8661}"

PGBIN=""
for dir in $(echo "$PATH" | tr ':' ' ') /usr/lib/postgresql/*/bin; do
  if [ -x "$dir/initdb" ] && [ -x "$dir/pg_ctl" ] && [ -x "$dir/postgres" ]; then
    PGBIN="$dir"
    break
  fi
done
if [ -z "$PGBIN" ]; then
  echo "initdb/pg_ctl ausentes (instale o servidor PostgreSQL); pulando o teste pg"
  exit 0
fi

PSQL=$(command -v psql || true)
[ -n "$PSQL" ] || {
  echo "psql ausente; pulando o teste pg"
  exit 0
}

tmp=$(mktemp -d)
data="$tmp/data"
sock="$tmp/sock"
mkdir -p "$data" "$sock"
pg_pid=""
cleanup() {
  if [ -n "$pg_pid" ]; then
    "$PGBIN/pg_ctl" -D "$data" -m fast stop >/dev/null 2>&1 || true
  fi
  rm -rf "$tmp"
}
trap cleanup EXIT

"$PGBIN/initdb" -D "$data" -A trust -U postgres \
  >"$tmp/initdb.log" 2>&1 || {
  echo "initdb falhou:"; tail -20 "$tmp/initdb.log"; exit 1;
}

# sobe o servidor na primeira porta livre a partir de PORT_BASE (pg_ctl -w
# falha rapido quando a porta esta ocupada)
PORTA=""
for p in $(seq "$PORT_BASE" $((PORT_BASE + 19))); do
  if "$PGBIN/pg_ctl" -D "$data" -l "$tmp/server.log" -w \
      -o "-p $p -c listen_addresses=127.0.0.1 -c unix_socket_directories=$sock" \
      start >"$tmp/pg_ctl.log" 2>&1; then
    PORTA="$p"
    break
  fi
  "$PGBIN/pg_ctl" -D "$data" -m fast stop >/dev/null 2>&1 || true
done
[ -n "$PORTA" ] || { echo "nenhuma porta livre a partir de $PORT_BASE"; tail -20 "$tmp/server.log"; exit 1; }
pg_pid=1

PG_URL="postgres://postgres@127.0.0.1:$PORTA/postgres"
export PG_URL

# --- roundtrip via tilt executar -------------------------------------------------
out=$(env PG_URL="$PG_URL" "$BIN" executar "$FIXTURE")

fail=0
# (a) INSERT + UPDATE refletidos no SELECT de volta
echo "$out" | grep -q "linha: 1 ana 10.5" || {
  echo "saida sem 'linha: 1 ana 10.5': $out"; fail=1;
}
echo "$out" | grep -q "linha: 2 bruno 99.9" || {
  echo "saida sem 'linha: 2 bruno 99.9': $out"; fail=1;
}
# (b) erro de constraint do Postgres capturado com a mensagem do servidor
echo "$out" | grep -q "erro-constraint:.*not.null" || {
  echo "saida sem erro de constraint capturado: $out"; fail=1;
}

# (c) psql confirma que a tabela existe e tem as 2 linhas gravadas
tbl=$("$PSQL" "postgres://postgres@127.0.0.1:$PORTA/postgres" \
  -tAc "select count(*) from clientes")
[ "$tbl" = "2" ] || { echo "psql: esperado 2 linhas em clientes, obtido '$tbl'"; fail=1; }

[ "$fail" = 0 ] && echo "pg_test ok"
exit "$fail"
