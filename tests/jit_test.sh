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
# O JIT nativo so existe em x86_64 (jit.cpp); nas demais arquiteturas o
# resultado igual ao interpretador vem pelo fallback transparente.
case "$(uname -m)" in
  x86_64|amd64) grep -q '^\[jit native\]' "$tmp/jit.err" ;;
  *) grep -q '^\[jit fallback\]' "$tmp/jit.err" ;;
esac

"$BIN" executar "$FIXTURES/nativo2.tilt" >"$tmp/fallback-interp.out"
TILT_JIT_DEBUG=1 "$BIN" executar --jit "$FIXTURES/nativo2.tilt" \
  >"$tmp/fallback-jit.out" 2>"$tmp/fallback.err"
diff -u "$tmp/fallback-interp.out" "$tmp/fallback-jit.out"
grep -q '^\[jit fallback\]' "$tmp/fallback.err"

echo "jit_test ok"
