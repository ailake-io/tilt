#!/usr/bin/env sh
# Helper script to scaffold a new golden test case.
#
# Usage: sh new_golden.sh <case-name> <cmd-line...>
#
# Example: sh new_golden.sh run-minimo executar exemplos/ola_dados.tilt
#
# Creates tests/golden/<case-name>/ with cmd, input.tilt (optional),
# and runs UPDATE=1 to generate expected files.
#
# To add an input fixture, write it to tests/golden/<case-name>/input.tilt
# before running this script.

set -eu

NAME="$1"
shift
CMD="$*"
DIR="$(cd "$(dirname "$0")/.." && pwd)/tests/golden/$NAME"

if [ -d "$DIR" ]; then
  echo "Case '$NAME' already exists at $DIR"
  exit 1
fi

mkdir -p "$DIR"
printf '%s\n' "$CMD" > "$DIR/cmd"
echo "Created $DIR/cmd with: $CMD"

if [ ! -f "$DIR/input.tilt" ]; then
  echo "# Add your fixture here" > "$DIR/input.tilt"
fi

echo "Run: UPDATE=1 sh tests/run_golden.sh build/bin/tilt tests/golden/$NAME"
echo "     to generate expected files."