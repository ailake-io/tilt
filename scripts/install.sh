#!/usr/bin/env sh
# Builds the `tilt` toolchain from source and installs it under a prefix.
#
#   sh scripts/install.sh                  # -> ~/.local
#   sh scripts/install.sh --prefix=/usr/local   (may need sudo)
#   sh scripts/install.sh --uninstall
set -eu

PREFIX="${PREFIX:-$HOME/.local}"
ACTION=install

for arg in "$@"; do
  case "$arg" in
    --prefix=*) PREFIX="${arg#--prefix=}" ;;
    --uninstall) ACTION=uninstall ;;
    -h | --help)
      echo "uso: $0 [--prefix=DIR] [--uninstall]"
      exit 0
      ;;
    *)
      echo "opcao desconhecida: $arg" >&2
      exit 1
      ;;
  esac
done

SRC=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ "$ACTION" = uninstall ]; then
  rm -f "$PREFIX/bin/tilt"
  rm -rf "$PREFIX/share/tilt"
  echo "tilt removido de $PREFIX"
  exit 0
fi

command -v cmake >/dev/null 2>&1 || {
  echo "erro: cmake nao encontrado (>= 3.20)" >&2
  exit 1
}

BUILD="$SRC/build/install"
cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --build "$BUILD" --parallel >/dev/null
cmake --install "$BUILD" --strip >/dev/null

echo "tilt instalado: $PREFIX/bin/tilt"
case ":$PATH:" in
  *":$PREFIX/bin:"*) ;;
  *) echo "adicione ao PATH:  export PATH=\"$PREFIX/bin:\$PATH\"" ;;
esac
"$PREFIX/bin/tilt" versao
