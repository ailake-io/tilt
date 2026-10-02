#!/usr/bin/env sh
# `abandono: p` (dropout) no treino: com p > 0 a rede treinada difere da de p = 0,
# duas execucoes com a mesma semente dao pesos identicos (mascara deterministica
# por semente/epoca/lote), sementes diferentes dao pesos diferentes e p fora de
# [0, 1) e erro claro. Compara so hashes entre si (portavel entre plataformas).
set -eu

BIN="$1"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"

gera() { # gera <arquivo> <p> <semente>
  cat >"$1.tilt" <<EOF2
modelo Rede:
  entrada: tensor[f32, 4]
  camadas:
    - densa: 16
      ativacao: relu
    - abandono: $2
    - densa: 2
    - softmax

treino Rede:
  dados: { x: [[1, 0, 0, 1], [0, 1, 1, 0], [1, 1, 0, 0], [0, 0, 1, 1], [1, 0, 1, 0], [0, 1, 0, 1]], y: [0, 1, 0, 1, 0, 1] }
  perda: entropia_cruzada
  otimizador: adam
  taxa: 0.05
  epocas: 30
  lote: 2
  semente: $3

pipeline p:
  passos:
    - modelo Rede.salvar_pesos "$1.json"
    - imprimir sha256(ler_texto("$1.json"))
EOF2
}
hash_de() { "$BIN" executar "$1.tilt" 2>&1 | tail -1; }

gera zero 0.0 3; gera meio_a 0.5 3; gera meio_b 0.5 3; gera meio_s4 0.5 4
h0=$(hash_de zero); ha=$(hash_de meio_a); hb=$(hash_de meio_b); hs=$(hash_de meio_s4)
fail=0
[ ${#h0} = 64 ] || { echo "FALHA: sem hash (execucao falhou?): $h0"; fail=1; }
[ "$ha" = "$hb" ] || { echo "FALHA: mesma semente deveria dar pesos identicos"; fail=1; }
[ "$ha" != "$h0" ] || { echo "FALHA: abandono 0.5 nao alterou o treino"; fail=1; }
[ "$ha" != "$hs" ] || { echo "FALHA: semente diferente deveria alterar as mascaras"; fail=1; }

gera ruim 1.5 3
out=$("$BIN" executar ruim.tilt 2>&1 || true)
echo "$out" | grep -q "abandono deve estar em \[0, 1)" || { echo "FALHA: p invalido sem erro claro: $out"; fail=1; }

[ "$fail" = 0 ] && echo "abandono_test ok"
exit "$fail"
