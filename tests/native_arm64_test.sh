#!/usr/bin/env sh
# Teste do backend ARM64 do `tilt compilar`: gera o Assembly AArch64 de um
# fixture do subconjunto nativo, confere os pontos-chave (prologo AAPCS,
# chamadas ao runtime) e monta com um assembler cross (clang --target ou
# aarch64-linux-gnu-as). Quando ha toolchain completa (aarch64-linux-gnu-gcc
# ou CC_AARCH64) e qemu-aarch64, tambem linka, roda sob qemu-user e compara a
# saida com o interpretador. Sem assembler cross, pula com exit 0.
set -eu

BIN="$1"
FIXTURE="$2"

# 1) algum assembler cross de AArch64 disponivel?
AS_CROSS=""
if command -v aarch64-linux-gnu-as >/dev/null 2>&1; then
  AS_CROSS="aarch64-linux-gnu-as"
elif command -v clang >/dev/null 2>&1; then
  AS_CROSS="clang --target=aarch64-linux-gnu -c"
fi
if [ -z "$AS_CROSS" ]; then
  echo "assembler cross AArch64 ausente; pulando o teste ARM64"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# 2) geracao do ASM. Em host x86-64 o link cruzado falha (sem toolchain) — o
#    que importa aqui e o .s (--asm mantem os intermediarios).
"$BIN" compilar "$FIXTURE" --saida "$tmp/prog" --arch arm64 --asm >/dev/null 2>&1 || true
asm="$tmp/prog.tilt.s"
[ -f "$asm" ] || {
  echo "ASM ARM64 nao foi gerado"
  exit 1
}

# 3) pontos-chave do backend: prologo/epilogo AAPCS e chamadas ao runtime C.
grep -q "stp x29, x30, \[sp, #-16\]!" "$asm" || {
  echo "prologo AAPCS ausente no ASM"
  exit 1
}
grep -q "bl tv_print" "$asm" || {
  echo "chamadas ao runtime ausentes no ASM"
  exit 1
}
grep -q "stp x19, x20" "$asm" || {
  echo "salvamento de callee-saved ausente no ASM"
  exit 1
}
echo "native_arm64 ok (geracao): $(basename "$FIXTURE")"

# 4) montagem com o assembler cross.
set -- $AS_CROSS
"$@" "$asm" -o "$tmp/prog.o" || {
  echo "falha ao montar o ASM ARM64 ($AS_CROSS)"
  exit 1
}
echo "native_arm64 ok (montagem): $(basename "$FIXTURE")"

# 5) execucao sob qemu-user, quando ha linker cross + qemu-aarch64.
CC_AARCH64="${CC_AARCH64:-aarch64-linux-gnu-gcc}"
if command -v "$CC_AARCH64" >/dev/null 2>&1 && command -v qemu-aarch64 >/dev/null 2>&1; then
  "$CC_AARCH64" -O2 -o "$tmp/prog.a64" "$tmp/prog.o" "$tmp/prog.tilt.rt.c" -lm
  "$BIN" executar "$FIXTURE" >"$tmp/interp.out"
  qemu-aarch64 "$tmp/prog.a64" >"$tmp/native.out"
  if diff -u "$tmp/interp.out" "$tmp/native.out"; then
    echo "native_arm64 ok (execucao qemu): $(basename "$FIXTURE")"
  else
    echo "saida nativa ARM64 difere do interpretador"
    exit 1
  fi
else
  echo "toolchain/qemu AArch64 incompletos; execucao sob qemu pulada"
fi
