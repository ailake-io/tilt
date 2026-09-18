#!/usr/bin/env sh
# Valida o caminho JIT nativo e o fallback transparente para a VM.
set -eu

BIN="$1"
FIXTURES="$2"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$BIN" executar "$FIXTURES/jit_inteiros.tilt" >"$tmp/interp.out"
TILT_JIT_DEBUG=1 "$BIN" executar --jit "$FIXTURES/jit_inteiros.tilt" \
  >"$tmp/jit.out" 2>"$tmp/jit.err"
diff -u "$tmp/interp.out" "$tmp/jit.out"
grep -q '^\[jit native\]' "$tmp/jit.err"

"$BIN" executar "$FIXTURES/nativo2.tilt" >"$tmp/fallback-interp.out"
TILT_JIT_DEBUG=1 "$BIN" executar --jit "$FIXTURES/nativo2.tilt" \
  >"$tmp/fallback-jit.out" 2>"$tmp/fallback.err"
diff -u "$tmp/fallback-interp.out" "$tmp/fallback-jit.out"
grep -q '^\[jit fallback\]' "$tmp/fallback.err"

echo "jit_test ok"
