#include "lsp/workspace_index.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "diagnostics/diagnostic.hpp"
#include "lexer/lexer.hpp"
#include "parser/ast.hpp"
#include "parser/parser.hpp"

namespace tilt::lsp {

namespace {

namespace fs = std::filesystem;

std::string uri_to_path(std::string uri) {
  if (uri.rfind("file://", 0) == 0) uri.erase(0, 7);
  // LSP file URIs commonly percent-encode spaces and a few punctuation
  // characters. Keep decoding deliberately small and deterministic.
  std::string out;
  out.reserve(uri.size());
  for (std::size_t i = 0; i < uri.size(); ++i) {
    if (uri[i] == '%' && i + 2 < uri.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = hex(uri[i + 1]), lo = hex(uri[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(uri[i]);
  }
  return out;
}

std::string path_to_uri(const fs::path& path) {
  std::error_code ec;
  const fs::path absolute = fs::absolute(path, ec);
  return "file://" + (ec ? path : absolute).generic_string();
}

bool same_span(const Span& a, const Span& b) {
  return a.offset == b.offset && a.length == b.length && a.line == b.line && a.column == b.column;
}

bool span_contains(const Span& span, std::uint32_t line, std::uint32_t column) {
  if (span.line != line || column < span.column) return false;
  const std::uint32_t length = span.length == 0 ? 1 : span.length;
  return column <= span.column + length;
}

bool is_identifier_token(const Token& token) { return token.kind == TokenKind::Identifier; }

std::string module_key(const std::string& uri) { return "@module:" + uri; }

struct Symbol {
  std::string uri;
  std::string name;
  Span span;
};

struct Occurrence {
  std::string uri;
  Span span;
  bool declaration = false;
};

struct Document {
  std::string uri;
  std::string text;
  std::shared_ptr<SourceFile> source;
  std::vector<Token> tokens;
  std::unique_ptr<ast::Program> program;
};

struct Binding {
  std::unordered_map<std::string, std::string> direct;
  std::unordered_map<std::string, std::string> modules;
  std::unordered_map<std::uint32_t, std::string> spans;
};

}  // namespace

struct WorkspaceIndex::Impl {
  std::vector<fs::path> roots;
  std::unordered_map<std::string, Document> docs;
  std::unordered_map<std::string, Symbol> symbols;
  std::unordered_map<std::string, std::vector<Occurrence>> occurrences;
  std::unordered_map<std::string, Binding> bindings;
  bool dirty = true;

  void parse(Document& doc) {
    doc.source = std::make_shared<SourceFile>(uri_to_path(doc.uri), doc.text);
    DiagnosticEngine diag(doc.source.get());
    Lexer lexer(*doc.source, diag);
    doc.tokens = lexer.tokenize();
    Parser parser(doc.tokens, diag);
    try {
      doc.program = std::make_unique<ast::Program>(parser.parse_program());
    } catch (...) {
      doc.program = std::make_unique<ast::Program>();
    }
  }

  fs::path find_module(const std::string& from_uri, const std::string& name) const {
    fs::path from(uri_to_path(from_uri));
    std::vector<fs::path> dirs;
    std::error_code ec;
    dirs.push_back(from.parent_path());
    fs::path parent = fs::absolute(from.parent_path(), ec);
    while (!ec && !parent.empty()) {
      dirs.push_back(parent / "modulos");
      if (fs::is_regular_file(parent / "tilt.toml", ec)) break;
      ec.clear();
      const fs::path next = parent.parent_path();
      if (next == parent) break;
      parent = next;
    }
    if (const char* env = std::getenv("TILT_STDLIB_PATH")) {
#if defined(_WIN32)
      constexpr char separator = ';';
#else
      constexpr char separator = ':';
#endif
      std::string paths(env);
      std::size_t begin = 0;
      while (begin <= paths.size()) {
        const std::size_t end = paths.find(separator, begin);
        const std::string part = paths.substr(begin, end - begin);
        if (!part.empty()) dirs.emplace_back(part);
        if (end == std::string::npos) break;
        begin = end + 1;
      }
    }
    dirs.push_back(fs::current_path() / "stdlib");
    std::string rel = name;
    // `path::preferred_separator` is wchar_t on MSVC, while the module name
    // is UTF-8/char. Use the platform separator with the matching type.
    const char path_separator =
        fs::path::preferred_separator == static_cast<fs::path::value_type>('/') ? '/' : '\\';
    std::replace(rel.begin(), rel.end(), '.', path_separator);
    for (const fs::path& dir : dirs) {
      for (const fs::path& candidate : {dir / (rel + ".tilt"), dir / name / "__init__.tilt"}) {
        if (fs::is_regular_file(candidate, ec) && !ec) return fs::absolute(candidate, ec);
        ec.clear();
      }
    }
    return {};
  }

  void add_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec || path.extension() != ".tilt") return;
    const std::string uri = path_to_uri(path);
    if (docs.find(uri) != docs.end()) return;
    std::ifstream file(path);
    if (!file) return;
    Document doc;
    doc.uri = uri;
    doc.text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    parse(doc);
    docs.emplace(uri, std::move(doc));
  }

  void scan() {
    for (const fs::path& root : roots) {
      std::error_code ec;
      if (!fs::is_directory(root, ec) || ec) continue;
      fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
      for (; it != end && !ec; it.increment(ec)) {
        const fs::path p = it->path();
        if (it->is_directory(ec)) {
          const std::string n = p.filename().string();
          if (n == ".git" || n == "build" || n == "node_modules" || n == ".tilt") it.disable_recursion_pending();
          continue;
        }
        add_file(p);
      }
    }
  }

  std::string key_at(const std::string& uri, std::uint32_t line, std::uint32_t column) const {
    auto it = docs.find(uri);
    if (it == docs.end()) return {};
    for (const auto& [key, refs] : occurrences) {
      for (const Occurrence& ref : refs) {
        if (ref.uri == uri && span_contains(ref.span, line, column)) return key;
      }
    }
    return {};
  }

  std::optional<WorkspaceLocation> symbol_location(const std::string& key) const {
    if (key.rfind("@module:", 0) == 0) {
      const std::string uri = key.substr(8);
      auto doc = docs.find(uri);
      if (doc == docs.end() || !doc->second.program) return std::nullopt;
      for (const auto& item : doc->second.program->items) {
        if (item && item->kind == ast::ItemKind::Decl && !item->header.empty() && item->header[0])
          return WorkspaceLocation{uri, item->header[0]->span};
      }
      return WorkspaceLocation{uri, Span{0, 1, 1, 1}};
    }
    auto it = symbols.find(key);
    if (it == symbols.end()) return std::nullopt;
    return WorkspaceLocation{it->second.uri, it->second.span};
  }

  void mark_import(const Document& doc, const ast::Item& item, Binding& binding) {
    if (item.key != "importar" && item.key != "de") return;
    const auto names = ast::nomes_importados(item);
    if (names.empty()) return;
    std::vector<const ast::Expr*> headers;
    for (const auto& h : item.header) if (h) headers.push_back(h.get());
    if (item.key == "importar") {
      for (const auto& n : names) {
        const fs::path module_path = find_module(doc.uri, n.nome);
        if (module_path.empty()) continue;
        const std::string target_uri = path_to_uri(module_path);
        binding.modules[n.alias] = target_uri;
        for (const ast::Expr* h : headers)
          if (h->text == n.nome || h->text == n.alias) binding.spans[h->span.offset] = module_key(target_uri);
      }
      return;
    }
    // `de modulo importar exportado como local`: the first header is the
    // module; every later name identifies a symbol exported by that module.
    const fs::path module_path = find_module(doc.uri, names[0].nome);
    if (module_path.empty()) return;
    const std::string target_uri = path_to_uri(module_path);
    binding.spans[headers.front()->span.offset] = module_key(target_uri);
    for (std::size_t k = 1; k < names.size(); ++k) {
      const auto& n = names[k];
      const std::string key = target_uri + "#" + n.nome;
      binding.direct[n.alias] = key;
      for (const ast::Expr* h : headers)
        if (h->text == n.nome || h->text == n.alias) binding.spans[h->span.offset] = key;
    }
  }

  void rebuild() {
    symbols.clear(); occurrences.clear(); bindings.clear();
    scan();
    for (const auto& [uri, doc] : docs) {
      if (!doc.program) continue;
      for (const auto& item : doc.program->items) {
        if (!item || item->kind != ast::ItemKind::Decl || item->header.empty() || !item->header[0] ||
            item->header[0]->kind != ast::ExprKind::Name) continue;
        const auto& h = *item->header[0];
        symbols[uri + "#" + h.text] = Symbol{uri, h.text, h.span};
      }
    }
    for (auto& [uri, doc] : docs) {
      Binding& binding = bindings[uri];
      if (!doc.program) continue;
      for (const auto& item : doc.program->items) {
        if (item) mark_import(doc, *item, binding);
      }
      std::unordered_map<std::string, std::string> local;
      for (const auto& [key, symbol] : symbols)
        if (symbol.uri == uri) local[symbol.name] = key;
      for (std::size_t i = 0; i < doc.tokens.size(); ++i) {
        const Token& token = doc.tokens[i];
        if (!is_identifier_token(token)) continue;
        std::string key;
        if (auto by_span = binding.spans.find(token.span.offset); by_span != binding.spans.end()) {
          key = by_span->second;
        } else if (i >= 2 && doc.tokens[i - 1].kind == TokenKind::Dot &&
                   is_identifier_token(doc.tokens[i - 2])) {
          auto mit = binding.modules.find(std::string(doc.tokens[i - 2].lexeme));
          if (mit != binding.modules.end()) key = mit->second + "#" + std::string(token.lexeme);
        }
        if (key.empty()) {
          auto dit = binding.direct.find(std::string(token.lexeme));
          if (dit != binding.direct.end()) key = dit->second;
        }
        if (key.empty()) {
          auto lit = local.find(std::string(token.lexeme));
          if (lit != local.end()) key = lit->second;
        }
        if (key.empty()) continue;
        bool declaration = false;
        auto sit = symbols.find(key);
        if (sit != symbols.end()) declaration = same_span(sit->second.span, token.span);
        occurrences[key].push_back(Occurrence{uri, token.span, declaration});
      }
    }
    dirty = false;
  }

  void ensure() { if (dirty) rebuild(); }
};

WorkspaceIndex::WorkspaceIndex() : impl_(new Impl) {}
WorkspaceIndex::~WorkspaceIndex() { delete impl_; }

void WorkspaceIndex::set_roots(std::vector<std::string> roots) {
  impl_->roots.clear();
  for (const std::string& root : roots) {
    if (!root.empty()) impl_->roots.emplace_back(uri_to_path(root));
  }
  impl_->dirty = true;
}

void WorkspaceIndex::upsert(const std::string& uri, const std::string& text) {
  Document doc;
  doc.uri = uri;
  doc.text = text;
  impl_->parse(doc);
  impl_->docs[uri] = std::move(doc);
  const fs::path path(uri_to_path(uri));
  if (!path.parent_path().empty() && path.parent_path() != fs::path("/")) {
    bool known = false;
    for (const auto& root : impl_->roots) if (root == path.parent_path()) known = true;
    if (!known) impl_->roots.push_back(path.parent_path());
  }
  impl_->dirty = true;
}

void WorkspaceIndex::remove(const std::string& uri) {
  impl_->docs.erase(uri);
  impl_->dirty = true;
}

void WorkspaceIndex::refresh() {
  // Open documents invalidate the index through upsert(). A full recursive
  // scan on every completion/definition request makes large workspaces
  // noticeably slower, so refresh only materializes a pending rebuild here.
  impl_->ensure();
}

std::optional<WorkspaceLocation> WorkspaceIndex::definition(const std::string& uri,
                                                            std::uint32_t line,
                                                            std::uint32_t column) {
  impl_->ensure();
  const std::string key = impl_->key_at(uri, line, column);
  if (key.empty()) return std::nullopt;
  return impl_->symbol_location(key);
}

std::vector<WorkspaceLocation> WorkspaceIndex::references(const std::string& uri,
                                                          std::uint32_t line,
                                                          std::uint32_t column,
                                                          bool include_declaration) {
  impl_->ensure();
  const std::string key = impl_->key_at(uri, line, column);
  if (key.empty()) return {};
  std::vector<WorkspaceLocation> out;
  for (const auto& ref : impl_->occurrences[key]) {
    if (!include_declaration && ref.declaration) continue;
    out.push_back({ref.uri, ref.span});
  }
  std::sort(out.begin(), out.end(), [](const WorkspaceLocation& a, const WorkspaceLocation& b) {
    if (a.uri != b.uri) return a.uri < b.uri;
    if (a.span.line != b.span.line) return a.span.line < b.span.line;
    return a.span.column < b.span.column;
  });
  return out;
}

std::vector<std::string> WorkspaceIndex::exported_names() {
  impl_->ensure();
  std::vector<std::string> names;
  names.reserve(impl_->symbols.size());
  for (const auto& [key, symbol] : impl_->symbols) {
    if (key.rfind("@module:", 0) == 0) continue;
    names.push_back(symbol.name);
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

bool WorkspaceIndex::external_symbol_at(const std::string& uri, std::uint32_t line,
                                        std::uint32_t column) {
  impl_->ensure();
  const std::string key = impl_->key_at(uri, line, column);
  if (key.empty()) return false;
  if (key.rfind("@module:", 0) == 0) return key.substr(8) != uri;
  const auto it = impl_->symbols.find(key);
  return it != impl_->symbols.end() && it->second.uri != uri;
}

}  // namespace tilt::lsp
