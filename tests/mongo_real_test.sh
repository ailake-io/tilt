#!/usr/bin/env sh
# MongoDB real (mongod 7): handshake com $db, cursores paginados por getMore em
# find e aggregate (250 docs > batchSize default 101; o cursor.id e int64) e os
# operadores de update alem de $set/$inc. O mock em python nao reproduz nada
# disso. Sem TILT_MONGO_REAL_URL (ex.: mongodb://127.0.0.1:27017/tilt_real), pula.
set -eu

BIN="$1"
[ -n "${TILT_MONGO_REAL_URL:-}" ] || {
  echo "TILT_MONGO_REAL_URL nao definida; pulando o teste mongo_real"
  exit 0
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
col="real_$$_$(date +%s)"

cat >"$tmp/prog.tilt" <<EOF2
pipeline mongo_real:
  passos:
    - para cada i em 0..250:
        mongo_inserir "$col", {n: i, grupo: "g" + texto(i % 3), tags: ["a"]}
    - todos = mongo_buscar "$col", {max: 0}
    - imprimir "buscar:", tamanho(todos)
    - agg = mongo_agregar "$col", [{"\$match": {}}]
    - imprimir "agregar:", tamanho(agg)
    - g = mongo_agregar "$col", [{"\$group": {"_id": "\$grupo", "total": {"\$sum": 1}}}, {"\$sort": {"_id": 1}}]
    - imprimir "grupos:", tamanho(g), g[0].total + g[1].total + g[2].total
    - m = mongo_atualizar "$col", {n: 1}, {"\$push": {tags: "b"}, "\$unset": {grupo: ""}}
    - imprimir "modificados:", m
    - um = mongo_buscar "$col", {filtro: {n: 1}}
    - imprimir "tags:", um[0].tags, um[0]?.grupo
    - apagados = mongo_deletar "$col", {}
    - imprimir "apagados:", apagados
EOF2

out=$(MONGO_URL="$TILT_MONGO_REAL_URL" "$BIN" executar "$tmp/prog.tilt" 2>&1) || {
  echo "execucao falhou: $out"; exit 1; }
printf '%s\n' "$out"

fail=0
for esperado in "buscar: 250" "agregar: 250" "grupos: 3 250" "modificados: 1" "tags: [a, b] nulo" "apagados: 250"; do
  echo "$out" | grep -qF "$esperado" || { echo "FALHA: sem '$esperado'"; fail=1; }
done
[ "$fail" = 0 ] && echo "mongo_real_test ok"
exit "$fail"
