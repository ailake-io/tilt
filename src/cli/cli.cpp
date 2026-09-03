#include "cli/cli.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
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
#include "codegen/codegen_x86_64.hpp"
#include "interp/interpreter.hpp"
#include "parser/ast_dump.hpp"
#include "parser/parser.hpp"
#include "runtime/json.hpp"
#include "runtime/value.hpp"
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
     << "  referencia                         referencia compacta da linguagem\n"
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

rt::Value diagnostics_to_json(std::string_view path, const DiagnosticEngine& diag) {
  rt::Value out = rt::Value::mapa();
  out.map->set("arquivo", rt::Value::texto(std::string(path)));
  out.map->set("ok", rt::Value::logico(!diag.has_errors()));
  rt::Value arr = rt::Value::lista();
  for (const Diagnostic& d : diag.all()) {
    rt::Value e = rt::Value::mapa();
    e.map->set("codigo", rt::Value::texto(std::string(diag_code_string(d.code))));
    e.map->set("severidade",
               rt::Value::texto(d.severity == Severity::Error     ? "erro"
                                : d.severity == Severity::Warning ? "aviso"
                                                                  : "nota"));
    e.map->set("linha", rt::Value::inteiro(d.span.line));
    e.map->set("coluna", rt::Value::inteiro(d.span.column));
    e.map->set("mensagem", rt::Value::texto(d.message));
    rt::Value notes = rt::Value::lista();
    for (const std::string& n : d.notes) notes.list->push_back(rt::Value::texto(n));
    e.map->set("notas", std::move(notes));
    if (d.suggestion) e.map->set("sugestao", rt::Value::texto(*d.suggestion));
    arr.list->push_back(std::move(e));
  }
  out.map->set("erros", std::move(arr));
  return out;
}

