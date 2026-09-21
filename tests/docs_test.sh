#!/usr/bin/env sh
# Verifica os exemplos tilt embutidos na documentacao (guias + README +
# CLAUDE): blocos ```tilt run precisam checar+executar (hermetico, com
# TILT_LLM=mock), ```tilt check so checar (servicos externos) e
# ```tilt skip sao fragmentos ignorados. Bloco sem tag falha.
set -eu

BIN="$1"
DIR="${2:-${0%/*}/../}"

exec python3 "${0%/*}/docs_check.py" "$BIN" \
  "$DIR/CLAUDE.md" \
  "$DIR/README.md" \
  "$DIR/docs/guia-01-sintaxe.md" \
  "$DIR/docs/guia-02-tipos.md" \
  "$DIR/docs/guia-03-dados.md" \
  "$DIR/docs/guia-04-ml-dl.md" \
  "$DIR/docs/guia-05-llm-rag.md" \
  "$DIR/docs/guia-06-agentes.md" \
  "$DIR/docs/guia-07-http.md" \
  "$DIR/docs/guia-08-cli.md" \
  "$DIR/docs/guia-09-vm-nativo.md" \
  "$DIR/docs/guia-10-ia-editores.md" \
  "$DIR/docs/guia-11-diagnosticos.md" \
  "$DIR/docs/guia-12-limitacoes.md" \
  "$DIR/docs/guia-13-instalacao.md" \
  "$DIR/docs/guia-14-roteiro.md" \
  "$DIR/docs/guia-15-troubleshooting.md" \
  "$DIR/docs/guia-17-interoperabilidade.md" \
  "$DIR/docs/guia-18-palavras-em-ingles.md"
