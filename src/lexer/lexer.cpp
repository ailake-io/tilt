#include "lexer/lexer.hpp"

#include <string>
#include <utility>

namespace tilt {
namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_ident_start(char c) {
  return is_alpha(c) || c == '_' || static_cast<unsigned char>(c) >= 0x80;
}
bool is_ident_continue(char c) { return is_ident_start(c) || is_digit(c); }

}  // namespace

Lexer::Lexer(const SourceFile& source, DiagnosticEngine& diag)
    : src_(source.text()), diag_(diag) {
  indent_stack_.push_back(0);
}

char Lexer::peek(std::size_t offset) const {
  const std::size_t i = pos_ + offset;
  return i < src_.size() ? src_[i] : '\0';
}

char Lexer::advance() {
  const char c = src_[pos_++];
  if (c == '\n') {
    ++line_;
    col_ = 1;
  } else {
    ++col_;
  }
  return c;
}

bool Lexer::match(char expected) {
  if (peek() != expected) return false;
  advance();
  return true;
}

void Lexer::push(TokenKind kind, std::string_view lexeme, Span span) {
  out_.push_back(Token{kind, lexeme, span});
}

void Lexer::push_structural(TokenKind kind) {
  out_.push_back(Token{kind, std::string_view{},
                       Span{static_cast<std::uint32_t>(pos_), 0, line_, col_}});
}

Span Lexer::span_from(std::size_t start, std::uint32_t line, std::uint32_t col) const {
  return Span{static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(pos_ - start), line, col};
}

void Lexer::report(DiagCode code, Span span, std::string message, std::vector<std::string> notes,
                   std::optional<std::string> suggestion) {
  Diagnostic d;
  d.severity = Severity::Error;
  d.code = code;
  d.span = span;
  d.message = std::move(message);
  d.notes = std::move(notes);
  d.suggestion = std::move(suggestion);
  diag_.report(std::move(d));
}

std::vector<Token> Lexer::tokenize() {
  while (pos_ < src_.size()) {
    if (at_line_start_) {
      handle_line_start();
      continue;
    }
    // Inline whitespace and trailing comments do not produce tokens.
    while (true) {
      const char c = peek();
      if (c == ' ' || c == '\t' || c == '\r') {
        advance();
      } else if (c == '#') {
        while (peek() != '\n' && peek() != '\0') advance();
      } else {
        break;
      }
    }

    const char c = peek();
    if (c == '\0') break;
    if (c == '\n') {
      if (bracket_depth_ == 0 && !out_.empty() && out_.back().kind != TokenKind::Newline) {
        push_structural(TokenKind::Newline);
      }
      advance();
      at_line_start_ = true;
      continue;
    }
    lex_token();
  }
  finish();
  return std::move(out_);
}

void Lexer::handle_line_start() {
  const std::size_t ws_start = pos_;
  const std::uint32_t ws_line = line_;
  const std::uint32_t ws_col = col_;
  bool tab_seen = false;
  std::uint32_t width = 0;

  while (true) {
    const char c = peek();
    if (c == ' ') {
      advance();
      ++width;
    } else if (c == '\t') {
      tab_seen = true;
      advance();
    } else {
      break;
    }
  }

  const char c = peek();

  // Blank or comment-only lines are skipped without emitting NEWLINE/INDENT/DEDENT.
  if (c == '\0') {
    pos_ = src_.size();
    return;
  }
  if (c == '\n') {
    advance();
    return;
  }
  if (c == '#') {
    while (peek() != '\n' && peek() != '\0') advance();
    if (peek() == '\n') advance();
    return;
  }

  // Inside ( [ { the indentation of a continuation line is not significant.
  if (bracket_depth_ > 0) {
    at_line_start_ = false;
    return;
  }

  if (tab_seen) {
    report(DiagCode::TabInIndent,
           Span{static_cast<std::uint32_t>(ws_start), static_cast<std::uint32_t>(pos_ - ws_start),
                ws_line, ws_col},
           "tab encontrado na indentacao", {}, std::string("troque tabs por 2 espacos"));
  }
  if (width % 2 != 0) {
    report(DiagCode::IndentNotMultipleOfTwo,
           Span{static_cast<std::uint32_t>(ws_start), width, ws_line, ws_col},
           "indentacao de " + std::to_string(width) + " espacos nao e multiplo de 2", {},
           std::string("use blocos de 2 espacos"));
  }

  const int level = static_cast<int>(width);
  const int top = indent_stack_.back();
  if (level > top) {
    indent_stack_.push_back(level);
    push_structural(TokenKind::Indent);
  } else if (level < top) {
    while (indent_stack_.size() > 1 && indent_stack_.back() > level) {
      indent_stack_.pop_back();
      push_structural(TokenKind::Dedent);
    }
    if (indent_stack_.back() != level) {
      report(DiagCode::UnexpectedIndent,
             Span{static_cast<std::uint32_t>(ws_start), width, ws_line, ws_col},
             "indentacao inesperada", {"nenhum bloco aberto neste nivel de indentacao"},
             std::nullopt);
      indent_stack_.push_back(level);
      push_structural(TokenKind::Indent);
    }
  }

  at_line_start_ = false;
}

