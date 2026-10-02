#!/usr/bin/env sh
set -eu

BIN="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
printf 'arquivo-secreto\n' >"$TMP/key"
cat >"$TMP/keys.tilt" <<EOF
pipeline keys:
  passos:
    - dados = [{ id: 7 }]
    - escrever_parquet dados, "env.parquet", chave_env: "TILT_PARQUET_TEST_KEY"
    - env_lido = ler_parquet "env.parquet"
    - escrever_parquet dados, "file.parquet", chave_arquivo: "$TMP/key"
    - file_lido = ler_parquet "file.parquet"
    - imprimir env_lido[0].id, file_lido[0].id
EOF
(cd "$TMP" && TILT_PARQUET_TEST_KEY=ambiente "$BIN" executar keys.tilt) >"$TMP/out"
grep -F '7 7' "$TMP/out" >/dev/null
echo "parquet_key_provider_test ok"
