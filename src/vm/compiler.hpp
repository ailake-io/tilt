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
// body uses anything outside the supported pure subset (literals, locals,
// arithmetic/comparison/logic, se/enquanto/retornar, calls to `funcao`s in
// `known_funcs`, and imprimir/tamanho).
Chunk compile_function(const ast::Item& fn, const std::unordered_set<std::string>& known_funcs);

}  // namespace tilt::vm
