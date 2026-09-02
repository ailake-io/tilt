#include "lexer/token.hpp"

namespace tilt {

const char* token_kind_name(TokenKind kind) {
  switch (kind) {
    case TokenKind::EndOfFile:
      return "EOF";
    case TokenKind::Newline:
      return "NEWLINE";
    case TokenKind::Indent:
      return "INDENT";
    case TokenKind::Dedent:
      return "DEDENT";
    case TokenKind::Identifier:
      return "IDENT";
    case TokenKind::Integer:
      return "INT";
    case TokenKind::Decimal:
      return "DECIMAL";
    case TokenKind::Text:
      return "TEXT";
    case TokenKind::Colon:
      return "COLON";
    case TokenKind::Dash:
      return "DASH";
    case TokenKind::Comma:
      return "COMMA";
    case TokenKind::Dot:
      return "DOT";
    case TokenKind::DotDot:
      return "DOTDOT";
    case TokenKind::Pipe:
      return "PIPE";
    case TokenKind::LBrace:
      return "LBRACE";
    case TokenKind::RBrace:
      return "RBRACE";
    case TokenKind::LBracket:
      return "LBRACKET";
    case TokenKind::RBracket:
      return "RBRACKET";
    case TokenKind::LParen:
      return "LPAREN";
    case TokenKind::RParen:
      return "RPAREN";
    case TokenKind::Equal:
      return "EQ";
    case TokenKind::EqualEqual:
      return "EQEQ";
    case TokenKind::BangEqual:
      return "NEQ";
    case TokenKind::Less:
      return "LT";
    case TokenKind::LessEqual:
      return "LE";
    case TokenKind::Greater:
      return "GT";
    case TokenKind::GreaterEqual:
      return "GE";
    case TokenKind::Plus:
      return "PLUS";
    case TokenKind::Star:
      return "STAR";
    case TokenKind::Slash:
      return "SLASH";
    case TokenKind::Percent:
      return "PERCENT";
    case TokenKind::QuestionDot:
      return "QDOT";
    case TokenKind::Invalid:
      return "INVALID";
  }
  return "?";
}

}  // namespace tilt
