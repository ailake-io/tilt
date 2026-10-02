#!/usr/bin/env sh
set -eu
BIN="$1"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat >"$tmp/loop.tilt" <<'TILT'
pipeline paralelo:
  passos:
    - saida = [0, 0, 0, 0, 0, 0]
    - para cada i em intervalo(0, 6):
        saida[i] = i * i
    - imprimir saida
TILT
out=$(TILT_LOOP_PARALLEL=1 "$BIN" executar "$tmp/loop.tilt")
echo "$out" | grep -Fq '[0, 1, 4, 9, 16, 25]'
