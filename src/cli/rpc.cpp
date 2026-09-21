#include "cli/rpc.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "common/source.hpp"
#include "diagnostics/diagnostic.hpp"
#include "interp/interpreter.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "runtime/json.hpp"
#include "semantic/checker.hpp"
#include "tilt/version.hpp"

namespace tilt {

namespace {

constexpr int kOk = 0;
constexpr int kFalha = 1;
constexpr int kUso = 2;
constexpr int kProtocolo = 1;

using rt::Value;

// Programa carregado e verificado; vive enquanto o servico roda.
struct Programa {
  std::optional<SourceFile> fonte;
  std::optional<DiagnosticEngine> diag;
  ast::Program ast;
};

// Carrega, analisa e checa `caminho`. Devolve false (erros em stderr) se falhar.
bool carregar(const std::string& caminho, Programa& p) {
  try {
    p.fonte = SourceFile::load(caminho);
  } catch (const std::exception& e) {
    std::cerr << "tilt: " << e.what() << "\n";
    return false;
  }
  p.diag.emplace(&p.fonte.value());
  Lexer lexer(p.fonte.value(), *p.diag);
  const std::vector<Token> tokens = lexer.tokenize();
  Parser parser(tokens, *p.diag);
  p.ast = parser.parse_program();
  check_program(p.ast, *p.diag);
  if (p.diag->has_errors()) {
    p.diag->render(std::cerr, false);
    std::cerr << "corrija os erros antes de expor o programa\n";
    return false;
  }
  return true;
}

Value texto(const std::string& s) { return Value::texto(s); }

std::string resposta_erro(const Value* id, const std::string& msg, const std::string& saida) {
  Value m = Value::mapa();
  if (id) m.map->items.emplace_back("id", *id);
  m.map->items.emplace_back("ok", Value::logico(false));
  m.map->items.emplace_back("erro", texto(msg));
  if (!saida.empty()) m.map->items.emplace_back("saida", texto(saida));
  return rt::json_dump_compacto(m);
}

const Value* campo(const Value& m, const std::string& chave) {
  if (m.kind != rt::ValueKind::Mapa || !m.map) return nullptr;
  for (const auto& kv : m.map->items) {
    if (kv.first == chave) return &kv.second;
  }
  return nullptr;
}

std::string banner(Interpreter& interp) {
  Value funcoes = Value::lista();
  for (const Interpreter::FuncaoPublica& f : interp.funcoes_publicas()) {
    Value params = Value::lista();
    for (const std::string& p : f.params) params.list->push_back(texto(p));
    Value item = Value::mapa();
    item.map->items.emplace_back("nome", texto(f.nome));
    item.map->items.emplace_back("params", params);
    funcoes.list->push_back(item);
  }
  Value pipelines = Value::lista();
  for (const std::string& n : interp.pipelines_publicos()) pipelines.list->push_back(texto(n));
  Value m = Value::mapa();
  m.map->items.emplace_back("tilt", texto("rpc"));
  m.map->items.emplace_back("versao", texto(std::string(kVersion)));
  m.map->items.emplace_back("protocolo", Value::inteiro(kProtocolo));
  m.map->items.emplace_back("funcoes", funcoes);
  m.map->items.emplace_back("pipelines", pipelines);
  return rt::json_dump_compacto(m);
}

// Trata uma requisicao; devolve a linha de resposta. `sair` liga quando o
// cliente pediu para encerrar.
std::string tratar(Interpreter& interp, std::ostringstream& saida, const std::string& linha,
                   bool& sair) {
  saida.str("");
  saida.clear();
  Value req;
  try {
    req = rt::json_parse(linha);
  } catch (const std::exception& e) {
    return resposta_erro(nullptr, std::string("requisicao invalida: ") + e.what(), "");
  }
  if (req.kind != rt::ValueKind::Mapa) {
    return resposta_erro(nullptr, "requisicao invalida: esperado um objeto JSON", "");
  }
  const Value* id = campo(req, "id");
  auto ok = [&](const Value* resultado) {
    Value m = Value::mapa();
    if (id) m.map->items.emplace_back("id", *id);
    m.map->items.emplace_back("ok", Value::logico(true));
    if (resultado) m.map->items.emplace_back("resultado", *resultado);
    if (!saida.str().empty()) m.map->items.emplace_back("saida", texto(saida.str()));
    return rt::json_dump_compacto(m);
  };

  if (const Value* s = campo(req, "sair"); s && s->kind == rt::ValueKind::Logico && s->b) {
    sair = true;
    return ok(nullptr);
  }
  if (campo(req, "ping")) return ok(nullptr);
  if (campo(req, "listar")) {
    const Value info = rt::json_parse(banner(interp));
    return ok(&info);
  }
  if (const Value* nome = campo(req, "pipeline")) {
    if (nome->kind != rt::ValueKind::Texto) {
      return resposta_erro(id, "'pipeline' deve ser o nome (texto)", "");
    }
    std::string erro;
    if (!interp.rodar_pipeline_por_nome(nome->s, erro)) return resposta_erro(id, erro, saida.str());
    return ok(nullptr);
  }
  const Value* fn = campo(req, "chamar");
  if (!fn || fn->kind != rt::ValueKind::Texto) {
    return resposta_erro(id, "requisicao sem 'chamar', 'pipeline', 'listar', 'ping' ou 'sair'", "");
  }
  // `lote`: lista de listas de argumentos; uma so ida e volta para N chamadas.
  // O resultado e a lista de resultados; a primeira falha aborta o lote.
  if (const Value* lote = campo(req, "lote")) {
    if (lote->kind != rt::ValueKind::Lista || !lote->list) {
      return resposta_erro(id, "'lote' deve ser uma lista de listas de argumentos", "");
    }
    Value resultados = Value::lista();
    for (std::size_t k = 0; k < lote->list->size(); ++k) {
      const Value& item = (*lote->list)[k];
      if (item.kind != rt::ValueKind::Lista || !item.list) {
        return resposta_erro(id, "'lote' deve ser uma lista de listas de argumentos", "");
      }
      Value um;
      std::string erro;
      if (!interp.chamar_por_nome(fn->s, *item.list, {}, um, erro)) {
        return resposta_erro(id, "item " + std::to_string(k) + " do lote: " + erro, saida.str());
      }
      resultados.list->push_back(std::move(um));
    }
    return ok(&resultados);
  }
  std::vector<Value> args;
  if (const Value* a = campo(req, "args")) {
    if (a->kind != rt::ValueKind::Lista || !a->list) {
      return resposta_erro(id, "'args' deve ser uma lista", "");
    }
    args = *a->list;
  }
  std::vector<std::pair<std::string, Value>> nomeados;
  if (const Value* n = campo(req, "nomeados")) {
    if (n->kind != rt::ValueKind::Mapa || !n->map) {
      return resposta_erro(id, "'nomeados' deve ser um objeto", "");
    }
    for (const auto& kv : n->map->items) nomeados.emplace_back(kv.first, kv.second);
  }
  Value resultado;
  std::string erro;
  if (!interp.chamar_por_nome(fn->s, std::move(args), nomeados, resultado, erro)) {
    return resposta_erro(id, erro, saida.str());
  }
  return ok(&resultado);
}

}  // namespace

int cmd_rpc(const std::vector<std::string_view>& args) {
  if (args.size() != 2 || args[1].rfind("-", 0) == 0) {
    std::cerr << "tilt: uso: tilt rpc <arquivo>\n";
    return kUso;
  }
  const std::string caminho(args[1]);
  Programa prog;
  if (!carregar(caminho, prog)) return kFalha;
  std::ostringstream saida;
  Interpreter interp(prog.ast, *prog.diag, saida);
  interp.set_entry_dir(std::filesystem::path(caminho).parent_path().string());
  std::string erro;
  if (!interp.preparar_chamadas(erro)) {
    std::cerr << "erro[T901]: " << erro << "\n";
    return kFalha;
  }
  std::cout << banner(interp) << "\n" << std::flush;
  std::string linha;
  bool sair = false;
  while (!sair && std::getline(std::cin, linha)) {
    if (linha.empty() || linha.find_first_not_of(" \t\r") == std::string::npos) continue;
    std::cout << tratar(interp, saida, linha, sair) << "\n" << std::flush;
  }
  return kOk;
}

int cmd_chamar(const std::vector<std::string_view>& args) {
  if (args.size() < 3) {
    std::cerr << "tilt: uso: tilt chamar <arquivo> <funcao> [arg-json...]\n";
    return kUso;
  }
  const std::string caminho(args[1]);
  Programa prog;
  if (!carregar(caminho, prog)) return kFalha;
  std::ostringstream saida;
  Interpreter interp(prog.ast, *prog.diag, saida);
  interp.set_entry_dir(std::filesystem::path(caminho).parent_path().string());
  std::string erro;
  if (!interp.preparar_chamadas(erro)) {
    std::cerr << "erro[T901]: " << erro << "\n";
    return kFalha;
  }
  std::vector<Value> valores;
  for (std::size_t k = 3; k < args.size(); ++k) {
    const std::string bruto(args[k]);
    try {
      valores.push_back(rt::json_parse(bruto));
    } catch (const std::exception&) {
      valores.push_back(texto(bruto));  // nao e JSON: texto puro
    }
  }
  Value resultado;
  const bool deu_certo =
      interp.chamar_por_nome(std::string(args[2]), std::move(valores), {}, resultado, erro);
  std::cerr << saida.str();  // stdout fica so com o resultado
  if (!deu_certo) {
    std::cerr << "erro[T901]: " << erro << "\n";
    return kFalha;
  }
  std::cout << rt::json_dump_compacto(resultado) << "\n";
  return kOk;
}

}  // namespace tilt
