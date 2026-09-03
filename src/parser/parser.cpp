#include "parser/parser.hpp"

#include <initializer_list>
#include <string>
#include <utility>

namespace tilt {

using namespace ast;

namespace {

bool word_in(std::string_view w, std::initializer_list<std::string_view> set) {
  for (std::string_view s : set) {
    if (w == s) return true;
  }
  return false;
}

bool is_decl_keyword(std::string_view w) {
  return word_in(w, {"tipo", "fonte", "pipeline", "verificar", "modelo", "treino", "tarefa",
                     "experimento", "llm", "indice", "fluxo", "ferramenta", "agente", "equipe",
                     "servico"});
}

bool is_stmt_keyword(std::string_view w) {
  return word_in(w, {"se", "senao", "para", "enquanto", "tentar", "capturar", "retornar"});
}

bool is_operator_word(std::string_view w) {
  return word_in(w, {"e", "ou", "nao", "contem", "em", "no"});
}

ExprPtr make_expr(ExprKind kind, Span span) {
  auto e = std::make_unique<Expr>();
  e->kind = kind;
  e->span = span;
  return e;
}

}  // namespace

Parser::Parser(const std::vector<Token>& tokens, DiagnosticEngine& diag)
    : toks_(tokens), diag_(diag) {}

const Token& Parser::cur() const { return toks_[pos_ < toks_.size() ? pos_ : toks_.size() - 1]; }

const Token& Parser::peek(std::size_t offset) const {
  std::size_t i = pos_ + offset;
  if (i >= toks_.size()) i = toks_.size() - 1;
  return toks_[i];
}

bool Parser::at(TokenKind kind) const { return cur().kind == kind; }

bool Parser::at_keyword(std::string_view word) const {
  return cur().kind == TokenKind::Identifier && cur().lexeme == word;
}

const Token& Parser::advance() {
  const Token& t = cur();
  if (pos_ + 1 < toks_.size()) ++pos_;
  return t;
}

bool Parser::accept(TokenKind kind) {
  if (!at(kind)) return false;
  advance();
  return true;
}

bool Parser::expect(TokenKind kind, std::string_view what) {
  if (accept(kind)) return true;
  report(DiagCode::ExpectedToken, cur().span,
         "esperado " + std::string(what) + ", encontrado " + token_kind_name(cur().kind));
  return false;
}

void Parser::skip_newlines() {
  while (at(TokenKind::Newline)) advance();
}

void Parser::synchronize() {
  while (!at(TokenKind::Newline) && !at(TokenKind::Dedent) && !at(TokenKind::EndOfFile)) advance();
  accept(TokenKind::Newline);
}

void Parser::report(DiagCode code, Span span, std::string message, std::vector<std::string> notes) {
  Diagnostic d;
  d.severity = Severity::Error;
  d.code = code;
  d.span = span;
  d.message = std::move(message);
  d.notes = std::move(notes);
  diag_.report(std::move(d));
}

// --------------------------------------------------------------------- program

Program Parser::parse_program() {
  Program program;
  skip_newlines();
  while (!at(TokenKind::EndOfFile)) {
    if (at(TokenKind::Newline) || at(TokenKind::Indent) || at(TokenKind::Dedent)) {
      advance();
      continue;
    }
    std::size_t before = pos_;
    ItemPtr item = parse_top_level();
    if (item) program.items.push_back(std::move(item));
    if (pos_ == before) advance();  // guarantee progress
    skip_newlines();
  }
  return program;
}

ItemPtr Parser::parse_top_level() {
  if (at(TokenKind::Identifier)) {
    std::string_view w = cur().lexeme;
    if (w == "funcao") return parse_funcao_decl();
    if (is_decl_keyword(w)) return parse_decl();
    if (word_in(w, {"importar", "de", "seja", "constante"})) return parse_simple_decl();
  }
  report(DiagCode::UnexpectedToken, cur().span,
         "esperado uma declaracao no nivel principal (tipo, pipeline, servico, modelo, ...)");
  synchronize();
  return nullptr;
}

ItemPtr Parser::parse_decl() {
  auto it = std::make_unique<Item>();
  it->kind = ItemKind::Decl;
  it->span = cur().span;
  it->key = std::string(advance().lexeme);  // keyword

  while (!at(TokenKind::Colon) && !at(TokenKind::Newline) && !at(TokenKind::EndOfFile)) {
    std::size_t before = pos_;
    it->header.push_back(parse_primary());
    if (pos_ == before) advance();
  }
  if (expect(TokenKind::Colon, "':' apos o cabecalho da declaracao")) {
    if (at(TokenKind::Newline)) {
      advance();
      if (at(TokenKind::Indent)) it->block = std::make_unique<Block>(parse_block());
    } else {
      synchronize();
    }
  }
  return it;
}

ItemPtr Parser::parse_funcao_decl() {
  auto it = std::make_unique<Item>();
  it->kind = ItemKind::Decl;
  it->span = cur().span;
  it->key = std::string(advance().lexeme);  // 'funcao'
  if (at(TokenKind::Identifier)) {
    auto name = make_expr(ExprKind::Name, cur().span);
    name->text = std::string(advance().lexeme);
    it->header.push_back(std::move(name));
  }
  // Signature: `<param> [: <type>]` repeated, then optional `-> <return type>`,
  // then the trailing ':' that opens the body.
  while (!at(TokenKind::Newline) && !at(TokenKind::EndOfFile)) {
    if (at(TokenKind::Colon) && peek(1).kind == TokenKind::Newline) break;
    if (at(TokenKind::Dash) && peek(1).kind == TokenKind::Greater) {
      advance();
      advance();
      it->value = parse_postfix();  // return type
      continue;
    }
    if (at(TokenKind::Identifier)) {
      Arg p;
      p.name = std::string(advance().lexeme);
      if (accept(TokenKind::Colon)) p.value = parse_postfix();
      it->params.push_back(std::move(p));
      continue;
    }
    advance();  // tolerate a stray token
  }
  accept(TokenKind::Colon);
  if (at(TokenKind::Newline)) {
    advance();
    if (at(TokenKind::Indent)) it->block = std::make_unique<Block>(parse_block());
  }
  return it;
}

ItemPtr Parser::parse_simple_decl() {
  auto it = std::make_unique<Item>();
  it->kind = ItemKind::Decl;
  it->span = cur().span;
  it->key = std::string(advance().lexeme);

  if (it->key == "seja" || it->key == "constante") {
    if (at(TokenKind::Identifier)) {
      auto name = make_expr(ExprKind::Name, cur().span);
      name->text = std::string(advance().lexeme);
      it->header.push_back(std::move(name));
    }
    if (expect(TokenKind::Equal, "'=' apos o nome")) it->value = parse_expr();
  } else {  // importar / de
    while (!at(TokenKind::Newline) && !at(TokenKind::EndOfFile)) {
      if (at(TokenKind::Identifier)) {
        auto name = make_expr(ExprKind::Name, cur().span);
        name->text = std::string(advance().lexeme);
        it->header.push_back(std::move(name));
      } else {
        advance();
      }
    }
  }
  accept(TokenKind::Newline);
  return it;
}

// ---------------------------------------------------------------------- blocks

Block Parser::parse_block() {
  Block block;
  block.span = cur().span;
  if (!expect(TokenKind::Indent, "bloco indentado")) return block;

  while (!at(TokenKind::Dedent) && !at(TokenKind::EndOfFile)) {
    skip_newlines();
    if (at(TokenKind::Dedent) || at(TokenKind::EndOfFile)) break;
    std::size_t before = pos_;
    ItemPtr item = parse_item();
    if (item) block.items.push_back(std::move(item));
    if (pos_ == before) {
      advance();
      synchronize();
    }
  }
  accept(TokenKind::Dedent);
  return block;
}

Block Parser::parse_body() {
  if (at(TokenKind::Newline)) {
    advance();
    if (at(TokenKind::Indent)) return parse_block();
  }
  return Block{};
}

ItemPtr Parser::parse_item() {
  Span span = cur().span;
  bool is_list = accept(TokenKind::Dash);
  ItemPtr content = parse_line_content(span);
  if (!is_list) return content;

  auto it = std::make_unique<Item>();
  it->kind = ItemKind::ListEntry;
  it->span = span;

  // `- chave: valor` followed by a deeper block is a mapping whose first
  // entry sits on the dash line (YAML block-sequence-of-mappings).
  if (at(TokenKind::Indent)) {
    Block rest = parse_block();
    auto folded = std::make_unique<Block>();
    folded->span = span;
    if (content) folded->items.push_back(std::move(content));
    for (auto& x : rest.items) folded->items.push_back(std::move(x));
    it->block = std::move(folded);
  } else {
    it->child = std::move(content);
  }
  return it;
}

ItemPtr Parser::parse_line_content(Span span) {
  if (at(TokenKind::Identifier) && is_stmt_keyword(cur().lexeme) && cur().lexeme != "senao") {
    auto it = std::make_unique<Item>();
    it->kind = ItemKind::Stmt;
    it->span = span;
    it->stmt = parse_stmt();
    return it;
  }

  LineScan ls = scan_line();
  if (ls.has_toplevel_colon && ls.header_before_colon && !ls.equal_before_colon) {
    return parse_field();
  }

  auto it = std::make_unique<Item>();
  it->kind = ItemKind::Stmt;
  it->span = span;
  it->stmt = parse_assign_or_expr_stmt();
  return it;
}

ItemPtr Parser::parse_field() {
  auto it = std::make_unique<Item>();
  it->kind = ItemKind::Field;
  it->span = cur().span;
  it->key = std::string(advance().lexeme);

  while (!at(TokenKind::Colon) && !at(TokenKind::Newline) && !at(TokenKind::EndOfFile)) {
    std::size_t before = pos_;
    it->header.push_back(parse_primary());
    if (pos_ == before) advance();
  }
  expect(TokenKind::Colon, "':' apos a chave");

  if (at(TokenKind::Newline)) {
    it->block = std::make_unique<Block>(parse_body());
  } else {
    it->value = parse_expr();
    if (!attach_trailing_block(it->value.get())) {
      accept(TokenKind::Newline);
      // `chave: tag` followed by a deeper block is a tagged config section
      // (e.g. `otimizador: adam` / `taxa: 0.001`).
      if (at(TokenKind::Indent)) it->block = std::make_unique<Block>(parse_block());
    }
  }
  return it;
}

// ------------------------------------------------------------------ statements

StmtPtr Parser::parse_stmt() {
  std::string_view w = cur().lexeme;
  if (w == "se") return parse_if();
  if (w == "para") return parse_for_each();
  if (w == "enquanto") return parse_while();
  if (w == "tentar") return parse_try();
  if (w == "retornar") return parse_return();
  // 'senao' / 'capturar' reaching here are stray
  report(DiagCode::UnexpectedToken, cur().span, "'" + std::string(w) + "' sem instrucao correspondente");
  synchronize();
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::Expr;
  s->span = cur().span;
  return s;
}

StmtPtr Parser::parse_if() {
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::If;
  s->span = advance().span;  // 'se'
  s->a = parse_expr();
  if (expect(TokenKind::Colon, "':' apos a condicao")) s->body = parse_body();

  while (at_keyword("senao")) {
    advance();
    if (at_keyword("se")) {
      advance();
      ElseIf ei;
      ei.cond = parse_expr();
      expect(TokenKind::Colon, "':' apos a condicao");
      ei.body = parse_body();
      s->elifs.push_back(std::move(ei));
    } else {
      expect(TokenKind::Colon, "':' apos 'senao'");
      s->else_body = std::make_unique<Block>(parse_body());
      break;
    }
  }
  return s;
}

StmtPtr Parser::parse_for_each() {
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::ForEach;
  s->span = advance().span;  // 'para'
  if (at_keyword("cada")) {
    advance();
  } else {
    report(DiagCode::ExpectedToken, cur().span, "esperado 'cada' apos 'para'");
  }
  if (at(TokenKind::Identifier)) s->name = std::string(advance().lexeme);
  if (at_keyword("em")) {
    advance();
  } else {
    report(DiagCode::ExpectedToken, cur().span, "esperado 'em' na iteracao 'para cada'");
  }
  s->a = parse_expr();
  if (expect(TokenKind::Colon, "':' apos o iteravel")) s->body = parse_body();
  return s;
}

StmtPtr Parser::parse_while() {
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::While;
  s->span = advance().span;  // 'enquanto'
  s->a = parse_expr();
  if (expect(TokenKind::Colon, "':' apos a condicao")) s->body = parse_body();
  return s;
}

StmtPtr Parser::parse_try() {
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::Try;
  s->span = advance().span;  // 'tentar'
  if (expect(TokenKind::Colon, "':' apos 'tentar'")) s->body = parse_body();
  if (at_keyword("capturar")) {
    advance();
    if (at(TokenKind::Identifier)) s->name = std::string(advance().lexeme);
    if (expect(TokenKind::Colon, "':' apos o nome do erro"))
      s->catch_body = std::make_unique<Block>(parse_body());
  }
  return s;
}

StmtPtr Parser::parse_return() {
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::Return;
  s->span = advance().span;  // 'retornar'
  if (!at(TokenKind::Newline) && !at(TokenKind::Dedent) && !at(TokenKind::EndOfFile)) {
    s->a = parse_expr();
  }
  accept(TokenKind::Newline);
  return s;
}

StmtPtr Parser::parse_assign_or_expr_stmt() {
  auto s = std::make_unique<Stmt>();
  s->span = cur().span;
  ExprPtr first = parse_expr();

  if (accept(TokenKind::Equal)) {
    s->kind = StmtKind::Assign;
    s->a = std::move(first);
    s->b = parse_expr();
    if (!attach_trailing_block(s->b.get())) accept(TokenKind::Newline);
  } else {
    s->kind = StmtKind::Expr;
    if (!attach_trailing_block(first.get())) accept(TokenKind::Newline);
    s->a = std::move(first);
  }
  return s;
}

// ----------------------------------------------------------------- expressions

ExprPtr Parser::parse_expr() { return parse_union(); }

// Lowest precedence: '|' builds union-of-literals types ("a" | "b" | "c").
ExprPtr Parser::parse_union() {
  ExprPtr lhs = parse_or();
  while (at(TokenKind::Pipe)) {
    auto e = make_expr(ExprKind::Binary, lhs->span);
    e->text = "|";
    advance();
    e->lhs = std::move(lhs);
    e->rhs = parse_or();
    lhs = std::move(e);
  }
  return lhs;
}

ExprPtr Parser::parse_or() {
  ExprPtr lhs = parse_and();
  while (at_keyword("ou")) {
    auto e = make_expr(ExprKind::Binary, lhs->span);
    e->text = std::string(advance().lexeme);
    e->lhs = std::move(lhs);
    e->rhs = parse_and();
    lhs = std::move(e);
  }
  return lhs;
}

ExprPtr Parser::parse_and() {
  ExprPtr lhs = parse_equality();
  while (at_keyword("e")) {
    auto e = make_expr(ExprKind::Binary, lhs->span);
    e->text = std::string(advance().lexeme);
    e->lhs = std::move(lhs);
    e->rhs = parse_equality();
    lhs = std::move(e);
  }
  return lhs;
}

ExprPtr Parser::parse_equality() {
  ExprPtr lhs = parse_comparison();
  while (at(TokenKind::EqualEqual) || at(TokenKind::BangEqual) || at_keyword("contem")) {
    auto e = make_expr(ExprKind::Binary, lhs->span);
    e->text = at(TokenKind::EqualEqual) ? "==" : at(TokenKind::BangEqual) ? "!=" : "contem";
    advance();
    e->lhs = std::move(lhs);
    e->rhs = parse_comparison();
    lhs = std::move(e);
  }
  return lhs;
}

ExprPtr Parser::parse_comparison() {
  ExprPtr lhs = parse_additive();
  while (at(TokenKind::Less) || at(TokenKind::LessEqual) || at(TokenKind::Greater) ||
         at(TokenKind::GreaterEqual)) {
    auto e = make_expr(ExprKind::Binary, lhs->span);
    e->text = at(TokenKind::Less)        ? "<"
              : at(TokenKind::LessEqual) ? "<="
              : at(TokenKind::Greater)   ? ">"
                                         : ">=";
    advance();
    e->lhs = std::move(lhs);
    e->rhs = parse_additive();
    lhs = std::move(e);
  }
  return lhs;
}

ExprPtr Parser::parse_additive() {
  ExprPtr lhs = parse_multiplicative();
  while (at(TokenKind::Plus) || at(TokenKind::Dash)) {
    auto e = make_expr(ExprKind::Binary, lhs->span);
    e->text = at(TokenKind::Plus) ? "+" : "-";
    advance();
    e->lhs = std::move(lhs);
    e->rhs = parse_multiplicative();
    lhs = std::move(e);
  }
  return lhs;
}

ExprPtr Parser::parse_multiplicative() {
  ExprPtr lhs = parse_unary();
  while (at(TokenKind::Star) || at(TokenKind::Slash) || at(TokenKind::Percent)) {
    auto e = make_expr(ExprKind::Binary, lhs->span);
    e->text = at(TokenKind::Star) ? "*" : at(TokenKind::Slash) ? "/" : "%";
    advance();
    e->lhs = std::move(lhs);
    e->rhs = parse_unary();
    lhs = std::move(e);
  }
  return lhs;
}

ExprPtr Parser::parse_unary() {
  if (at_keyword("nao") || at(TokenKind::Dash)) {
    auto e = make_expr(ExprKind::Unary, cur().span);
    e->text = at(TokenKind::Dash) ? "-" : "nao";
    advance();
    e->rhs = parse_unary();
    return e;
  }
  return parse_postfix();
}

ExprPtr Parser::parse_postfix() {
  ExprPtr e = parse_primary();

  while (true) {
    if (at(TokenKind::LParen) &&
        (e->kind == ExprKind::Name || e->kind == ExprKind::Member)) {
      // Parenthesized call: `f(a, b)` / `x.m(a)`. Unambiguous, unlike the
      // paren-less bare-call form.
      advance();
      auto call = make_expr(ExprKind::Call, e->span);
      call->lhs = std::move(e);
      skip_newlines();
      while (!at(TokenKind::RParen) && !at(TokenKind::EndOfFile)) {
        Arg a;
        if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon &&
            peek(2).kind != TokenKind::Newline && peek(2).kind != TokenKind::RParen) {
          a.name = std::string(advance().lexeme);
          advance();  // ':'
        }
        a.value = parse_expr();
        call->args.push_back(std::move(a));
        if (!accept(TokenKind::Comma)) break;
        skip_newlines();
      }
      expect(TokenKind::RParen, "')'");
      e = std::move(call);
      continue;
    }
    if (at(TokenKind::Dot) || at(TokenKind::QuestionDot)) {
      bool optional = at(TokenKind::QuestionDot);
      advance();
      auto m = make_expr(ExprKind::Member, e->span);
      m->optional = optional;
      if (at(TokenKind::Identifier)) {
        m->text = std::string(advance().lexeme);
      } else {
        report(DiagCode::ExpectedToken, cur().span, "esperado um nome apos '.'");
      }
      m->lhs = std::move(e);
      e = std::move(m);
    } else if (at(TokenKind::LBracket)) {
      advance();
      auto acc = make_expr(ExprKind::Index, e->span);
      acc->lhs = std::move(e);
      ExprPtr first = parse_expr();
      if (accept(TokenKind::DotDot)) {
        acc->kind = ExprKind::Slice;
        acc->rhs = std::move(first);
        acc->extra = parse_expr();
      } else {
        acc->elems.push_back(std::move(first));
        while (accept(TokenKind::Comma)) {
          if (at(TokenKind::RBracket)) break;
          acc->elems.push_back(parse_expr());
        }
      }
      expect(TokenKind::RBracket, "']'");
      e = std::move(acc);
    } else {
      break;
    }
  }

