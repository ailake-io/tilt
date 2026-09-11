# 08 — CLI

`tilt <comando> [argumentos]`. Códigos de saída: `0` ok · `1` diagnósticos/erro
de execução · `2` uso incorreto · `3` recurso não implementado.

## `tilt checar <arquivo> [--json]`

Lexer + parser + análise semântica. Sem `--json`: imprime `ok: ...` ou os
diagnósticos formatados (código `Tnnn`, trecho, `esperado` vs `encontrado`,
sugestão). Com `--json`:

```json
{
  "arquivo": "prog.tilt",
  "ok": false,
  "erros": [
    { "codigo": "T033", "severidade": "erro", "linha": 2, "coluna": 6,
      "mensagem": "tipo desconhecido 'X'", "notas": ["..."] }
  ]
}
```

## `tilt executar <arquivo> [--agendar]`

Roda `checar` e, se limpo, executa: todo `pipeline` de topo na ordem do arquivo;
sem pipeline, `funcao principal`; sem nenhum, roda os `treino` de topo.
Funções puras compilam para bytecode e rodam na VM. `--agendar` valida o
`agenda:` cron e reconhece o modo.

## `tilt servir <arquivo> [--porta N] [--requisicoes N]`

Sobe o primeiro `servico` declarado. `--porta` sobrepõe `porta:`.
`--requisicoes N` atende N e encerra (0 = para sempre). Ver [guia 07](guia-07-http.md).

## `tilt compilar <arquivo> --saida <bin> [--asm] [--arch x86_64|arm64]`

Gera Assembly do subconjunto **inteiro puro** (backends x86-64 e ARM64/AArch64,
ELF) e monta/linka com `$CC` (alvo do host; flags `-O2 -no-pie` no x86-64) +
um runtime C. Alvo cruzado usa `aarch64-linux-gnu-gcc` / `x86_64-linux-gnu-gcc`
ou o env `CC_AARCH64` / `CC_X86_64`. Exige `funcao principal` ou pipelines.
`--asm` mantém o `.s` e o `.rt.c`. Ver [guia 09](guia-09-vm-nativo.md).

## `tilt completar <arquivo> --linha L --coluna C [--json]`

Candidatos de autocomplete para o cursor em `(L, C)` 1-based. Sem `--json`:
`label<TAB>kind<TAB>detalhe` por linha. Ver [guia 10](guia-10-ia-editores.md).

## `tilt lsp`

Servidor Language Server por stdio (JSON-RPC, framing `Content-Length`):
`initialize` (completion com gatilhos `.` e `:`), `textDocument/didOpen` e
`didChange` → `publishDiagnostics`, `textDocument/completion`, `shutdown`/`exit`.

## `tilt referencia`

Referência compacta da linguagem (declarações, expressões, builtins, CLI,
códigos `Tnnn`) — pensada para o contexto de um LLM. A lista de builtins
inclui o HTTP genérico (`http_get_json`/`http_post_json`, JSON sobre `curl`;
ver guia 03).

## `tilt ast <arquivo>` / `tilt tokens <arquivo>`

Despejam a árvore sintática (S-expression) e o fluxo de tokens. Debug.

## `tilt versao` / `tilt ajuda`

## Variáveis de ambiente

| Var | Comandos | Efeito |
|---|---|---|
| `TILT_LLM` | executar, servir | `mock` = offline determinístico; vazio = `curl` real |
| `TILT_GPU` | executar | `off` (padrão) · `auto` · `fake` |
| `TILT_VM_DEBUG` | executar | `1` despeja o bytecode das funções |
| `TILT_STDLIB_PATH` | executar, servir | diretórios com módulos `importar` (sep. `:`), consultados antes de `../share/tilt/stdlib` |
| `CC` | compilar | compilador C do link final |
| `NO_COLOR` | todos | diagnósticos sem cor |
