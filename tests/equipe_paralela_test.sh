#!/usr/bin/env sh
# `equipe` com `estrategia: paralelo` roda os agentes ao mesmo tempo: tres
# agentes cujas ferramentas dormem 1s cada terminam em ~1s (sequencial seria
# ~3s). Modo mock, sem rede. O teste confere tambem a ordem declarada no texto
# combinado e no rastro.
set -eu

BIN="$1"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/prog.tilt" <<'EOF2'
llm gpt:
  provedor: "anthropic"
  modelo: "claude-sonnet-5"
  chave: env "ANTHROPIC_API_KEY"

ferramenta devagar:
  descricao: "Espera 1s."
  entrada:
    texto: texto
  executar:
    dormir(1)
    retornar "ok"

agente A:
  llm: gpt
  papel: "a"
  ferramentas: [devagar]
  max_passos: 2

agente B:
  llm: gpt
  papel: "b"
  ferramentas: [devagar]
  max_passos: 2

agente C:
  llm: gpt
  papel: "c"
  ferramentas: [devagar]
  max_passos: 2

equipe T:
  agentes:
    - a: A
    - b: B
    - c: C
  estrategia: paralelo
  objetivo: "todos respondem"

pipeline p:
  passos:
    - r = T.responder "oi"
    - imprimir tamanho(r.rastro)
    - para cada item em r.rastro:
        imprimir item.agente
EOF2

inicio=$(date +%s)
out=$(cd "$tmp" && TILT_LLM=mock "$BIN" executar prog.tilt 2>&1) || { echo "falhou: $out"; exit 1; }
fim=$(date +%s)
printf '%s\n' "$out"

fail=0
[ $((fim - inicio)) -le 2 ] || { echo "equipe paralela levou $((fim - inicio))s (esperado ~1s)"; fail=1; }
[ "$(printf '%s\n' "$out" | sed -n '2p')" = "3" ] || { echo "rastro sem 3 entradas"; fail=1; }
[ "$(printf '%s\n' "$out" | sed -n '3,5p' | tr '\n' ',')" = "a,b,c," ] || { echo "ordem do rastro"; fail=1; }
[ "$fail" = 0 ] && echo "equipe_paralela_test ok"
exit "$fail"
