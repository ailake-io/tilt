#!/usr/bin/env sh
# Lint incremental de C/C++: clang-tidy usa o compile_commands do build
# dedicado e cpplint aplica as regras novas sem reformatar o legado inteiro.
set -eu

BASE="${1:-}"
HEAD="${2:-HEAD}"
BUILD_DIR="${3:-build/lint}"
if [ -z "$BASE" ]; then
  echo "lint-cpp: BASE_SHA ausente"
  exit 2
fi

files=$(git diff --diff-filter=ACMR --name-only "$BASE" "$HEAD" --   '*.c' '*.cc' '*.cpp' '*.h' '*.hh' '*.hpp')
if [ -z "$files" ]; then
  echo "lint-cpp: nenhum arquivo C/C++ alterado"
  exit 0
fi

command -v clang-tidy >/dev/null 2>&1 || {
  echo "lint-cpp: clang-tidy ausente" >&2
  exit 1
}
command -v cpplint >/dev/null 2>&1 || {
  echo "lint-cpp: cpplint ausente" >&2
  exit 1
}
[ -f "$BUILD_DIR/compile_commands.json" ] || {
  echo "lint-cpp: compile_commands.json ausente em $BUILD_DIR" >&2
  exit 1
}

# O projeto preserva regras antigas de indentacao, referencias nao-const,
# int C e includes; as demais categorias ficam ativas para arquivos tocados.
cpplint   --linelength=120   --filter=-build/c++17,-build/include_what_you_use,-legal/copyright,-readability/braces,-runtime/int,-runtime/references,-whitespace/indent_namespace,-whitespace/line_length   $files
clang-tidy -p "$BUILD_DIR" --quiet $files

echo "lint-cpp: ok ($(printf '%s\n' "$files" | wc -l | tr -d ' ') arquivos)"
