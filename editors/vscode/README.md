# Tilt para VS Code

Realce de sintaxe + cliente Language Server (`tilt lsp`): diagnósticos ao
salvar/editar e autocomplete sensível a contexto (gatilhos `.` e `:`).

## Pré-requisito

O binário `tilt` no `PATH`. Confira com `tilt versao`. Se estiver em outro
lugar, ajuste `tilt.path` nas configurações do VS Code.

## Rodar sem empacotar (desenvolvimento)

```bash
cd editors/vscode
npm install
code .
```

No VS Code, pressione **F5** — abre um *Extension Development Host* com a
extensão carregada. Abra qualquer `.tilt`.

## Empacotar e instalar

```bash
cd editors/vscode
npm install
npm run package                       # gera tilt-0.1.0.vsix
code --install-extension tilt-0.1.0.vsix
```

## Configurações

| Chave | Padrão | Efeito |
|---|---|---|
| `tilt.path` | `tilt` | caminho do binário para `tilt lsp` |
| `tilt.lsp.enabled` | `true` | liga/desliga o servidor (só realce se `false`) |

## O que vem de onde

- **Realce**: `syntaxes/tilt.tmLanguage.json` (TextMate) — funciona sem o binário.
- **Diagnósticos + autocomplete**: `tilt lsp`, iniciado por `extension.js` via
  `vscode-languageclient`.

## Publicar no Marketplace (opcional)

```bash
npx @vscode/vsce login ailake-io
npm run package
npx @vscode/vsce publish
```
