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

Sem extensão publicada ainda. Use um cliente LSP genérico apontando
`tilt lsp` para arquivos `*.tilt`, ou chame `tilt completar <arquivo>
--linha L --coluna C --json` a partir de um script.

## Sem editor

```bash
tilt completar programa.tilt --linha 12 --coluna 5 --json
tilt checar programa.tilt --json
tilt referencia
```
