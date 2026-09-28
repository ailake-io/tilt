#!/usr/bin/env sh
set -eu

bin="$1"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/features.tilt" <<'EOF'
modelo Q:
  entrada: tensor[f32, 32]
  camadas:
    - linear: [32, 32]
    - linear: [32, 32]
    - softmax

modelo E:
  entrada: tensor[i64, 3]
  camadas:
    - incorporacao: [5, 2]
    - achatar
    - linear: [6, 2]
    - softmax

busca B:
  modelo: Q
  dados: { x: [[0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0], [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]], y: [0, 1] }
  perda: entropia_cruzada
  epocas: 1
  lote: 2
  estrategia: bayesiana
  tentativas: 4
  semente: 2
  grade:
    taxa: [0.1, 0.01, 0.001]
    otimizador: [sgd, adam]

pipeline p:
  passos:
    - modelo Q.exportar_gguf "q.gguf", quantizacao: "q8_0"
    - modelo Q.carregar_pesos "q.gguf"
    - modelo E.exportar_onnx "e.onnx"
    - imprimir "ok"
EOF

(cd "$tmp" && "$bin" executar features.tilt >out)
grep -q 'bayesiana' "$tmp/out"
grep -q 'pesos GGUF carregados' "$tmp/out"
grep -q 'onnx exportado' "$tmp/out"
test -s "$tmp/q.gguf"
test -s "$tmp/e.onnx"
grep -a -q 'Gather' "$tmp/e.onnx"
echo "ml_features ok"
