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

## `tilt executar <arquivo> [--agendar|--vm|--jit]`

Roda `checar` e, se limpo, executa: todo `pipeline` de topo na ordem do arquivo;
sem pipeline, `funcao principal`; sem nenhum, roda os `treino` de topo.
Funções puras compilam para bytecode e rodam na VM. `--vm` força pipelines compiláveis pela VM. `--jit` emite código nativo em runtime para o subconjunto inteiro; pipelines fora dele caem para a VM automaticamente. `--agendar` valida o `agenda:` cron e reconhece o modo.

## `tilt testar [caminho...] [--filtro TEXTO] [--verboso]`

Roda os blocos `teste` de arquivos `.tilt` (um arquivo, ou uma pasta —
recursivo, ignorando pastas ocultas; sem argumento, a pasta atual). Cada teste é um
bloco com `passos:`; use `afirmar cond [, "mensagem"]` e `afirmar_igual(obtido,
esperado)`. Um teste que falha (afirmação ou erro de execução) mostra a linha e a
saída (`imprimir`) que ele produziu, e não interrompe os outros. Saída `0` se
tudo passou, `1` se algum falhou; `--filtro` roda só os testes cujo nome contém o
texto.

```tilt check
funcao dobro x:
  retornar x * 2

teste dobro_de_dois:
  passos:
    - afirmar_igual(dobro(2), 4)
    - afirmar dobro(0) == 0, "zero fica zero"
```

`teste` funciona em qualquer arquivo (inclusive ao lado do código, importando
módulos com `importar`), e `tilt executar` o ignora.

## `tilt formatar <caminho...> [--verificar] [--stdout]`

Normaliza espaços: fim de linha LF, sem espaços no fim das linhas (o conteúdo de
textos `"""..."""` é preservado), no máximo 2 linhas em branco seguidas, sem
linhas em branco no início e uma única quebra de linha no fim. Não altera a
indentação nem o conteúdo das linhas; é idempotente. `--verificar` não grava e sai
com `1` se algum arquivo precisaria mudar (útil no CI); `--stdout` imprime o
resultado.

## `tilt repl`

Laço interativo (stdin → stdout, também aceita entrada por pipe) com estado entre
linhas: variáveis, funções, lambdas e entidades (`llm`, `tipo`, ...) persistem.
Uma expressão solta imprime o valor (`x * 2` → `6`); um erro de execução mostra a
mensagem e a sessão continua. Uma linha terminada em `:` abre um bloco que segue
enquanto as próximas linhas forem indentadas (ou `senao`/`capturar`); linha vazia
fecha. Comandos: `:carregar <arquivo>` registra as declarações de um `.tilt`,
`:ajuda`, `:sair`. Sem o verificador semântico (cada linha é analisada sozinha).

## `tilt novo <nome>`

Cria a pasta `<nome>/` com `principal.tilt` (uma `funcao` e um `pipeline`),
`testes.tilt` (exemplo de `teste`), `README.md` e `.gitignore`.

## `tilt servir <arquivo> [--porta N] [--requisicoes N]`

Sobe o primeiro `servico` declarado. `--porta` sobrepõe `porta:`.
`--requisicoes N` atende N e encerra (0 = para sempre). Ver [guia 07](guia-07-http.md).

## `tilt servir-catalogo <diretorio-raiz> [--porta N] [--prefixo P]`

Sobe um **catálogo Iceberg REST Open API read-only** (porta default 8191,
prefixo default `/v1`) expondo as tabelas Iceberg locais — subdiretórios de
`<diretorio-raiz>` que contêm `metadata/` — no namespace `default`. Engines
como Spark SQL configuram `SparkCatalog` tipo `rest` com a URI do servidor e
lêem pelo nome (`spark.read.table("catalogo.default.tabela")`). Lista as
tabelas servidas ao subir. O loadTable devolve o metadata mais recente com as
locations reescritas para URLs deste servidor; `/v1/files/<rel>` serve os
arquivos (metadata.json, manifests, data files) validando que o path fica
dentro do root (traversal → 403). Escrita (createTable/commit) → 501.
`--sem-reecrita-manifests` mantém `file://` nas manifest-lists — necessário
para o Spark/Hadoop (o `fs.http` reporta length -1 e o leitor Avro do Iceberg
rejeita); nesse modo o leitor precisa acessar os arquivos locais.
Ver [guia 03](guia-03-dados.md#tilt-servir-catalogo-catálogo-rest-server-fase-30).

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
`initialize`, `textDocument/didOpen` e `didChange` → `publishDiagnostics`,
`textDocument/diagnostic` (pull com cache por conteúdo),
`completion` (gatilhos `.` e `:`), `hover` (docs + tipos do checker),
`definition`, `references` e `rename` (same-file), `signatureHelp`, `formatting`,
`shutdown`/`exit`. `references` aceita `context.includeDeclaration` conforme
o LSP e devolve locations no mesmo documento; `rename` devolve um
`WorkspaceEdit` para todos os usos e a declaração.

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
| `TILT_JIT_DEBUG` | executar | `1` informa JIT nativo ou fallback por pipeline |
| `TILT_STDLIB_PATH` | executar, servir | diretórios com módulos `importar` (sep. `:`), consultados antes de `../share/tilt/stdlib` |
| `TILT_JANELA_ESTADO` | executar | `memoria` desliga o offset persistente do streaming `janela:` |
| `TILT_TLS_SKIP_VERIFY` | executar, servir | `1` desliga verificação de certificado TLS nos clientes (testes com cert auto-assinado) |
| `CC` | compilar | compilador C do link final |
| `NO_COLOR` | todos | diagnósticos sem cor |
