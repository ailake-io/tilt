// Interoperabilidade: `tilt rpc` e `tilt chamar` expoem as funcoes e os pipelines
// de um arquivo .tilt a outros processos (Python, PySpark, Kof, shell) por um
// protocolo neutro de linhas JSON. Ver docs/guia-17-interoperabilidade.md.
#pragma once

#include <string_view>
#include <vector>

namespace tilt {

// `tilt rpc <arquivo> [--porta N [--host H]]`: le uma requisicao JSON por linha do stdin e responde
// uma linha JSON por requisicao no stdout (a saida de `imprimir` vai no campo `saida`).
int cmd_rpc(const std::vector<std::string_view>& args);

// `tilt chamar <arquivo> <funcao> [arg-json...]`: chama uma funcao uma vez e
// imprime o resultado como JSON. Cada argumento e JSON (`3`, `"ana"`, `[1,2]`);
// texto que nao e JSON valido vale como texto puro.
int cmd_chamar(const std::vector<std::string_view>& args);

}  // namespace tilt
