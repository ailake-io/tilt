#!/usr/bin/env sh
set -eu

BIN="$1"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/adocao.tilt" <<'EOF'
llm Mock:
  provedor: local
  contabilidade: "var/llm.jsonl"
  observabilidade: "var/llm.obs.jsonl"
  custo_entrada_mil: 1
  custo_saida_mil: 2
  registrar_prompts: verdadeiro

indice base:
  armazenamento: "memoria"
  llm: Mock

pipeline p:
  passos:
    - imprimir tamanho(fragmentar("Uma frase. Outra frase.", tamanho: 40, modo: "sentenca"))
    - base.inserir([{ id: "a1", texto: "fatura vence hoje" }])
    - m = base.avaliar([{ consulta: "fatura", relevantes: ["a1"] }], top_k: 1)
    - imprimir m
    - perguntar("Mock", usuario: "segredo")
    - imprimir llm_metricas("Mock")
EOF

(cd "$tmp" && TILT_LLM=mock "$BIN" executar adocao.tilt >out1)
(cd "$tmp" && TILT_LLM=mock "$BIN" executar adocao.tilt >out2)
grep -q 'recall' "$tmp/out1"
grep -q 'total:' "$tmp/out2"
test "$(wc -l < "$tmp/var/llm.jsonl")" -ge 6
grep -q 'operacao": "embedding"' "$tmp/var/llm.jsonl"
grep -q 'segredo' "$tmp/var/llm.obs.jsonl"
echo "llm_adoption ok"
