#include "cli/dev_cmds.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>

#include "common/source.hpp"
#include "diagnostics/diagnostic.hpp"
#include "interp/interpreter.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "semantic/checker.hpp"
#include "tilt/version.hpp"

namespace tilt {

namespace {

constexpr int kOk = 0;
constexpr int kFalhas = 1;
constexpr int kUso = 2;

// Arquivos .tilt de cada caminho: o proprio arquivo, ou (diretorio) todos os
// .tilt abaixo dele em ordem alfabetica, sem entrar em pastas ocultas.
std::vector<std::string> coletar_arquivos(const std::vector<std::string>& caminhos,
                                          std::string& erro) {
  namespace fs = std::filesystem;
  std::vector<std::string> out;
  for (const std::string& c : caminhos) {
    std::error_code ec;
    if (fs::is_directory(c, ec)) {
      std::vector<std::string> achados;
      for (fs::recursive_directory_iterator
               it(c, fs::directory_options::skip_permission_denied, ec),
           fim;
           it != fim; it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        const std::string nome = p.filename().string();
        if (it->is_directory(ec) && !nome.empty() && nome[0] == '.') {
          it.disable_recursion_pending();
          continue;
        }
        if (it->is_regular_file(ec) && p.extension() == ".tilt") achados.push_back(p.string());
      }
      std::sort(achados.begin(), achados.end());
      out.insert(out.end(), achados.begin(), achados.end());
    } else if (fs::is_regular_file(c, ec)) {
      out.push_back(c);
    } else {
      erro = "caminho '" + c + "' nao existe";
      return {};
    }
  }
  return out;
}

bool tem_teste(const ast::Program& program) {
  return std::any_of(program.items.begin(), program.items.end(), [](const ast::ItemPtr& it) {
    return it && it->kind == ast::ItemKind::Decl && it->key == "teste";
  });
}

}  // namespace

// ------------------------------------------------------------------ testar

int cmd_testar(const std::vector<std::string_view>& args) {
  std::vector<std::string> caminhos;
  std::string filtro;
  bool verboso = false;
  for (std::size_t k = 1; k < args.size(); ++k) {
    if (args[k] == "--filtro" && k + 1 < args.size()) {
      filtro = std::string(args[++k]);
    } else if (args[k] == "--verboso" || args[k] == "-v") {
      verboso = true;
    } else if (args[k].rfind("-", 0) == 0) {
      std::cerr << "tilt: opcao desconhecida '" << args[k] << "'\n";
      return kUso;
    } else {
      caminhos.emplace_back(args[k]);
    }
  }
  if (caminhos.empty()) caminhos.emplace_back(".");

  std::string erro;
  const std::vector<std::string> arquivos = coletar_arquivos(caminhos, erro);
  if (!erro.empty()) {
    std::cerr << "tilt: " << erro << "\n";
    return kUso;
  }

  int total = 0;
  int falhas = 0;
  for (const std::string& arquivo : arquivos) {
    std::optional<SourceFile> src;
    try {
      src = SourceFile::load(arquivo);
    } catch (const std::exception& e) {
      std::cerr << "tilt: " << e.what() << "\n";
      ++total;
      ++falhas;
      continue;
    }
    DiagnosticEngine diag(&src.value());
    Lexer lexer(src.value(), diag);
    const std::vector<Token> tokens = lexer.tokenize();
    Parser parser(tokens, diag);
    const ast::Program program = parser.parse_program();
    check_program(program, diag);
    if (diag.has_errors()) {
      // Um arquivo com erros so entra na conta se declara testes.
      if (!tem_teste(program)) continue;
      ++total;
      ++falhas;
      std::cout << "  FALHOU  " << arquivo << " (erros de sintaxe/tipos; rode `tilt checar`)\n";
      diag.render(std::cout, false);
      continue;
    }
    if (!tem_teste(program)) continue;

    std::ostringstream saida;
    Interpreter interp(program, diag, saida);
    interp.set_entry_dir(std::filesystem::path(arquivo).parent_path().string());
    interp.run_testes(filtro, [&](const Interpreter::ResultadoTeste& r) {
      ++total;
      if (!r.ok) ++falhas;
      std::cout << (r.ok ? "  ok      " : "  FALHOU  ") << arquivo << "::" << r.nome << "\n";
      if (!r.ok) std::cout << "          " << r.mensagem << "\n";
      const std::string texto = saida.str();
      if ((!r.ok || verboso) && !texto.empty()) {
        std::istringstream linhas(texto);
        std::string linha;
        while (std::getline(linhas, linha)) std::cout << "          | " << linha << "\n";
      }
      saida.str("");
      saida.clear();
    });
  }

  if (total == 0) {
    std::cout << "nenhum teste encontrado (declare `teste nome:` com `passos:` e `afirmar`)\n";
    return kOk;
  }
  std::cout << total << " teste(s): " << (total - falhas) << " ok, " << falhas << " falhou(ram)\n";
  return falhas == 0 ? kOk : kFalhas;
}

// ---------------------------------------------------------------- formatar

std::string formatar_fonte(const std::string& fonte) {
  std::vector<std::string> linhas;
  {
    std::string atual;
    for (std::size_t i = 0; i < fonte.size(); ++i) {
      if (fonte[i] == '\r' && i + 1 < fonte.size() && fonte[i + 1] == '\n') continue;
      if (fonte[i] == '\n') {
        linhas.push_back(std::move(atual));
        atual.clear();
      } else {
        atual += fonte[i];
      }
    }
    if (!atual.empty()) linhas.push_back(std::move(atual));
  }

  std::vector<std::string> saida;
  bool em_texto = false;  // dentro de """ ... """: o conteudo e do usuario
  int em_branco = 0;
  for (std::string& linha : linhas) {
    bool aberto_antes = em_texto;
    std::size_t pos = 0;
    while ((pos = linha.find("\"\"\"", pos)) != std::string::npos) {
      em_texto = !em_texto;
      pos += 3;
    }
    // Espacos no fim so sao do usuario quando a linha termina dentro de um texto
    // """...""": a linha que fecha o texto e limpa depois do delimitador.
    if (!em_texto) {
      while (!linha.empty() && (linha.back() == ' ' || linha.back() == '\t')) linha.pop_back();
    }
    if (linha.empty() && !aberto_antes) {
      if (saida.empty()) continue;  // sem linhas em branco no inicio
      if (++em_branco > 2) continue;
    } else {
      em_branco = 0;
    }
    saida.push_back(std::move(linha));
  }
  while (!saida.empty() && saida.back().empty()) saida.pop_back();

  std::string out;
  for (const std::string& l : saida) {
    out += l;
    out += '\n';
  }
  return out;
}

int cmd_formatar(const std::vector<std::string_view>& args) {
  std::vector<std::string> caminhos;
  bool verificar = false;
  bool para_stdout = false;
  for (std::size_t k = 1; k < args.size(); ++k) {
    if (args[k] == "--verificar") {
      verificar = true;
    } else if (args[k] == "--stdout") {
      para_stdout = true;
    } else if (args[k].rfind("-", 0) == 0) {
      std::cerr << "tilt: opcao desconhecida '" << args[k] << "'\n";
      return kUso;
    } else {
      caminhos.emplace_back(args[k]);
    }
  }
  if (caminhos.empty()) {
    std::cerr << "tilt: uso: tilt formatar <arquivo|diretorio>... [--verificar] [--stdout]\n";
    return kUso;
  }
  std::string erro;
  const std::vector<std::string> arquivos = coletar_arquivos(caminhos, erro);
  if (!erro.empty()) {
    std::cerr << "tilt: " << erro << "\n";
    return kUso;
  }

  int mudariam = 0;
  for (const std::string& arquivo : arquivos) {
    std::ifstream in(arquivo, std::ios::binary);
    if (!in) {
      std::cerr << "tilt: nao foi possivel abrir '" << arquivo << "'\n";
      return kUso;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string original = ss.str();
    const std::string formatado = formatar_fonte(original);
    if (para_stdout) {
      std::cout << formatado;
      continue;
    }
    if (formatado == original) continue;
    ++mudariam;
    if (verificar) {
      std::cout << "precisa formatar: " << arquivo << "\n";
      continue;
    }
    std::ofstream out(arquivo, std::ios::binary | std::ios::trunc);
    out << formatado;
    if (!out) {
      std::cerr << "tilt: nao foi possivel gravar '" << arquivo << "'\n";
      return kUso;
    }
    std::cout << "formatado: " << arquivo << "\n";
  }
  if (verificar && mudariam > 0) {
    std::cout << mudariam << " arquivo(s) precisam de formatacao\n";
    return kFalhas;
  }
  return kOk;
}

// -------------------------------------------------------------------- repl

namespace {

std::string aparar_repl(const std::string& s) {
  std::size_t a = 0;
  std::size_t b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// Declaracoes de topo aceitas direto no REPL (o resto vira passo de um pipeline).
bool eh_declaracao(const std::string& linha) {
  static const char* const kDecl[] = {"funcao",    "tipo",   "importar", "de",         "seja",
                                      "constante", "llm",    "indice",   "ferramenta", "agente",
                                      "equipe",    "modelo", "fonte"};
  std::size_t fim = 0;
  while (fim < linha.size() &&
         (std::isalnum(static_cast<unsigned char>(linha[fim])) != 0 || linha[fim] == '_')) {
    ++fim;
  }
  const std::string primeira = linha.substr(0, fim);
  for (const char* d : kDecl) {
    if (primeira == d) return true;
  }
  return false;
}

const ast::Block* bloco_passos(const ast::Program& programa) {
  for (const auto& item : programa.items) {
    if (!item || item->kind != ast::ItemKind::Decl || item->key != "pipeline" || !item->block) {
      continue;
    }
    for (const auto& f : item->block->items) {
      if (f && f->kind == ast::ItemKind::Field && f->key == "passos" && f->block) {
        return f->block.get();
      }
    }
  }
  return nullptr;
}

}  // namespace

int cmd_repl(const std::vector<std::string_view>& args) {
  if (args.size() > 1) {
    std::cerr << "tilt: uso: tilt repl\n";
    return kUso;
  }
#if defined(_WIN32)
  const bool interativo = _isatty(_fileno(stdin)) != 0;
#else
  const bool interativo = isatty(STDIN_FILENO) != 0;
#endif
  // Mantem vivos os fontes e programas da sessao: funcoes, lambdas e entidades
  // apontam para nos da AST.
  std::vector<std::unique_ptr<SourceFile>> fontes;
  std::vector<std::unique_ptr<ast::Program>> programas;
  SourceFile base("repl", "");
  DiagnosticEngine diag_base(&base);
  ast::Program vazio;
  Interpreter interp(vazio, diag_base, std::cout);
  interp.set_entry_dir(".");

  // Faz o parse de um texto e devolve o Program (nullptr + diagnosticos em stderr se falhar).
  auto analisar = [&](const std::string& texto) -> const ast::Program* {
    fontes.push_back(std::make_unique<SourceFile>("repl", texto));
    DiagnosticEngine diag(fontes.back().get());
    Lexer lexer(*fontes.back(), diag);
    const std::vector<Token> tokens = lexer.tokenize();
    Parser parser(tokens, diag);
    auto programa = std::make_unique<ast::Program>(parser.parse_program());
    if (diag.has_errors()) {
      diag.render(std::cerr, false);
      return nullptr;
    }
    programas.push_back(std::move(programa));
    return programas.back().get();
  };

  auto avaliar = [&](const std::string& entrada) {
    if (eh_declaracao(entrada)) {
      const ast::Program* p = analisar(entrada + "\n");
      if (!p) return;
      try {
        interp.repl_registrar(*p);
      } catch (const std::exception& e) {
        std::cerr << "erro[T901]: " << e.what() << "\n";
      }
      return;
    }
    std::string fonte = "pipeline __repl:\n  passos:\n";
    std::istringstream linhas(entrada);
    std::string linha;
    bool primeira = true;
    while (std::getline(linhas, linha)) {
      // `senao`/`capturar` alinham com o `- se`/`- tentar` (4 espacos); o resto do
      // bloco fica 6 espacos a direita (2 alem do texto depois de `- `).
      const bool alinha_com_traco = linha.rfind("senao", 0) == 0 || linha.rfind("capturar", 0) == 0;
      fonte += primeira ? "    - " : (alinha_com_traco ? "    " : "      ");
      fonte += linha + "\n";
      primeira = false;
    }
    const ast::Program* p = analisar(fonte);
    if (!p) return;
    const ast::Block* passos = bloco_passos(*p);
    if (!passos) return;
    std::string erro;
    if (!interp.repl_executar(*passos, true, erro)) {
      std::cout.flush();
      std::cerr << "erro[T901]: " << erro << "\n";
    }
  };

  if (interativo) {
    std::cout << "tilt " << kVersion << " — REPL. `:ajuda` lista os comandos, `:sair` encerra.\n";
  }
  std::string linha;
  std::optional<std::string> pendente;  // linha lida a mais ao fechar um bloco
  while (true) {
    if (pendente) {
      linha = *pendente;
      pendente.reset();
    } else {
      if (interativo) std::cout << "tilt> " << std::flush;
      if (!std::getline(std::cin, linha)) break;
    }
    std::string entrada = aparar_repl(linha);
    if (entrada.empty()) continue;
    if (entrada == ":sair" || entrada == ":q") break;
    if (entrada == ":ajuda") {
      std::cout
          << ":sair               encerra\n"
          << ":carregar <arquivo>  registra as declaracoes (funcao, tipo, llm...) de um .tilt\n"
          << "linha terminada em ':' abre um bloco; linha vazia o fecha\n"
          << "uma expressao solta imprime o valor; variaveis persistem entre linhas\n";
      continue;
    }
    if (entrada.rfind(":carregar ", 0) == 0) {
      const std::string caminho = aparar_repl(entrada.substr(10));
      std::ifstream in(caminho, std::ios::binary);
      if (!in) {
        std::cerr << "tilt: nao foi possivel abrir '" << caminho << "'\n";
        continue;
      }
      std::ostringstream ss;
      ss << in.rdbuf();
      if (const ast::Program* p = analisar(ss.str())) {
        try {
          interp.repl_registrar(*p);
          std::cout << "carregado: " << caminho << "\n";
        } catch (const std::exception& e) {
          std::cerr << "erro[T901]: " << e.what() << "\n";
        }
      }
      continue;
    }
    // Bloco: linha terminada em ':' continua enquanto as proximas forem indentadas
    // (ou `senao`/`capturar`, que continuam o bloco anterior); uma linha vazia ou
    // sem indentacao o fecha — esta ultima vira a proxima entrada.
    if (!entrada.empty() && entrada.back() == ':') {
      std::string mais;
      while (true) {
        if (interativo) std::cout << "...   " << std::flush;
        if (!std::getline(std::cin, mais)) break;
        if (aparar_repl(mais).empty()) break;
        const bool indentada = mais[0] == ' ' || mais[0] == '\t';
        const std::string cabeca = aparar_repl(mais);
        const bool continua_bloco =
            cabeca.rfind("senao", 0) == 0 || cabeca.rfind("capturar", 0) == 0;
        if (!indentada && !continua_bloco) {
          pendente = mais;
          break;
        }
        entrada += "\n" + mais;
      }
    }
    avaliar(entrada);
  }
  return kOk;
}

// -------------------------------------------------------------------- novo

int cmd_novo(const std::vector<std::string_view>& args) {
  namespace fs = std::filesystem;
  if (args.size() != 2 || args[1].rfind("-", 0) == 0) {
    std::cerr << "tilt: uso: tilt novo <nome>\n";
    return kUso;
  }
  const std::string nome(args[1]);
  const bool nome_ok = !nome.empty() && std::all_of(nome.begin(), nome.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '-';
  });
  if (!nome_ok) {
    std::cerr << "tilt: nome de projeto invalido '" << nome
              << "' (use letras, digitos, '_' ou '-')\n";
    return kUso;
  }
  std::error_code ec;
  if (fs::exists(nome, ec) && !fs::is_empty(nome, ec)) {
    std::cerr << "tilt: '" << nome << "' ja existe e nao esta vazio\n";
    return kUso;
  }
  fs::create_directories(nome, ec);
  if (ec) {
    std::cerr << "tilt: nao foi possivel criar '" << nome << "': " << ec.message() << "\n";
    return kUso;
  }

  auto escrever = [&](const std::string& rel, const std::string& conteudo) {
    std::ofstream out(fs::path(nome) / rel, std::ios::binary);
    out << conteudo;
    return static_cast<bool>(out);
  };
  const bool ok =
      escrever("principal.tilt", "# " + nome +
                                     " — rode com `tilt executar principal.tilt`.\n"
                                     "funcao saudar quem, saudacao = \"Ola\":\n"
                                     "  retornar saudacao + \", \" + quem + \"!\"\n"
                                     "\n"
                                     "pipeline principal:\n"
                                     "  passos:\n"
                                     "    - imprimir saudar(\"mundo\")\n") &&
      escrever("testes.tilt",
               "# Rode com `tilt testar`.\n"
               "importar principal\n"
               "\n"
               "teste saudacao_padrao:\n"
               "  passos:\n"
               "    - afirmar_igual(principal.saudar(\"ana\"), \"Ola, ana!\")\n"
               "\n"
               "teste saudacao_personalizada:\n"
               "  passos:\n"
               "    - afirmar_igual(principal.saudar(\"ana\", \"Oi\"), \"Oi, ana!\")\n") &&
      escrever("README.md", "# " + nome +
                                "\n\n"
                                "Projeto Tilt.\n\n"
                                "```sh\n"
                                "tilt executar principal.tilt   # roda o pipeline\n"
                                "tilt testar                    # roda os blocos `teste`\n"
                                "tilt checar principal.tilt     # verifica sintaxe e tipos\n"
                                "tilt formatar .                # normaliza espacos\n"
                                "```\n") &&
      escrever(".gitignore", "*.tiltc\n.env\n");
  if (!ok) {
    std::cerr << "tilt: falha ao gravar os arquivos de '" << nome << "'\n";
    return kUso;
  }
  std::cout << "projeto '" << nome << "' criado:\n"
            << "  " << nome << "/principal.tilt\n"
            << "  " << nome << "/testes.tilt\n"
            << "  " << nome << "/README.md\n"
            << "proximo passo: cd " << nome << " && tilt executar principal.tilt && tilt testar\n";
  return kOk;
}

}  // namespace tilt