  if ((e->kind == ExprKind::Name || e->kind == ExprKind::Member)) {
    // bare-call: a juxtaposed argument on the same logical line
    bool starts_args = !at(TokenKind::Newline) && !at(TokenKind::EndOfFile) &&
                       !at(TokenKind::Colon) && !at(TokenKind::Comma) && !at(TokenKind::Dedent) &&
                       !at(TokenKind::Indent) && !at(TokenKind::RBrace) && !at(TokenKind::RBracket) &&
                       !at(TokenKind::RParen) && !at(TokenKind::Dot) && !at(TokenKind::QuestionDot) &&
                       !at(TokenKind::Equal) && !at(TokenKind::EqualEqual) &&
                       !at(TokenKind::BangEqual) && !at(TokenKind::Less) &&
                       !at(TokenKind::LessEqual) && !at(TokenKind::Greater) &&
                       !at(TokenKind::GreaterEqual) && !at(TokenKind::Plus) &&
                       !at(TokenKind::Dash) && !at(TokenKind::Star) && !at(TokenKind::Slash) &&
                       !at(TokenKind::Percent) && !at(TokenKind::Pipe) && !at(TokenKind::LBracket);
    if (starts_args && at(TokenKind::Identifier) && is_operator_word(cur().lexeme)) starts_args = false;
    if (starts_args) {
      auto call = make_expr(ExprKind::Call, e->span);
      call->lhs = std::move(e);
      call->args = parse_bare_args();
      e = std::move(call);
    }
  }

