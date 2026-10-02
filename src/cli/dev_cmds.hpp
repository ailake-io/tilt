// Comandos de desenvolvimento da CLI: `tilt testar`, `tilt formatar` e
// `tilt novo`. Ficam fora de cli.cpp para o dispatcher continuar pequeno.
#pragma once

#include <optional>
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

// `tilt adicionar <nome> <arquivo.tilt>`: vendoriza um modulo local e fixa seu hash.
int cmd_adicionar(const std::vector<std::string_view>& args);

// Registry local de artefatos de modelo (sem servidor externo).
int cmd_registrar_modelo(const std::vector<std::string_view>& args);
int cmd_listar_modelos(const std::vector<std::string_view>& args);
int cmd_promover_modelo(const std::vector<std::string_view>& args);
int cmd_resolver_modelo(const std::vector<std::string_view>& args);
int cmd_rollback_modelo(const std::vector<std::string_view>& args);
int cmd_linhagem_modelo(const std::vector<std::string_view>& args);

// Resolve um artefato do registry local para comandos que precisam carregar o
// mesmo modelo que `resolver-modelo`, sem duplicar a leitura do manifesto.
std::optional<std::string> resolver_modelo_local(const std::string& nome,
                                                 const std::string& versao,
                                                 const std::string& stage,
                                                 const std::string& registro,
                                                 std::string& erro);

// Normaliza espacos de um fonte .tilt: fim de linha LF, sem espacos no fim das
// linhas (exceto dentro de texto """...""" ), no maximo 2 linhas em branco
// seguidas, sem linhas em branco no inicio e exatamente uma quebra de linha no
// fim. Idempotente; nao altera indentacao nem o conteudo das linhas.
std::string formatar_fonte(const std::string& fonte);

}  // namespace tilt
