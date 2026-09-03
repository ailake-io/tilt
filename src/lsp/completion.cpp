#include "lsp/completion.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostics/diagnostic.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"

namespace tilt::lsp {

namespace {

bool ident_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
  if (prefix.size() > s.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

const std::array<std::string_view, 20> kDeclKeywords = {
    "tipo",   "funcao",   "seja",   "constante", "importar", "de",     "fonte",
    "pipeline", "verificar", "modelo", "treino",  "tarefa",   "experimento", "llm",
    "indice", "fluxo",    "ferramenta", "agente", "equipe",  "servico"};

const std::array<std::string_view, 7> kStmtKeywords = {
    "se", "senao", "para cada", "enquanto", "tentar", "capturar", "retornar"};

const std::array<std::string_view, 24> kBuiltins = {
    "imprimir",  "registrar",  "env",        "tamanho",     "contar",  "somar",
    "media",     "min",        "max",        "intervalo",   "dividir", "ler_csv",
    "escrever_csv", "ler_json", "escrever_json", "ler",      "carregador", "perguntar",
    "incorporar", "dividir_texto", "responder", "tensor",   "zeros",   "checar_tilt"};

const std::array<std::string_view, 11> kTableMethods = {
    "filtrar",  "derivar",   "mapear",   "agrupar_por", "selecionar", "ordenar_por",
    "limite",   "primeiros", "distinto", "tamanho",     "inserir"};

const std::array<std::string_view, 12> kTensorMethods = {
    "forma", "matmul", "transposta", "reformar", "relu",  "gelu",
    "tanh",  "softmax", "soma",       "media",    "argmax", "item"};

struct FieldSet {
  std::string_view decl;
  std::vector<std::string_view> fields;
};

const std::vector<FieldSet>& field_sets() {
  static const std::vector<FieldSet> sets = {
      {"tipo", {}},
      {"llm", {"provedor", "modelo", "temperatura", "max_tokens", "chave", "base_url"}},
      {"modelo", {"camadas", "dispositivo", "pesos", "entrada", "arquitetura"}},
      {"treino",
       {"dados", "perda", "otimizador", "epocas", "taxa", "taxa_aprendizado", "lote", "verboso"}},
      {"tarefa", {"entrada", "executar"}},
      {"indice", {"embeddings", "armazenamento", "dimensao", "metrica"}},
      {"fonte", {"tipo", "caminho", "arquivo", "url", "consulta", "formato", "brokers", "topico"}},
      {"pipeline", {"passos", "agenda", "ao_falhar"}},
      {"fluxo", {"entrada", "passos"}},
      {"ferramenta", {"descricao", "entrada", "executar"}},
      {"agente", {"llm", "papel", "ferramentas", "memoria", "max_passos"}},
      {"equipe", {"agentes", "estrategia", "supervisor", "objetivo"}},
      {"servico", {"porta", "dispositivo", "meio", "rota"}},
      {"verificar", {"nao_nulo", "unico", "intervalo", "ao_violar"}},
  };
  return sets;
}

void push(std::vector<CompletionItem>& out, std::string_view prefix, std::string_view label,
          const char* kind, std::string detail) {
  if (!prefix.empty() && !starts_with_ci(label, prefix)) return;
  for (const auto& e : out) {
    if (e.label == label) return;
  }
  out.push_back({std::string(label), kind, std::move(detail)});
}

// Walk tokens up to the cursor, tracking the stack of enclosing declaration
// keywords via INDENT/DEDENT.
std::string enclosing_decl(const std::vector<Token>& toks, std::uint32_t line, std::uint32_t col) {
  std::vector<std::string> stack;
  std::string line_decl;     // decl keyword at the start of the line being scanned
  std::string pending_open;  // decl of the line that just ended; consumed by the next INDENT
  bool at_line_start = true;

  const auto is_decl = [](std::string_view w) {
    return std::find(kDeclKeywords.begin(), kDeclKeywords.end(), w) != kDeclKeywords.end();
  };

  for (const Token& t : toks) {
    if (t.span.line > line || (t.span.line == line && t.span.column > col)) break;
    switch (t.kind) {
      case TokenKind::Newline:
        pending_open = line_decl;
        line_decl.clear();
        at_line_start = true;
        break;
      case TokenKind::Indent:
        stack.push_back(!pending_open.empty() ? pending_open
                                             : (stack.empty() ? std::string() : stack.back()));
        pending_open.clear();
        at_line_start = true;
        break;
      case TokenKind::Dedent:
        if (!stack.empty()) stack.pop_back();
        at_line_start = true;
        break;
      case TokenKind::Identifier:
        if (at_line_start) {
          line_decl = is_decl(t.lexeme) ? std::string(t.lexeme) : std::string();
          at_line_start = false;
        }
        break;
      default:
        at_line_start = false;
        break;
    }
  }
  for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
    if (!it->empty()) return *it;
  }
  return {};
}

}  // namespace

std::vector<CompletionItem> complete(const SourceFile& src, std::uint32_t line, std::uint32_t column) {
  std::vector<CompletionItem> out;

  const std::string_view text = src.line_text(line);
  std::uint32_t c = column > 0 ? column - 1 : 0;
  if (c > text.size()) c = static_cast<std::uint32_t>(text.size());

  std::uint32_t start = c;
  while (start > 0 && ident_char(text[start - 1])) --start;
  const std::string_view prefix = text.substr(start, c - start);
  const bool dot = start > 0 && text[start - 1] == '.';
  const bool at_line_head = text.substr(0, start).find_first_not_of(" \t") == std::string_view::npos;

  DiagnosticEngine diag(&src);
  Lexer lexer(src, diag);
  const std::vector<Token> toks = lexer.tokenize();
  // A line that begins at column 1 can only hold a top-level declaration,
  // regardless of DEDENT tokens the lexer defers to end-of-file.
  const bool top_level = at_line_head && start == 0;
  const std::string decl = top_level ? std::string() : enclosing_decl(toks, line, column);

  if (dot) {
    for (auto m : kTableMethods) push(out, prefix, m, "method", "metodo de tabela/lista");
    for (auto m : kTensorMethods) push(out, prefix, m, "method", "metodo de tensor");
    push(out, prefix, "executar", "method", "modelo/ferramenta");
    push(out, prefix, "responder", "method", "agente");
    push(out, prefix, "buscar", "method", "indice");
    push(out, prefix, "texto", "method", "resposta de LLM");
    return out;
  }

  if (top_level || (at_line_head && decl.empty())) {
    for (auto k : kDeclKeywords) push(out, prefix, k, "keyword", "declaracao de topo");
    return out;
  }

  if (at_line_head && !decl.empty()) {
    for (const auto& fs : field_sets()) {
      if (fs.decl == decl) {
        for (auto f : fs.fields) push(out, prefix, f, "field", "campo de '" + decl + "'");
      }
    }
    for (auto k : kStmtKeywords) push(out, prefix, k, "keyword", "instrucao");
  }

  for (auto b : kBuiltins) push(out, prefix, b, "builtin", "funcao embutida");
  for (auto k : kStmtKeywords) push(out, prefix, k, "keyword", "instrucao");

  // Names declared in this file.
  Parser parser(toks, diag);
  const ast::Program prog = parser.parse_program();
  for (const auto& it : prog.items) {
    if (!it || it->kind != ast::ItemKind::Decl) continue;
    if (it->header.empty() || !it->header[0] || it->header[0]->kind != ast::ExprKind::Name) continue;
    push(out, prefix, it->header[0]->text, "name", "declarado em '" + it->key + "'");
  }

  std::sort(out.begin(), out.end(),
            [](const CompletionItem& a, const CompletionItem& b) { return a.label < b.label; });
  return out;
}

}  // namespace tilt::lsp
