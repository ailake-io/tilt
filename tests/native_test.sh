#!/usr/bin/env sh
# Differential test for `tilt compilar`: the native binary must produce the same
# output as `tilt executar` for an integer-only program. Skipped when no C
# compiler or not on x86-64.
set -eu

BIN="$1"
FIXTURE="$2"

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

"$BIN" executar "$FIXTURE" >"$tmp/interp.out"
"$BIN" compilar "$FIXTURE" --saida "$tmp/prog" >/dev/null
"$tmp/prog" >"$tmp/native.out"

if diff -u "$tmp/interp.out" "$tmp/native.out"; then
  echo "native_test ok"
else
  echo "saida nativa difere do interpretador"
  exit 1
fi
