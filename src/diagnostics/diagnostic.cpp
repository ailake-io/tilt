#include "diagnostics/diagnostic.hpp"

#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace tilt {

void DiagnosticEngine::report(Diagnostic diag) {
  if (diag.severity == Severity::Error) ++errors_;
  diags_.push_back(std::move(diag));
}

namespace {

const char* severity_word(Severity s) {
  switch (s) {
    case Severity::Error:
      return "erro";
    case Severity::Warning:
      return "aviso";
    case Severity::Note:
      return "nota";
  }
  return "erro";
}

}  // namespace

void DiagnosticEngine::render(std::ostream& os, bool color) const {
  const char* col_sev = color ? "\033[1;31m" : "";
  const char* col_accent = color ? "\033[34m" : "";
  const char* col_bold = color ? "\033[1m" : "";
  const char* col_reset = color ? "\033[0m" : "";

  const std::string path = source_ ? source_->path() : std::string("<desconhecido>");

  for (const auto& d : diags_) {
    os << col_sev << severity_word(d.severity) << "[" << diag_code_string(d.code) << "]" << col_reset
       << col_bold << ": " << d.message << col_reset << "\n";

    os << "  " << col_accent << "-->" << col_reset << " " << path << ":" << d.span.line << ":"
       << d.span.column << "\n";

    if (source_ && d.span.line >= 1 && d.span.line <= source_->line_count()) {
      const std::string num = std::to_string(d.span.line);
      const std::string pad(num.size(), ' ');
      const std::string_view text = source_->line_text(d.span.line);

      os << " " << pad << " " << col_accent << "|" << col_reset << "\n";
      os << " " << col_accent << num << " |" << col_reset << " " << text << "\n";

      os << " " << pad << " " << col_accent << "|" << col_reset << " ";
      const uint32_t skip = d.span.column > 0 ? d.span.column - 1 : 0;
      for (uint32_t i = 0; i < skip; ++i) os << ' ';
      const uint32_t carets = d.span.length > 0 ? d.span.length : 1;
      os << col_sev;
      for (uint32_t i = 0; i < carets; ++i) os << '^';
      os << col_reset << "\n";

      os << " " << pad << " " << col_accent << "|" << col_reset << "\n";
    }

    for (const auto& note : d.notes) {
      os << "   " << col_accent << "=" << col_reset << " " << note << "\n";
    }
    if (d.suggestion) {
      os << "   " << col_accent << "=" << col_reset << " sugestao: " << *d.suggestion << "\n";
    }
    os << "\n";
  }
}

}  // namespace tilt
