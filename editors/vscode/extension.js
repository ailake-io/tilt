"use strict";

const { workspace, window } = require("vscode");
const { LanguageClient, TransportKind } = require("vscode-languageclient/node");

/** @type {import("vscode-languageclient/node").LanguageClient | undefined} */
let client;

function activate() {
  const cfg = workspace.getConfiguration("tilt");
  if (!cfg.get("lsp.enabled", true)) return;

  const command = cfg.get("path", "tilt");
  const serverOptions = {
    run: { command, args: ["lsp"], transport: TransportKind.stdio },
    debug: { command, args: ["lsp"], transport: TransportKind.stdio },
  };
  const clientOptions = {
    documentSelector: [{ scheme: "file", language: "tilt" }],
    synchronize: { fileEvents: workspace.createFileSystemWatcher("**/*.tilt") },
  };

  client = new LanguageClient("tilt", "Tilt Language Server", serverOptions, clientOptions);
  client.start().catch((err) => {
    window.showWarningMessage(
      `Tilt: nao foi possivel iniciar 'tilt lsp' (${err.message}). ` +
        `Verifique se o binario esta no PATH ou ajuste 'tilt.path' nas configuracoes.`,
    );
  });
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
