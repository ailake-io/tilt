#include "common/source.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace tilt {

SourceFile::SourceFile(std::string path, std::string contents)
    : path_(std::move(path)), contents_(std::move(contents)) {
  line_starts_.push_back(0);
  for (uint32_t i = 0; i < contents_.size(); ++i) {
    if (contents_[i] == '\n') line_starts_.push_back(i + 1);
  }
  // Drop a phantom empty line when the file ends with a newline.
  if (line_starts_.size() > 1 && line_starts_.back() == contents_.size()) {
    line_starts_.pop_back();
  }
}

SourceFile SourceFile::load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("nao foi possivel abrir: " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return SourceFile(path, ss.str());
}

std::string_view SourceFile::line_text(uint32_t line) const {
  if (line < 1 || line > line_starts_.size()) return {};
  uint32_t start = line_starts_[line - 1];
  uint32_t end =
      (line < line_starts_.size()) ? line_starts_[line] : static_cast<uint32_t>(contents_.size());
  std::string_view sv(contents_.data() + start, end - start);
  while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r')) sv.remove_suffix(1);
  return sv;
}

}  // namespace tilt
