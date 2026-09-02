#!/usr/bin/env sh
# Golden test runner.
#
# Each case is a directory under <golden-dir>/ containing:
#   cmd            required. Arguments passed to `tilt` (single line, space split).
#   input.tilt     optional. A fixture the cmd can reference by relative path;
#                  `tilt` runs with the case directory as its working directory.
#   expected.code  optional. Exact process exit code.
#   expected.out   optional. Exact stdout.
#   expected.err   optional. Exact stderr.
#
# Regenerate expected files with:  UPDATE=1 sh run_golden.sh <bin> <golden-dir>

set -eu

BIN="$1"
DIR="$2"
UPDATE="${UPDATE:-0}"
export NO_COLOR=1

fail=0

for case_dir in "$DIR"/*/; do
  [ -f "$case_dir/cmd" ] || continue
  name=$(basename "$case_dir")
  # shellcheck disable=SC2046
  set -- $(cat "$case_dir/cmd")

  code=0
  ( cd "$case_dir" && "$BIN" "$@" ) >"$case_dir/.actual.out" 2>"$case_dir/.actual.err" || code=$?
  printf '%s' "$code" >"$case_dir/.actual.code"

  if [ "$UPDATE" = "1" ]; then
    cp "$case_dir/.actual.out" "$case_dir/expected.out"
    cp "$case_dir/.actual.err" "$case_dir/expected.err"
    printf '%s' "$code" >"$case_dir/expected.code"
    echo "[$name] atualizado"
    continue
  fi

  ok=1
  if [ -f "$case_dir/expected.code" ]; then
    exp=$(cat "$case_dir/expected.code")
    if [ "$code" != "$exp" ]; then
      echo "[$name] codigo: esperado $exp, obtido $code"
      ok=0
    fi
  fi
  for stream in out err; do
    if [ -f "$case_dir/expected.$stream" ]; then
      if ! diff -u "$case_dir/expected.$stream" "$case_dir/.actual.$stream"; then
        echo "[$name] $stream difere"
        ok=0
      fi
    fi
  done

  if [ "$ok" = 1 ]; then
    echo "[$name] ok"
  else
    fail=1
  fi
done

exit $fail
