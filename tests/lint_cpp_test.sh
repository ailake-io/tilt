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
# int C e includes. O cpplint nao tem filtro nativo por linha, entao captura
# o diagnostico do arquivo inteiro e conserva somente linhas tocadas no diff;
# assim um arquivo legado pode ser alterado sem reabrir todo o passivo.
tmp_cpplint=$(mktemp)
trap 'rm -f "$tmp_cpplint"' EXIT
set +e
cpplint --linelength=120 \
  --filter=-build/c++17,-build/include_what_you_use,-build/include_order,-build/namespaces,-legal/copyright,-readability/braces,-runtime/int,-runtime/references,-whitespace/indent_namespace,-whitespace/line_length,-whitespace/newline \
  $files >"$tmp_cpplint" 2>&1
cpplint_status=$?
set -e
python3 - "$BASE" "$HEAD" "$tmp_cpplint" $files <<'PY'
import re
import subprocess
import sys

base, head, log_path, *files = sys.argv[1:]
ranges = {}
for path in files:
    diff = subprocess.run(
        ["git", "diff", "--unified=0", base, head, "--", path],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    spans = []
    for line in diff.splitlines():
        match = re.match(r"^@@ .* \+(\d+)(?:,(\d+))? .*@$", line)
        if match:
            start = int(match.group(1))
            count = int(match.group(2) or "1")
            if count:
                spans.append((start, start + count - 1))
    ranges[path] = spans

def changed(path, line):
    path = path.removeprefix("./")
    for candidate, spans in ranges.items():
        if path == candidate.removeprefix("./"):
            return any(start <= line <= end for start, end in spans)
    return False

diagnostics = []
with open(log_path, encoding="utf-8", errors="replace") as stream:
    for raw in stream:
        match = re.match(r"^(.+?):(\d+):\s", raw)
        if match and changed(match.group(1), int(match.group(2))):
            diagnostics.append(raw.rstrip())
if diagnostics:
    print("\n".join(diagnostics))
    raise SystemExit(1)
PY
clang-tidy -p "$BUILD_DIR" --quiet $files

echo "lint-cpp: ok ($(printf '%s\n' "$files" | wc -l | tr -d ' ') arquivos)"
