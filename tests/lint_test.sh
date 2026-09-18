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

# Formata apenas as linhas tocadas. Alguns arquivos antigos ainda não seguem
# o .clang-format inteiro; exigir a reformatacao completa deles bloquearia
# mudancas sem relacao com o diff.
for file in $files; do
  ranges=$(git diff --unified=0 "$BASE" "$HEAD" -- "$file" |
    sed -nE 's/^@@ .* \+([0-9]+)(,([0-9]+))? .*$/\1:\3/p')
  for range in $ranges; do
    start=${range%%:*}
    count=${range#*:}
    if [ -z "$count" ]; then count=1; fi
    if [ "$count" -eq 0 ]; then continue; fi
    end=$((start + count - 1))
    clang-format --dry-run --Werror --lines="$start:$end" "$file"
  done
done
echo "lint: ok ($(printf '%s\n' "$files" | wc -l | tr -d ' ') arquivos, linhas alteradas)"
