#!/usr/bin/env sh
set -eu
BIN="$1"
FIXTURE="${2:-${0%/*}/fixtures/llm_observabilidade.tilt}"
command -v python3 >/dev/null 2>&1 || { echo "python3 ausente; pulando llm_observabilidade"; exit 0; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
out=$(cd "$tmp" && TILT_LLM=mock K=ok "$BIN" executar "$FIXTURE")
echo "$out"
echo "$out" | grep -q '^verdadeiro$' || { echo "contexto não foi compactado"; exit 1; }
[ -s "$tmp/observabilidade.jsonl" ] || { echo "arquivo de observabilidade ausente"; exit 1; }
[ -s "$tmp/ledger.jsonl" ] || { echo "ledger ausente"; exit 1; }
python3 - "$tmp/observabilidade.jsonl" "$tmp/ledger.jsonl" <<'PY'
import json, sys
obs = [json.loads(line) for line in open(sys.argv[1], encoding='utf-8')]
ledger = [json.loads(line) for line in open(sys.argv[2], encoding='utf-8')]
spans = [x for x in obs if x.get('tipo') == 'span']
assert spans, obs
assert all(x.get('trace_id') and x.get('span_id') for x in spans), spans
assert all('start_time_unix_nano' in x and 'end_time_unix_nano' in x for x in spans), spans
assert all(x.get('duracao_us', -1) >= 0 for x in spans), spans
assert any(x.get('parent_span_id') for x in spans), spans
assert any(x.get('agente') == 'Memoria' for x in spans), spans
assert ledger and all('custo' in x and 'trace_id' in x for x in ledger), ledger
print('llm_observabilidade: ok')
PY
