#!/usr/bin/env sh
# Benchmarks de referencia (guia 16): laco numerico, recursao e agregacao de CSV,
# no interpretador, na VM e no JIT. Uso: bench/rodar.sh [caminho-do-tilt]
# Numeros dependem da maquina: compare antes/depois na MESMA maquina.
set -eu

BIN="${1:-build/release/bin/tilt}"
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"  # absoluto: o caso de dados muda de diretorio
DIR="$(cd "$(dirname "$0")" && pwd)"
export LC_ALL=C

cronometrar() { # cronometrar <rotulo> <comando...>
  rotulo=$1; shift
  inicio=$(date +%s.%N)
  saida=$("$@" 2>&1 | tail -1 | cut -c1-30)
  fim=$(date +%s.%N)
  awk -v a="$inicio" -v b="$fim" -v r="$rotulo" -v s="$saida" 'BEGIN { printf "%-32s %7.2fs  %s\n", r, b - a, s }'
}

[ -f "$DIR/vendas.csv" ] || python3 "$DIR/gerar_csv.py" >/dev/null

echo "== logica (TILT_VM_NOCACHE=1 para nao ler .tiltc)"
for modo in "executar" "executar --vm" "executar --jit"; do
  # shellcheck disable=SC2086
  TILT_VM_NOCACHE=1 cronometrar "laco 3M   [$modo]" "$BIN" $modo "$DIR/laco.tilt"
done
for modo in "executar" "executar --vm" "executar --jit"; do
  # shellcheck disable=SC2086
  TILT_VM_NOCACHE=1 cronometrar "fib(30)   [$modo]" "$BIN" $modo "$DIR/fib.tilt"
done
echo "== dados"
(cd "$DIR" && cronometrar "csv 1M: ler+agrupar+parquet" "$BIN" executar dados.tilt)
rm -f "$DIR/saida.parquet"
