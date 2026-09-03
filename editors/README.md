# Editores

O servidor `tilt lsp` fala Language Server Protocol por stdio: diagnósticos
(`textDocument/publishDiagnostics`) e autocomplete (`textDocument/completion`,
gatilhos `.` e `:`).

## Neovim (nvim-lspconfig)

```lua
vim.filetype.add({ extension = { tilt = "tilt" } })

local configs = require("lspconfig.configs")
if not configs.tilt then
  configs.tilt = {
    default_config = {
      cmd = { "tilt", "lsp" },
      filetypes = { "tilt" },
      root_dir = require("lspconfig.util").root_pattern("CLAUDE.md", ".git"),
    },
  }
end
require("lspconfig").tilt.setup({})
```

## VS Code

Extensão em [`vscode/`](vscode/) — realce de sintaxe + cliente LSP.

```bash
cd editors/vscode
npm install
npm run package                      # tilt-0.1.0.vsix
code --install-extension tilt-0.1.0.vsix
```

Ou abra `editors/vscode/` no VS Code e pressione **F5**. Requer o binário
`tilt` no `PATH` (ou ajuste `tilt.path`). Detalhes em
[`vscode/README.md`](vscode/README.md).

## Sem editor

```bash
tilt completar programa.tilt --linha 12 --coluna 5 --json
tilt checar programa.tilt --json
tilt referencia
```
