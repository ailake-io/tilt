#pragma once

#include <iosfwd>

#include "parser/ast.hpp"

namespace tilt {

// Writes an indented S-expression rendering of the AST. Deterministic; used
// by `tilt ast` and by golden tests.
void dump_program(std::ostream& os, const ast::Program& program);

}  // namespace tilt
