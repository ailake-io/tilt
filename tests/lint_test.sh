#!/usr/bin/env sh
# Lint incremental: verifica whitespace e clang-format apenas nos C/C++
# alterados entre dois commits. O projeto tem fontes históricos anteriores ao
# formato atual; arquivos tocados passam a obedecer ao .clang-format.
set -eu

BASE="${1:-}"
HEAD="${2:-HEAD}"

if [ -n "$BASE" ]; then
  git diff --check "$BASE" "$HEAD"
  files=$(git diff --name-only "$BASE" "$HEAD" -- '*.c' '*.cc' '*.cpp' '*.h' '*.hh' '*.hpp')
else
  git diff --check
  files=$(git ls-files '*.c' '*.cc' '*.cpp' '*.h' '*.hh' '*.hpp')
fi

if [ -z "$files" ]; then
  echo "lint: nenhum arquivo C/C++ alterado"
  exit 0
fi

command -v clang-format >/dev/null 2>&1 || {
  echo "lint: clang-format ausente"
  exit 1
}

# shellcheck disable=SC2086
clang-format --dry-run --Werror $files
echo "lint: ok ($(printf '%s\n' "$files" | wc -l | tr -d ' ') arquivos)"
