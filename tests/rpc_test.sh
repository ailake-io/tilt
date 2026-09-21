#!/usr/bin/env sh
# `tilt rpc` (JSON-lines) e `tilt chamar` de ponta a ponta com
# exemplos/interop/vendas.tilt: banner, chamadas posicionais e nomeadas, tabelas
# em JSON, erros de execucao sem derrubar o servico, saida de `imprimir` no
# campo `saida`, pipeline e encerramento.
set -eu

BIN="$1"
EX="$2"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

cat >"$tmp/req.jsonl" <<'REQ'
{"id":1,"chamar":"classificar","args":[120]}
{"id":2,"chamar":"total_por_regiao","args":[[{"regiao":"sul","valor":30},{"regiao":"norte","valor":120},{"regiao":"sul","valor":5}]]}
{"id":3,"chamar":"so_altos","args":[[{"regiao":"sul","valor":30},{"regiao":"norte","valor":120}]],"nomeados":{"minimo":20}}
{"id":4,"chamar":"saudar","args":["Ana \"a\" \u00e7\u0001"]}
{"id":5,"chamar":"falhar"}
{"id":6,"chamar":"nao_existe"}
{"id":7,"chamar":"classificar","args":[1,2,3]}
{"id":8,"chamar":"classificar","nomeados":{"zzz":1}}
esta linha nao e json
[1,2]
{"id":9,"pipeline":"demo"}
{"id":10,"chamar":"enriquecer","args":[[{"valor":10},{"valor":60},{"valor":100.5}]]}
{"id":11,"ping":true}
{"id":12,"sair":true}
{"id":13,"chamar":"classificar","args":[1]}
REQ

"$BIN" rpc "$EX/vendas.tilt" <"$tmp/req.jsonl" >"$tmp/resp.jsonl"

ck() { # ck <trecho esperado em alguma linha>
  grep -qF -- "$1" "$tmp/resp.jsonl" || { echo "FALHA: faltou $1"; fail=1; }
}
head -1 "$tmp/resp.jsonl" | grep -qF '"protocolo":1' || { echo "FALHA: banner"; fail=1; }
head -1 "$tmp/resp.jsonl" | grep -qF '"nome":"total_por_regiao"' || { echo "FALHA: banner sem funcoes"; fail=1; }
head -1 "$tmp/resp.jsonl" | grep -qF '"pipelines":["demo"]' || { echo "FALHA: banner sem pipelines"; fail=1; }
ck '{"id":1,"ok":true,"resultado":"alto"}'
ck '{"id":2,"ok":true,"resultado":[{"regiao":"sul","total":35,"pedidos":2},{"regiao":"norte","total":120,"pedidos":1}]}'
ck '{"id":3,"ok":true,"resultado":[{"regiao":"sul","valor":30},{"regiao":"norte","valor":120}]}'
ck '"saida":"saudando Ana \"a\" ç\u0001\n"'
ck '{"id":5,"ok":false,"erro":"linha 25: afirmar: afirmacao falhou: sempre falha"'
ck '{"id":6,"ok":false,"erro":"funcao '"'"'nao_existe'"'"' nao existe"}'
ck '{"id":7,"ok":false,"erro":"'"'"'classificar'"'"' aceita 1 argumento(s), recebeu 3"}'
ck "nao tem o parametro 'zzz'"
ck '{"ok":false,"erro":"requisicao invalida:'
ck '"erro":"requisicao invalida: esperado um objeto JSON"'
ck '{"id":9,"ok":true,"saida":"== pipeline demo ==\npipeline demo rodou\n"}'
ck '{"id":10,"ok":true,"resultado":[{"valor":10,"faixa":"baixo"},{"valor":60,"faixa":"medio"},{"valor":100.5,"faixa":"alto"}]}'
ck '{"id":11,"ok":true}'
ck '{"id":12,"ok":true}'
grep -qF '"id":13' "$tmp/resp.jsonl" && { echo "FALHA: respondeu depois de sair"; fail=1; }

# --- tilt chamar
out=$("$BIN" chamar "$EX/vendas.tilt" classificar 60)
[ "$out" = '"medio"' ] || { echo "FALHA: chamar: $out"; fail=1; }
out=$("$BIN" chamar "$EX/vendas.tilt" saudar Bia 2>/dev/null)
[ "$out" = '"ola, Bia"' ] || { echo "FALHA: chamar com texto puro: $out"; fail=1; }
out=$("$BIN" chamar "$EX/vendas.tilt" total_por_regiao '[{"regiao":"x","valor":2}]')
[ "$out" = '[{"regiao":"x","total":2,"pedidos":1}]' ] || { echo "FALHA: chamar tabela: $out"; fail=1; }
"$BIN" chamar "$EX/vendas.tilt" falhar >/dev/null 2>"$tmp/err" && { echo "FALHA: chamar devia sair com erro"; fail=1; }
grep -q "sempre falha" "$tmp/err" || { echo "FALHA: chamar sem mensagem: $(cat "$tmp/err")"; fail=1; }
"$BIN" chamar "$EX/vendas.tilt" >/dev/null 2>&1 && { echo "FALHA: chamar sem funcao"; fail=1; }

# --- programa com erro de checagem nao sobe
printf 'funcao f:\n  retornar nao_definido\n' >"$tmp/ruim.tilt"
"$BIN" rpc "$tmp/ruim.tilt" </dev/null >/dev/null 2>&1 && { echo "FALHA: rpc subiu com erro de checagem"; fail=1; }

[ "$fail" = 0 ] && echo "rpc ok"
exit "$fail"
