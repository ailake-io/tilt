#!/bin/sh
set -u

bin=$1
root=$2
tmp=$(mktemp -d "${TMPDIR:-/tmp}/tilt-cluster-failure.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

python3 - "$root" "$tmp" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
tmp = pathlib.Path(sys.argv[2])
source = (root / "tests/golden/run-treino-fluxo/input.tilt").read_text()
data = (root / "tests/golden/run-treino-fluxo/dados.csv").resolve()
source = source.replace('"dados.csv"', '"' + str(data) + '"')
source = source.replace("  epocas: 30\n", "  epocas: 1\n")
source = source.replace(
    "  semente: 7\n",
    "  semente: 7\n  cluster: { dir: \"" + str(tmp) +
    "\", rank: 0, mundo: 2, timeout: 1, recuperar: verdadeiro, tentativas: 1 }\n",
)
(tmp / "rank0.tilt").write_text(source)
PY

if "$bin" executar "$tmp/rank0.tilt" >"$tmp/out" 2>&1; then
  echo "rank ausente deveria causar falha" >&2
  exit 1
fi
grep -q "workers ausentes" "$tmp/out"
grep -q "rank 1" "$tmp/out"
