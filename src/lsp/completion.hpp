#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/source.hpp"

namespace tilt::lsp {

struct CompletionItem {
  std::string label;
  std::string kind;    // keyword | field | builtin | name | method | snippet
  std::string detail;  // short hint
};

// Context-aware completion candidates for a cursor at 1-based (line, column).
std::vector<CompletionItem> complete(const SourceFile& src, std::uint32_t line, std::uint32_t column);

// Markdown documentation for the symbol under the cursor at 1-based (line, column).
// Covers builtins, keywords, tensor/table methods and names declared in the file.
// Returns an empty string when nothing is known about the symbol.
std::string hover(const SourceFile& src, std::uint32_t line, std::uint32_t column);

// Go-to-definition for the symbol under the cursor, resolved within the same file.
// Returns a zero-length span when there is no known declaration.
Span definition(const SourceFile& src, std::uint32_t line, std::uint32_t column);

// Signature help when the cursor is inside a call to a known builtin.
struct SigHelp {
  bool found = false;
  std::string label;                 // "ler_csv(caminho)"
  std::vector<std::string> params;   // parameter names, in order
  std::size_t active_parameter = 0;  // index of the parameter being typed
};

SigHelp signature_help(const SourceFile& src, std::uint32_t line, std::uint32_t column);

// Deterministic whole-document formatting: normalizes indentation to the block
// level derived from the token stream using the file's own style (1 tab per
// level in tab files, 2 spaces per level otherwise), trims trailing
// whitespace and guarantees a single final newline. Lines inside multiline
// (triple-quoted) strings, comment lines and bracket-continuation lines are
// left untouched.
std::string format_document(const std::string& text);

}  // namespace tilt::lsp
