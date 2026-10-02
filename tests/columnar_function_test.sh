#!/usr/bin/env sh
set -eu

bin=$1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cat >"$dir/dados.csv" <<'EOF'
grupo,valor
sul,0
norte,3
leste,
oeste,-1
sul,2
EOF

cat >"$dir/precos.csv" <<'EOF'
grupo,preco
sul,10
norte,20
EOF

cat >"$dir/esquerda_ordenada.csv" <<'EOF'
id,nome
1,a
2,b
2,c
4,d
EOF

cat >"$dir/direita_ordenada.csv" <<'EOF'
id,preco
2,20
2,21
3,30
EOF

cat >"$dir/chaves.csv" <<'EOF'
a,b
2,3
1,9
2,1
1,2
EOF

for mode in falso verdadeiro; do
  cat >"$dir/$mode.tilt" <<EOF
funcao selecionar t:
  retornar t.filtrar linha.valor >= 2

pipeline p:
  passos:
    - t = ler_csv "dados.csv", colunar: $mode
    - a = selecionar(t)
    - derivada = t.derivar { valor_copia: linha.valor }
    - calculada = t.derivar { dobro: linha.valor * 2 }
    - composta = t.derivar { ajuste: linha.valor + linha.valor }
    - imprimir tamanho(a), a
    - imprimir tamanho(derivada), derivada[0].valor_copia
    - imprimir tamanho(calculada), calculada[1].dobro
    - imprimir tamanho(composta), composta[1].ajuste
    - imprimir tamanho(t.filtrar linha.valor >= 0 e linha.valor < 3)
    - imprimir tamanho(t.filtrar linha.grupo == "sul" ou linha.valor < 0)
    - imprimir tamanho(t.filtrar linha.valor == 0)
    - imprimir tamanho(t.filtrar linha.valor != 2)
    - imprimir tamanho(t.filtrar linha.valor < 0)
    - ordenado = t.ordenar_por "valor"
    - imprimir ordenado[0].valor, ordenado[4].valor
    - ordenado_desc = t.ordenar_por "valor", desc: verdadeiro
    - imprimir ordenado_desc[0].valor, ordenado_desc[4].valor
    - ordenado_texto = t.ordenar_por "grupo"
    - imprimir ordenado_texto[0].grupo, ordenado_texto[4].grupo
    - chaves = ler_csv "chaves.csv", colunar: $mode, tipos: { a: "inteiro", b: "inteiro" }
    - chaves_ordenadas = chaves.ordenar_por "a", "b"
    - imprimir chaves_ordenadas[0].a, chaves_ordenadas[0].b, chaves_ordenadas[3].a, chaves_ordenadas[3].b
    - p = ler_csv "precos.csv", colunar: $mode
    - j = t.juntar p, por: "grupo"
    - imprimir tamanho(j), j[0].preco
    - jl = t.juntar p, por: ["grupo"], tipo: "left"
    - imprimir tamanho(jl), jl[2].preco
    - jr = t.juntar p, por: "grupo", tipo: "right"
    - jf = t.juntar p, por: "grupo", tipo: "full"
    - imprimir tamanho(jr), tamanho(jf), jr[0].preco, jr[1].preco
    - esq = ler_csv "esquerda_ordenada.csv", colunar: $mode, tipos: { id: "inteiro" }
    - dir = ler_csv "direita_ordenada.csv", colunar: $mode, tipos: { id: "inteiro" }
    - jm = esq.juntar dir, por: "id"
    - jml = esq.juntar dir, por: "id", tipo: "left"
    - imprimir tamanho(jm), jm[0].preco, jm[1].preco, jm[2].preco
    - imprimir tamanho(jml), jml[0].preco, jml[5].preco
    - estat = t.agrupar_por "grupo", { vari: variancia "valor", distintos: distintos "valor" }
    - imprimir estat[0].vari, estat[0].distintos
    - quant = t.agrupar_por "grupo", { mediana: mediana "valor", q75: quantil "valor", 0.75 }
    - imprimir quant[0].mediana, quant[0].q75
EOF
  (cd "$dir" && "$bin" executar "$mode.tilt") >"$dir/$mode.out"
  grep -q '^1 2$' "$dir/$mode.out"
  grep -q '^4 20 21 20$' "$dir/$mode.out"
  grep -q '^6 nulo nulo$' "$dir/$mode.out"
  grep -q '^1 2$' "$dir/$mode.out"
  grep -q '^1 1.5$' "$dir/$mode.out"
  grep -q '^leste sul$' "$dir/$mode.out"
  grep -q '^1 2 2 3$' "$dir/$mode.out"
done
diff -u "$dir/falso.out" "$dir/verdadeiro.out"
