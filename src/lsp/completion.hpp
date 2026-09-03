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

}  // namespace tilt::lsp
