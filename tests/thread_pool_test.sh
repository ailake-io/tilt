#!/bin/sh
set -eu

root=$1
out=$(mktemp "${TMPDIR:-/tmp}/tilt-thread-pool.XXXXXX")
trap 'rm -f "$out"' EXIT HUP INT TERM
c++ -std=c++20 -Wall -Wextra -Werror -I "$root/src" \
  "$root/tests/thread_pool_unit.cpp" -pthread -o "$out"
"$out"
