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

// Indentation style of a file: detected at the first indented line and then
// enforced for the rest of the file. 1 tab == 1 level, or 2 spaces == 1 level.
enum class IndentStyle { Undetected, Spaces, Tabs };

// Turns a `.tilt` source file into a token stream, driving an INDENT/DEDENT
// state machine for significant indentation (2 spaces or 1 tab per level,
// one style per file). Reads without copying.
class Lexer {
 public:
  Lexer(const SourceFile& source, DiagnosticEngine& diag);

  std::vector<Token> tokenize();

  // Style detected while tokenizing (Undetected if no line was indented).
  IndentStyle indent_style() const { return style_; }

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
  int bracket_depth_ = 0;  // inside ( [ { : newlines/indentation are not significant
  std::vector<int> indent_stack_;
  IndentStyle style_ = IndentStyle::Undetected;
  std::uint32_t style_line_ = 0;  // line where the file's style was defined
  std::vector<Token> out_;
};

}  // namespace tilt
