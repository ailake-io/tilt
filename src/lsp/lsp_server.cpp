#include "lsp/lsp_server.hpp"

#include <cstdlib>
#include <istream>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/source.hpp"
#include "diagnostics/diagnostic.hpp"
#include "lexer/lexer.hpp"
#include "lsp/completion.hpp"
#include "parser/parser.hpp"
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
  return v.kind == rt::ValueKind::Mapa && v.map ? v.map->find(key) : nullptr;
}

long member_int(const Value& v, const char* key) {
  const Value* m = member(v, key);
  return m ? static_cast<long>(m->as_number()) : 0;
}

std::string member_str(const Value& v, const char* key) {
  const Value* m = member(v, key);
  return m && m->kind == rt::ValueKind::Texto ? m->s : std::string();
}

Value make_response(const Value& id, Value result) {
  Value r = Value::mapa();
  r.map->set("jsonrpc", Value::texto("2.0"));
  if (id.kind != rt::ValueKind::Nulo) r.map->set("id", id);
  r.map->set("result", std::move(result));
  return r;
}

Value span_to_range(const Span& s) {
  const long l = static_cast<long>(s.line) - 1;
  const long ch = static_cast<long>(s.column) - 1;
  const long len = s.length > 0 ? static_cast<long>(s.length) : 1;
  Value start = Value::mapa();
  start.map->set("line", Value::inteiro(l < 0 ? 0 : l));
  start.map->set("character", Value::inteiro(ch < 0 ? 0 : ch));
  Value end = Value::mapa();
  end.map->set("line", Value::inteiro(l < 0 ? 0 : l));
  end.map->set("character", Value::inteiro((ch < 0 ? 0 : ch) + len));
  Value range = Value::mapa();
  range.map->set("start", std::move(start));
  range.map->set("end", std::move(end));
  return range;
}

void publish_diagnostics(std::ostream& out, const std::string& uri, const std::string& text) {
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
    start.map->set("line", Value::inteiro(l < 0 ? 0 : l));
    start.map->set("character", Value::inteiro(ch < 0 ? 0 : ch));
    Value end = Value::mapa();
    end.map->set("line", Value::inteiro(l < 0 ? 0 : l));
    end.map->set("character", Value::inteiro((ch < 0 ? 0 : ch) + len));
    Value range = Value::mapa();
    range.map->set("start", std::move(start));
    range.map->set("end", std::move(end));
    Value item = Value::mapa();
    item.map->set("range", std::move(range));
    item.map->set("severity", Value::inteiro(d.severity == Severity::Error ? 1 : 2));
    item.map->set("code", Value::texto(std::string(diag_code_string(d.code))));
    item.map->set("source", Value::texto("tilt"));
    item.map->set("message", Value::texto(d.message));
    arr.list->push_back(std::move(item));
  }

  Value params = Value::mapa();
  params.map->set("uri", Value::texto(uri));
  params.map->set("diagnostics", std::move(arr));
  Value note = Value::mapa();
  note.map->set("jsonrpc", Value::texto("2.0"));
  note.map->set("method", Value::texto("textDocument/publishDiagnostics"));
  note.map->set("params", std::move(params));
  write_message(out, note);
}

}  // namespace

int run_lsp(std::istream& in, std::ostream& out) {
  std::unordered_map<std::string, std::string> docs;
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
      caps.map->set("textDocumentSync", Value::inteiro(1));  // full sync
      Value comp = Value::mapa();
      Value triggers = Value::lista();
      triggers.list->push_back(Value::texto("."));
      triggers.list->push_back(Value::texto(":"));
      comp.map->set("triggerCharacters", std::move(triggers));
      caps.map->set("completionProvider", std::move(comp));
      caps.map->set("hoverProvider", Value::logico(true));
      caps.map->set("definitionProvider", Value::logico(true));
      caps.map->set("documentFormattingProvider", Value::logico(true));
      Value sig = Value::mapa();
      Value sig_triggers = Value::lista();
      sig_triggers.list->push_back(Value::texto(","));
      sig_triggers.list->push_back(Value::texto("("));
      sig.map->set("triggerCharacters", std::move(sig_triggers));
      caps.map->set("signatureHelpProvider", std::move(sig));
      Value result = Value::mapa();
      result.map->set("capabilities", std::move(caps));
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
        publish_diagnostics(out, uri, docs[uri]);
      }
    } else if (method == "textDocument/didChange" && params) {
      const Value* td = member(*params, "textDocument");
      const Value* changes = member(*params, "contentChanges");
      if (td && changes && changes->kind == rt::ValueKind::Lista && changes->list &&
          !changes->list->empty()) {
        const std::string uri = member_str(*td, "uri");
        docs[uri] = member_str((*changes->list)[0], "text");
        publish_diagnostics(out, uri, docs[uri]);
      }
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
        ci.map->set("label", Value::texto(it.label));
        ci.map->set("kind", Value::inteiro(completion_kind(it.kind)));
        ci.map->set("detail", Value::texto(it.detail));
        arr.list->push_back(std::move(ci));
      }
      Value result = Value::mapa();
      result.map->set("isIncomplete", Value::logico(false));
      result.map->set("items", std::move(arr));
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
        contents.map->set("kind", Value::texto("markdown"));
        contents.map->set("value", Value::texto(md));
        Value result = Value::mapa();
        result.map->set("contents", std::move(contents));
        write_message(out, make_response(id, std::move(result)));
      }
    } else if (method == "textDocument/definition" && params) {
      const Value* td = member(*params, "textDocument");
      const Value* pos = member(*params, "position");
      const std::string uri = td ? member_str(*td, "uri") : "";
      const long l = pos ? member_int(*pos, "line") : 0;
      const long ch = pos ? member_int(*pos, "character") : 0;
      SourceFile src(uri_to_path(uri), docs.count(uri) ? docs[uri] : std::string());
      const Span def = definition(src, static_cast<std::uint32_t>(l + 1),
                                  static_cast<std::uint32_t>(ch + 1));
      if (def.length == 0 && def.line == 0) {
        write_message(out, make_response(id, Value::nulo()));
      } else {
        Value result = Value::mapa();
        result.map->set("uri", Value::texto(uri));
        result.map->set("range", span_to_range(def));
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
        sig0.map->set("label", Value::texto(help.label));
        Value ps = Value::lista();
        for (const std::string& p : help.params) {
          Value pi = Value::mapa();
          pi.map->set("label", Value::texto(p));
          ps.list->push_back(std::move(pi));
        }
        sig0.map->set("parameters", std::move(ps));
        Value sigs = Value::lista();
        sigs.list->push_back(std::move(sig0));
        Value result = Value::mapa();
        result.map->set("signatures", std::move(sigs));
        result.map->set("activeSignature", Value::inteiro(0));
        result.map->set("activeParameter", Value::inteiro(static_cast<long>(help.active_parameter)));
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
        start.map->set("line", Value::inteiro(0));
        start.map->set("character", Value::inteiro(0));
        Value end = Value::mapa();
        end.map->set("line", Value::inteiro(static_cast<long>(src.line_count())));
        end.map->set("character", Value::inteiro(0));
        Value range = Value::mapa();
        range.map->set("start", std::move(start));
        range.map->set("end", std::move(end));
        Value edit = Value::mapa();
        edit.map->set("range", std::move(range));
        edit.map->set("newText", Value::texto(formatted));
        Value edits = Value::lista();
        edits.list->push_back(std::move(edit));
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
