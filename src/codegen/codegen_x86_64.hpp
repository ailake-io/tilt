#pragma once

#include <string>

#include "parser/ast.hpp"

namespace tilt::codegen {

struct Result {
  bool ok = false;
  std::string asm_text;  // AT&T x86-64
  std::string error;
};

// Emits x86-64 assembly for the integer-only subset: `funcao`s whose bytecode
// uses only integer constants and arithmetic/comparison/logic, plus a
// `funcao principal` used as the entry point. `imprimir` is supported for
// integer arguments. Anything else -> Result{ok=false}.
Result emit_program(const ast::Program& program);

// C source for the tiny link-time runtime (`tilt_print_row`).
const char* runtime_source();

}  // namespace tilt::codegen
