# 10 — IA e editores

Tilt foi desenhada para ser lida, escrita e verificada por assistentes de IA:
sintaxe pequena, uma forma óbvia de fazer cada coisa, saída determinística e
interfaces legíveis por máquina.

## `tilt checar --json`

Diagnósticos estruturados para um agente rodar → corrigir → repetir:

```bash
tilt checar prog.tilt --json
```

```json
{ "arquivo": "prog.tilt", "ok": false,
  "erros": [ { "codigo": "T033", "severidade": "erro",
               "linha": 2, "coluna": 6,
               "mensagem": "tipo desconhecido 'X'",
               "notas": ["tipos base: texto, inteiro, ..."] } ] }
```

## `tilt referencia`

Imprime uma referência compacta da linguagem (declarações, expressões,
builtins, CLI, códigos `Tnnn`). Cole no contexto de um LLM antes de pedir código
Tilt.

## builtin `checar_tilt`

De dentro de um programa Tilt (numa `ferramenta` de um `agente`, por exemplo):

```tilt
ferramenta revisar:
  entrada: { caminho: texto }
  executar:
    retornar checar_tilt(caminho)     # { ok, erros: [{codigo, linha, coluna, mensagem, notas}] }
```

Ver [`../exemplos/copiloto.tilt`](../exemplos/copiloto.tilt) — um agente que
revisa código Tilt.

## Autocomplete

### CLI

```bash
tilt completar prog.tilt --linha 12 --coluna 5 --json
```

Candidatos sensíveis a contexto:

| Onde está o cursor | Sugere |
|---|---|
| início de linha na coluna 1 | declarações de topo (`tipo`, `pipeline`, `modelo`, ...) |
| dentro de um bloco (`modelo`, `llm`, `treino`, `servico`, `agente`, `indice`, `fonte`, ...) | os campos daquele bloco + instruções |
| logo após `.` | métodos de tabela / tensor + `executar` / `responder` / `buscar` |
| demais posições | builtins + instruções + nomes declarados no arquivo |

Filtro por prefixo (case-insensitive).

### Servidor LSP

`tilt lsp` fala Language Server Protocol por stdio: diagnósticos ao salvar/editar
e autocomplete (gatilhos `.` e `:`). Configuração de editor em
[`../editors/`](../editors/).

## VS Code

Extensão em [`../editors/vscode/`](../editors/vscode/) — realce de sintaxe +
cliente LSP apontando para `tilt lsp`.

```bash
cd editors/vscode
npm install
npm run package          # gera tilt-<versao>.vsix
code --install-extension tilt-*.vsix
```

Ou abra `editors/vscode/` no VS Code e pressione **F5** (Extension Development
Host). Requer o binário `tilt` no `PATH` (ou ajuste `tilt.path` nas
configurações). Detalhes em [`../editors/vscode/README.md`](../editors/vscode/README.md).

## Neovim

`nvim-lspconfig` apontando `cmd = { "tilt", "lsp" }` para `filetypes = { "tilt" }`.
Snippet completo em [`../editors/README.md`](../editors/README.md).
