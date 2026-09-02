#pragma once

#include <cstdint>
#include <string_view>

#include "common/source.hpp"

namespace tilt {

enum class TokenKind : std::uint8_t {
  EndOfFile,
  Newline,
  Indent,
  Dedent,

  Identifier,  // also covers keywords; the parser resolves them
  Integer,
  Decimal,
  Text,  // string literal; lexeme is the inner content, without quotes

  Colon,      // :
  Dash,       // -   (list item and subtraction)
  Comma,      // ,
  Dot,        // .
  DotDot,     // ..
  Pipe,       // |
  LBrace,     // {
  RBrace,     // }
  LBracket,   // [
  RBracket,   // ]
  LParen,     // (
  RParen,     // )
  Equal,      // =
  EqualEqual, // ==
  BangEqual,  // !=
  Less,       // <
  LessEqual,  // <=
  Greater,    // >
  GreaterEqual, // >=
  Plus,       // +
  Star,       // *
  Slash,      // /
  Percent,    // %
  QuestionDot, // ?.

  Invalid,
};

const char* token_kind_name(TokenKind kind);

struct Token {
  TokenKind kind = TokenKind::Invalid;
  std::string_view lexeme;  // slice of the source; empty for structural tokens
  Span span;
};

}  // namespace tilt
