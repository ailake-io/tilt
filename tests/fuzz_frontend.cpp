// Fuzz por mutacao do frontend (lexer -> parser -> checker), em processo.
//
// Pega arquivos .tilt de semente (golden, exemplos, docs), aplica mutacoes
// aleatorias deterministicas (semente fixa: falhas reproduzem) e passa cada
// resultado pelo lexer, pelo parser e pelo verificador semantico. O que se
// procura e crash, hang (o ctest tem TIMEOUT) ou erro de memoria (rode no preset
// `debug` com ASan/UBSan); diagnosticos de erro sao esperados e ignorados. O
// interpretador NAO roda: programas mutados poderiam fazer I/O.
//
// uso: fuzz_frontend <mutacoes-por-arquivo> <arquivo-ou-diretorio>...
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/source.hpp"
#include "diagnostics/diagnostic.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "semantic/checker.hpp"

namespace {

namespace fs = std::filesystem;

std::string ler(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Alfabeto de fragmentos que costumam quebrar parsers de indentacao/YAML.
const char* const kFragmentos[] = {
    "\t",   "  ",      "\n",  "\r\n",  ":",   "- ",    "\"\"\"",    "{{",  "}}",      "..",
    "->",   "[",       "]",   "{",     "}",   "(",     ")",         ",",   "#",       "\\",
    "\"",   "'",       "@",   "\0",    "se ", "senao", "para cada", "em ", "funcao ", "retornar",
    " se ", " senao ", "0..", "1e999", "-",   "**",    "=",         "==",  "!=",      "\xff\xfe"};

std::string mutar(const std::string& base, std::mt19937_64& rng) {
  std::string s = base;
  const int rodadas = 1 + static_cast<int>(rng() % 4);
  for (int r = 0; r < rodadas; ++r) {
    const std::size_t n = s.size();
    switch (rng() % 8) {
      case 0:  // apaga um trecho
        if (n > 0) {
          const std::size_t a = rng() % n;
          s.erase(a, std::min<std::size_t>(1 + rng() % 16, n - a));
        }
        break;
      case 1: {  // insere um fragmento
        const std::size_t a = n == 0 ? 0 : rng() % (n + 1);
        const char* f = kFragmentos[rng() % (sizeof(kFragmentos) / sizeof(kFragmentos[0]))];
        s.insert(a, f,
                 std::char_traits<char>::length(f) == 0 ? 1 : std::char_traits<char>::length(f));
        break;
      }
      case 2:  // troca um byte
        if (n > 0) s[rng() % n] = static_cast<char>(rng() & 0xFF);
        break;
      case 3:  // duplica um trecho
        if (n > 0) {
          const std::size_t a = rng() % n;
          const std::size_t len = std::min<std::size_t>(1 + rng() % 64, n - a);
          s.insert(rng() % (n + 1), s.substr(a, len));
        }
        break;
      case 4:  // trunca
        if (n > 0) s.resize(rng() % n);
        break;
      case 5:  // embaralha a indentacao de uma linha
        if (n > 0) {
          const std::size_t a = s.rfind('\n', rng() % n);
          const std::size_t pos = a == std::string::npos ? 0 : a + 1;
          s.insert(pos, std::string(rng() % 7, rng() % 2 ? ' ' : '\t'));
        }
        break;
      case 6:  // remove uma quebra de linha
        if (n > 0) {
          const std::size_t a = s.find('\n', rng() % n);
          if (a != std::string::npos) s.erase(a, 1);
        }
        break;
      default:  // repete um trecho muitas vezes (aninhamento profundo)
        if (n > 0) {
          const std::size_t a = rng() % n;
          const std::size_t len = std::min<std::size_t>(1 + rng() % 8, n - a);
          std::string trecho = s.substr(a, len), rep;
          for (int i = 0; i < 40; ++i) rep += trecho;
          s.insert(a, rep);
        }
        break;
    }
  }
  return s;
}

// Passa o texto pelo frontend; diagnosticos sao ignorados.
void frontend(const std::string& texto) {
  tilt::SourceFile src("fuzz.tilt", texto);
  tilt::DiagnosticEngine diag(&src);
  tilt::Lexer lexer(src, diag);
  const std::vector<tilt::Token> tokens = lexer.tokenize();
  tilt::Parser parser(tokens, diag);
  const tilt::ast::Program program = parser.parse_program();
  tilt::check_program(program, diag);
  std::ostringstream sink;
  diag.render(sink, false);  // a renderizacao tambem e superficie de bugs
}

// Watchdog: se um caso passa de kLimiteSegundos, grava a entrada em
// fuzz_travou.tilt (diretorio atual) e aborta — um hang do frontend e bug.
constexpr int kLimiteSegundos = 20;
std::atomic<std::uint64_t> g_caso{0};
std::atomic<bool> g_fim{false};
std::atomic<bool> g_valida{false};
std::mutex g_mu;

// Entrada em processamento (protegida por g_mu; string em variavel estatica local
// porque o cpplint proibe std::string global).
std::string& entrada_atual() {
  static std::string* atual = new std::string();
  return *atual;
}

void vigiar() {
  std::uint64_t visto = 0;
  int parado = 0;
  while (!g_fim.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const std::uint64_t agora = g_caso.load();
    parado = agora == visto ? parado + 1 : 0;
    visto = agora;
    if (parado >= kLimiteSegundos && g_valida.load()) {
      std::ofstream out("fuzz_travou.tilt", std::ios::binary);
      {
        std::lock_guard<std::mutex> lk(g_mu);
        out << entrada_atual();
      }
      out.close();
      std::cerr << "fuzz_frontend: caso " << agora << " travou por mais de " << kLimiteSegundos
                << "s; entrada salva em fuzz_travou.tilt\n";
      std::abort();
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "uso: fuzz_frontend <mutacoes-por-arquivo> <arquivo-ou-diretorio>...\n";
    return 2;
  }
  const int mutacoes = std::max(1, std::atoi(argv[1]));
  std::vector<fs::path> arquivos;
  for (int i = 2; i < argc; ++i) {
    std::error_code ec;
    if (fs::is_directory(argv[i], ec)) {
      for (fs::recursive_directory_iterator it(argv[i], ec), fim; !ec && it != fim;
           it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".tilt") {
          arquivos.push_back(it->path());
        }
      }
    } else if (fs::is_regular_file(argv[i], ec)) {
      arquivos.emplace_back(argv[i]);
    }
  }
  std::sort(arquivos.begin(), arquivos.end());
  if (arquivos.empty()) {
    std::cerr << "fuzz_frontend: nenhuma semente .tilt encontrada\n";
    return 2;
  }

  // TILT_FUZZ_SALVAR=<arquivo>: grava cada entrada antes de processa-la, para
  // recuperar o caso de um crash (segfault nao passa pelo catch nem pelo watchdog).
  const char* salvar = std::getenv("TILT_FUZZ_SALVAR");
  std::thread vigia(vigiar);
  std::mt19937_64 rng(0xF022C0DEULL);
  std::size_t casos = 0;
  for (const fs::path& arquivo : arquivos) {
    const std::string base = ler(arquivo);
    frontend(base);  // a propria semente
    ++casos;
    for (int m = 0; m < mutacoes; ++m) {
      const std::string variante = mutar(base, rng);
      {
        std::lock_guard<std::mutex> lk(g_mu);
        entrada_atual() = variante;
      }
      g_valida = true;
      g_caso = casos;
      if (salvar != nullptr && *salvar != '\0') {
        std::ofstream out(salvar, std::ios::binary | std::ios::trunc);
        out << variante;
      }
      try {
        frontend(variante);
      } catch (const std::exception& e) {
        // Excecao escapando do frontend e bug: mostra a entrada que reproduz.
        std::cerr << "fuzz_frontend: excecao '" << e.what() << "' em " << arquivo << " (mutacao "
                  << m << ")\n--- entrada ---\n"
                  << variante << "\n--- fim ---\n";
        return 1;
      }
      ++casos;
    }
  }
  g_fim = true;
  vigia.join();
  std::cout << "fuzz_frontend ok: " << casos << " casos de " << arquivos.size() << " sementes\n";
  return 0;
}
