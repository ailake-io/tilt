#!/usr/bin/env sh
set -eu

BIN="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cat >"$TMP/nested4.tilt" <<'EOF'
pipeline nested4:
  passos:
    - dados = [{ n: [[[[1, 2], [3]], [[4]]]] }, { n: [[[[5]]]] }, { n: nulo }]
    - escrever_parquet dados, "nested4.parquet"
    - volta = ler_parquet "nested4.parquet"
    - imprimir volta[0].n
    - imprimir volta[1].n
    - imprimir volta[2].n
EOF
(cd "$TMP" && "$BIN" executar nested4.tilt) >"$TMP/out"
grep -F '[[[[1, 2], [3]], [[4]]]]' "$TMP/out" >/dev/null
grep -F '[[[[5]]]]' "$TMP/out" >/dev/null
grep -F 'nulo' "$TMP/out" >/dev/null
echo "parquet_nested4_test ok"
