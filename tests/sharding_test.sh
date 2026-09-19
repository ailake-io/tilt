#!/bin/sh
set -eu

bin=$1
root=$2
tmp=$(mktemp -d "${TMPDIR:-/tmp}/tilt-sharding.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cp "$root/tests/golden/run-treino-fluxo/dados.csv" "$tmp/dados.csv"

python3 - "$root" "$tmp" <<'PY'
import sys
from pathlib import Path

root = Path(sys.argv[1])
tmp = Path(sys.argv[2])
source = (root / "tests/golden/run-treino-fluxo/input.tilt").read_text()
for shard in (0, 1):
    text = source.replace('"dados.csv"', f'"{(tmp / "dados.csv").as_posix()}"')
    text = text.replace(
        "epocas: 30",
        f"epocas: 2\n  shard_id: {shard}\n  num_shards: 2",
    )
    (tmp / f"shard-{shard}.tilt").write_text(text)
PY

for shard in 0 1; do
  "$bin" executar "$tmp/shard-$shard.tilt" >"$tmp/out-$shard"
  grep -q '^treino M: perda caiu ' "$tmp/out-$shard"
  grep -q '^== pipeline p ==' "$tmp/out-$shard"
done
