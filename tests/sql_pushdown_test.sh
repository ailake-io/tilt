#!/bin/sh
set -eu

BIN=$1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/tilt-sql-pushdown.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/pushdown.tilt" <<'TILT'
fonte pessoas:
  tipo: sqlite
  caminho: env "SQLITE_DB"
  consulta: "select id, nome, idade, ativo from pessoas order by id"
  pushdown:
    colunas: ["id", "nome"]
    onde: { ativo: verdadeiro, idade: 18 }
    limite: 1

pipeline p:
  passos:
    - url = "sqlite://" + env "SQLITE_DB"
    - executar_sql url, "create table pessoas (id integer, nome text, idade integer, ativo integer)"
    - executar_sql url, "insert into pessoas values (1, 'ana', 30, 1)"
    - executar_sql url, "insert into pessoas values (2, 'bob', 18, 1)"
    - executar_sql url, "insert into pessoas values (3, 'carla', 18, 0)"
    - rows = ler pessoas
    - para cada row em rows:
        imprimir row.id, row.nome
TILT

out=$(cd "$tmp" && env SQLITE_DB="$tmp/pessoas.db" "$BIN" executar "$tmp/pushdown.tilt")
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -qF "2 bob"
if printf '%s\n' "$out" | grep -qE '1 ana|3 carla'; then
  echo "pushdown retornou linhas fora do filtro" >&2
  exit 1
fi