int cmd_checar(const std::vector<std::string_view>& args) {
  std::string_view path;
  bool as_json = false;
  for (std::size_t k = 1; k < args.size(); ++k) {
    if (args[k] == "--json") {
      as_json = true;
    } else if (args[k].rfind("--", 0) == 0) {
      std::cerr << "tilt: opcao desconhecida '" << args[k] << "'\n";
      return kUsage;
    } else if (path.empty()) {
      path = args[k];
    }
  }
  if (path.empty()) {
    std::cerr << "tilt: uso: tilt checar <arquivo> [--json]\n";
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

  if (as_json) {
    std::cout << rt::json_dump(diagnostics_to_json(path, diag));
    return diag.has_errors() ? kDiagnostics : kOk;
  }
  if (diag.has_errors()) {
    diag.render(std::cerr, want_color());
    std::cerr << diag.error_count() << " erro(s) em " << path << "\n";
    return kDiagnostics;
  }
  std::cout << "ok: " << path << " sem erros\n";
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

int cmd_referencia() {
  std::cout <<
      R"(TILT -- referencia compacta (para consumo por ferramentas e assistentes de IA)

DECLARACOES DE TOPO
  tipo Nome:            registro; campos `chave: <tipo> [= padrao]`; uniao "a" | "b"
  funcao f p: T -> R:   funcao; corpo com `retornar`
  seja x = <expr>       variavel de topo;  constante NOME = <expr>
  importar mod          / de mod importar nome
  fonte X:              tipo: csv|json|postgres|kafka|s3 ; caminho:/url: (env "VAR")
  pipeline X:           passos: (lista `-`); agenda: "<cron>"; ao_falhar: repetir N
  verificar (passo):    regras nao_nulo:/unico:/intervalo:  ; ao_violar: abortar|avisar
  modelo X:             camadas: (densa:/linear:[e,s]/ativacao:/softmax/abandono:)
                        dispositivo: auto|cpu|gpu|"cuda:N"
  treino X:             treina `modelo X`; dados: carregador; perda:; otimizador: sgd|adam
  llm X:                provedor:/modelo:/temperatura:/chave: env "..."
  indice X:             embeddings:/armazenamento: "memoria"
  ferramenta X:         descricao:/entrada:/executar:
  agente X:             llm:/papel:/ferramentas:/memoria:/max_passos:
  equipe X:             agentes:/estrategia: sequencial|paralelo
  servico X:            porta:/ rota <metodo> "/rota": entrada:/passos:

EXPRESSOES
  literais: 42  3.14  "texto {{var}}"  verdadeiro/falso  nulo
  operadores: + - * / %   == != < <= > >=   e  ou  nao  contem   |(uniao)
  f(a, b)  (nao ambiguo)   |   f a, b   (estilo declarativo)
  x.campo   x?.campo   x[i]   x[a..b]   [..lista..]   { chave: valor }
  tensor [..]   zeros [..]   uns [..]   aleatorio [..]

INSTRUCOES (passos:/executar:/funcao)
  x = <expr>   |   se .. / senao se .. / senao   |   para cada k em <it>
  enquanto <cond>   |   tentar / capturar e   |   retornar <expr>

BUILTINS
  imprimir registrar env tamanho contar somar media min max intervalo dividir
  ler_csv escrever_csv ler_json escrever_json ler <fonte> carregador
  perguntar perguntar_em_fluxo incorporar dividir_texto  (TILT_LLM=mock offline)
  modelo X.executar <tensor>   |   <indice>.inserir / .buscar
  checar_tilt "<arquivo>"  -> { ok, erros: [{codigo,linha,coluna,mensagem,notas}] }

CLI
  tilt checar <a> [--json]   ast <a>   executar <a> [--agendar]
  tilt servir <a> [--porta N]   compilar <a> --saida <bin> [--asm]
  tilt tokens <a>   referencia   versao

DIAGNOSTICOS (codigo estavel Tnnn)
  T001 indent nao-multiplo-de-2   T002 tab   T003 texto aberto   T004 char invalido
  T010 indent inesperada   T013 token esperado   T014 token inesperado
  T020 segredo literal (use env)   T021 dispositivo invalido
  T030 nome nao definido   T031 referencia desconhecida   T032 declaracao duplicada
  T033 tipo desconhecido   T034 tensor malformado
  T900 conector nao implementado   T901 erro de execucao   T902 recurso nao implementado
  T910 violacao de qualidade de dados
)";
  return kOk;
}

int cmd_compilar(const std::vector<std::string_view>& args) {
  std::string_view path;
  std::string out_bin = "a.out";
  bool keep_asm = false;
  for (std::size_t k = 1; k < args.size(); ++k) {
    if ((args[k] == "--saida" || args[k] == "-o") && k + 1 < args.size()) {
      out_bin = std::string(args[++k]);
    } else if (args[k] == "--asm") {
      keep_asm = true;
    } else if (args[k].rfind("--", 0) == 0) {
      std::cerr << "tilt: opcao desconhecida '" << args[k] << "'\n";
      return kUsage;
    } else if (path.empty()) {
      path = args[k];
    }
  }
  if (path.empty()) {
    std::cerr << "tilt: uso: tilt compilar <arquivo> --saida <bin> [--asm]\n";
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

  codegen::Result r = codegen::emit_program(program);
  if (!r.ok) {
    std::cerr << "tilt: codegen nativo: " << r.error << "\n";
    return kNotImplemented;
  }

  const std::string base = out_bin + ".tilt";
  const std::string asm_path = base + ".s";
  const std::string rt_path = base + ".rt.c";
  {
    std::ofstream a(asm_path);
    a << r.asm_text;
    std::ofstream c(rt_path);
    c << codegen::runtime_source();
  }

  const char* cc_env = std::getenv("CC");
  const std::string cc = cc_env ? cc_env : "cc";
  const std::string cmd = cc + " -O2 -no-pie -o " + out_bin + " " + asm_path + " " + rt_path;
  const int rc = std::system(cmd.c_str());

  if (!keep_asm) {
    std::remove(asm_path.c_str());
    std::remove(rt_path.c_str());
  }
  if (rc != 0) {
    std::cerr << "tilt: falha ao montar/linkar (" << cc << ")\n";
    return kDiagnostics;
  }
  std::cout << "binario nativo: " << out_bin << "\n";
  return kOk;
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
  if (cmd == "referencia" || cmd == "ref") return cmd_referencia();
  if (cmd == "checar") return cmd_checar(args);
  if (cmd == "executar") return cmd_executar(args);
  if (cmd == "servir") return cmd_servir(args);

  if (cmd == "compilar") return cmd_compilar(args);

  std::cerr << "tilt: comando desconhecido '" << cmd << "'\n\n";
  print_usage(std::cerr);
  return kUsage;
}

}  // namespace tilt
