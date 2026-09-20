#pragma once

#include <cstddef>
#include <cstdint>
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
  ast::StmtPtr parse_loop_control();  // `parar` / `continuar`
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
  bool at_lambda() const;  // `funcao a, b: <expr>` em posicao de expressao

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
  // Profundidade de recursao (expressoes e blocos): entrada hostil como 20 mil
  // `[` seguidos estouraria a pilha (segfault) em vez de virar diagnostico.
  int profundidade_ = 0;
  static constexpr int kProfundidadeMax = 200;
  // Alem da contagem de niveis, limita os bytes de pilha usados pelo parser desde
  // parse_program: o MSVC (pilha de 1 MB, frames grandes em Debug) estoura bem antes
  // de 200 niveis, e o tamanho do frame varia entre compiladores.
  static constexpr std::uintptr_t kPilhaMaxBytes = 128 * 1024;
  std::uintptr_t base_pilha_ = 0;
  struct Nivel {
    explicit Nivel(Parser& p) : p_(p), estourou_(++p.profundidade_ > kProfundidadeMax) {
      if (!estourou_ && p.base_pilha_ != 0) {
        char marcador = 0;
        const auto agora = reinterpret_cast<std::uintptr_t>(&marcador);
        const std::uintptr_t usado =
            agora < p.base_pilha_ ? p.base_pilha_ - agora : agora - p.base_pilha_;
        estourou_ = usado > kPilhaMaxBytes;
      }
    }
    ~Nivel() { --p_.profundidade_; }
    Nivel(const Nivel&) = delete;
    Nivel& operator=(const Nivel&) = delete;
    bool estourou() const { return estourou_; }

   private:
    Parser& p_;
    bool estourou_;
  };
  // Reporta (uma vez por arquivo) o aninhamento excessivo e pula o resto da linha.
  ast::ExprPtr recuperar_profundidade();
  bool profundidade_reportada_ = false;
  int map_depth_ = 0;  // inside a `{ ... }` literal: `ident:` is a key, not a named arg
};

}  // namespace tilt
