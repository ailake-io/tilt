#!/bin/sh
set -eu

root=$1
out=$(mktemp "${TMPDIR:-/tmp}/tilt-tensor-benchmark.XXXXXX")
trap 'rm -f "$out"' EXIT HUP INT TERM
python3 "$root/scripts/benchmark_tensor_ops.py" \
  --backend tilt --quick --iterations 1 --warmup 0 >"$out"
grep -q '^tilt matmul ' "$out"
grep -q '^tilt conv2d ' "$out"