  if (at_keyword("no") && peek(1).kind == TokenKind::Identifier && peek(1).lexeme == "dispositivo") {
    advance();
    advance();
    auto dev = make_expr(ExprKind::Device, e->span);
    dev->lhs = std::move(e);
    if (at(TokenKind::Identifier)) {
      dev->text = std::string(advance().lexeme);
    } else {
      report(DiagCode::ExpectedToken, cur().span, "esperado o nome do dispositivo");
    }
    e = std::move(dev);
  }

  return e;
}

std::vector<Arg> Parser::parse_bare_args() {
  const bool allow_named = map_depth_ == 0;
  std::vector<Arg> args;
  while (true) {
    Arg a;
    if (allow_named && at(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon &&
        peek(2).kind != TokenKind::Newline) {
      a.name = std::string(advance().lexeme);
      advance();  // ':'
      a.value = parse_expr();
    } else {
      a.value = parse_expr();
    }
    args.push_back(std::move(a));

    if (!at(TokenKind::Comma)) break;
    // Inside a `{ ... }` literal a following `ident:` is the next map key: the
    // comma belongs to the map, not to this argument list.
    if (map_depth_ > 0 && peek(1).kind == TokenKind::Identifier &&
        peek(2).kind == TokenKind::Colon) {
      break;
    }
    advance();  // consume the comma
    if (at(TokenKind::Newline) || at(TokenKind::Colon) || at(TokenKind::EndOfFile) ||
        at(TokenKind::Dedent) || at(TokenKind::RBrace) || at(TokenKind::RBracket)) {
      break;
    }
  }
  return args;
}

bool Parser::attach_trailing_block(Expr* value) {
  if (!at(TokenKind::Colon)) return false;
  if (peek(1).kind != TokenKind::Newline) return false;
  advance();  // ':'
  Block body = parse_body();

  Expr* target = value;
  if (target && target->kind == ExprKind::Assign) target = target->rhs.get();
  if (target && target->kind == ExprKind::Device) target = target->lhs.get();
  if (target && target->kind == ExprKind::Call) {
    target->block = std::make_unique<Block>(std::move(body));
  } else {
    report(DiagCode::UnexpectedToken, value ? value->span : cur().span,
           "bloco ':' so e permitido apos uma chamada");
  }
  return true;
}

ExprPtr Parser::parse_primary() {
  Span span = cur().span;

  if (at(TokenKind::Integer)) {
    auto e = make_expr(ExprKind::IntLit, span);
    e->text = std::string(advance().lexeme);
    return e;
  }
  if (at(TokenKind::Decimal)) {
    auto e = make_expr(ExprKind::DecimalLit, span);
    e->text = std::string(advance().lexeme);
    return e;
  }
  if (at(TokenKind::Text)) {
    auto e = make_expr(ExprKind::TextLit, span);
    e->text = std::string(advance().lexeme);
    return e;
  }
  if (at(TokenKind::Identifier)) {
    std::string_view w = cur().lexeme;
    if (w == "verdadeiro" || w == "falso") {
      auto e = make_expr(ExprKind::BoolLit, span);
      e->boolean = (w == "verdadeiro");
      advance();
      return e;
    }
    if (w == "nulo") {
      advance();
      return make_expr(ExprKind::NullLit, span);
    }
    auto e = make_expr(ExprKind::Name, span);
    e->text = std::string(advance().lexeme);
    return e;
  }
  if (accept(TokenKind::LParen)) {
    ExprPtr inner = parse_expr();
    expect(TokenKind::RParen, "')'");
    return inner;
  }
  if (accept(TokenKind::LBracket)) {
    auto e = make_expr(ExprKind::ListLit, span);
    skip_newlines();
    while (!at(TokenKind::RBracket) && !at(TokenKind::EndOfFile)) {
      e->elems.push_back(parse_expr());
      if (!accept(TokenKind::Comma)) break;
      skip_newlines();
    }
    expect(TokenKind::RBracket, "']'");
    return e;
  }
  if (accept(TokenKind::LBrace)) {
    auto e = make_expr(ExprKind::MapLit, span);
    ++map_depth_;
    skip_newlines();
    while (!at(TokenKind::RBrace) && !at(TokenKind::EndOfFile)) {
      MapEntry entry;
      if (at(TokenKind::Identifier) || at(TokenKind::Text)) {
        entry.key = std::string(advance().lexeme);
      } else {
        report(DiagCode::ExpectedToken, cur().span, "esperado uma chave no mapa");
      }
      expect(TokenKind::Colon, "':' apos a chave do mapa");
      entry.value = parse_expr();
      e->entries.push_back(std::move(entry));
      if (!accept(TokenKind::Comma)) break;
      skip_newlines();
    }
    expect(TokenKind::RBrace, "'}'");
    --map_depth_;
    return e;
  }

  report(DiagCode::UnexpectedToken, span,
         std::string("esperado uma expressao, encontrado ") + token_kind_name(cur().kind));
  if (!at(TokenKind::Newline) && !at(TokenKind::Dedent) && !at(TokenKind::EndOfFile)) advance();
  return make_expr(ExprKind::NullLit, span);
}

Parser::LineScan Parser::scan_line() const {
  LineScan r;
  int depth = 0;
  for (std::size_t i = pos_; i < toks_.size(); ++i) {
    TokenKind k = toks_[i].kind;
    if (k == TokenKind::Newline || k == TokenKind::EndOfFile || k == TokenKind::Indent ||
        k == TokenKind::Dedent) {
      break;
    }
    if (k == TokenKind::LParen || k == TokenKind::LBracket || k == TokenKind::LBrace) {
      ++depth;
    } else if (k == TokenKind::RParen || k == TokenKind::RBracket || k == TokenKind::RBrace) {
      --depth;
    } else if (depth == 0 && k == TokenKind::Colon && !r.has_toplevel_colon) {
      r.has_toplevel_colon = true;
      bool header_ok = (i > pos_);
      for (std::size_t j = pos_; j < i && header_ok; ++j) {
        TokenKind kk = toks_[j].kind;
        if (kk != TokenKind::Identifier && kk != TokenKind::Text && kk != TokenKind::Integer &&
            kk != TokenKind::Decimal) {
          header_ok = false;
        }
      }
      if (header_ok && toks_[pos_].kind != TokenKind::Identifier) header_ok = false;
      r.header_before_colon = header_ok;
    } else if (depth == 0 && k == TokenKind::Equal && !r.has_toplevel_equal) {
      r.has_toplevel_equal = true;
      r.equal_before_colon = !r.has_toplevel_colon;
    }
  }
  if (at(TokenKind::Identifier)) r.starts_with_stmt_keyword = is_stmt_keyword(cur().lexeme);
  return r;
}

}  // namespace tilt
