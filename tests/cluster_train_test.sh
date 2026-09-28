#!/bin/sh
set -u

bin=$1
root=$2
tmp=$(mktemp -d "${TMPDIR:-/tmp}/tilt-cluster-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

python3 - "$root" "$tmp" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
tmp = pathlib.Path(sys.argv[2])
source = (root / "tests/golden/run-treino-fluxo/input.tilt").read_text()
data = (root / "tests/golden/run-treino-fluxo/dados.csv").resolve()
source = source.replace('"dados.csv"', '"' + str(data) + '"')
source = source.replace("  epocas: 30\n", "  epocas: 2\n")
for rank in (0, 1):
    config = source.replace(
        "  semente: 7\n",
        "  semente: 7\n  cluster: { dir: \"" + str(tmp) + "\", rank: " + str(rank) +
        ", mundo: 2, timeout: 10 }\n",
    )
    (tmp / f"rank{rank}.tilt").write_text(config)
PY

"$bin" executar "$tmp/rank0.tilt" >"$tmp/out0" 2>&1 & p0=$!
"$bin" executar "$tmp/rank1.tilt" >"$tmp/out1" 2>&1 & p1=$!
if wait "$p0"; then c0=0; else c0=$?; fi
if wait "$p1"; then c1=0; else c1=$?; fi

if [ "$c0" -ne 0 ] || [ "$c1" -ne 0 ]; then
  echo "cluster rank0=$c0 rank1=$c1" >&2
  tail -20 "$tmp/out0" >&2 || true
  tail -20 "$tmp/out1" >&2 || true
  exit 1
fi

test -s "$tmp/aggregate-epoch-1.json"
test -s "$tmp/aggregate-epoch-2.json"
grep -q "treino M:" "$tmp/out0"
grep -q "treino M:" "$tmp/out1"

# Parquet com row groups independentes: cada rank do cluster processa apenas
# os grupos que lhe pertencem nas epocas, mantendo os rotulos globais corretos.
if python3 -c 'import pyarrow' >/dev/null 2>&1; then
  python3 - "$root" "$tmp" <<'PY'
import csv
import pathlib
import sys
import pyarrow as pa
import pyarrow.parquet as pq

root = pathlib.Path(sys.argv[1])
tmp = pathlib.Path(sys.argv[2])
source = (root / "tests/golden/run-treino-fluxo/input.tilt").read_text()
with (root / "tests/golden/run-treino-fluxo/dados.csv").open() as f:
    rows = [{k: int(v) for k, v in row.items()} for row in csv.DictReader(f)]
pq.write_table(pa.Table.from_pylist(rows), tmp / "dados.parquet", row_group_size=4,
               use_dictionary=False)
source = source.replace('"dados.csv"', '"' + str(tmp / "dados.parquet") + '"')
source = source.replace("  epocas: 30\n", "  epocas: 2\n")
for world in (2, 4):
    for rank in range(world):
        config = source.replace(
            "  semente: 7\n",
            "  semente: 7\n  cluster: { dir: \"" + str(tmp / f"parquet-cluster{world}") +
            "\", rank: " + str(rank) + f", mundo: {world}, timeout: 10 }}\n",
        )
        (tmp / f"parquet-{world}-rank{rank}.tilt").write_text(config)
PY
  for world in 2 4; do
    pids=""
    for rank in $(seq 0 $((world - 1))); do
      "$bin" executar "$tmp/parquet-$world-rank$rank.tilt" \
        >"$tmp/parquet-$world-out$rank" 2>&1 &
      pids="$pids $!"
    done
    for pid in $pids; do
      if ! wait "$pid"; then
        echo "parquet cluster mundo=$world falhou" >&2
        for rank in $(seq 0 $((world - 1))); do
          tail -20 "$tmp/parquet-$world-out$rank" >&2 || true
        done
        exit 1
      fi
    done
    test -s "$tmp/parquet-cluster$world/aggregate-epoch-2.json"
    for rank in $(seq 0 $((world - 1))); do
      grep -q "treino M:" "$tmp/parquet-$world-out$rank"
    done
  done
fi
