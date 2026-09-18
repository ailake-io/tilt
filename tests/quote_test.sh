#!/usr/bin/env sh
# Quoting de shell (compat): tilt_posix_quote e tilt_win_quote sao funcoes
# puras e testaveis em qualquer SO via driver standalone (sem runner Windows).
set -eu

SRC="${0%/*}/../src"
UNIT="${0%/*}/quote_unit.cpp"

command -v g++ >/dev/null 2>&1 || {
  echo "g++ ausente; pulando o teste quote"
  exit 0
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

g++ -std=c++20 -Wall -Wextra -Werror -I "$SRC" "$UNIT" "$SRC/runtime/compat.cpp" \
  -o "$tmp/quote_unit" -ldl || {
  echo "falha ao compilar o driver de quoting"
  exit 1
}

"$tmp/quote_unit" || {
  echo "falha no unit de quoting"
  exit 1
}

echo "quote_test ok"
