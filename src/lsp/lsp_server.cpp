#include "lsp/lsp_server.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <istream>
#include <iterator>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/source.hpp"
#include "diagnostics/diagnostic.hpp"
#include "lexer/lexer.hpp"
#include "lsp/completion.hpp"
#include "parser/parser.hpp"
#include "parser/ast.hpp"
#include "runtime/json.hpp"
#include "runtime/value.hpp"
#include "semantic/checker.hpp"

namespace tilt::lsp {

namespace {

using rt::Value;

int completion_kind(const std::string& k) {
  if (k == "keyword") return 14;
  if (k == "field") return 5;
  if (k == "builtin") return 3;
  if (k == "method") return 2;
  if (k == "snippet") return 15;
  return 6;  // variable / name
}

std::string uri_to_path(const std::string& uri) {
  if (uri.rfind("file://", 0) == 0) return uri.substr(7);
  return uri;
}

struct ImportedDefinition {
  std::string uri;
  Span span;
};

// Resolve explicit imports against the same local path rule as the runtime.
// This deliberately opens only the named module; no workspace-wide scan is
// needed to navigate `de modulo importar funcao` or `modulo.funcao`.
std::optional<ImportedDefinition> imported_definition(
    const std::string& uri, const std::string& text, long line, long character,
    const std::unordered_map<std::string, std::string>& docs) {
  SourceFile src(uri_to_path(uri), text);
  if (line < 0 || character < 0) return std::nullopt;
  const std::string_view row = src.line_text(static_cast<std::uint32_t>(line + 1));
  std::size_t pos = static_cast<std::size_t>(character);
  if (pos >= row.size()) {
    if (pos == 0 || pos > row.size()) return std::nullopt;
    --pos;
  }
  const auto ident = [](char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_';
  };
  if (!ident(row[pos])) return std::nullopt;
  std::size_t start = pos, end = pos + 1;
  while (start > 0 && ident(row[start - 1])) --start;
  while (end < row.size() && ident(row[end])) ++end;
  const std::string name(row.substr(start, end - start));
  std::string receiver;
  if (start > 1 && row[start - 1] == '.') {
    std::size_t prev = start - 1;
    while (prev > 0 && ident(row[prev - 1])) --prev;
    receiver = std::string(row.substr(prev, start - 1 - prev));
  }

  DiagnosticEngine diag(&src);
  Lexer lexer(src, diag);
  const std::vector<Token> tokens = lexer.tokenize();
  Parser parser(tokens, diag);
  const ast::Program prog = parser.parse_program();
  std::string module_name, symbol_name;
  for (const auto& item : prog.items) {
    if (!item || item->kind != ast::ItemKind::Decl) continue;
    const auto imports = ast::nomes_importados(*item);
    if (item->key == "de" && imports.size() > 1 && receiver.empty()) {
      for (std::size_t i = 1; i < imports.size(); ++i) {
        if (imports[i].alias == name) {
          module_name = imports[0].nome;
          symbol_name = imports[i].nome;
          break;
        }
      }
    } else if (item->key == "importar") {
      for (const auto& imp : imports) {
        if ((!receiver.empty() && imp.alias == receiver) ||
            (receiver.empty() && imp.alias == name)) {
          module_name = imp.nome;
          symbol_name = receiver.empty() ? std::string() : name;
          break;
        }
      }
    }
    if (!module_name.empty()) break;
  }
  if (module_name.empty()) return std::nullopt;

  const std::filesystem::path project_dir = std::filesystem::path(uri_to_path(uri)).parent_path();
  std::vector<std::filesystem::path> dirs = {project_dir, project_dir / "modulos"};
  std::error_code project_ec;
  std::filesystem::path parent = std::filesystem::absolute(project_dir, project_ec);
  while (!project_ec && !parent.empty()) {
    if (std::filesystem::is_regular_file(parent / "tilt.toml", project_ec)) {
      if (parent != project_dir) dirs.push_back(parent / "modulos");
      break;
    }
    project_ec.clear();
    const std::filesystem::path next = parent.parent_path();
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
      const std::size_t next = paths.find(separator, begin);
      const std::string part = paths.substr(begin, next - begin);
      if (!part.empty()) dirs.emplace_back(part);
      if (next == std::string::npos) break;
      begin = next + 1;
    }
  }
  dirs.push_back(std::filesystem::current_path() / "stdlib");
  for (const auto& dir : dirs) {
    const auto path = dir / (module_name + ".tilt");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) continue;
    const std::string target_uri = "file://" + std::filesystem::absolute(path).string();
    std::string source;
    if (auto it = docs.find(target_uri); it != docs.end()) {
      source = it->second;
    } else {
      std::ifstream file(path);
      source.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    SourceFile module(path.string(), source);
    if (symbol_name.empty()) return ImportedDefinition{target_uri, Span{0, 1, 1, 1}};
    DiagnosticEngine module_diag(&module);
    Lexer module_lexer(module, module_diag);
    const std::vector<Token> module_tokens = module_lexer.tokenize();
    Parser module_parser(module_tokens, module_diag);
    const ast::Program module_prog = module_parser.parse_program();
    for (const auto& decl : module_prog.items) {
      if (decl && decl->kind == ast::ItemKind::Decl && !decl->header.empty() &&
          decl->header[0] && decl->header[0]->kind == ast::ExprKind::Name &&
          decl->header[0]->text == symbol_name) {
        return ImportedDefinition{target_uri, decl->header[0]->span};
      }
    }
    return std::nullopt;
  }
  return std::nullopt;
}

bool read_message(std::istream& in, std::string& body) {
  std::size_t content_length = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;  // end of headers
    const std::string key = "Content-Length:";
    if (line.rfind(key, 0) == 0) {
      content_length = static_cast<std::size_t>(std::strtoul(line.c_str() + key.size(), nullptr, 10));
    }
  }
  if (content_length == 0) return false;
  body.resize(content_length);
  in.read(body.data(), static_cast<std::streamsize>(content_length));
  return in.good() || in.eof();
}

