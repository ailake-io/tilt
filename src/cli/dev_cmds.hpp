// Comandos de desenvolvimento da CLI: `tilt testar`, `tilt formatar` e
// `tilt novo`. Ficam fora de cli.cpp para o dispatcher continuar pequeno.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tilt {

// `tilt testar <arquivo|diretorio>... [--filtro TEXTO] [--verboso]`
int cmd_testar(const std::vector<std::string_view>& args);

// `tilt formatar <arquivo|diretorio>... [--verificar] [--stdout]`
int cmd_formatar(const std::vector<std::string_view>& args);

// `tilt repl`: laco interativo (stdin -> stdout) com estado entre linhas.
int cmd_repl(const std::vector<std::string_view>& args);

// `tilt novo <nome>`: cria um projeto com programa, testes e README.
int cmd_novo(const std::vector<std::string_view>& args);

// Normaliza espacos de um fonte .tilt: fim de linha LF, sem espacos no fim das
// linhas (exceto dentro de texto """...""" ), no maximo 2 linhas em branco
// seguidas, sem linhas em branco no inicio e exatamente uma quebra de linha no
// fim. Idempotente; nao altera indentacao nem o conteudo das linhas.
std::string formatar_fonte(const std::string& fonte);

}  // namespace tilt
