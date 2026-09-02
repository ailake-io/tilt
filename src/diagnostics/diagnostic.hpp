#pragma once

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "common/source.hpp"
#include "diagnostics/codes.hpp"

namespace tilt {

enum class Severity { Error, Warning, Note };

struct Diagnostic {
  Severity severity = Severity::Error;
  DiagCode code{};
  Span span{};
  std::string message;
  std::vector<std::string> notes;         // rendered as "= <note>" lines
  std::optional<std::string> suggestion;  // rendered as "= sugestao: <text>"
};

// Collects diagnostics for a single source file and renders them
// in the format described in CLAUDE.md section 18.
class DiagnosticEngine {
 public:
  explicit DiagnosticEngine(const SourceFile* source) : source_(source) {}

  void report(Diagnostic diag);

  bool has_errors() const { return errors_ > 0; }
  std::size_t error_count() const { return errors_; }
  const std::vector<Diagnostic>& all() const { return diags_; }

  void render(std::ostream& os, bool color) const;

 private:
  const SourceFile* source_;
  std::vector<Diagnostic> diags_;
  std::size_t errors_ = 0;
};

}  // namespace tilt
