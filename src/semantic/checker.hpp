#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/source.hpp"
#include "diagnostics/diagnostic.hpp"
#include "parser/ast.hpp"
#include "semantic/type.hpp"

namespace tilt {

// First semantic pass: builds the global symbol table and runs the checks
// that do not need full type inference — duplicate declarations, unknown type
// names, malformed tensor annotations, hard-coded secrets, invalid devices,
// agent tool references, statement-level name resolution (T030) and the
// shape solver (T012): cadeia densa/linear nos `modelo`s e propagacao de
// formas literais (conv2d, norma_lote, softmax, reformar, transposta,
// matmul) nos corpos de `funcao`/`pipeline`/`servico`.
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
  void check_bodies();
  void check_model_shapes();

  using Scope = std::unordered_set<std::string>;
  using TensorShape = std::vector<std::int64_t>;
  using ShapeEnv = std::unordered_map<std::string, TensorShape>;

  void scan_for_bodies(const ast::Block& block, Scope scope, ShapeEnv shapes);
  void walk_stmt_block(const ast::Block& block, Scope scope, ShapeEnv shapes);
  void walk_stmt(const ast::Stmt& stmt, Scope& scope, ShapeEnv& shapes);
  void check_expr(const ast::Expr& expr, const Scope& scope);

  // Shape solver: infere a forma de uma expressao tensora a partir de
  // literais, construtores (uns/zeros/aleatorio) e anotacoes
  // conhecidas em `shapes`, validando dimensoes (T012) como conv2d,
  // reformar, transposta e matmul. `std::nullopt` = forma desconhecida
  // (solver nao infere atraves de chamadas de funcao, condicionais etc.).
  std::optional<TensorShape> infer_shape(const ast::Expr& expr, const ShapeEnv& shapes);
  std::optional<TensorShape> check_conv2d(const ast::Expr& call, const ShapeEnv& shapes);
  std::optional<TensorShape> check_reshape(Span span, std::optional<TensorShape> in,
                                           const TensorShape& to);

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
