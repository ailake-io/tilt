#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/source.hpp"
#include "diagnostics/diagnostic.hpp"
#include "lexer/token.hpp"

namespace tilt {

// Turns a `.tilt` source file into a token stream, driving an INDENT/DEDENT
// state machine for significant 2-space indentation. Reads without copying.
class Lexer {
 public:
  Lexer(const SourceFile& source, DiagnosticEngine& diag);

  std::vector<Token> tokenize();

 private:
  char peek(std::size_t offset = 0) const;
  char advance();
  bool match(char expected);

  void handle_line_start();
  void lex_token();
  void lex_number(std::size_t start, std::uint32_t line, std::uint32_t col);
  void lex_text(std::size_t start, std::uint32_t line, std::uint32_t col);
  void finish();

  void push(TokenKind kind, std::string_view lexeme, Span span);
  void push_structural(TokenKind kind);
  Span span_from(std::size_t start, std::uint32_t line, std::uint32_t col) const;
  void report(DiagCode code, Span span, std::string message,
              std::vector<std::string> notes = {},
              std::optional<std::string> suggestion = std::nullopt);

  std::string_view src_;
  DiagnosticEngine& diag_;
  std::size_t pos_ = 0;
  std::uint32_t line_ = 1;
  std::uint32_t col_ = 1;
  bool at_line_start_ = true;
  std::vector<int> indent_stack_;
  std::vector<Token> out_;
};

}  // namespace tilt
