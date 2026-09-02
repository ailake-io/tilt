#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tilt {

// A byte range in a source file, with a 1-based line/column for display.
struct Span {
  uint32_t offset = 0;  // byte offset of the first char
  uint32_t length = 0;  // length in bytes (0 => render a single caret)
  uint32_t line = 1;    // 1-based
  uint32_t column = 1;  // 1-based, counted in bytes
};

// Owns the text of one `.tilt` file and an index of line starts.
class SourceFile {
 public:
  SourceFile(std::string path, std::string contents);

  // Reads `path` from disk. Throws std::runtime_error if it cannot be opened.
  static SourceFile load(const std::string& path);

  const std::string& path() const { return path_; }
  std::string_view text() const { return contents_; }

  uint32_t line_count() const { return static_cast<uint32_t>(line_starts_.size()); }

  // 1-based line contents, without the trailing newline. Empty view if out of range.
  std::string_view line_text(uint32_t line) const;

 private:
  std::string path_;
  std::string contents_;
  std::vector<uint32_t> line_starts_;
};

}  // namespace tilt