void Lexer::lex_token() {
  const std::size_t start = pos_;
  const std::uint32_t sl = line_;
  const std::uint32_t sc = col_;
  const auto tok = [&](TokenKind k) {
    push(k, src_.substr(start, pos_ - start), span_from(start, sl, sc));
  };

  const char c = advance();
  switch (c) {
    case '(':
    case '[':
    case '{':
      ++bracket_depth_;
      break;
    case ')':
    case ']':
    case '}':
      if (bracket_depth_ > 0) --bracket_depth_;
      break;
    default:
      break;
  }
  switch (c) {
    case ':':
      tok(TokenKind::Colon);
      return;
    case '-':
      tok(TokenKind::Dash);
      return;
    case ',':
      tok(TokenKind::Comma);
      return;
    case '|':
      tok(TokenKind::Pipe);
      return;
    case '{':
      tok(TokenKind::LBrace);
      return;
    case '}':
      tok(TokenKind::RBrace);
      return;
    case '[':
      tok(TokenKind::LBracket);
      return;
    case ']':
      tok(TokenKind::RBracket);
      return;
    case '(':
      tok(TokenKind::LParen);
      return;
    case ')':
      tok(TokenKind::RParen);
      return;
    case '+':
      tok(TokenKind::Plus);
      return;
    case '*':
      tok(TokenKind::Star);
      return;
    case '/':
      tok(TokenKind::Slash);
      return;
    case '%':
      tok(TokenKind::Percent);
      return;
    case '.':
      tok(match('.') ? TokenKind::DotDot : TokenKind::Dot);
      return;
    case '=':
      tok(match('=') ? TokenKind::EqualEqual : TokenKind::Equal);
      return;
    case '<':
      tok(match('=') ? TokenKind::LessEqual : TokenKind::Less);
      return;
    case '>':
      tok(match('=') ? TokenKind::GreaterEqual : TokenKind::Greater);
      return;
    case '!':
      if (match('=')) {
        tok(TokenKind::BangEqual);
        return;
      }
      report(DiagCode::InvalidCharacter, span_from(start, sl, sc), "caractere invalido '!'", {},
             std::string("use 'nao' para negacao ou '!=' para diferente"));
      tok(TokenKind::Invalid);
      return;
    case '?':
      if (match('.')) {
        tok(TokenKind::QuestionDot);
        return;
      }
      report(DiagCode::InvalidCharacter, span_from(start, sl, sc), "caractere invalido '?'", {},
             std::string("use '?.' para acesso opcional"));
      tok(TokenKind::Invalid);
      return;
    case '"':
      lex_text(start, sl, sc);
      return;
    default:
      break;
  }

  if (is_digit(c)) {
    lex_number(start, sl, sc);
    return;
  }
  if (is_ident_start(c)) {
    while (is_ident_continue(peek())) advance();
    tok(TokenKind::Identifier);
    return;
  }

  std::string message = "caractere invalido";
  if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7f) {
    message += " '";
    message += c;
    message += "'";
  }
  report(DiagCode::InvalidCharacter, span_from(start, sl, sc), std::move(message));
  tok(TokenKind::Invalid);
}

void Lexer::lex_number(std::size_t start, std::uint32_t sl, std::uint32_t sc) {
  while (is_digit(peek())) advance();

  bool is_decimal = false;
  if (peek() == '.' && is_digit(peek(1))) {
    is_decimal = true;
    advance();  // consume '.'
    while (is_digit(peek())) advance();
  }
  if (peek() == 'e' || peek() == 'E') {
    const char n1 = peek(1);
    const char n2 = peek(2);
    if (is_digit(n1) || ((n1 == '+' || n1 == '-') && is_digit(n2))) {
      is_decimal = true;
      advance();  // consume 'e'
      if (peek() == '+' || peek() == '-') advance();
      while (is_digit(peek())) advance();
    }
  }

  push(is_decimal ? TokenKind::Decimal : TokenKind::Integer, src_.substr(start, pos_ - start),
       span_from(start, sl, sc));
}

void Lexer::lex_text(std::size_t start, std::uint32_t sl, std::uint32_t sc) {
  // The opening '"' is already consumed.
  const bool triple = (peek() == '"' && peek(1) == '"');
  if (triple) {
    advance();
    advance();
    const std::size_t content_start = pos_;
    while (true) {
      if (peek() == '\0') {
        report(DiagCode::UnterminatedString, span_from(start, sl, sc),
               "texto de tres aspas nao terminado", {}, std::string("feche com \"\"\""));
        push(TokenKind::Text, src_.substr(content_start, pos_ - content_start),
             span_from(start, sl, sc));
        return;
      }
      if (peek() == '"' && peek(1) == '"' && peek(2) == '"') {
        const std::string_view content = src_.substr(content_start, pos_ - content_start);
        advance();
        advance();
        advance();
        push(TokenKind::Text, content, span_from(start, sl, sc));
        return;
      }
      advance();
    }
  }

  const std::size_t content_start = pos_;
  while (true) {
    const char c = peek();
    if (c == '"') {
      const std::string_view content = src_.substr(content_start, pos_ - content_start);
      advance();
      push(TokenKind::Text, content, span_from(start, sl, sc));
      return;
    }
    if (c == '\0' || c == '\n') {
      report(DiagCode::UnterminatedString, span_from(start, sl, sc), "texto nao terminado", {},
             std::string("feche o texto com aspas na mesma linha"));
      push(TokenKind::Text, src_.substr(content_start, pos_ - content_start),
           span_from(start, sl, sc));
      return;
    }
    if (c == '\\') {
      advance();
      if (peek() != '\0' && peek() != '\n') advance();
      continue;
    }
    advance();
  }
}

void Lexer::finish() {
  if (!out_.empty() && out_.back().kind != TokenKind::Newline &&
      out_.back().kind != TokenKind::Indent && out_.back().kind != TokenKind::Dedent) {
    push_structural(TokenKind::Newline);
  }
  while (indent_stack_.size() > 1) {
    indent_stack_.pop_back();
    push_structural(TokenKind::Dedent);
  }
  push_structural(TokenKind::EndOfFile);
}

}  // namespace tilt