void write_message(std::ostream& out, const Value& msg) {
  const std::string payload = rt::json_dump(msg);
  out << "Content-Length: " << payload.size() << "\r\n\r\n" << payload;
  out.flush();
}

const Value* member(const Value& v, const char* key) {
  return v.kind == rt::ValueKind::Mapa && v.map_ref() ? v.map_ref()->find(key) : nullptr;
}

long member_int(const Value& v, const char* key) {
  const Value* m = member(v, key);
  return m ? static_cast<long>(m->as_number()) : 0;
}

std::string member_str(const Value& v, const char* key) {
  const Value* m = member(v, key);
  return m && m->kind == rt::ValueKind::Texto ? m->s : std::string();
}

bool valid_identifier(const std::string& value) {
  if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_')) {
    return false;
  }
  for (std::size_t i = 1; i < value.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (!(std::isalnum(c) || value[i] == '_')) return false;
  }
  return true;
}

Value make_response(const Value& id, Value result) {
  Value r = Value::mapa();
  r.map_ref()->set("jsonrpc", Value::texto("2.0"));
  if (id.kind != rt::ValueKind::Nulo) r.map_ref()->set("id", id);
  r.map_ref()->set("result", std::move(result));
  return r;
}

Value span_to_range(const Span& s) {
  const long l = static_cast<long>(s.line) - 1;
  const long ch = static_cast<long>(s.column) - 1;
  const long len = s.length > 0 ? static_cast<long>(s.length) : 1;
  Value start = Value::mapa();
  start.map_ref()->set("line", Value::inteiro(l < 0 ? 0 : l));
  start.map_ref()->set("character", Value::inteiro(ch < 0 ? 0 : ch));
  Value end = Value::mapa();
  end.map_ref()->set("line", Value::inteiro(l < 0 ? 0 : l));
  end.map_ref()->set("character", Value::inteiro((ch < 0 ? 0 : ch) + len));
  Value range = Value::mapa();
  range.map_ref()->set("start", std::move(start));
  range.map_ref()->set("end", std::move(end));
  return range;
}

Value compute_diagnostics(const std::string& uri, const std::string& text) {
  SourceFile src(uri_to_path(uri), text);
  DiagnosticEngine diag(&src);
  Lexer lexer(src, diag);
  const std::vector<Token> toks = lexer.tokenize();
  Parser parser(toks, diag);
  const ast::Program prog = parser.parse_program();
  check_program(prog, diag);

  Value arr = Value::lista();
  for (const Diagnostic& d : diag.all()) {
    const long l = static_cast<long>(d.span.line) - 1;
    const long ch = static_cast<long>(d.span.column) - 1;
    const long len = d.span.length > 0 ? static_cast<long>(d.span.length) : 1;
    Value start = Value::mapa();
    start.map_ref()->set("line", Value::inteiro(l < 0 ? 0 : l));
    start.map_ref()->set("character", Value::inteiro(ch < 0 ? 0 : ch));
    Value end = Value::mapa();
    end.map_ref()->set("line", Value::inteiro(l < 0 ? 0 : l));
    end.map_ref()->set("character", Value::inteiro((ch < 0 ? 0 : ch) + len));
    Value range = Value::mapa();
    range.map_ref()->set("start", std::move(start));
    range.map_ref()->set("end", std::move(end));
    Value item = Value::mapa();
    item.map_ref()->set("range", std::move(range));
    item.map_ref()->set("severity", Value::inteiro(d.severity == Severity::Error ? 1 : 2));
    item.map_ref()->set("code", Value::texto(std::string(diag_code_string(d.code))));
    item.map_ref()->set("source", Value::texto("tilt"));
    item.map_ref()->set("message", Value::texto(d.message));
    arr.list_ref()->push_back(std::move(item));
  }

  return arr;
}

