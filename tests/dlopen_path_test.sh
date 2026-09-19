#!/usr/bin/env sh
# Valida que bibliotecas opcionais podem ser fornecidas por TILT_DRIVER_PATH.
set -eu

SRC="${0%/*}/../src"
UNIT="${0%/*}/dlopen_path_unit.cpp"
command -v g++ >/dev/null 2>&1 || {
  echo "g++ ausente; pulando o teste dlopen_path"
  exit 0
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

g++ -std=c++20 -Wall -Wextra -Werror -I "$SRC" "$UNIT" "$SRC/runtime/compat.cpp" \
  -o "$tmp/dlopen_path_unit" -ldl || {
  echo "falha ao compilar o driver dlopen_path"
  exit 1
}

lib=""
for candidate in /lib/x86_64-linux-gnu/libm.so.6 /usr/lib/x86_64-linux-gnu/libm.so.6 \
                /usr/lib/libSystem.B.dylib /usr/lib/libm.dylib; do
  if [ -f "$candidate" ]; then lib="$candidate"; break; fi
done
[ -n "$lib" ] || {
  echo "libm do sistema ausente; pulando o teste dlopen_path"
  exit 0
}

name=libtilt_driver_test.so
ln -s "$lib" "$tmp/$name"
TILT_DRIVER_PATH="$tmp" TILT_DLOPEN_TEST_NAME="$name" "$tmp/dlopen_path_unit"
echo "dlopen_path_test ok"
