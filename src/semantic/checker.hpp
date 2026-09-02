#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/source.hpp"
#include "diagnostics/diagnostic.hpp"
#include "parser/ast.hpp"
#include "semantic/type.hpp"

namespace tilt {

// First semantic pass: builds the global symbol table and runs the checks
// that do not need full type inference — duplicate declarations, unknown type
// names, malformed tensor annotations, hard-coded secrets, invalid devices,
// and agent tool references. Statement-level name resolution and the tensor
// shape solver arrive in a later milestone.
class SemanticChecker {
 public:
  SemanticChecker(const ast::Program& program, DiagnosticEngine& diag);
  void run();

 private:
  struct Symbol {
    std::string name;
    std::string kind;  // declaring keyword, or "var" / "modulo"
    sema::Type type;
    Span span;
  };

  void collect();
  void resolve_types();
  void audit_blocks();

  void define(const std::string& name, std::string kind, sema::Type type, Span span);
  const Symbol* lookup(std::string_view name) const;

  sema::Type resolve_type_expr(const ast::Expr& expr);
  void resolve_type_annotations(const ast::Item& decl);

  void walk_block(const ast::Block& block, std::string_view entity_kw);
  void visit_item(const ast::Item& item, std::string_view entity_kw);
  void check_device(const ast::Expr& value);
  void check_tool_list(const ast::Item& field);

  void report(DiagCode code, Span span, std::string message,
              std::vector<std::string> notes = {});

  const ast::Program& program_;
  DiagnosticEngine& diag_;
  std::unordered_map<std::string, Symbol> globals_;
};

// Convenience wrapper used by the CLI.
void check_program(const ast::Program& program, DiagnosticEngine& diag);

}  // namespace tilt
