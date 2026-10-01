#!/usr/bin/env sh
set -eu

bin=$1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/dados.csv" <<'EOF'
grupo,valor,descartada
a,1,x
a,3,x
b,4,x
b,0,x
a,5,x
EOF

cat >"$tmp/fusao.tilt" <<EOF
pipeline fusao:
  passos:
    - t = ler_csv "$tmp/dados.csv", colunar: verdadeiro
    - filtrado = t.filtrar linha.valor >= 2 e linha.valor < 5
    - derivado = filtrado.derivar { dobro: linha.valor * 2 }
    - projetado = derivado.selecionar "grupo", "dobro"
    - grupos = projetado.agrupar_por "grupo", { total: somar "dobro", n: contar }
    - imprimir tamanho(grupos), grupos[0].grupo, grupos[0].total, grupos[0].n, grupos[1].grupo, grupos[1].total, grupos[1].n
    - imprimir tamanho(t.filtrar linha.valor != 0)
EOF

out=$($bin executar "$tmp/fusao.tilt")
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -q '^2 a 6 1 b 8 1$'
printf '%s\n' "$out" | grep -q '^4$'
echo "columnar_fused_plan_test ok"
