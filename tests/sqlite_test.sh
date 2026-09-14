#!/usr/bin/env sh
# Params `?` + transacao com ROLLBACK no SQLite (Marco 3 / D1): arquivo
# temporario, sem servidor. Roda fixtures/sqlite_params.tilt (inserts com
# aspas/unicode/nulo, transacao comitada, transacao com PK duplicada
# capturada) e confere a leitura final (sem id 4) + erro de contagem.
set -eu

BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/sqlite_params.tilt}"
case "$FIXTURE" in
  /*) ;;
  *) FIXTURE="$(pwd)/$FIXTURE" ;;
esac

command -v python3 >/dev/null 2>&1 || {
  echo "python3 ausente; pulando o teste sqlite"
  exit 0
}
python3 -c "import ctypes, ctypes.util; assert ctypes.util.find_library('sqlite3')" 2>/dev/null || {
  echo "libsqlite3 ausente; pulando o teste sqlite"
  exit 0
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

out=$(cd "$tmp" && env SQL_URL="sqlite://banco.db" "$BIN" executar "$FIXTURE")
printf '%s\n' "$out"

fail=0
confere() {
  printf '%s\n' "$out" | grep -qF "$1" || { echo "saida sem '$1'"; fail=1; }
}
confere "linha: 1 o'brien 10.5"
confere "linha: 2 bé nulo"
confere "linha: 3 carla 8"
confere "rollback:"
echo "$out" | grep -q "linha: 4" && { echo "ROLLBACK falhou (id 4 presente)"; fail=1; }

# contagem divergente de params: erro claro, sem tocar no banco
cat > "$tmp/conta.tilt" <<'TILTEOF'
pipeline principal:
  passos:
    - executar_sql "sqlite://banco.db", "insert into parametros values (?, ?, ?)", [9]
TILTEOF
if err=$(cd "$tmp" && "$BIN" executar conta.tilt 2>&1); then
  echo "contagem divergente deveria falhar"; fail=1
else
  echo "$err" | grep -q "parametro" || { echo "erro sem mencionar parametro: $err"; fail=1; }
fi

# python confere o arquivo (3 linhas, update aplicado)
python3 - "$tmp/banco.db" <<'PYEOF'
import sqlite3
import sys

rows = sqlite3.connect(sys.argv[1]).execute("select id, nome, nota from parametros order by id").fetchall()
assert rows == [(1, "o'brien", 10.5), (2, "bé", None), (3, "carla", 8.0)], rows
print("sqlite: arquivo validado (%d linhas)" % len(rows))
PYEOF

[ "$fail" = 0 ] && echo "sqlite_test ok"
exit "$fail"
