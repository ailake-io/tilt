#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostics/diagnostic.hpp"
#include "lexer/token.hpp"
#include "parser/ast.hpp"

namespace tilt {

// Recursive-descent parser over a full token stream. Recovers at line
// boundaries so a single bad line does not abort the whole parse.
class Parser {
 public:
  Parser(const std::vector<Token>& tokens, DiagnosticEngine& diag);

  ast::Program parse_program();

 private:
  const Token& cur() const;
  const Token& peek(std::size_t offset) const;
  bool at(TokenKind kind) const;
  bool at_keyword(std::string_view word) const;
  const Token& advance();
  bool accept(TokenKind kind);
  bool expect(TokenKind kind, std::string_view what);
  void skip_newlines();
  void synchronize();  // advance to the start of the next line

  // structure
  ast::ItemPtr parse_top_level();
  ast::ItemPtr parse_decl();
  ast::ItemPtr parse_funcao_decl();
  ast::ItemPtr parse_simple_decl();  // importar / de / seja / constante
  ast::Block parse_block();          // expects INDENT .. DEDENT
  ast::Block parse_body();           // ':' NEWLINE INDENT .. DEDENT
  ast::ItemPtr parse_item();
  ast::ItemPtr parse_line_content(Span span);
  ast::ItemPtr parse_field();

  // statements
  ast::StmtPtr parse_stmt();
  ast::StmtPtr parse_if();
  ast::StmtPtr parse_for_each();
  ast::StmtPtr parse_while();
  ast::StmtPtr parse_try();
  ast::StmtPtr parse_return();
  ast::StmtPtr parse_assign_or_expr_stmt();

  // expressions
  ast::ExprPtr parse_expr();
  ast::ExprPtr parse_union();
  ast::ExprPtr parse_or();
  ast::ExprPtr parse_and();
  ast::ExprPtr parse_equality();
  ast::ExprPtr parse_comparison();
  ast::ExprPtr parse_additive();
  ast::ExprPtr parse_multiplicative();
  ast::ExprPtr parse_unary();
  ast::ExprPtr parse_postfix();
  ast::ExprPtr parse_primary();
  std::vector<ast::Arg> parse_bare_args();
  bool attach_trailing_block(ast::Expr* value);

  // line classification helpers (scan the current logical line)
  struct LineScan {
    bool has_toplevel_colon = false;
    bool has_toplevel_equal = false;
    bool equal_before_colon = false;
    bool starts_with_stmt_keyword = false;
    bool header_before_colon = false;  // only Identifier/Text/number tokens before the ':'
  };
  LineScan scan_line() const;

  void report(DiagCode code, Span span, std::string message,
              std::vector<std::string> notes = {});

  const std::vector<Token>& toks_;
  DiagnosticEngine& diag_;
  std::size_t pos_ = 0;
};

}  // namespace tilt
