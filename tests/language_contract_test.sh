#!/usr/bin/env sh
# Imported functions are checked from their local module signatures and must
# obey the same arity contract in the interpreter and VM paths.
set -eu

bin=$1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

cat > "$dir/lib.tilt" <<'EOF'
funcao soma a, b = 2:
  retornar a + b
EOF

cat > "$dir/ok.tilt" <<'EOF'
importar lib
de lib importar soma como soma_local
pipeline main:
  passos:
    - imprimir lib.soma(3)
    - imprimir lib.soma(3, 4)
    - imprimir lib.soma(a: 3)
    - imprimir lib.soma(b: 4, a: 3)
    - imprimir lib.soma(3, b: 4)
    - imprimir soma_local(5)
EOF
"$bin" checar "$dir/ok.tilt" >/dev/null
out=$("$bin" executar "$dir/ok.tilt")
[ "$out" = "== pipeline main ==
5
7
5
7
7
7" ]
vm_out=$("$bin" executar --vm "$dir/ok.tilt")
[ "$vm_out" = "$out" ]

for case_name in missing extra; do
  if [ "$case_name" = missing ]; then call='lib.soma()'; else call='lib.soma(1, 2, 3)'; fi
  cat > "$dir/$case_name.tilt" <<EOF
importar lib
pipeline main:
  passos:
    - imprimir $call
EOF
  if "$bin" checar "$dir/$case_name.tilt" >"$dir/out" 2>"$dir/err"; then
    echo "FALHA: checker aceitou chamada importada $case_name" >&2
    exit 1
  fi
  grep -q "espera" "$dir/err"
  if "$bin" executar "$dir/$case_name.tilt" >"$dir/out" 2>"$dir/err"; then
    echo "FALHA: $case_name aceito na execucao" >&2
    exit 1
  fi
  grep -q "espera" "$dir/err"
done

# The checker must reject excess positional arguments with either call syntax.
cat > "$dir/local.tilt" <<'EOF'
funcao soma a, b:
  retornar a + b
pipeline main:
  passos:
    - imprimir soma 1, 2, 3
EOF
if "$bin" checar "$dir/local.tilt" >"$dir/out" 2>"$dir/err"; then
  echo "FALHA: checker aceitou argumento posicional excedente" >&2
  exit 1
fi
grep -q "espera 2 argumento(s), encontrou 3" "$dir/err"

for bad_call in 'soma(b: 2)' 'soma(1, a: 2)' 'soma(c: 1, a: 2)' 'soma(a: 1, 2)'; do
  cat > "$dir/local_named.tilt" <<EOF
funcao soma a, b = 2:
  retornar a + b
pipeline main:
  passos:
    - imprimir $bad_call
EOF
  if "$bin" checar "$dir/local_named.tilt" >"$dir/out" 2>"$dir/err"; then
    echo "FALHA: checker aceitou $bad_call" >&2
    exit 1
  fi
done
