#include "cli/cli.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/source.hpp"
#include "diagnostics/diagnostic.hpp"
#include "lexer/lexer.hpp"
#include "lexer/token.hpp"
#include "interp/interpreter.hpp"
#include "parser/ast_dump.hpp"
#include "parser/parser.hpp"
#include "semantic/checker.hpp"
#include "tilt/version.hpp"

namespace tilt {
namespace {

constexpr int kOk = 0;
constexpr int kDiagnostics = 1;
constexpr int kUsage = 2;
constexpr int kNotImplemented = 3;

bool want_color() { return std::getenv("NO_COLOR") == nullptr && isatty(STDERR_FILENO) != 0; }

void print_usage(std::ostream& os) {
  os << "tilt " << kVersion << "\n\n"
     << "uso: tilt <comando> [argumentos]\n\n"
     << "comandos:\n"
     << "  checar <arquivo>                   verifica sintaxe, indentacao e tipos\n"
     << "  executar <arquivo> [--agendar]     roda o programa no interpretador\n"
     << "  servir <arquivo> [--porta N]       sobe o 'servico' HTTP declarado\n"
     << "  compilar <arquivo> --saida <bin>   gera binario nativo\n"
     << "  tokens <arquivo>                   despeja o fluxo de tokens (debug)\n"
     << "  ast <arquivo>                      despeja a arvore sintatica (debug)\n"
     << "  versao                             mostra a versao\n"
     << "  ajuda                              mostra esta mensagem\n";
}

std::string escape_lexeme(std::string_view s) {
  std::string r;
  r.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '\n':
        r += "\\n";
        break;
      case '\t':
        r += "\\t";
        break;
      case '\r':
        r += "\\r";
        break;
      default:
        r += c;
    }
  }
  return r;
}

// Renders a representative diagnostic so the formatter has a golden test
// before the semantic phases exist. Hidden command: `tilt _diag-demo`.
int cmd_diag_demo() {
  SourceFile src("exemplo.tilt", "tipo Entrada:\n  x: tensor[f32, 4]\n  y: inteiro\n");
  DiagnosticEngine diag(&src);

  Diagnostic d;
  d.severity = Severity::Error;
  d.code = DiagCode::TensorShapeMismatch;
  d.span = Span{/*offset=*/18, /*length=*/14, /*line=*/2, /*column=*/6};
  d.message = "forma de tensor incompativel";
  d.notes = {
      "esperado tensor[f32, _, 1536], encontrado tensor[f32, 4]",
      "a primeira camada 'densa' foi declarada como [1536, 512]",
  };
  d.suggestion = "ajuste a entrada para 1536 colunas ou a camada para [4, 512]";
  diag.report(std::move(d));

  diag.render(std::cerr, /*color=*/false);
  return kDiagnostics;
}

std::optional<SourceFile> load_source(const std::vector<std::string_view>& args,
                                      std::string_view usage) {
  if (args.size() < 2) {
    std::cerr << "tilt: uso: " << usage << "\n";
    return std::nullopt;
  }
  try {
    return SourceFile::load(std::string(args[1]));
  } catch (const std::exception& e) {
    std::cerr << "tilt: " << e.what() << "\n";
    return std::nullopt;
  }
}

int cmd_tokens(const std::vector<std::string_view>& args) {
  std::optional<SourceFile> src = load_source(args, "tilt tokens <arquivo>");
  if (!src) return kUsage;

  DiagnosticEngine diag(&src.value());
  Lexer lexer(src.value(), diag);
  const std::vector<Token> tokens = lexer.tokenize();

  for (const Token& t : tokens) {
    char pos[16];
    std::snprintf(pos, sizeof(pos), "%4u:%-4u", t.span.line, t.span.column);
    std::string line = pos;
    line += ' ';
    line += token_kind_name(t.kind);
    if (!t.lexeme.empty()) {
      if (line.size() < 24) line.append(24 - line.size(), ' ');
      line += '\'';
      line += escape_lexeme(t.lexeme);
      line += '\'';
    }
    std::cout << line << "\n";
  }

  if (diag.has_errors()) {
    diag.render(std::cerr, want_color());
    return kDiagnostics;
  }
  return kOk;
}

int cmd_ast(const std::vector<std::string_view>& args) {
  std::optional<SourceFile> src = load_source(args, "tilt ast <arquivo>");
  if (!src) return kUsage;

  DiagnosticEngine diag(&src.value());
  Lexer lexer(src.value(), diag);
  const std::vector<Token> tokens = lexer.tokenize();
  Parser parser(tokens, diag);
  const ast::Program program = parser.parse_program();

  dump_program(std::cout, program);

  if (diag.has_errors()) {
    diag.render(std::cerr, want_color());
    return kDiagnostics;
  }
  return kOk;
}

