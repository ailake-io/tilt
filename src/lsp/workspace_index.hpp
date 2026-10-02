#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/source.hpp"

namespace tilt::lsp {

// A source location returned by workspace-aware navigation. Span coordinates
// remain 1-based, like the rest of the frontend; the LSP adapter converts
// them to zero-based ranges.
struct WorkspaceLocation {
  std::string uri;
  Span span;
};

// Lightweight index for .tilt files in the open workspace. It indexes
// top-level declarations and explicit imports, while keeping the existing
// same-file resolver as a fallback for local variables and parameters.
class WorkspaceIndex {
 public:
  WorkspaceIndex();
  ~WorkspaceIndex();
  WorkspaceIndex(const WorkspaceIndex&) = delete;
  WorkspaceIndex& operator=(const WorkspaceIndex&) = delete;

  void set_roots(std::vector<std::string> roots);
  void upsert(const std::string& uri, const std::string& text);
  void remove(const std::string& uri);
  // Materializa uma atualização pendente. Documentos abertos invalidam o
  // índice via upsert; consultas repetidas não fazem varredura recursiva.
  void refresh();

  std::optional<WorkspaceLocation> definition(const std::string& uri,
                                              std::uint32_t line,
                                              std::uint32_t column);
  std::vector<WorkspaceLocation> references(const std::string& uri,
                                            std::uint32_t line,
                                            std::uint32_t column,
                                            bool include_declaration);
  std::vector<std::string> exported_names();

  // True when the identifier under the cursor resolves through an import to
  // another document. The caller uses this to choose workspace navigation
  // over the local lexical resolver for function parameters and locals.
  bool external_symbol_at(const std::string& uri, std::uint32_t line,
                          std::uint32_t column);

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace tilt::lsp