void publish_diagnostics(std::ostream& out, const std::string& uri, const Value& diagnostics) {
  Value params = Value::mapa();
  params.map_ref()->set("uri", Value::texto(uri));
  params.map_ref()->set("diagnostics", diagnostics);
  Value note = Value::mapa();
  note.map_ref()->set("jsonrpc", Value::texto("2.0"));
  note.map_ref()->set("method", Value::texto("textDocument/publishDiagnostics"));
  note.map_ref()->set("params", std::move(params));
  write_message(out, note);
}

}  // namespace

int run_lsp(std::istream& in, std::ostream& out) {
  std::unordered_map<std::string, std::string> docs;
  struct DiagnosticCache {
    std::string text;
    Value diagnostics = Value::lista();
  };
  std::unordered_map<std::string, DiagnosticCache> diagnostic_cache;
  auto diagnostics_for = [&](const std::string& uri) -> const Value& {
    auto& cached = diagnostic_cache[uri];
    const std::string& text = docs[uri];
    if (cached.text != text) {
      cached.text = text;
      cached.diagnostics = compute_diagnostics(uri, text);
    }
    return cached.diagnostics;
  };
  std::string body;

  while (read_message(in, body)) {
    Value msg;
    try {
      msg = rt::json_parse(body);
    } catch (...) {
      continue;
    }
    const std::string method = member_str(msg, "method");
    const Value* idp = member(msg, "id");
    const Value id = idp ? *idp : Value::nulo();
    const Value* params = member(msg, "params");

    if (method == "initialize") {
      Value caps = Value::mapa();
      caps.map_ref()->set("textDocumentSync", Value::inteiro(1));  // full sync
      Value comp = Value::mapa();
      Value triggers = Value::lista();
      triggers.list_ref()->push_back(Value::texto("."));
      triggers.list_ref()->push_back(Value::texto(":"));
      comp.map_ref()->set("triggerCharacters", std::move(triggers));
      caps.map_ref()->set("completionProvider", std::move(comp));
      caps.map_ref()->set("hoverProvider", Value::logico(true));
      caps.map_ref()->set("definitionProvider", Value::logico(true));
      caps.map_ref()->set("referencesProvider", Value::logico(true));
      caps.map_ref()->set("renameProvider", Value::logico(true));
      Value diagnostic_provider = Value::mapa();
      diagnostic_provider.map_ref()->set("interFileDependencies", Value::logico(false));
      diagnostic_provider.map_ref()->set("workspaceDiagnostics", Value::logico(false));
      caps.map_ref()->set("diagnosticProvider", std::move(diagnostic_provider));
      caps.map_ref()->set("documentFormattingProvider", Value::logico(true));
      Value sig = Value::mapa();
      Value sig_triggers = Value::lista();
      sig_triggers.list_ref()->push_back(Value::texto(","));
      sig_triggers.list_ref()->push_back(Value::texto("("));
      sig.map_ref()->set("triggerCharacters", std::move(sig_triggers));
      caps.map_ref()->set("signatureHelpProvider", std::move(sig));
      Value result = Value::mapa();
      result.map_ref()->set("capabilities", std::move(caps));
      write_message(out, make_response(id, std::move(result)));
    } else if (method == "shutdown") {
      write_message(out, make_response(id, Value::nulo()));
    } else if (method == "exit") {
      return 0;
    } else if (method == "textDocument/didOpen" && params) {
      const Value* td = member(*params, "textDocument");
      if (td) {
        const std::string uri = member_str(*td, "uri");
        docs[uri] = member_str(*td, "text");
        publish_diagnostics(out, uri, diagnostics_for(uri));
      }
    } else if (method == "textDocument/didChange" && params) {
      const Value* td = member(*params, "textDocument");
      const Value* changes = member(*params, "contentChanges");
      if (td && changes && changes->kind == rt::ValueKind::Lista && changes->list_ref() &&
          !changes->list_ref()->empty()) {
        const std::string uri = member_str(*td, "uri");
        docs[uri] = member_str((*changes->list_ref())[0], "text");
        publish_diagnostics(out, uri, diagnostics_for(uri));
      }
    } else if (method == "textDocument/diagnostic" && params) {
      const Value* td = member(*params, "textDocument");
      const std::string uri = td ? member_str(*td, "uri") : "";
      Value result = Value::mapa();
      result.map_ref()->set("kind", Value::texto("full"));
      result.map_ref()->set("items", diagnostics_for(uri));
      write_message(out, make_response(id, std::move(result)));
    } else if (method == "textDocument/completion" && params) {
      const Value* td = member(*params, "textDocument");
      const Value* pos = member(*params, "position");
      const std::string uri = td ? member_str(*td, "uri") : "";
      const long l = pos ? member_int(*pos, "line") : 0;
      const long ch = pos ? member_int(*pos, "character") : 0;
      SourceFile src(uri_to_path(uri), docs.count(uri) ? docs[uri] : std::string());
      const auto items = complete(src, static_cast<std::uint32_t>(l + 1),
                                  static_cast<std::uint32_t>(ch + 1));
      Value arr = Value::lista();
      for (const auto& it : items) {
        Value ci = Value::mapa();
        ci.map_ref()->set("label", Value::texto(it.label));
        ci.map_ref()->set("kind", Value::inteiro(completion_kind(it.kind)));
        ci.map_ref()->set("detail", Value::texto(it.detail));
        arr.list_ref()->push_back(std::move(ci));
      }
      Value result = Value::mapa();
      result.map_ref()->set("isIncomplete", Value::logico(false));
      result.map_ref()->set("items", std::move(arr));
      write_message(out, make_response(id, std::move(result)));
    } else if (method == "textDocument/hover" && params) {
      const Value* td = member(*params, "textDocument");
      const Value* pos = member(*params, "position");
      const std::string uri = td ? member_str(*td, "uri") : "";
      const long l = pos ? member_int(*pos, "line") : 0;
      const long ch = pos ? member_int(*pos, "character") : 0;
      SourceFile src(uri_to_path(uri), docs.count(uri) ? docs[uri] : std::string());
      const std::string md = hover(src, static_cast<std::uint32_t>(l + 1),
                                   static_cast<std::uint32_t>(ch + 1));
      if (md.empty()) {
        write_message(out, make_response(id, Value::nulo()));
      } else {
        Value contents = Value::mapa();
        contents.map_ref()->set("kind", Value::texto("markdown"));
        contents.map_ref()->set("value", Value::texto(md));
        Value result = Value::mapa();
        result.map_ref()->set("contents", std::move(contents));
        write_message(out, make_response(id, std::move(result)));
      }
    } else if (method == "textDocument/definition" && params) {
      const Value* td = member(*params, "textDocument");
      const Value* pos = member(*params, "position");
      const std::string uri = td ? member_str(*td, "uri") : "";
      const long l = pos ? member_int(*pos, "line") : 0;
      const long ch = pos ? member_int(*pos, "character") : 0;
      SourceFile src(uri_to_path(uri), docs.count(uri) ? docs[uri] : std::string());
      const auto external = imported_definition(uri, std::string(src.text()), l, ch, docs);
      const Span def = external ? external->span
          : definition(src, static_cast<std::uint32_t>(l + 1),
                       static_cast<std::uint32_t>(ch + 1));
      if (def.length == 0 && def.line == 0) {
        write_message(out, make_response(id, Value::nulo()));
      } else {
        Value result = Value::mapa();
        result.map_ref()->set("uri", Value::texto(external ? external->uri : uri));
        result.map_ref()->set("range", span_to_range(def));
        write_message(out, make_response(id, std::move(result)));
      }
    } else if (method == "textDocument/references" && params) {
      const Value* td = member(*params, "textDocument");
      const Value* pos = member(*params, "position");
      const Value* context = member(*params, "context");
      const std::string uri = td ? member_str(*td, "uri") : "";
      const long l = pos ? member_int(*pos, "line") : 0;
      const long ch = pos ? member_int(*pos, "character") : 0;
      bool include_declaration = false;
      if (context) {
        const Value* include = member(*context, "includeDeclaration");
        include_declaration = include && include->kind == rt::ValueKind::Logico && include->b;
      }
      SourceFile src(uri_to_path(uri), docs.count(uri) ? docs[uri] : std::string());
      const auto refs = references(src, static_cast<std::uint32_t>(l + 1),
                                   static_cast<std::uint32_t>(ch + 1), include_declaration);
      Value result = Value::lista();
      for (const Span& ref : refs) {
        Value location = Value::mapa();
        location.map_ref()->set("uri", Value::texto(uri));
        location.map_ref()->set("range", span_to_range(ref));
        result.list_ref()->push_back(std::move(location));
      }
      write_message(out, make_response(id, std::move(result)));
    } else if (method == "textDocument/rename" && params) {
      const Value* td = member(*params, "textDocument");
      const Value* pos = member(*params, "position");
      const std::string uri = td ? member_str(*td, "uri") : "";
      const std::string new_name = member_str(*params, "newName");
      const long l = pos ? member_int(*pos, "line") : 0;
      const long ch = pos ? member_int(*pos, "character") : 0;
      SourceFile src(uri_to_path(uri), docs.count(uri) ? docs[uri] : std::string());
      const auto refs = valid_identifier(new_name)
                            ? references(src, static_cast<std::uint32_t>(l + 1),
                                         static_cast<std::uint32_t>(ch + 1), true)
                            : std::vector<Span>();
      if (refs.empty()) {
        write_message(out, make_response(id, Value::nulo()));
      } else {
        Value edits = Value::lista();
        for (const Span& ref : refs) {
          Value edit = Value::mapa();
          edit.map_ref()->set("range", span_to_range(ref));
          edit.map_ref()->set("newText", Value::texto(new_name));
          edits.list_ref()->push_back(std::move(edit));
        }
        Value changes = Value::mapa();
        changes.map_ref()->set(uri, std::move(edits));
        Value result = Value::mapa();
        result.map_ref()->set("changes", std::move(changes));
        write_message(out, make_response(id, std::move(result)));
      }
    } else if (method == "textDocument/signatureHelp" && params) {
      const Value* td = member(*params, "textDocument");
      const Value* pos = member(*params, "position");
      const std::string uri = td ? member_str(*td, "uri") : "";
      const long l = pos ? member_int(*pos, "line") : 0;
      const long ch = pos ? member_int(*pos, "character") : 0;
      SourceFile src(uri_to_path(uri), docs.count(uri) ? docs[uri] : std::string());
      const SigHelp help = signature_help(src, static_cast<std::uint32_t>(l + 1),
                                          static_cast<std::uint32_t>(ch + 1));
      if (!help.found) {
        write_message(out, make_response(id, Value::nulo()));
      } else {
        Value sig0 = Value::mapa();
        sig0.map_ref()->set("label", Value::texto(help.label));
        Value ps = Value::lista();
        for (const std::string& p : help.params) {
          Value pi = Value::mapa();
          pi.map_ref()->set("label", Value::texto(p));
          ps.list_ref()->push_back(std::move(pi));
        }
        sig0.map_ref()->set("parameters", std::move(ps));
        Value sigs = Value::lista();
        sigs.list_ref()->push_back(std::move(sig0));
        Value result = Value::mapa();
        result.map_ref()->set("signatures", std::move(sigs));
        result.map_ref()->set("activeSignature", Value::inteiro(0));
        result.map_ref()->set("activeParameter", Value::inteiro(static_cast<long>(help.active_parameter)));
        write_message(out, make_response(id, std::move(result)));
      }
    } else if (method == "textDocument/formatting" && params) {
      const Value* td = member(*params, "textDocument");
      const std::string uri = td ? member_str(*td, "uri") : "";
      const std::string text = docs.count(uri) ? docs[uri] : std::string();
      const std::string formatted = format_document(text);
      if (formatted == text) {
        write_message(out, make_response(id, Value::nulo()));
      } else {
        SourceFile src(uri_to_path(uri), text);
        Value start = Value::mapa();
        start.map_ref()->set("line", Value::inteiro(0));
        start.map_ref()->set("character", Value::inteiro(0));
        Value end = Value::mapa();
        end.map_ref()->set("line", Value::inteiro(static_cast<long>(src.line_count())));
        end.map_ref()->set("character", Value::inteiro(0));
        Value range = Value::mapa();
        range.map_ref()->set("start", std::move(start));
        range.map_ref()->set("end", std::move(end));
        Value edit = Value::mapa();
        edit.map_ref()->set("range", std::move(range));
        edit.map_ref()->set("newText", Value::texto(formatted));
        Value edits = Value::lista();
        edits.list_ref()->push_back(std::move(edit));
        write_message(out, make_response(id, std::move(edits)));
      }
    } else if (!method.empty() && idp) {
      // Unknown request: reply with an empty result so the client doesn't hang.
      write_message(out, make_response(id, Value::nulo()));
    }
  }
  return 0;
}

}  // namespace tilt::lsp
