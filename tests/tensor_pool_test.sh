#!/bin/sh
set -eu

root=$1
out=$(mktemp "${TMPDIR:-/tmp}/tilt-tensor-pool.XXXXXX")
trap 'rm -f "$out"' EXIT HUP INT TERM

c++ -std=c++20 -Wall -Wextra -Werror -I "$root/src" \
  "$root/tests/tensor_pool_unit.cpp" "$root/src/runtime/tensor.cpp" \
  -pthread -o "$out"

"$out"
