#!/usr/bin/env sh
set -eu

bin=$1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cd "$dir"

"$bin" novo app >/dev/null
cat > util.tilt <<'EOF'
funcao dobro x:
  retornar x * 2
EOF
(cd app && "$bin" adicionar util ../util.tilt >/dev/null)
[ -f app/tilt.toml ]
[ -f app/tilt.lock ]
[ -f app/modulos/util.tilt ]
grep -q 'util = "modulos/util.tilt"' app/tilt.toml
grep -Eq 'util = "[0-9a-f]{64}"' app/tilt.lock

cat > app/usar.tilt <<'EOF'
importar util
pipeline principal:
  passos:
    - imprimir util.dobro(21)
EOF
out=$($bin executar app/usar.tilt)
printf '%s\n' "$out" | grep -qx '42'
mkdir app/src
cp app/usar.tilt app/src/usar.tilt
out=$($bin executar app/src/usar.tilt)
printf '%s\n' "$out" | grep -qx '42'

# A copy must still run without the original source path.
cp -R app copia
out=$($bin executar copia/usar.tilt)
printf '%s\n' "$out" | grep -qx '42'

printf '\n# alterado\n' >> copia/modulos/util.tilt
if "$bin" executar copia/usar.tilt >out 2>err; then
  echo 'FALHA: modulo alterado passou pela verificacao' >&2
  exit 1
fi
grep -q 'difere de tilt.lock' err

cat > util2.tilt <<'EOF'
funcao dobro x:
  retornar x * 3
EOF
(cd copia && "$bin" atualizar util ../util2.tilt >/dev/null)
out=$($bin executar copia/usar.tilt)
printf '%s\n' "$out" | grep -qx '63'

if (cd app && "$bin" adicionar util ../util.tilt >out 2>err); then
  echo 'FALHA: modulo duplicado foi aceito' >&2
  exit 1
fi
