#!/usr/bin/env sh
# Differential test for `tilt compilar`: the native binary must produce the
# same output as `tilt executar` for each fixture (integer/texto/decimal/
# lista/pipeline subsets). Skipped when no C compiler or not on x86-64.
set -eu

BIN="$1"
shift

arch=$(uname -m 2>/dev/null || echo unknown)
case "$arch" in
  x86_64 | amd64) ;;
  *)
    echo "arch $arch != x86_64; pulando o teste de codegen nativo"
    exit 0
    ;;
esac
command -v "${CC:-cc}" >/dev/null 2>&1 || {
  echo "compilador C ausente; pulando o teste de codegen nativo"
  exit 0
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

for FIXTURE in "$@"; do
  nome=$(basename "$FIXTURE")
  "$BIN" executar "$FIXTURE" >"$tmp/$nome.interp.out"
  "$BIN" compilar "$FIXTURE" --saida "$tmp/$nome.bin" >/dev/null
  "$tmp/$nome.bin" >"$tmp/$nome.native.out"

  if diff -u "$tmp/$nome.interp.out" "$tmp/$nome.native.out"; then
    echo "native_test ok: $nome"
  else
    echo "saida nativa difere do interpretador: $nome"
    exit 1
  fi
done
