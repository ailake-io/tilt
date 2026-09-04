#pragma once

#include <string>
#include <unordered_set>

#include "parser/ast.hpp"
#include "vm/bytecode.hpp"

namespace tilt::vm {

struct NotCompilable {
  std::string reason;
};

// Compiles a `funcao` declaration to bytecode. Throws NotCompilable when the
// body uses anything outside the supported pure subset (literals incl. listas
// e indice, locals, arithmetic/comparison/logic, se/enquanto/para
// cada/retornar, calls to `funcao`s in `known_funcs`, and imprimir/tamanho).
Chunk compile_function(const ast::Item& fn, const std::unordered_set<std::string>& known_funcs);

// Compiles the `passos:` block of a `pipeline` declaration to bytecode.
// Throws NotCompilable for pipelines com `agenda:`/`ao_falhar:` ou passos
// fora do subconjunto (o chamador deve cair de volta para o interpretador).
Chunk compile_pipeline(const ast::Item& pipeline, const std::unordered_set<std::string>& known_funcs);

}  // namespace tilt::vm