int cmd_checar(const std::vector<std::string_view>& args) {
  std::optional<SourceFile> src = load_source(args, "tilt checar <arquivo>");
  if (!src) return kUsage;

  DiagnosticEngine diag(&src.value());
  Lexer lexer(src.value(), diag);
  const std::vector<Token> tokens = lexer.tokenize();
  Parser parser(tokens, diag);
  const ast::Program program = parser.parse_program();
  check_program(program, diag);

  if (diag.has_errors()) {
    diag.render(std::cerr, want_color());
    std::cerr << diag.error_count() << " erro(s) em " << args[1] << "\n";
    return kDiagnostics;
  }
  std::cout << "ok: " << args[1] << " sem erros\n";
  return kOk;
}

int cmd_executar(const std::vector<std::string_view>& args) {
  std::string_view path;
  bool schedule = false;
  for (std::size_t k = 1; k < args.size(); ++k) {
    if (args[k] == "--agendar") {
      schedule = true;
    } else if (args[k].rfind("--", 0) == 0) {
      std::cerr << "tilt: opcao desconhecida '" << args[k] << "'\n";
      return kUsage;
    } else if (path.empty()) {
      path = args[k];
    }
  }
  if (path.empty()) {
    std::cerr << "tilt: uso: tilt executar <arquivo> [--agendar]\n";
    return kUsage;
  }

  std::optional<SourceFile> src;
  try {
    src = SourceFile::load(std::string(path));
  } catch (const std::exception& e) {
    std::cerr << "tilt: " << e.what() << "\n";
    return kUsage;
  }

  DiagnosticEngine diag(&src.value());
  Lexer lexer(src.value(), diag);
  const std::vector<Token> tokens = lexer.tokenize();
  Parser parser(tokens, diag);
  const ast::Program program = parser.parse_program();
  check_program(program, diag);

  if (diag.has_errors()) {
    diag.render(std::cerr, want_color());
    std::cerr << "corrija os erros antes de executar\n";
    return kDiagnostics;
  }

  Interpreter interp(program, diag, std::cout);
  interp.set_schedule_mode(schedule);
  int rc = interp.run();
  if (diag.has_errors()) {
    diag.render(std::cerr, want_color());
    return kDiagnostics;
  }
  return rc == 0 ? kOk : kDiagnostics;
}

int cmd_servir(const std::vector<std::string_view>& args) {
  std::string_view path;
  int port = 0;
  int max_requests = 0;
  for (std::size_t k = 1; k < args.size(); ++k) {
    if (args[k] == "--porta" && k + 1 < args.size()) {
      port = std::atoi(std::string(args[++k]).c_str());
    } else if (args[k] == "--requisicoes" && k + 1 < args.size()) {
      max_requests = std::atoi(std::string(args[++k]).c_str());
    } else if (args[k].rfind("--", 0) == 0) {
      std::cerr << "tilt: opcao desconhecida '" << args[k] << "'\n";
      return kUsage;
    } else if (path.empty()) {
      path = args[k];
    }
  }
  if (path.empty()) {
    std::cerr << "tilt: uso: tilt servir <arquivo> [--porta N] [--requisicoes N]\n";
    return kUsage;
  }

  std::optional<SourceFile> src;
  try {
    src = SourceFile::load(std::string(path));
  } catch (const std::exception& e) {
    std::cerr << "tilt: " << e.what() << "\n";
    return kUsage;
  }

  DiagnosticEngine diag(&src.value());
  Lexer lexer(src.value(), diag);
  const std::vector<Token> tokens = lexer.tokenize();
  Parser parser(tokens, diag);
  const ast::Program program = parser.parse_program();
  check_program(program, diag);
  if (diag.has_errors()) {
    diag.render(std::cerr, want_color());
    return kDiagnostics;
  }

  Interpreter interp(program, diag, std::cout);
  const int rc = interp.serve(port, max_requests);
  if (diag.has_errors()) {
    diag.render(std::cerr, want_color());
    return kDiagnostics;
  }
  return rc == 0 ? kOk : kDiagnostics;
}

}  // namespace

int run_cli(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + (argc > 0 ? 1 : 0), argv + argc);

  if (args.empty()) {
    print_usage(std::cerr);
    return kUsage;
  }

  const std::string_view cmd = args[0];

  if (cmd == "versao" || cmd == "--version" || cmd == "-V") {
    std::cout << "tilt " << kVersion << "\n";
    return kOk;
  }
  if (cmd == "ajuda" || cmd == "--help" || cmd == "-h") {
    print_usage(std::cout);
    return kOk;
  }
  if (cmd == "_diag-demo") return cmd_diag_demo();
  if (cmd == "tokens") return cmd_tokens(args);
  if (cmd == "ast") return cmd_ast(args);
  if (cmd == "checar") return cmd_checar(args);
  if (cmd == "executar") return cmd_executar(args);
  if (cmd == "servir") return cmd_servir(args);

  if (cmd == "compilar") {
    std::cerr << "tilt: comando '" << cmd << "' ainda nao implementado (em desenvolvimento)\n";
    return kNotImplemented;
  }

  std::cerr << "tilt: comando desconhecido '" << cmd << "'\n\n";
  print_usage(std::cerr);
  return kUsage;
}

}  // namespace tilt
