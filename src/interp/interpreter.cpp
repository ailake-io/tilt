#include "interp/interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <ostream>
#include <random>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "runtime/gpu_runtime.hpp"
#include "runtime/http_server.hpp"
#include "runtime/http_client.hpp"
#include "runtime/json.hpp"
#include "runtime/llm.hpp"
#include "runtime/compat.hpp"
#include "runtime/parquet.hpp"
#include "runtime/delta.hpp"
#include "runtime/iceberg.hpp"
#include "runtime/qdrant.hpp"
#include "runtime/redis.hpp"
#include "runtime/kafka.hpp"
#include "runtime/leader.hpp"
#include "runtime/checkpoint.hpp"
#include "runtime/mongo.hpp"
#include "runtime/s3.hpp"
#include "runtime/sqlite.hpp"
#include "runtime/postgres.hpp"
#include "runtime/duckdb.hpp"
#include "runtime/mysql.hpp"
#include "runtime/clickhouse.hpp"
#include "runtime/elasticsearch.hpp"
#include "runtime/livy.hpp"
#include "runtime/pgvector.hpp"
#include "runtime/weaviate.hpp"
#include "runtime/pinecone.hpp"
#include "runtime/chroma.hpp"
#include "runtime/vectorstore.hpp"
#include "semantic/checker.hpp"
#include "vm/compiler.hpp"
#include "vm/vm.hpp"

namespace tilt {

// Estado por thread do pool de rotas (serve()): cada worker executa uma
// rota de ponta a ponta na sua thread, entao a resposta corrente, o flag de
// 'se' e o dispositivo ativo nunca sao compartilhados entre requisicoes.
thread_local RouteResponse* route_resp_ = nullptr;  // non-null only while handling a request
thread_local bool last_if_taken_ = false;
thread_local bool use_gpu_ = false;  // active for the current model/treino call

using ast::Expr;
using ast::ExprKind;
using ast::Item;
using ast::ItemKind;
using ast::Stmt;
using ast::StmtKind;
using rt::Value;
using rt::ValueKind;

namespace {

constexpr std::int64_t kLoopGuard = 5'000'000;

bool word_in(std::string_view w, std::initializer_list<std::string_view> set) {
  for (std::string_view s : set) {
    if (w == s) return true;
  }
  return false;
}

std::string decl_name(const Item& it) {
  if (!it.header.empty() && it.header[0] && it.header[0]->kind == ExprKind::Name) {
    return it.header[0]->text;
  }
  return {};
}

// Diretorio do executavel (para localizar a stdlib ao lado dele).
std::string exe_dir() {
  const std::string exe = rt::tilt_exe_path();
  if (exe.empty()) return {};
  return std::filesystem::path(exe).parent_path().string();
}

// Separa uma lista de diretorios em TILT_STDLIB_PATH (':' no POSIX, ';' no
// Windows). Entradas vazias sao ignoradas.
std::vector<std::string> split_path_list(const char* env_value) {
  std::vector<std::string> out;
  if (!env_value) return out;
  const char sep =
#if defined(_WIN32)
      ';';
#else
      ':';
#endif
  std::string cur;
  std::istringstream ss(env_value);
  while (std::getline(ss, cur, sep)) {
    if (!cur.empty()) out.push_back(cur);
  }
  return out;
}

const Item* find_field(const ast::Block& block, std::string_view key) {
  for (const auto& it : block.items) {
    if (it && it->kind == ItemKind::Field && it->key == key) return it.get();
  }
  return nullptr;
}

Value parse_scalar(const std::string& cell) {
  if (!cell.empty()) {
    char* end = nullptr;
    long long asi = std::strtoll(cell.c_str(), &end, 10);
    if (end && *end == '\0') return Value::inteiro(asi);
    double asd = std::strtod(cell.c_str(), &end);
    if (end && *end == '\0') return Value::decimal(asd);
  }
  return Value::texto(cell);
}

std::vector<std::string> split_csv_line(const std::string& line) {
  std::vector<std::string> out;
  std::string field;
  bool quoted = false;
  for (std::size_t k = 0; k < line.size(); ++k) {
    char c = line[k];
    if (quoted) {
      if (c == '"' && k + 1 < line.size() && line[k + 1] == '"') {
        field += '"';
        ++k;
      } else if (c == '"') {
        quoted = false;
      } else {
        field += c;
      }
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      out.push_back(field);
      field.clear();
    } else if (c != '\r') {
      field += c;
    }
  }
  out.push_back(field);
  return out;
}

}  // namespace

// ------------------------------------------------------------------ Env

Value* Interpreter::Env::lookup(const std::string& name) {
  for (Env* e = this; e; e = e->parent) {
    auto it = e->vars.find(name);
    if (it != e->vars.end()) return &it->second;
  }
  return nullptr;
}

void Interpreter::Env::set(const std::string& name, Value value) {
  for (Env* e = this; e; e = e->parent) {
    auto it = e->vars.find(name);
    if (it != e->vars.end()) {
      it->second = std::move(value);
      return;
    }
  }
  vars[name] = std::move(value);
}

// ------------------------------------------------------------------ setup

Interpreter::Interpreter(const ast::Program& program, DiagnosticEngine& diag, std::ostream& out)
    : program_(program), diag_(diag), out_(out) {}

void Interpreter::fail(Span span, std::string message, DiagCode code) {
  throw RuntimeAbort{span, std::move(message), code, {}};
}

void Interpreter::register_decls() {
  for (const auto& item : program_.items) {
    if (!item || item->kind != ItemKind::Decl) continue;
    const std::string& kw = item->key;
    const std::string name = decl_name(*item);
    if (kw == "seja" || kw == "constante") {
      if (!name.empty()) root_.vars[name] = item->value ? eval(*item->value, root_) : Value::nulo();
    } else if (kw == "funcao") {
      if (!name.empty()) functions_[name] = item.get();
    } else if (kw == "importar") {
      for (const auto& h : item->header) {
        if (h && h->kind == ExprKind::Name && h->text != "importar") {
          load_module(h->text, entry_dir_.empty() ? "." : entry_dir_, item->span);
        }
      }
    } else if (kw == "de") {
      // `de <modulo> importar <nome>...`: primeiro nome e o modulo, os
      // demais (exceto a palavra 'importar') entram no escopo principal.
      std::string module_name;
      std::vector<std::string> imported;
      for (const auto& h : item->header) {
        if (!h || h->kind != ExprKind::Name || h->text == "importar") continue;
        if (module_name.empty()) {
          module_name = h->text;
        } else {
          imported.push_back(h->text);
        }
      }
      if (module_name.empty()) continue;
      const std::string from = entry_dir_.empty() ? "." : entry_dir_;
      std::shared_ptr<Module> mod = load_module(module_name, from, item->span);
      for (const std::string& n : imported) {
        if (auto f = mod->funcs.find(n); f != mod->funcs.end()) {
          functions_[n] = f->second;
          func_module_[f->second] = mod;
        } else if (auto v = mod->scope.vars.find(n); v != mod->scope.vars.end()) {
          root_.vars[n] = v->second;
        } else {
          std::string exports;
          for (const auto& kv : mod->funcs) exports += (exports.empty() ? "" : ", ") + kv.first;
          for (const auto& kv : mod->scope.vars) exports += (exports.empty() ? "" : ", ") + kv.first;
          fail(item->span, "modulo '" + module_name + "' nao exporta '" + n + "'" +
                               (exports.empty() ? " (modulo vazio)"
                                                : " (exporta: " + exports + ")"));
        }
      }
    } else if (kw == "pipeline") {
      pipelines_.push_back(item.get());
    } else if (kw == "treino") {
      // handled by the dedicated training loop; must not shadow `modelo <name>`
    } else if (!name.empty()) {
      entities_[name] = item.get();
    }
  }
}

// ------------------------------------------------------------------ modules
//
// `importar io` carrega `io.tilt` procurando: (1) ao lado do arquivo que
// esta importando; (2) cada diretorio em TILT_STDLIB_PATH; (3) `stdlib/` ao
// lado do binario; (4) `<exe>/../share/tilt/stdlib` (layout de instalacao).
// O erro lista todos os caminhos tentados.

std::vector<std::string> Interpreter::stdlib_dirs() const {
  std::vector<std::string> dirs = split_path_list(std::getenv("TILT_STDLIB_PATH"));
  const std::string exe = exe_dir();
  if (!exe.empty()) {
    dirs.push_back((std::filesystem::path(exe) / "stdlib").string());
    dirs.push_back((std::filesystem::path(exe) / ".." / "share" / "tilt" / "stdlib").string());
  }
  return dirs;
}

std::shared_ptr<Interpreter::Module> Interpreter::load_module(const std::string& name,
                                                              const std::string& from_dir,
                                                              Span span) {
  if (auto it = modules_.find(name); it != modules_.end()) return it->second;

  std::vector<std::string> tried;
  std::string path;
  auto probe = [&](const std::string& dir) {
    const std::filesystem::path p =
        dir.empty() ? std::filesystem::path(name + ".tilt")
                    : std::filesystem::path(dir) / (name + ".tilt");
    tried.push_back(p.string());
    std::error_code ec;
    if (path.empty() && std::filesystem::is_regular_file(p, ec) && !ec) path = p.string();
  };
  probe(from_dir);
  for (const std::string& dir : stdlib_dirs()) probe(dir);
  if (path.empty()) {
    std::string where;
    for (const std::string& t : tried) where += (where.empty() ? "" : ", ") + std::string("'") + t + "'";
    fail(span, "modulo '" + name + "' nao encontrado; procurei em " + where);
  }

  if (auto cached = modules_by_path_.find(path); cached != modules_by_path_.end()) {
    modules_[name] = cached->second;
    return cached->second;
  }
  if (loading_modules_.count(path)) {
    fail(span, "ciclo de importacao envolvendo o modulo '" + name + "' (" + path + ")");
  }
  loading_modules_.insert(path);

  SourceFile src = SourceFile::load(path);  // existe: is_regular_file acima
  DiagnosticEngine md(&src);
  Lexer lexer(src, md);
  const std::vector<Token> tokens = lexer.tokenize();
  Parser parser(tokens, md);
  ast::Program program = parser.parse_program();
  check_program(program, md);
  if (md.has_errors()) {
    loading_modules_.erase(path);
    const Diagnostic& first = md.all().front();
    fail(span, "modulo '" + name + "' (" + path + ":" + std::to_string(first.span.line) +
                   ") tem erros: " + first.message);
  }

  auto mod = std::make_shared<Module>();
  mod->path = path;
  mod->program = std::make_shared<ast::Program>(std::move(program));
  mod->scope.parent = &root_;
  const std::string mod_dir = std::filesystem::path(path).parent_path().string();

  // Registra o conteudo: 'funcao' vira exportacao (e fica visivel para as
  // irmas via scope.funcs), 'seja'/'constante' entram no escopo do modulo,
  // 'importar'/'de ... importar' aninhados resolvem primeiro (as funcoes do
  // modulo podem usa-los).
  for (const auto& item : mod->program->items) {
    if (!item || item->kind != ItemKind::Decl) continue;
    const std::string iname = decl_name(*item);
    if (item->key == "funcao") {
      if (!iname.empty()) mod->funcs[iname] = item.get();
    } else if (item->key == "seja" || item->key == "constante") {
      if (!iname.empty()) {
        mod->scope.vars[iname] = item->value ? eval(*item->value, mod->scope) : Value::nulo();
      }
    } else if (item->key == "importar") {
      for (const auto& h : item->header) {
        if (h && h->kind == ExprKind::Name && h->text != "importar") {
          load_module(h->text, mod_dir, item->span);
        }
      }
    } else if (item->key == "de") {
      std::string module_name;
      std::vector<std::string> imported;
      for (const auto& h : item->header) {
        if (!h || h->kind != ExprKind::Name || h->text == "importar") continue;
        if (module_name.empty()) {
          module_name = h->text;
        } else {
          imported.push_back(h->text);
        }
      }
      if (module_name.empty()) continue;
      std::shared_ptr<Module> dep = load_module(module_name, mod_dir, item->span);
      for (const std::string& n : imported) {
        if (auto f = dep->funcs.find(n); f != dep->funcs.end()) {
          mod->funcs[n] = f->second;  // chamavel sem prefixo dentro do modulo
        } else if (auto v = dep->scope.vars.find(n); v != dep->scope.vars.end()) {
          mod->scope.vars[n] = v->second;
        } else {
          fail(item->span, "modulo '" + module_name + "' nao exporta '" + n + "'");
        }
      }
    }
  }
  mod->scope.funcs = &mod->funcs;

  loading_modules_.erase(path);
  modules_by_path_[path] = mod;
  modules_[name] = mod;
  return mod;
}

namespace {

// Extracts N from `ao_falhar: repetir N[, espera: "..."]`. Returns 0 if absent.
int retry_count(const Item& pipeline) {
  const Item* f = find_field(*pipeline.block, "ao_falhar");
  if (!f || !f->value) return 0;
  const Expr* v = f->value.get();
  if (v->kind == ExprKind::Call && v->lhs && v->lhs->kind == ExprKind::Name &&
      v->lhs->text == "repetir" && !v->args.empty() &&
      v->args[0].value->kind == ExprKind::IntLit) {
    return static_cast<int>(std::strtol(v->args[0].value->text.c_str(), nullptr, 10));
  }
  return 0;
}

// ------------------------------------------------------------------ janela
// `janela: N` (IntLit) e janela de contagem; `janela: "30s"` / `"5min"` /
// `"1h"` e janela de tempo (throttle quando sem `entrada:`).
struct JanelaSpec {
  enum Kind { Contagem, Tempo } kind = Contagem;
  long count = 0;         // Contagem: elementos por execucao
  std::time_t dur = 0;    // Tempo: duracao em segundos
  bool valid = false;
  std::string erro;       // motivo da invalidade (mensagem para fail())
};

// "30s" | "5min" | "1h" -> segundos. Falso para qualquer outro formato.
bool parse_duracao(const std::string& s, std::time_t& out) {
  std::size_t i = 0;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
  if (i == 0 || i == s.size()) return false;
  const long n = std::strtol(s.substr(0, i).c_str(), nullptr, 10);
  if (n <= 0) return false;
  const std::string suf = s.substr(i);
  long mult;
  if (suf == "s") mult = 1;
  else if (suf == "min") mult = 60;
  else if (suf == "h") mult = 3600;
  else return false;
  out = static_cast<std::time_t>(n * mult);
  return true;
}

JanelaSpec parse_janela(const Item& field) {
  JanelaSpec spec;
  const Expr* v = field.value.get();
  auto rejeita = [&](std::string msg) {
    spec.valid = false;
    spec.erro = std::move(msg);
    return spec;
  };
  if (!v) return rejeita("espera um inteiro (contagem) ou uma duracao (\"30s\", \"5min\", \"1h\")");
  if (v->kind == ExprKind::IntLit) {
    spec.kind = JanelaSpec::Contagem;
    spec.count = std::strtol(v->text.c_str(), nullptr, 10);
    if (spec.count <= 0) return rejeita("contagem precisa ser um inteiro positivo");
    spec.valid = true;
    return spec;
  }
  if (v->kind == ExprKind::TextLit && parse_duracao(v->text, spec.dur)) {
    spec.kind = JanelaSpec::Tempo;
    spec.valid = true;
    return spec;
  }
  return rejeita("valor invalido; use um inteiro (contagem) ou duracao \"30s\", \"5min\", \"1h\"");
}

// ------------------------------------------------------------------ cron
// Subconjunto de cron de 5 campos: minuto hora dia-do-mes mes dia-da-semana.
// Cada campo aceita: * | n | a-b | a-b/n | */n | listas com virgula.
struct CronRange {
  int a, b, step;
  bool matches(int v) const { return v >= a && v <= b && (v - a) % step == 0; }
};

struct CronField {
  bool any = false;
  bool is_dow = false;
  std::vector<CronRange> ranges;

  bool matches(int v) const {
    if (any) return true;
    for (const CronRange& r : ranges) {
      if (r.matches(v)) return true;
    }
    // domingo: 0 e 7 sao equivalentes
    if (is_dow && v == 0) {
      for (const CronRange& r : ranges) {
        if (r.matches(7)) return true;
      }
    }
    return false;
  }
};

bool parse_cron_field(const std::string& s, int lo, int hi, bool is_dow, CronField& out) {
  out = CronField{};
  out.is_dow = is_dow;
  if (s == "*") {
    out.any = true;
    return true;
  }
  std::size_t pos = 0;
  while (pos <= s.size()) {
    const std::size_t comma = s.find(',', pos);
    const std::string part =
        s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (part.empty()) return false;
    int step = 1;
    std::string range = part;
    if (const std::size_t slash = part.find('/'); slash != std::string::npos) {
      step = std::atoi(part.substr(slash + 1).c_str());
      range = part.substr(0, slash);
    }
    int a, b;
    if (range == "*") {
      a = lo;
      b = hi;
    } else if (const std::size_t dash = range.find('-'); dash != std::string::npos) {
      a = std::atoi(range.substr(0, dash).c_str());
      b = std::atoi(range.substr(dash + 1).c_str());
    } else {
      a = b = std::atoi(range.c_str());
    }
    if (step < 1 || a < lo || a > b || b > hi) return false;
    out.ranges.push_back({a, b, step});
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return !out.ranges.empty();
}

struct CronSpec {
  CronField min, hour, dom, mon, dow;
  bool valid = false;
};

CronSpec parse_cron(const std::string& expr) {
  CronSpec c;
  std::vector<std::string> parts;
  std::size_t pos = 0;
  while (pos <= expr.size()) {
    const std::size_t sp = expr.find_first_of(" \t", pos);
    parts.push_back(expr.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos));
    if (sp == std::string::npos) break;
    pos = expr.find_first_not_of(" \t", sp);
    if (pos == std::string::npos) break;
  }
  c.valid = parts.size() == 5 && parse_cron_field(parts[0], 0, 59, false, c.min) &&
            parse_cron_field(parts[1], 0, 23, false, c.hour) &&
            parse_cron_field(parts[2], 1, 31, false, c.dom) &&
            parse_cron_field(parts[3], 1, 12, false, c.mon) &&
            parse_cron_field(parts[4], 0, 7, true, c.dow);
  return c;
}

std::time_t next_cron_fire(const CronSpec& c, std::time_t after) {
  // arredonda para o inicio do proximo minuto
  std::time_t t = after + 60;
  std::tm local = rt::tilt_localtime(t);
  local.tm_sec = 0;
  t = std::mktime(&local);
  for (int i = 0; i < 366 * 24 * 60; ++i, t += 60) {
    std::tm cur = rt::tilt_localtime(t);
    const int dow = cur.tm_wday;
    if (c.min.matches(cur.tm_min) && c.hour.matches(cur.tm_hour) &&
        c.dom.matches(cur.tm_mday) && c.mon.matches(cur.tm_mon + 1) && c.dow.matches(dow)) {
      return t;
    }
  }
  return -1;
}

std::string format_local(std::time_t t) {
  const std::tm local = rt::tilt_localtime(t);
  char buf[64];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", local.tm_year + 1900,
                local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
  return buf;
}

// Relogio fake para testes: TILT_AGORA="YYYY-MM-DDTHH:MM[:SS]" (hora local;
// tambem aceita espaco no lugar de 'T'). Com relogio fake, o loop nao dorme
// e avanca o tempo sozinho; TILT_AGENDAR_MAX limita o numero de execucoes
// (padrao 1 no modo fake).
bool fake_now(std::time_t& out) {
  const char* env = std::getenv("TILT_AGORA");
  if (!env || !*env) return false;
  std::tm local{};
  int sec = 0;
  if (std::sscanf(env, "%d-%d-%dT%d:%d:%d", &local.tm_year, &local.tm_mon, &local.tm_mday,
                  &local.tm_hour, &local.tm_min, &sec) < 5 &&
      std::sscanf(env, "%d-%d-%d %d:%d:%d", &local.tm_year, &local.tm_mon, &local.tm_mday,
                  &local.tm_hour, &local.tm_min, &sec) < 5) {
    return false;
  }
  local.tm_year -= 1900;
  local.tm_mon -= 1;
  local.tm_sec = sec;
  out = std::mktime(&local);
  return true;
}

}  // namespace
int Interpreter::run() {
  try {
    register_decls();

    bool did_something = false;
    for (const auto& item : program_.items) {
      if (item && item->kind == ItemKind::Decl && item->key == "treino") {
        run_treino(*item);
        did_something = true;
      }
      if (item && item->kind == ItemKind::Decl && item->key == "experimento") {
        run_experimento(*item);
        did_something = true;
      }
    }

    if (!pipelines_.empty()) {
      for (const Item* p : pipelines_) run_pipeline(*p);
    } else if (auto it = functions_.find("principal"); it != functions_.end()) {
      call_function(*it->second, {}, it->second->span);
    } else if (!did_something) {
      out_ << "nada para executar: nenhum 'pipeline', 'treino', 'experimento' nem 'funcao principal'\n";
    }
    return 0;
  } catch (const RuntimeAbort& a) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = a.code;
    d.span = a.span;
    d.message = a.message;
    d.notes = a.notes;
    diag_.report(std::move(d));
    return 1;
  }
}

int Interpreter::run_vm() {
  try {
    register_decls();

    bool did_something = false;
    for (const auto& item : program_.items) {
      if (item && item->kind == ItemKind::Decl && item->key == "treino") {
        run_treino(*item);
        did_something = true;
      }
      if (item && item->kind == ItemKind::Decl && item->key == "experimento") {
        run_experimento(*item);
        did_something = true;
      }
    }

    if (!pipelines_.empty()) {
      std::unordered_set<std::string> names;
      for (const auto& kv : functions_) names.insert(kv.first);
      for (const Item* p : pipelines_) {
        // Pipeline no subconjunto -> bytecode VM; fora dele -> arvore.
        std::shared_ptr<vm::Chunk> chunk;
        try {
          chunk = std::make_shared<vm::Chunk>(vm::compile_pipeline(*p, names));
        } catch (const vm::NotCompilable&) {
          chunk = nullptr;
        }
        if (!chunk) {
          run_pipeline(*p);
          continue;
        }
        out_ << "== pipeline " << decl_name(*p) << " ==\n";
        vm::Vm machine(out_, [this](const std::string& name, std::vector<Value>& a, bool* handled) {
          auto f = functions_.find(name);
          if (f == functions_.end()) {
            *handled = false;
            return Value::nulo();
          }
          *handled = true;
          Env* scope = nullptr;
          if (auto fm = func_module_.find(f->second); fm != func_module_.end()) {
            scope = &fm->second->scope;
          }
          return call_function(*f->second, std::move(a), Span{}, scope);
        });
        try {
          machine.run(*chunk, {});
        } catch (const std::exception& e) {
          fail(p->span, std::string("VM: ") + e.what());
        }
      }
    } else if (auto it = functions_.find("principal"); it != functions_.end()) {
      call_function(*it->second, {}, it->second->span);
    } else if (!did_something) {
      out_ << "nada para executar: nenhum 'pipeline', 'treino', 'experimento' nem 'funcao principal'\n";
    }
    return 0;
  } catch (const RuntimeAbort& a) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = a.code;
    d.span = a.span;
    d.message = a.message;
    d.notes = a.notes;
    diag_.report(std::move(d));
    return 1;
  }
}

int Interpreter::run_scheduled() {
  try {
    register_decls();

    struct Scheduled {
      const Item* pipeline;
      CronSpec cron;
    };
    std::vector<Scheduled> scheduled;
    std::vector<const Item*> immediate;
    for (const Item* p : pipelines_) {
      const Item* ag = p->block ? find_field(*p->block, "agenda") : nullptr;
      if (ag && ag->value && ag->value->kind == ExprKind::TextLit) {
        CronSpec c = parse_cron(ag->value->text);
        if (!c.valid) {
          fail(ag->value->span, "agenda '" + ag->value->text + "' nao e um cron valido de 5 campos");
        }
        scheduled.push_back({p, c});
      } else {
        immediate.push_back(p);
      }
    }

    for (const Item* p : immediate) run_pipeline(*p);
    if (scheduled.empty()) {
      out_ << "nenhuma agenda definida; nada a agendar\n";
      return 0;
    }

    // treinos, experimentos e funcao principal nao entram no loop; rodam uma vez antes
    for (const auto& item : program_.items) {
      if (item && item->kind == ItemKind::Decl && item->key == "treino") run_treino(*item);
      if (item && item->kind == ItemKind::Decl && item->key == "experimento") run_experimento(*item);
    }

    std::time_t now = std::time(nullptr);
    const bool fake = fake_now(now);
    long max_runs = 0;
    if (const char* m = std::getenv("TILT_AGENDAR_MAX")) {
      max_runs = std::atol(m);
    } else if (fake) {
      max_runs = 1;
    }
    if (fake && max_runs <= 0) {
      fail({}, "TILT_AGORA definido sem TILT_AGENDAR_MAX; o relogio fake precisa de limite");
    }

    long fired_total = 0;
    const char* lease_env = std::getenv("TILT_LEADER_LEASE");
    const std::string lease_path = lease_env ? lease_env : "";
    int lease_ttl = 15;
    if (const char* ttl = std::getenv("TILT_LEADER_TTL")) lease_ttl = std::atoi(ttl);
    while (true) {
      // proximo disparo entre todos os pipelines agendados
      std::time_t next = -1;
      for (const Scheduled& s : scheduled) {
        const std::time_t t = next_cron_fire(s.cron, now);
        if (t < 0) {
          fail({}, "agenda sem proxima ocorrencia nos proximos 366 dias");
        }
        if (next < 0 || t < next) next = t;
      }
      out_ << "proxima execucao: " << format_local(next) << "\n";
      if (!fake) {
        const std::time_t cur = std::time(nullptr);
        if (next > cur) {
          std::this_thread::sleep_for(std::chrono::seconds(next - cur));
        }
      }
      now = next;
      // Eleicao de lider (Fase 12-4): com TILT_LEADER_LEASE, so o dono do
      // lease dispara; followers pulam o tick com log.
      if (!lease_path.empty()) {
        std::string motivo;
        if (!rt::leader_tentar(lease_path, lease_ttl, motivo)) {
          out_ << "sem lideranca (" << motivo << "); tick pulado\n";
          ++fired_total;
          if (max_runs > 0 && fired_total >= max_runs) break;
          continue;
        }
      }
      for (const Scheduled& s : scheduled) {
        if (next_cron_fire(s.cron, now - 60) == now) run_pipeline(*s.pipeline, now);
      }
      ++fired_total;
      if (max_runs > 0 && fired_total >= max_runs) break;
    }
    return 0;
  } catch (const RuntimeAbort& a) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = a.code;
    d.span = a.span;
    d.message = a.message;
    d.notes = a.notes;
    diag_.report(std::move(d));
    return 1;
  }
}


void Interpreter::run_pipeline(const Item& pipeline, std::time_t now) {
  out_ << "== pipeline " << decl_name(pipeline) << " ==\n";
  if (!pipeline.block) return;

  if (const Item* ag = find_field(*pipeline.block, "agenda");
      ag && ag->value && ag->value->kind == ExprKind::TextLit) {
    const std::string& cron = ag->value->text;
    if (!parse_cron(cron).valid) {
      fail(ag->value->span, "agenda '" + cron + "' nao e um cron valido de 5 campos");
    }
  }

  const Item* passos = find_field(*pipeline.block, "passos");
  if (!passos || !passos->block) {
    out_ << "  (sem passos)\n";
    return;
  }

  // Streaming com `janela:`: acumula elementos da `entrada:` e so executa os
  // passos quando a janela fecha, com `linhas` = lote consumido.
  std::vector<Value> janela_lotes;
  const Item* janela = find_field(*pipeline.block, "janela");
  if (janela) {
    if (now < 0) {
      std::time_t t;
      now = fake_now(t) ? t : std::time(nullptr);
    }
    if (!run_janela(*janela, pipeline, now, janela_lotes)) {
      out_ << "  (janela nao fechou; passos nao executados)\n";
      return;
    }
  }

  const int retries = retry_count(pipeline);
  for (int attempt = 0; attempt <= retries; ++attempt) {
    try {
      Env env;
      env.parent = &root_;
      if (janela) env.vars["linhas"] = Value::lista(janela_lotes);
      exec_block(*passos->block, env);
      return;
    } catch (const RuntimeAbort& a) {
      if (attempt >= retries) throw;
      out_ << "[retry] passo falhou (" << a.message << "); tentativa " << (attempt + 2) << "/"
           << (retries + 1) << "\n";
    }
  }
}

bool Interpreter::run_janela(const Item& janela, const Item& pipeline, std::time_t now,
                             std::vector<Value>& batch) {
  const JanelaSpec spec = parse_janela(janela);
  if (!spec.valid) fail(janela.span, "janela: " + spec.erro);

  // `sobreposicao: M` (default 0 = tumbling): o lote mantem os M ultimos
  // elementos do lote anterior no inicio de `linhas` — janela deslizante.
  long sobreposicao = 0;
  if (const Item* sp = find_field(*pipeline.block, "sobreposicao")) {
    if (!sp->value || sp->value->kind != ExprKind::IntLit) {
      fail(sp->span, "sobreposicao: espera um inteiro (elementos mantidos entre lotes)");
    }
    sobreposicao = std::strtol(sp->value->text.c_str(), nullptr, 10);
    if (sobreposicao < 0) fail(sp->span, "sobreposicao: precisa ser >= 0");
    if (spec.kind != JanelaSpec::Contagem) {
      fail(sp->span, "sobreposicao: exige janela de contagem (janela: N inteiro)");
    }
    if (sobreposicao >= spec.count) {
      fail(sp->span, "sobreposicao: precisa ser menor que a janela");
    }
  }

  const Item* entrada = find_field(*pipeline.block, "entrada");
  std::string fonte;
  if (entrada) {
    if (!entrada->value || entrada->value->kind != ExprKind::Name) {
      fail(entrada->span, "entrada: espera o nome de uma 'fonte' declarada");
    }
    fonte = entrada->value->text;
    if (!entities_.count(fonte)) {
      fail(entrada->span, "entrada: fonte '" + fonte + "' nao declarada");
    }
  }
  if (spec.kind == JanelaSpec::Contagem && fonte.empty()) {
    fail(janela.span, "janela: contagem exige 'entrada:' (fonte)");
  }

  const std::string pipe_name = decl_name(pipeline);
  WindowState& st = window_states_[pipe_name];
  // Offset persistente: janela de contagem OU de tempo com fonte de arquivo, e
  // nunca com TILT_JANELA_ESTADO=memoria (pipelines efemeros/testes).
  // Fase 12-4: o arquivo mora em TILT_CHECKPOINT_DIR quando configurado.
  std::string offset_file;
  if (!fonte.empty()) {
    const char* estado = std::getenv("TILT_JANELA_ESTADO");
    if (!estado || std::string(estado) != "memoria") {
      offset_file = janela_offset_file(fonte);
      if (!offset_file.empty()) janela_offset_load(st, pipe_name, offset_file);
    }
  }
  if (!fonte.empty()) {
    // Le a fonte inteira a cada tick; so os elementos alem do offset acumulam.
    Value data = read_fonte(fonte, entrada->span);
    if (data.list) {
      if (st.offset > data.list->size()) st.offset = data.list->size();  // fonte encolheu
      for (std::size_t i = st.offset; i < data.list->size(); ++i) {
        st.buffer.push_back((*data.list)[i]);
      }
      st.offset = data.list->size();
    }
    // Grava so quando o offset avanca (nada consumido = sem arquivo novo).
    if (!offset_file.empty() && st.offset > st.persisted_offset) {
      janela_offset_save(st, pipe_name, offset_file);
    }
  }

  bool roda = false;
  if (spec.kind == JanelaSpec::Contagem) {
    // Contagem nao depende do relogio: fecha com novos elementos suficientes.
    // Com sobreposicao M, o lote inteiro tem N elementos mas so (N - M) saem
    // do buffer — os M ultimos repetem no proximo lote.
    const long consome = spec.count - sobreposicao;
    if (st.buffer.size() >= static_cast<std::size_t>(spec.count)) {
      batch.assign(st.buffer.begin(), st.buffer.begin() + spec.count);
      st.buffer.erase(st.buffer.begin(), st.buffer.begin() + consome);
      roda = true;
    }
  } else {
    const bool decorreu = !st.ran_once || (now - st.last_run) >= spec.dur;
    if (fonte.empty()) {
      roda = decorreu;  // throttle: no maximo 1 execucao por duracao
    } else if (decorreu && !st.buffer.empty()) {
      batch = st.buffer;  // janela de tempo: entrega tudo que acumulou e zera
      st.buffer.clear();
      roda = true;
    }
  }
  if (roda) {
    st.ran_once = true;
    st.last_run = now;
    // Persiste o relogio para janelas de tempo/throttle entre replicas
    // (contagem mantem o formato legado numero-puro).
    if (!offset_file.empty() && spec.kind != JanelaSpec::Contagem) {
      janela_offset_save(st, pipe_name, offset_file, true);
    }
  }
  return roda;
}

void Interpreter::run_verificar(const Item& field, Env& env) {
  const std::string var =
      (!field.header.empty() && field.header[0] && field.header[0]->kind == ExprKind::Name)
          ? field.header[0]->text
          : "";
  Value* target = env.lookup(var);
  if (!target || (target->kind != ValueKind::Tabela && target->kind != ValueKind::Lista) ||
      !target->list) {
    fail(field.span, "verificar: '" + var + "' nao e uma tabela");
  }
  const rt::ValueList& rows = *target->list;

  std::string on_violate = "abortar";
  std::vector<std::string> violations;
  auto note = [&](std::string m) {
    if (violations.size() < 5) violations.push_back(std::move(m));
  };

  if (field.block) {
    for (const auto& raw : field.block->items) {
      if (!raw) continue;
      const Item* rule = (raw->kind == ItemKind::ListEntry && raw->child) ? raw->child.get()
                                                                          : raw.get();
      if (rule->kind != ItemKind::Field) continue;
      const std::string& k = rule->key;

      if (k == "ao_violar") {
        if (rule->value && rule->value->kind == ExprKind::Name) on_violate = rule->value->text;
        continue;
      }
      if (k == "nao_nulo" && rule->value) {
        std::vector<std::string> cols;
        if (rule->value->kind == ExprKind::ListLit) {
          for (const auto& el : rule->value->elems) {
            if (el && el->kind == ExprKind::Name) cols.push_back(el->text);
          }
        } else if (rule->value->kind == ExprKind::Name) {
          cols.push_back(rule->value->text);
        }
        for (std::size_t r = 0; r < rows.size(); ++r) {
          for (const std::string& c : cols) {
            const Value* cell = rows[r].map ? rows[r].map->find(c) : nullptr;
            if (!cell || cell->kind == ValueKind::Nulo) {
              note("coluna '" + c + "' nula na linha " + std::to_string(r + 1));
            }
          }
        }
      } else if (k == "unico" && rule->value && rule->value->kind == ExprKind::Name) {
        const std::string col = rule->value->text;
        std::vector<std::string> seen;
        for (std::size_t r = 0; r < rows.size(); ++r) {
          const Value* cell = rows[r].map ? rows[r].map->find(col) : nullptr;
          std::string key = cell ? to_display(*cell) : "";
          if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            note("valor repetido em '" + col + "' na linha " + std::to_string(r + 1));
          } else {
            seen.push_back(key);
          }
        }
      } else if (k == "intervalo" && rule->value) {
        for (std::size_t r = 0; r < rows.size(); ++r) {
          Env inner;
          inner.parent = &env;
          inner.vars["linha"] = rows[r];
          if (!eval(*rule->value, inner).truthy()) {
            note("condicao de intervalo falhou na linha " + std::to_string(r + 1));
          }
        }
      }
    }
  }

  if (violations.empty()) {
    out_ << "verificar " << var << ": ok (" << rows.size() << " linhas)\n";
    return;
  }
  const std::string summary = "verificar " + var + ": " + std::to_string(violations.size()) +
                              (violations.size() >= 5 ? "+ violacao(oes)" : " violacao(oes)");
  if (on_violate == "avisar") {
    out_ << "[aviso] " << summary << "\n";
    for (const std::string& v : violations) out_ << "  - " << v << "\n";
    return;
  }
  throw RuntimeAbort{field.span, summary, DiagCode::DataQualityViolation, violations};
}

// ------------------------------------------------------------------ fontes
// Valor de texto de um campo de `fonte` (aceita texto, nome e env("...")).
std::string fonte_field_text(const ast::Block& block, std::string_view key) {
  const Item* f = find_field(block, key);
  if (!f || !f->value) return "";
  const Expr* v = f->value.get();
  if (v->kind == ExprKind::TextLit || v->kind == ExprKind::Name) return v->text;
  if (v->kind == ExprKind::Call && v->lhs && v->lhs->kind == ExprKind::Name &&
      v->lhs->text == "env" && !v->args.empty() &&
      v->args[0].value->kind == ExprKind::TextLit) {
    const char* e = std::getenv(v->args[0].value->text.c_str());
    return e ? std::string(e) : "";
  }
  return "";
}

// Arquivo de offset da janela de contagem para uma fonte: `<caminho>.tilt-offset`,
// compartilhado entre pipelines pela mesma fonte (o JSON dentro e um mapa por
// pipeline). Somente fontes baseadas em arquivo (csv/json); Kafka com `grupo:`
// ja tem checkpoint no broker e os demais conectores nao usam offset de arquivo.
// Fase 12-4: o path passa por checkpoint_resolve() — com TILT_CHECKPOINT_DIR
// o offset mora no diretorio compartilhado (multi-replica).
std::string Interpreter::janela_offset_file(const std::string& fonte) {
  const Item* decl = entities_.at(fonte);
  if (!decl->block) return "";
  const std::string tipo = fonte_field_text(*decl->block, "tipo");
  if (tipo != "csv" && tipo != "json") return "";
  std::string path = fonte_field_text(*decl->block, "caminho");
  if (path.empty()) path = fonte_field_text(*decl->block, "arquivo");
  if (path.empty()) path = fonte_field_text(*decl->block, "url");
  if (path.rfind("file://", 0) == 0) path = path.substr(7);
  if (path.empty()) return "";
  const std::string local = path + ".tilt-offset";
  try {
    return rt::checkpoint_resolve(local);
  } catch (const std::exception& e) {
    fail(decl->span, std::string(e.what()));
  }
}

void Interpreter::janela_offset_load(WindowState& st, const std::string& pipeline,
                                     const std::string& offset_file) {
  if (st.offset_loaded) return;
  st.offset_loaded = true;
  std::string bruto;
  try {
    bruto = rt::checkpoint_ler(offset_file);
  } catch (const std::exception&) {
    return;  // backend fora do ar: segue so com offset em memoria
  }
  if (bruto.empty()) return;  // sem arquivo: comeca do zero e nada cria ainda
  Value parsed;
  try {
    parsed = rt::json_parse(bruto);
  } catch (const std::exception&) {
    return;  // arquivo corrompido/incompleto: recomeca do zero
  }
  if (parsed.kind != ValueKind::Mapa || !parsed.map) return;
  if (const Value* v = parsed.map->find(pipeline)) {
    // Formato legado: numero puro = offset. Formato 12-4: mapa por pipeline
    // {offset, last_run} — permite retomar throttle/tempo entre replicas.
    if (v->is_number()) {
      st.offset = static_cast<std::size_t>(v->as_number());
      st.persisted_offset = st.offset;
    } else if (v->kind == ValueKind::Mapa && v->map) {
      if (const Value* o = v->map->find("offset"); o && o->is_number()) {
        st.offset = static_cast<std::size_t>(o->as_number());
        st.persisted_offset = st.offset;
      }
      if (const Value* lr = v->map->find("last_run"); lr && lr->is_number()) {
        st.last_run = static_cast<std::time_t>(lr->as_number());
        st.ran_once = true;
      }
    }
  }
}

void Interpreter::janela_offset_save(WindowState& st, const std::string& pipeline,
                                     const std::string& offset_file, bool com_relogio) {
  Value map = Value::mapa();
  try {
    const std::string bruto = rt::checkpoint_ler(offset_file);
    if (!bruto.empty()) {
      try {
        Value parsed = rt::json_parse(bruto);
        if (parsed.kind == ValueKind::Mapa && parsed.map) {
          for (const auto& [k, v] : parsed.map->items) {
            if (k == pipeline) continue;
            // Preserva tanto o formato legado (numero) quanto o 12-4 (mapa).
            if (v.is_number() || v.kind == ValueKind::Mapa) map.map->set(k, v);
          }
        }
      } catch (const std::exception&) {
        // sobrescreve conteudo ilegivel
      }
    }
  } catch (const std::exception&) {
    // backend fora do ar na leitura: segue com o mapa local
  }
  if (com_relogio && st.ran_once) {
    Value entry = Value::mapa();
    entry.map->set("offset", Value::inteiro(static_cast<std::int64_t>(st.offset)));
    entry.map->set("last_run", Value::inteiro(static_cast<std::int64_t>(st.last_run)));
    map.map->set(pipeline, std::move(entry));
  } else {
    map.map->set(pipeline, Value::inteiro(static_cast<std::int64_t>(st.offset)));
  }
  try {
    rt::checkpoint_gravar(offset_file, rt::json_dump(map) + "\n");
  } catch (const std::exception&) {
    return;  // sem permissao / backend fora do ar: segue so com offset em memoria
  }
  st.persisted_offset = st.offset;
}

Value Interpreter::read_csv_file(const std::string& path, Span span) {
  std::ifstream in(path);
  if (!in) fail(span, "nao foi possivel abrir '" + path + "'");
  std::string line;
  std::vector<std::string> headers;
  rt::ValueList rows;
  bool first = true;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    auto cells = split_csv_line(line);
    if (first) {
      headers = cells;
      first = false;
      continue;
    }
    Value row = Value::mapa();
    for (std::size_t k = 0; k < headers.size(); ++k) {
      row.map->set(headers[k], k < cells.size() ? parse_scalar(cells[k]) : Value::nulo());
    }
    rows.push_back(std::move(row));
  }
  return Value::tabela(std::move(rows));
}

Value Interpreter::read_fonte(const std::string& name, Span span) {
  const Item* decl = entities_.at(name);
  if (!decl->block) fail(span, "fonte '" + name + "' sem configuracao");

  auto field_text = [&](std::string_view key) { return fonte_field_text(*decl->block, key); };

  const std::string tipo = field_text("tipo");
  std::string path = field_text("caminho");
  if (path.empty()) path = field_text("arquivo");
  if (path.empty()) path = field_text("url");
  if (path.rfind("file://", 0) == 0) path = path.substr(7);

  if (tipo == "csv") {
    if (path.empty()) fail(span, "fonte '" + name + "': falta 'caminho:'");
    return read_csv_file(path, span);
  }
  if (tipo == "json") {
    if (path.empty()) fail(span, "fonte '" + name + "': falta 'caminho:'");
    std::ifstream in(path);
    if (!in) fail(span, "nao foi possivel abrir '" + path + "'");
    std::ostringstream ss;
    ss << in.rdbuf();
    Value parsed;
    try {
      parsed = rt::json_parse(ss.str());
    } catch (const std::exception& e) {
      fail(span, std::string(e.what()));
    }
    if (parsed.kind == ValueKind::Lista) parsed.kind = ValueKind::Tabela;
    return parsed;
  }
  if (tipo == "parquet") {
    if (path.empty()) fail(span, "fonte '" + name + "': falta 'caminho:'");
    try {
      Value t = rt::parquet_read(path);
      t.kind = ValueKind::Tabela;
      return t;
    } catch (const std::exception& e) {
      fail(span, std::string(e.what()));
    }
  }
  if (tipo == "delta") {
    if (path.empty()) fail(span, "fonte '" + name + "': falta 'caminho:'");
    try {
      Value t = rt::delta_read(path);
      t.kind = ValueKind::Tabela;
      return t;
    } catch (const std::exception& e) {
      fail(span, std::string(e.what()));
    }
  }
  if (tipo == "sqlite" || tipo == "postgres" || tipo == "duckdb" || tipo == "mysql" ||
      tipo == "clickhouse") {
    const Item* c = find_field(*decl->block, "consulta");
    if (!c || !c->value || c->value->kind != ExprKind::TextLit) {
      fail(span, "fonte '" + name + "': falta 'consulta: \"select ...\"'");
    }
    const std::string& sql = c->value->text;
    try {
      Value t = tipo == "duckdb"      ? rt::duckdb_query(path, sql)
                : tipo == "sqlite"    ? rt::sqlite_query(path, sql)
                : tipo == "mysql"     ? rt::mysql_query(path, sql)
                : tipo == "clickhouse" ? rt::clickhouse_query(path, sql)
                                      : rt::postgres_query(path, sql);
      t.kind = ValueKind::Tabela;
      return t;
    } catch (const std::exception& e) {
      fail(span, std::string(e.what()));
    }
  }
  if (tipo == "elasticsearch" || tipo == "opensearch") {
    if (path.empty()) {
      fail(span, "fonte '" + name + "': falta 'url: \"elasticsearch://...\"'");
    }
    const Item* c = find_field(*decl->block, "consulta");
    if (!c || !c->value || c->value->kind != ExprKind::TextLit) {
      fail(span, "fonte '" + name + "': falta 'consulta: \"{ ... }\"' (DSL de busca JSON)");
    }
    try {
      return rt::es_query(path, Value::texto(c->value->text));
    } catch (const std::exception& e) {
      fail(span, std::string(e.what()));
    }
  }
  if (tipo == "spark") {
    // Spark remoto via Apache Livy (REST): url do servidor Livy + consulta
    // SQL; opcionais: lingua ("scala" default | "pyspark") e conf da sessao.
    if (path.empty()) {
      fail(span, "fonte '" + name + "': falta 'url: \"http://host:8998\"'");
    }
    const Item* c = find_field(*decl->block, "consulta");
    if (!c || !c->value || c->value->kind != ExprKind::TextLit) {
      fail(span, "fonte '" + name + "': falta 'consulta: \"select ...\"'");
    }
    std::string lingua = field_text("lingua");
    if (lingua.empty()) lingua = "scala";
    Value conf = Value::nulo();
    if (const Item* cf = find_field(*decl->block, "conf"); cf && cf->value) {
      Env cenv;
      cenv.parent = &root_;
      conf = eval(*cf->value, cenv);
      if (conf.kind != ValueKind::Mapa) {
        fail(span, "fonte '" + name + "': 'conf' deve ser um mapa {...} (conf de sessao Spark)");
      }
    }
    try {
      Value t = rt::livy_sql(path, c->value->text, lingua,
                             conf.kind == ValueKind::Mapa ? &conf : nullptr);
      t.kind = ValueKind::Tabela;
      return t;
    } catch (const std::exception& e) {
      fail(span, std::string(e.what()));
    }
  }
  if (tipo == "kafka") {
    const std::string topico = field_text("topico");
    if (topico.empty()) {
      fail(span, "fonte '" + name + "': falta 'topico: \"...\"'");
    }
    const std::string broker = field_text("broker");
    const std::string grupo = field_text("grupo");
    std::int64_t max = 100;
    if (const Item* m = find_field(*decl->block, "max");
        m && m->value && m->value->kind == ExprKind::IntLit) {
      try {
        max = std::stoll(m->value->text);
      } catch (...) {
        fail(span, "fonte '" + name + "': 'max' deve ser inteiro");
      }
    }
    try {
      if (!grupo.empty()) {
        // Com grupo: o commit de offsets vira o checkpoint natural da fonte
        // (cada `ler` consome so o que ainda nao foi commitado).
        Value out = Value::lista();
        for (const auto& [part, valor] :
             rt::kafka_consume_group(broker, grupo, topico, static_cast<int>(max))) {
          (void)part;
          out.list->push_back(Value::texto(valor));
        }
        return out;
      }
      return rt::kafka_ler(topico, field_text("desde") == "fim", max, broker);
    } catch (const std::exception& e) {
      fail(span, std::string(e.what()));
    }
  }
  fail(span, "fonte '" + name + "': conector '" + (tipo.empty() ? "?" : tipo) +
                 "' nao implementado (M5.2)",
       DiagCode::ConnectorNotImplemented);
}

// ------------------------------------------------------------------ deep learning

namespace {

// Extracts a tensor from its JSON form { "forma": [...], "dados": [...] }.
bool tensor_from_json(const Value& v, rt::Tensor& out) {
  if (v.kind != ValueKind::Mapa || !v.map) return false;
  const Value* forma = v.map->find("forma");
  const Value* dados = v.map->find("dados");
  if (!forma || !dados || forma->kind != ValueKind::Lista || dados->kind != ValueKind::Lista ||
      !forma->list || !dados->list) {
    return false;
  }
  out.shape.clear();
  for (const Value& d : *forma->list) out.shape.push_back(static_cast<std::int64_t>(d.as_number()));
  out.data.clear();
  for (const Value& d : *dados->list) out.data.push_back(static_cast<float>(d.as_number()));
  return out.size() == static_cast<std::int64_t>(out.data.size());
}

void flatten_nested(const Value& v, std::vector<std::int64_t>& shape, std::vector<float>& data,
                    std::size_t depth) {
  if (v.kind == ValueKind::Lista && v.list) {
    if (shape.size() == depth) shape.push_back(static_cast<std::int64_t>(v.list->size()));
    for (const Value& e : *v.list) flatten_nested(e, shape, data, depth + 1);
  } else {
    data.push_back(static_cast<float>(v.as_number()));
  }
}

// Walks `camadas:` items (including folded `- densa: N` / `ativacao: relu` blocks).
void each_layer_spec(const ast::Block& block,
                     const std::function<void(const std::string&, const ast::Expr*)>& emit) {
  for (const auto& raw : block.items) {
    if (!raw) continue;
    if (raw->kind == ItemKind::ListEntry) {
      if (raw->block) {
        each_layer_spec(*raw->block, emit);
        continue;
      }
      const Item* c = raw->child.get();
      if (!c) continue;
      if (c->kind == ItemKind::Field) {
        emit(c->key, c->value.get());
      } else if (c->kind == ItemKind::Stmt && c->stmt && c->stmt->a &&
                 c->stmt->a->kind == ExprKind::Name) {
        emit(c->stmt->a->text, nullptr);
      }
    } else if (raw->kind == ItemKind::Field) {
      emit(raw->key, raw->value.get());
    }
  }
}

}  // namespace

rt::Tensor Interpreter::value_to_tensor(const Value& v, Span span) {
  if (v.kind == ValueKind::Tensor && v.tensor) return *v.tensor;
  if (v.kind == ValueKind::Lista) {
    std::vector<std::int64_t> shape;
    std::vector<float> data;
    flatten_nested(v, shape, data, 0);
    rt::Tensor t;
    t.shape = shape;
    t.data = std::move(data);
    if (t.size() != static_cast<std::int64_t>(t.data.size())) {
      fail(span, "lista aninhada irregular; nao forma um tensor");
    }
    return t;
  }
  if (v.is_number()) return rt::Tensor::filled({1}, static_cast<float>(v.as_number()));
  fail(span, std::string("nao e possivel converter '") + v.type_name() + "' em tensor");
}

std::vector<Interpreter::Layer> Interpreter::build_layers(const Item& decl, std::int64_t in_dim) {
  const std::string name = decl_name(decl);
  std::vector<Layer> layers;
  std::int64_t dim = in_dim;
  std::uint64_t seed = 0xC1A5;

  if (decl.block) {
    const Item* camadas = find_field(*decl.block, "camadas");
    if (camadas && camadas->block) {
      each_layer_spec(*camadas->block, [&](const std::string& key, const ast::Expr* value) {
        if (key == "densa" && value && value->kind == ExprKind::IntLit) {
          std::int64_t n = std::strtoll(value->text.c_str(), nullptr, 10);
          Layer l;
          l.kind = Layer::Dense;
          l.w = rt::Tensor::xavier({dim, n}, dim, n, seed++);
          l.b = rt::Tensor::zeros({n});
          layers.push_back(std::move(l));
          dim = n;
        } else if (key == "linear" && value && value->kind == ExprKind::ListLit &&
                   value->elems.size() == 2) {
          std::int64_t a = std::strtoll(value->elems[0]->text.c_str(), nullptr, 10);
          std::int64_t b = std::strtoll(value->elems[1]->text.c_str(), nullptr, 10);
          Layer l;
          l.kind = Layer::Dense;
          l.w = rt::Tensor::xavier({a, b}, a, b, seed++);
          l.b = rt::Tensor::zeros({b});
          layers.push_back(std::move(l));
          dim = b;
        } else if (key == "ativacao" && value && value->kind == ExprKind::Name) {
          Layer l;
          l.kind = Layer::Activation;
          l.act = value->text;
          layers.push_back(std::move(l));
        } else if (key == "softmax") {
          Layer l;
          l.kind = Layer::Softmax;
          layers.push_back(std::move(l));
        } else if (key == "abandono" || key == "dropout") {
          Layer l;
          l.kind = Layer::Dropout;
          layers.push_back(std::move(l));
        } else if (key == "norma_camada") {
          Layer l;
          l.kind = Layer::LayerNorm;
          layers.push_back(std::move(l));
        } else if (key == "norma_lote" || key == "conv2d") {
          fail(decl.span, "modelo '" + name + "': camada '" + key +
                              "' ainda nao suportada (M6.3); remova-a do modelo por ora");
        } else if (word_in(key, {"relu", "gelu", "silu", "sigmoide", "tanh"})) {
          Layer l;
          l.kind = Layer::Activation;
          l.act = key;
          layers.push_back(std::move(l));
        }
      });
    }
  }
  return layers;
}

void Interpreter::set_device(const Item& decl) {
  use_gpu_ = false;
  if (!decl.block) return;
  const Item* f = find_field(*decl.block, "dispositivo");
  std::string dev = "cpu";
  if (f && f->value && (f->value->kind == ExprKind::TextLit || f->value->kind == ExprKind::Name)) {
    dev = f->value->text;
  }
  if (rt::GpuRuntime::instance().ensure(dev)) {
    use_gpu_ = true;
    std::lock_guard<std::mutex> lk(log_mutex_);
    if (!gpu_announced_) {
      out_ << "[gpu] " << rt::GpuRuntime::instance().info() << "\n";
      gpu_announced_ = true;
    }
  }
}

rt::Tensor Interpreter::mm(const rt::Tensor& a, const rt::Tensor& b) {
  if (use_gpu_ && a.rank() == 2 && b.rank() == 2 && a.shape[1] == b.shape[0]) {
    rt::Tensor c;
    c.shape = {a.shape[0], b.shape[1]};
    c.data.resize(static_cast<std::size_t>(a.shape[0] * b.shape[1]));
    if (rt::GpuRuntime::instance().gemm(a.data.data(), b.data.data(), c.data.data(),
                                        static_cast<int>(a.shape[0]), static_cast<int>(a.shape[1]),
                                        static_cast<int>(b.shape[1]))) {
      return c;
    }
  }
  return rt::matmul(a, b);
}

rt::Tensor Interpreter::act_relu(const rt::Tensor& x) {
  if (use_gpu_) {
    rt::Tensor out = x;
    if (rt::GpuRuntime::instance().relu(out.data.data(), out.data.size())) return out;
  }
  return rt::apply_unary(x, "relu");
}

const std::vector<Interpreter::Layer>& Interpreter::build_model(const Item& decl,
                                                               std::int64_t in_dim, Span span) {
  const std::string name = decl_name(decl);
  {
    std::lock_guard<std::mutex> lk(model_cache_mutex_);
    if (auto it = model_cache_.find(name); it != model_cache_.end()) return it->second;
  }
  std::vector<Layer> layers = build_layers(decl, in_dim);

  // Carga de 'pesos: "arquivo"' (formato tilt-pesos, gerado por
  // modelo <Nome>.salvar_pesos). Arquivo ausente mantem o init Xavier.
  if (decl.block) {
    if (const Item* pw = find_field(*decl.block, "pesos");
        pw && pw->value && pw->value->kind == ExprKind::TextLit) {
      const std::string& path = pw->value->text;
      std::ifstream f(path);
      if (!f) {
        out_ << "[nota] modelo " << name << ": arquivo de pesos '" << path
             << "' nao encontrado; usando init Xavier\n";
      } else {
        std::ostringstream ss;
        ss << f.rdbuf();
        Value doc = Value::nulo();
        try {
          doc = rt::json_parse(ss.str());
        } catch (...) {
          doc = Value::nulo();
        }
        const Value* camadas =
            doc.kind == ValueKind::Mapa && doc.map ? doc.map->find("camadas") : nullptr;
        if (!camadas || camadas->kind != ValueKind::Lista || !camadas->list) {
          fail(span, "modelo '" + name + "': arquivo de pesos '" + path +
                         "' invalido (esperado JSON tilt-pesos com 'camadas')");
        }
        std::size_t li = 0;
        for (const Value& c : *camadas->list) {
          while (li < layers.size() && layers[li].kind != Layer::Dense) ++li;
          if (li >= layers.size()) {
            fail(span, "modelo '" + name + "': o arquivo '" + path +
                           "' tem mais camadas de pesos do que o modelo tem camadas densa");
          }
          rt::Tensor w, b;
          const Value* wv = c.kind == ValueKind::Mapa && c.map ? c.map->find("w") : nullptr;
          const Value* bv = c.kind == ValueKind::Mapa && c.map ? c.map->find("b") : nullptr;
          if (!wv || !bv || !tensor_from_json(*wv, w) || !tensor_from_json(*bv, b)) {
            fail(span, "modelo '" + name + "': arquivo de pesos '" + path +
                           "' invalido (cada camada precisa de 'w' e 'b' com forma e dados)");
          }
          if (w.shape != layers[li].w.shape || b.shape != layers[li].b.shape) {
            fail(span, "modelo '" + name + "': forma de pesos incompativel na camada densa " +
                           std::to_string(li) + " (modelo espera w " + layers[li].w.shape_str() +
                           " b " + layers[li].b.shape_str() + ", arquivo tem w " + w.shape_str() +
                           " b " + b.shape_str() + ")");
          }
          layers[li].w = std::move(w);
          layers[li].b = std::move(b);
          ++li;
        }
      }
    }
  }
  std::lock_guard<std::mutex> lk(model_cache_mutex_);
  return model_cache_.emplace(name, std::move(layers)).first->second;
}

rt::Tensor Interpreter::forward_layers(const std::vector<Layer>& layers, rt::Tensor x) {
  for (const Layer& l : layers) {
    switch (l.kind) {
      case Layer::Dense:
        x = rt::add(mm(x, l.w), l.b);
        break;
      case Layer::Activation:
        x = l.act == "relu" ? act_relu(x) : rt::apply_unary(x, l.act);
        break;
      case Layer::Softmax:
        x = rt::softmax_last(x);
        break;
      case Layer::LayerNorm:
        x = rt::layer_norm_last(x);
        break;
      case Layer::Dropout:
        break;
    }
  }
  return x;
}

rt::Value Interpreter::model_forward(const Item& decl, const Value& input, Span span) {
  set_device(decl);
  rt::Tensor x = value_to_tensor(input, span);
  std::int64_t in_dim = x.shape.empty() ? static_cast<std::int64_t>(x.data.size()) : x.shape.back();
  const std::vector<Layer>& layers = build_model(decl, in_dim, span);
  try {
    return Value::tensor_de(forward_layers(layers, std::move(x)));
  } catch (const std::exception& e) {
    fail(span, std::string("modelo ") + decl_name(decl) + ": " + e.what());
  }
}

rt::Value Interpreter::eval_modelo_call(const Expr& call, Env& env) {
  if (call.args.size() != 1 || call.args[0].value->kind != ExprKind::Call) {
    fail(call.span, "uso: modelo <Nome>.executar <entrada>");
  }
  const Expr& inner = *call.args[0].value;
  if (!inner.lhs || inner.lhs->kind != ExprKind::Member || !inner.lhs->lhs ||
      inner.lhs->lhs->kind != ExprKind::Name) {
    fail(inner.span, "uso: modelo <Nome>.executar <entrada>");
  }
  const std::string mname = inner.lhs->lhs->text;
  const std::string method = inner.lhs->text;

  auto it = entities_.find(mname);
  if (it == entities_.end() || it->second->key != "modelo") {
    fail(inner.span, "'" + mname + "' nao e um modelo declarado");
  }
  if (method == "executar" || method == "para_frente") {
    if (inner.args.empty()) fail(inner.span, method + " precisa de uma entrada");
    Value input = eval(*inner.args[0].value, env);
    return model_forward(*it->second, input, inner.span);
  }
  if (method == "salvar_pesos") {
    if (inner.args.empty() || inner.args[0].value->kind != ExprKind::TextLit) {
      fail(inner.span, "uso: modelo " + mname + ".salvar_pesos \"caminho\"");
    }
    const std::string& path = inner.args[0].value->text;
    // A dimensao de entrada vem da anotacao 'entrada: tensor[..., N]'.
    std::int64_t in_dim = -1;
    if (it->second->block) {
      if (const Item* ent = find_field(*it->second->block, "entrada");
          ent && ent->value && ent->value->kind == ExprKind::Index && !ent->value->elems.empty()) {
        const Expr* last = ent->value->elems.back().get();
        if (last && last->kind == ExprKind::IntLit) in_dim = std::stoll(last->text);
      }
    }
    if (in_dim < 0) {
      fail(inner.span, "modelo '" + mname +
                           "': 'salvar_pesos' precisa de 'entrada: tensor[..., N]' anotado "
                           "para inferir a dimensao de entrada");
    }
    const std::vector<Layer>& layers = build_model(*it->second, in_dim, inner.span);
    Value cl = Value::lista();
    for (const Layer& l : layers) {
      if (l.kind != Layer::Dense) continue;
      Value c = Value::mapa();
      c.map->set("tipo", Value::texto("densa"));
      c.map->set("w", Value::tensor_de(l.w));
      c.map->set("b", Value::tensor_de(l.b));
      cl.list->push_back(std::move(c));
    }
    Value doc = Value::mapa();
    doc.map->set("formato", Value::texto("tilt-pesos"));
    doc.map->set("versao", Value::inteiro(1));
    doc.map->set("camadas", cl);
    std::ofstream out(path, std::ios::trunc);
    if (!out) fail(inner.span, "modelo '" + mname + "': nao foi possivel gravar '" + path + "'");
    out << rt::json_dump(doc) << "\n";
    out_ << "modelo " << mname << ": pesos salvos em " << path << " (" << cl.list->size()
         << " camadas)\n";
    return Value::logico(true);
  }
  fail(inner.span, "metodo de modelo '" + method + "' desconhecido (use executar / para_frente / salvar_pesos)");
}

namespace {

int field_int(const ast::Block& block, std::string_view key, int fallback) {
  const Item* f = find_field(block, key);
  if (f && f->value && f->value->kind == ExprKind::IntLit) {
    return static_cast<int>(std::strtol(f->value->text.c_str(), nullptr, 10));
  }
  return fallback;
}
double field_num(const ast::Block& block, std::string_view key, double fallback) {
  const Item* f = find_field(block, key);
  if (f && f->value && (f->value->kind == ExprKind::DecimalLit || f->value->kind == ExprKind::IntLit)) {
    return std::strtod(f->value->text.c_str(), nullptr);
  }
  return fallback;
}
std::string field_word(const ast::Block& block, std::string_view key, std::string fallback) {
  const Item* f = find_field(block, key);
  if (f && f->value && f->value->kind == ExprKind::Name) return f->value->text;
  return fallback;
}

// Sum a [B, C] gradient over the batch dimension -> [C].
rt::Tensor col_sum(const rt::Tensor& g) {
  const auto c = static_cast<std::size_t>(g.shape.size() == 2 ? g.shape[1] : g.data.size());
  rt::Tensor out = rt::Tensor::zeros({static_cast<std::int64_t>(c)});
  for (std::size_t k = 0; k < g.data.size(); ++k) out.data[k % c] += g.data[k];
  return out;
}

float activation_deriv(const std::string& fn, float pre, float post) {
  if (fn == "relu") return pre > 0.0F ? 1.0F : 0.0F;
  if (fn == "sigmoide") return post * (1.0F - post);
  if (fn == "tanh") return 1.0F - post * post;
  if (fn == "silu") {
    float s = 1.0F / (1.0F + std::exp(-pre));
    return s * (1.0F + pre * (1.0F - s));
  }
  if (fn == "gelu") {
    // Mesma aproximacao tanh da forward (rt::apply_unary):
    // gelu(x) = 0.5 x (1 + tanh(u)), u = c (x + 0.044715 x^3), c = 0.7978845608
    // d/dx = 0.5 (1 + tanh(u)) + 0.5 x sech^2(u) c (1 + 0.134145 x^2)
    const float c = 0.7978845608F;
    const float u = c * (pre + 0.044715F * pre * pre * pre);
    const float t = std::tanh(u);
    return 0.5F * (1.0F + t) +
           0.5F * pre * (1.0F - t * t) * c * (1.0F + 0.134145F * pre * pre);
  }
  return 1.0F;
}

// Backward de norma_camada (sem affine), por linha da ultima dimensao:
// dL/dx = inv * (g - mean(g) - y * mean(g*y)), com y a saida normalizada do
// forward e inv = 1/sqrt(var + eps) recomputado da entrada (eps = 1e-5).
void layer_norm_backward(rt::Tensor& grad, const rt::Tensor& in, const rt::Tensor& y) {
  const auto w = static_cast<std::size_t>(in.shape.back());
  for (std::size_t base = 0; base < grad.data.size(); base += w) {
    float mean = 0.0F;
    for (std::size_t k = 0; k < w; ++k) mean += in.data[base + k];
    mean /= static_cast<float>(w);
    float var = 0.0F;
    for (std::size_t k = 0; k < w; ++k) {
      const float d = in.data[base + k] - mean;
      var += d * d;
    }
    var /= static_cast<float>(w);
    const float inv = 1.0F / std::sqrt(var + 1e-5F);
    float mg = 0.0F, mgy = 0.0F;
    for (std::size_t k = 0; k < w; ++k) {
      mg += grad.data[base + k];
      mgy += grad.data[base + k] * y.data[base + k];
    }
    mg /= static_cast<float>(w);
    mgy /= static_cast<float>(w);
    for (std::size_t k = 0; k < w; ++k) {
      grad.data[base + k] = inv * (grad.data[base + k] - mg - y.data[base + k] * mgy);
    }
  }
}

}  // namespace

void Interpreter::run_treino(const Item& decl) {
  const std::string name = decl_name(decl);
  auto mit = entities_.find(name);
  if (mit == entities_.end() || mit->second->key != "modelo") {
    fail(decl.span, "treino '" + name + "': nao existe 'modelo " + name + "'");
  }
  if (!decl.block) return;
  const ast::Block& cfg = *decl.block;

  const Item* dados = find_field(cfg, "dados");
  if (!dados || !dados->value) fail(decl.span, "treino '" + name + "': falta 'dados:'");
  Value data = eval(*dados->value, root_);
  if (data.kind != ValueKind::Mapa || !data.map || !data.map->find("x") || !data.map->find("y")) {
    fail(dados->value->span, "treino: 'dados' deve produzir { x: <tensor>, y: <lista> }");
  }
  rt::Tensor x = value_to_tensor(*data.map->find("x"), decl.span);
  if (x.rank() != 2) fail(decl.span, "treino: 'x' deve ser 2D [amostras, atributos]");
  const std::int64_t n = x.shape[0];
  const std::int64_t f = x.shape[1];

  const std::string perda = field_word(cfg, "perda", "entropia_cruzada");
  const bool ce = perda == "entropia_cruzada";
  const bool mse = perda == "quadratica";

  std::vector<double> yf;
  const Value* yv = data.map->find("y");
  if (yv->kind == ValueKind::Lista && yv->list) {
    for (const Value& e : *yv->list) yf.push_back(e.as_number());
  }
  if (static_cast<std::int64_t>(yf.size()) != n) fail(decl.span, "treino: |x| != |y|");
  std::vector<int> y;
  int classes = 1;
  for (double v : yf) {
    y.push_back(static_cast<int>(v));
    classes = std::max(classes, static_cast<int>(v) + 1);
  }

  const std::string otim = field_word(cfg, "otimizador", "sgd");
  double lr = field_num(cfg, "taxa", field_num(cfg, "taxa_aprendizado", 0.1));
  const int epocas = field_int(cfg, "epocas", 50);
  const Item* vf = find_field(cfg, "verboso");
  const bool verbose = vf && vf->value && vf->value->kind == ExprKind::BoolLit && vf->value->boolean;

  set_device(*mit->second);
  std::vector<Layer> layers = build_layers(*mit->second, f);
  if (!ce && !mse) {
    fail(decl.span, "treino: perda '" + perda +
                        "' nao suportada (use 'entropia_cruzada' | 'quadratica')");
  }
  if (layers.empty()) fail(decl.span, "treino '" + name + "': modelo sem camadas");
  if (ce && layers.back().kind != Layer::Softmax) {
    fail(decl.span, "treino: perda 'entropia_cruzada' exige 'softmax' na ultima camada");
  }
  if (mse) {
    if (layers.back().kind == Layer::Softmax) {
      fail(decl.span,
           "treino: perda 'quadratica' exige ultima camada 'densa'/'linear' sem 'softmax'");
    }
    std::int64_t width = 1;
    for (auto rit = layers.rbegin(); rit != layers.rend(); ++rit) {
      if (rit->kind == Layer::Dense) {
        width = rit->w.shape[1];
        break;
      }
    }
    if (width != 1) {
      fail(decl.span, "treino: perda 'quadratica' e regressao escalar; a saida do modelo deve ter "
                      "largura 1 (tem " +
                          std::to_string(width) + ")");
    }
  }
  for (Layer& l : layers) {
    if (l.kind != Layer::Dense) continue;
    l.m_w = rt::Tensor::zeros(l.w.shape);
    l.v_w = rt::Tensor::zeros(l.w.shape);
    l.m_b = rt::Tensor::zeros(l.b.shape);
    l.v_b = rt::Tensor::zeros(l.b.shape);
  }

  const float inv_n = 1.0F / static_cast<float>(n);
  float first_loss = 0.0F;
  float last_loss = 0.0F;
  int adam_t = 0;

  try {
    for (int epoch = 1; epoch <= epocas; ++epoch) {
      // Forward with cached inputs per layer.
      std::vector<rt::Tensor> ins;
      ins.reserve(layers.size() + 1);
      rt::Tensor cur = x;
      for (const Layer& l : layers) {
        ins.push_back(cur);
        switch (l.kind) {
          case Layer::Dense: cur = rt::add(mm(cur, l.w), l.b); break;
          case Layer::Activation:
            cur = l.act == "relu" ? act_relu(cur) : rt::apply_unary(cur, l.act);
            break;
          case Layer::Softmax: cur = rt::softmax_last(cur); break;
          case Layer::LayerNorm: cur = rt::layer_norm_last(cur); break;
          case Layer::Dropout: break;
        }
      }
      const rt::Tensor& probs = cur;  // [N, C] (probs no CE, valores no MSE)

      // Perda e gradiente da saida.
      float loss = 0.0F;
      rt::Tensor grad = probs;
      if (ce) {
        // entropia_cruzada + softmax: dL/d(logits) = probs - onehot(y), medio.
        for (std::int64_t i = 0; i < n; ++i) {
          const int label = y[static_cast<std::size_t>(i)];
          const auto idx = static_cast<std::size_t>(i * classes + label);
          loss -= std::log(std::max(probs.data[idx], 1e-9F));
          for (std::int64_t c = 0; c < classes; ++c) {
            auto g = static_cast<std::size_t>(i * classes + c);
            grad.data[g] = (grad.data[g] - (c == label ? 1.0F : 0.0F)) * inv_n;
          }
        }
        loss *= inv_n;
      } else {
        // quadratica (regressao escalar): saida [N, 1], dL/dy = 2*(pred - alvo)/n.
        for (std::int64_t i = 0; i < n; ++i) {
          const float d = probs.data[static_cast<std::size_t>(i)] -
                          static_cast<float>(yf[static_cast<std::size_t>(i)]);
          loss += d * d;
          grad.data[static_cast<std::size_t>(i)] = 2.0F * d * inv_n;
        }
        loss *= inv_n;
      }
      if (epoch == 1) first_loss = loss;
      last_loss = loss;
      if (verbose && (epoch == 1 || epoch % std::max(1, epocas / 10) == 0)) {
        out_ << "  epoca " << epoch << " perda " << loss << "\n";
      }

      // Backward: skip the softmax layer (fused above); update Dense/Activation.
      ++adam_t;
      for (std::int64_t li = static_cast<std::int64_t>(layers.size()) - 1; li >= 0; --li) {
        Layer& l = layers[static_cast<std::size_t>(li)];
        const rt::Tensor& in = ins[static_cast<std::size_t>(li)];
        if (l.kind == Layer::Softmax || l.kind == Layer::Dropout) continue;
        if (l.kind == Layer::LayerNorm) {
          // Saida normalizada do forward: entrada da proxima camada, ou a
          // propria saida final (probs) quando norma_camada e a ultima camada.
          const rt::Tensor& y = static_cast<std::size_t>(li) + 1 < ins.size()
                                    ? ins[static_cast<std::size_t>(li) + 1]
                                    : probs;
          layer_norm_backward(grad, in, y);
          continue;
        }
        if (l.kind == Layer::Activation) {
          const rt::Tensor& out_act = ins[static_cast<std::size_t>(li) + 1 < ins.size()
                                              ? static_cast<std::size_t>(li) + 1
                                              : static_cast<std::size_t>(li)];
          for (std::size_t k = 0; k < grad.data.size(); ++k) {
            grad.data[k] *= activation_deriv(l.act, in.data[k],
                                             k < out_act.data.size() ? out_act.data[k] : 0.0F);
          }
          continue;
        }
        // Dense
        rt::Tensor dw = rt::matmul(rt::transpose2d(in), grad);  // [F_in, C]
        rt::Tensor db = col_sum(grad);
        rt::Tensor grad_in = rt::matmul(grad, rt::transpose2d(l.w));

        if (otim == "adam") {
          const float b1 = 0.9F, b2 = 0.999F, eps = 1e-8F;
          const float c1 = 1.0F - std::pow(b1, static_cast<float>(adam_t));
          const float c2 = 1.0F - std::pow(b2, static_cast<float>(adam_t));
          auto step = [&](rt::Tensor& w, rt::Tensor& m, rt::Tensor& v, const rt::Tensor& g) {
            for (std::size_t k = 0; k < w.data.size(); ++k) {
              m.data[k] = b1 * m.data[k] + (1.0F - b1) * g.data[k];
              v.data[k] = b2 * v.data[k] + (1.0F - b2) * g.data[k] * g.data[k];
              float mh = m.data[k] / c1;
              float vh = v.data[k] / c2;
              w.data[k] -= static_cast<float>(lr) * mh / (std::sqrt(vh) + eps);
            }
          };
          step(l.w, l.m_w, l.v_w, dw);
          step(l.b, l.m_b, l.v_b, db);
        } else {
          for (std::size_t k = 0; k < l.w.data.size(); ++k) {
            l.w.data[k] -= static_cast<float>(lr) * dw.data[k];
          }
          for (std::size_t k = 0; k < l.b.data.size(); ++k) {
            l.b.data[k] -= static_cast<float>(lr) * db.data[k];
          }
        }
        grad = std::move(grad_in);
      }
    }

    if (ce) {
      // Final training-set accuracy.
      rt::Tensor probs = forward_layers(layers, x);
      int correct = 0;
      for (std::int64_t i = 0; i < n; ++i) {
        std::int64_t best = 0;
        for (std::int64_t c = 1; c < classes; ++c) {
          if (probs.data[static_cast<std::size_t>(i * classes + c)] >
              probs.data[static_cast<std::size_t>(i * classes + best)]) {
            best = c;
          }
        }
        if (best == y[static_cast<std::size_t>(i)]) ++correct;
      }
      out_ << "treino " << name << ": perda caiu " << (last_loss < first_loss ? "sim" : "nao")
           << " | acuracia " << correct << "/" << n << "\n";
    } else {
      char mse_buf[64];
      std::snprintf(mse_buf, sizeof(mse_buf), "%.4f", last_loss);
      out_ << "treino " << name << ": perda caiu " << (last_loss < first_loss ? "sim" : "nao")
           << " | mse " << mse_buf << "\n";
    }
  } catch (const std::exception& e) {
    fail(decl.span, std::string("treino ") + name + ": " + e.what());
  }

  {
    std::lock_guard<std::mutex> lk(model_cache_mutex_);
    model_cache_[name] = std::move(layers);
  }
}

// ------------------------------------------------------- ML classico (experimento)

namespace {

// Chave estavel de um valor (classes e categorias): texto, numero e logico
// com prefixo de tipo para nao colidir ("1" texto vs 1 inteiro).
std::string chave_valor(const Value& v) {
  switch (v.kind) {
    case ValueKind::Inteiro: return "i:" + std::to_string(v.i);
    case ValueKind::Decimal: {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.17g", v.d);
      return std::string("d:") + buf;
    }
    case ValueKind::Texto: return "t:" + v.s;
    case ValueKind::Logico: return std::string("b:") + (v.b ? "1" : "0");
    default: return "?";
  }
}

// Rotulo curto para a matriz de confusao (trunca texto longo).
std::string rotulo_valor(const Value& v) {
  std::string s;
  switch (v.kind) {
    case ValueKind::Inteiro: s = std::to_string(v.i); break;
    case ValueKind::Decimal: {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.4g", v.d);
      s = buf;
      break;
    }
    case ValueKind::Texto: s = v.s; break;
    case ValueKind::Logico: s = v.b ? "verdadeiro" : "falso"; break;
    default: s = "?"; break;
  }
  if (s.size() > 12) s = s.substr(0, 11) + ".";
  return s;
}

std::string fmt4(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.4f", v);
  return buf;
}

// Nomes crus de lista [a, b, "c"] (colunas; sem avaliar, pois coluna nao e
// variavel e pode colidir com nomes do escopo).
std::vector<std::string> nomes_crus(const Expr& e, const std::string& contexto) {
  if (e.kind != ExprKind::ListLit) {
    throw std::runtime_error(contexto + " espera uma lista ([col1, col2])");
  }
  std::vector<std::string> out;
  for (const auto& el : e.elems) {
    if (!el || (el->kind != ExprKind::Name && el->kind != ExprKind::TextLit)) {
      throw std::runtime_error(contexto + " espera nomes de coluna ([uso, plano])");
    }
    out.push_back(el->text);
  }
  if (out.empty()) throw std::runtime_error(contexto + " vazio (liste ao menos uma coluna)");
  return out;
}

// Celula numerica de atributo: inteiro/decimal entram crus, logico coage
// para 0/1; texto e nulo falham com sugestao acionavel.
double celula_num(const Value& v, const std::string& col) {
  if (v.kind == ValueKind::Inteiro || v.kind == ValueKind::Decimal ||
      v.kind == ValueKind::Logico) {
    return v.as_number();
  }
  if (v.kind == ValueKind::Nulo) {
    throw std::runtime_error("atributo '" + col +
                             "' tem nulo (sem imputacao na 1a passada; remova a linha)");
  }
  throw std::runtime_error("atributo '" + col + "' tem " + std::string(v.type_name()) +
                           " (declare '" + col + "' em pre_processar como categoria um_de_n)");
}

// Resolve A x = b por eliminacao de Gauss com pivo parcial.
std::vector<double> gauss(std::vector<std::vector<double>> a, std::vector<double> b) {
  const std::size_t n = a.size();
  if (n == 0 || a[0].size() != n || b.size() != n) throw std::runtime_error("sistema invalido");
  for (std::size_t c = 0; c < n; ++c) {
    std::size_t p = c;
    for (std::size_t r = c + 1; r < n; ++r) {
      if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
    }
    if (std::fabs(a[p][c]) < 1e-12) {
      throw std::runtime_error("matriz singular (atributos colineares ou linhas de menos?)");
    }
    if (p != c) {
      std::swap(a[p], a[c]);
      std::swap(b[p], b[c]);
    }
    for (std::size_t r = c + 1; r < n; ++r) {
      const double f = a[r][c] / a[c][c];
      for (std::size_t k = c; k < n; ++k) a[r][k] -= f * a[c][k];
      b[r] -= f * b[c];
    }
  }
  std::vector<double> x(n);
  for (std::size_t r = n; r-- > 0;) {
    double s = b[r];
    for (std::size_t k = r + 1; k < n; ++k) s -= a[r][k] * x[k];
    x[r] = s / a[r][r];
  }
  return x;
}

double sigmoide(double z) { return 1.0 / (1.0 + std::exp(-z)); }

}  // namespace

// Ajusta o pipeline do usuario no treino: categorias (ordem de aparição) e
// media/desvio dos atributos em `padronizar:`.
static void exp_ajustar(Interpreter::ExpModel& m, const std::vector<Value>& treino,
                        const std::vector<std::string>& std_cols) {
  m.categorias.clear();
  for (const std::string& col : m.quentes) {
    std::vector<std::string> cats;
    for (const Value& r : treino) {
      const Value* c = r.map->find(col);
      if (!c || c->kind == ValueKind::Nulo) {
        throw std::runtime_error("coluna categorica '" + col + "' tem valor ausente/nulo");
      }
      const std::string k = chave_valor(*c);
      if (std::find(cats.begin(), cats.end(), k) == cats.end()) cats.push_back(k);
    }
    if (cats.size() < 2) {
      throw std::runtime_error("coluna categorica '" + col + "' tem 1 unica categoria no treino");
    }
    m.categorias[col] = std::move(cats);
  }
  m.medias.assign(m.numericas.size(), 0.0);
  m.desvios.assign(m.numericas.size(), 1.0);
  m.usa_std.assign(m.numericas.size(), 0);
  for (std::size_t j = 0; j < m.numericas.size(); ++j) {
    if (std::find(std_cols.begin(), std_cols.end(), m.numericas[j]) == std_cols.end()) continue;
    m.usa_std[j] = 1;
    double soma = 0.0;
    for (const Value& r : treino) soma += celula_num(*r.map->find(m.numericas[j]), m.numericas[j]);
    const double media = soma / static_cast<double>(treino.size());
    double var = 0.0;
    for (const Value& r : treino) {
      const double d = celula_num(*r.map->find(m.numericas[j]), m.numericas[j]) - media;
      var += d * d;
    }
    var /= static_cast<double>(treino.size());
    m.medias[j] = media;
    m.desvios[j] = var > 0.0 ? std::sqrt(var) : 1.0;  // constante -> zeros
  }
}

// Largura do vetor final: numericas + blocos one-hot.
static std::size_t exp_largura(const Interpreter::ExpModel& m) {
  std::size_t w = m.numericas.size();
  for (const std::string& col : m.quentes) {
    auto it = m.categorias.find(col);
    if (it != m.categorias.end()) w += it->second.size();
  }
  return w;
}

// Aplica o pipeline numa linha (treino ou nova): numericas (com padronizar)
// + one-hot (categoria nova vira zeros).
static std::vector<double> exp_vetor(const Interpreter::ExpModel& m, const Value& row) {
  if (row.kind != ValueKind::Mapa || !row.map) throw std::runtime_error("linha nao e um mapa");
  std::vector<double> x;
  x.reserve(exp_largura(m));
  for (std::size_t j = 0; j < m.numericas.size(); ++j) {
    const Value* c = row.map->find(m.numericas[j]);
    if (!c) throw std::runtime_error("prever: falta o atributo '" + m.numericas[j] + "'");
    double v = celula_num(*c, m.numericas[j]);
    if (m.usa_std[j]) v = (v - m.medias[j]) / m.desvios[j];
    x.push_back(v);
  }
  for (const std::string& col : m.quentes) {
    const Value* c = row.map->find(col);
    if (!c) throw std::runtime_error("prever: falta o atributo '" + col + "'");
    const std::string k = chave_valor(*c);
    const std::vector<std::string>& cats = m.categorias.at(col);
    for (const std::string& cat : cats) x.push_back(cat == k ? 1.0 : 0.0);
  }
  return x;
}

// Padronizacao interna (logistica/knn): ajusta media/desvio por posicao no
// treino e aplica em qualquer vetor.
static void exp_std_ajustar(Interpreter::ExpModel& m, const std::vector<std::vector<double>>& xt) {
  const std::size_t f = xt.empty() ? 0 : xt[0].size();
  m.imedias.assign(f, 0.0);
  m.idesvios.assign(f, 1.0);
  for (std::size_t j = 0; j < f; ++j) {
    double soma = 0.0;
    for (const auto& r : xt) soma += r[j];
    const double media = soma / static_cast<double>(xt.size());
    double var = 0.0;
    for (const auto& r : xt) {
      const double d = r[j] - media;
      var += d * d;
    }
    m.imedias[j] = media;
    m.idesvios[j] = var > 0.0 ? std::sqrt(var / static_cast<double>(xt.size())) : 1.0;
  }
}

static std::vector<double> exp_std_aplicar(const Interpreter::ExpModel& m,
                                           const std::vector<double>& x) {
  std::vector<double> z = x;
  for (std::size_t j = 0; j < z.size() && j < m.imedias.size(); ++j) {
    z[j] = (z[j] - m.imedias[j]) / m.idesvios[j];
  }
  return z;
}

void Interpreter::run_experimento(const Item& decl) {
  const std::string name = decl_name(decl);
  if (!decl.block) fail(decl.span, "experimento '" + name + "' sem configuracao");
  const ast::Block& cfg = *decl.block;
  ExpModel m;
  // Particoes e metricas para impressao (preenchidas no try).
  std::vector<std::string> relatorio;
  std::size_t n_tr = 0, n_va = 0, n_te = 0, largura = 0, total = 0;
  try {
    // ---- dados
    const Item* fd = find_field(cfg, "dados");
    if (!fd || !fd->value) throw std::runtime_error("falta 'dados:' (tabela, lista de mapas ou caminho .csv/.parquet/.json)");
    Value dados = eval(*fd->value, root_);
    std::vector<Value> linhas;
    if (dados.kind == ValueKind::Tabela || dados.kind == ValueKind::Lista) {
      if (!dados.list) throw std::runtime_error("'dados' vazio");
      linhas.assign(dados.list->begin(), dados.list->end());
    } else if (dados.kind == ValueKind::Texto) {
      const std::string& caminho = dados.s;
      Value t;
      if (caminho.size() >= 4 && caminho.compare(caminho.size() - 4, 4, ".csv") == 0) {
        t = read_csv_file(caminho, fd->value->span);
      } else if (caminho.size() >= 8 && caminho.compare(caminho.size() - 8, 8, ".parquet") == 0) {
        t = rt::parquet_read(caminho);
      } else if (caminho.size() >= 5 && caminho.compare(caminho.size() - 5, 5, ".json") == 0) {
        std::ifstream in(caminho);
        if (!in) throw std::runtime_error("nao foi possivel abrir '" + caminho + "'");
        std::ostringstream ss;
        ss << in.rdbuf();
        t = rt::json_parse(ss.str());
      } else {
        throw std::runtime_error("'dados' como texto precisa de .csv, .parquet ou .json (veio '" + caminho + "')");
      }
      if (t.kind == ValueKind::Lista) t.kind = ValueKind::Tabela;
      if (t.kind != ValueKind::Tabela || !t.list) {
        throw std::runtime_error("arquivo '" + caminho + "' nao produziu tabela");
      }
      linhas.assign(t.list->begin(), t.list->end());
    } else {
      throw std::runtime_error("'dados' deve ser tabela, lista de mapas ou caminho (veio " +
                               std::string(dados.type_name()) + ")");
    }
    for (const Value& r : linhas) {
      if (r.kind != ValueKind::Mapa || !r.map) {
        throw std::runtime_error("'dados' deve ser lista de mapas (uma linha nao e mapa)");
      }
    }
    if (linhas.size() < 2) {
      throw std::runtime_error("precisa de ao menos 2 linhas (tem " +
                               std::to_string(linhas.size()) + ")");
    }
    total = linhas.size();

    // ---- modelo (antes do alvo: kmeans nao usa alvo)
    const Item* fm = find_field(cfg, "modelo");
    if (!fm || !fm->value || fm->value->kind != ExprKind::Name) {
      throw std::runtime_error("falta 'modelo: regressao_linear | regressao_logistica | knn | kmeans'");
    }
    const std::string kind = fm->value->text;
    const ast::Block* hyp = fm->block.get();
    auto hnum = [&](const char* k, double fb) { return hyp ? field_num(*hyp, k, fb) : fb; };
    auto hint = [&](const char* k, int fb) { return hyp ? field_int(*hyp, k, fb) : fb; };
    if (kind != "regressao_linear" && kind != "regressao_logistica" && kind != "knn" &&
        kind != "kmeans") {
      if (kind == "floresta_aleatoria" || kind == "gradiente_impulsionado" || kind == "svm") {
        fail(fm->value->span, "modelo '" + kind + "' ainda nao implementado na 1a passada",
             DiagCode::NotImplemented);
      }
      throw std::runtime_error("modelo '" + kind +
                               "' desconhecido (use regressao_linear | regressao_logistica | knn | kmeans)");
    }
    m.kind = kind;

    // ---- alvo
    std::string alvo;
    if (const Item* fa = find_field(cfg, "alvo"); fa && fa->value) {
      Value av = eval(*fa->value, root_);
      if (av.kind != ValueKind::Texto) throw std::runtime_error("'alvo' deve ser texto com o nome da coluna");
      alvo = av.s;
    }
    if (kind == "kmeans") {
      if (!alvo.empty()) throw std::runtime_error("kmeans nao usa 'alvo:' (remova o campo)");
    } else if (alvo.empty()) {
      throw std::runtime_error("falta 'alvo: \"coluna\"'");
    }
    if (!alvo.empty() && !linhas[0].map->find(alvo)) {
      throw std::runtime_error("coluna alvo '" + alvo + "' nao existe nos dados");
    }

    // ---- atributos (default: todas menos o alvo)
    const Item* fat = find_field(cfg, "atributos");
    if (fat && fat->value) {
      m.numericas = nomes_crus(*fat->value, "'atributos'");
    } else {
      for (const auto& kv : linhas[0].map->items) {
        if (kv.first != alvo) m.numericas.push_back(kv.first);
      }
      if (m.numericas.empty()) throw std::runtime_error("sem atributos (so ha a coluna alvo?)");
    }
    if (std::find(m.numericas.begin(), m.numericas.end(), alvo) != m.numericas.end()) {
      throw std::runtime_error("'alvo' nao pode estar em 'atributos'");
    }
    for (const std::string& a : m.numericas) {
      if (!linhas[0].map->find(a)) throw std::runtime_error("atributo '" + a + "' nao existe nos dados");
    }

    // ---- pre_processar: '- um_de_n: [cols]' / '- padronizar: [cols]'
    std::vector<std::string> std_cols;
    if (const Item* pp = find_field(cfg, "pre_processar"); pp && pp->block) {
      for (const auto& it : pp->block->items) {
        const Item* f = (it && it->kind == ItemKind::ListEntry && it->child) ? it->child.get()
                                                                             : it.get();
        if (!f || f->kind != ItemKind::Field || !f->value ||
            (f->key != "um_de_n" && f->key != "padronizar")) {
          throw std::runtime_error(
              "pre_processar: use '- um_de_n: [cols]' ou '- padronizar: [cols]'");
        }
        std::vector<std::string> cols = nomes_crus(*f->value, "pre_processar");
        for (const std::string& c : cols) {
          if (std::find(m.numericas.begin(), m.numericas.end(), c) == m.numericas.end()) {
            throw std::runtime_error("pre_processar: '" + c + "' nao esta em 'atributos'");
          }
        }
        if (f->key == "um_de_n") {
          m.quentes.insert(m.quentes.end(), cols.begin(), cols.end());
        } else {
          std_cols.insert(std_cols.end(), cols.begin(), cols.end());
        }
      }
    }
    // Separa numericas de quentes (quente sai da lista numerica, mantendo ordem).
    {
      std::vector<std::string> nums;
      for (const std::string& a : m.numericas) {
        if (std::find(m.quentes.begin(), m.quentes.end(), a) == m.quentes.end()) nums.push_back(a);
      }
      m.numericas.swap(nums);
    }
    for (const std::string& c : std_cols) {
      if (std::find(m.quentes.begin(), m.quentes.end(), c) != m.quentes.end()) {
        throw std::runtime_error("coluna '" + c + "' nao pode ser categoria e padronizada");
      }
      if (std::find(m.numericas.begin(), m.numericas.end(), c) == m.numericas.end()) {
        throw std::runtime_error("padronizar: '" + c + "' nao e atributo numerico");
      }
    }

    // ---- dividir
    double f_tr = 0.75, f_va = 0.0, f_te = 0.25;
    if (const Item* fdv = find_field(cfg, "dividir"); fdv && fdv->value) {
      Value dv = eval(*fdv->value, root_);
      if (dv.kind != ValueKind::Mapa || !dv.map) {
        throw std::runtime_error("'dividir' deve ser mapa { treino: _, teste: _ [, validacao: _] }");
      }
      auto fracao = [&](const char* k, double fb) {
        const Value* v = dv.map->find(k);
        if (!v) return fb;
        if (!v->is_number()) throw std::runtime_error("'dividir." + std::string(k) + "' deve ser numero");
        return v->as_number();
      };
      f_tr = fracao("treino", f_tr);
      f_va = fracao("validacao", 0.0);
      f_te = fracao("teste", f_te);
      if (!(f_tr > 0.0) || !(f_te > 0.0) || f_va < 0.0 || std::fabs(f_tr + f_va + f_te - 1.0) > 1e-9) {
        throw std::runtime_error("'dividir' invalido (treino > 0, teste > 0, validacao >= 0, soma = 1)");
      }
    }
    const int semente = field_int(cfg, "semente", 42);
    std::vector<std::size_t> idx(total);
    for (std::size_t i = 0; i < total; ++i) idx[i] = i;
    std::mt19937 rng(static_cast<std::uint32_t>(semente));
    std::shuffle(idx.begin(), idx.end(), rng);
    n_tr = std::max<std::size_t>(1, static_cast<std::size_t>(f_tr * total));
    n_va = static_cast<std::size_t>(f_va * total);
    if (n_tr + n_va >= total) n_tr = total - n_va - 1;
    n_te = total - n_tr - n_va;
    if (n_te == 0) {  // garante teste mesmo com poucas linhas
      n_te = 1;
      n_tr -= 1;
    }
    auto fatia = [&](std::size_t ini, std::size_t n) {
      std::vector<Value> out;
      for (std::size_t k = 0; k < n; ++k) out.push_back(linhas[idx[ini + k]]);
      return out;
    };
    const std::vector<Value> treino = fatia(0, n_tr);
    const std::vector<Value> valid = fatia(n_tr, n_va);
    const std::vector<Value> teste = fatia(n_tr + n_va, n_te);

    // ---- pipeline + tarefa
    exp_ajustar(m, treino, std_cols);
    largura = exp_largura(m);
    auto feats = [&](const std::vector<Value>& rs) {
      std::vector<std::vector<double>> out;
      out.reserve(rs.size());
      for (const Value& r : rs) out.push_back(exp_vetor(m, r));
      return out;
    };
    const std::vector<std::vector<double>> xt = feats(treino);
    const std::vector<std::vector<double>> xv = feats(valid);
    const std::vector<std::vector<double>> xs = feats(teste);

    // y do treino / classes
    std::vector<int> yt;
    std::vector<double> ytr;
    if (kind != "kmeans") {
      const bool regr = (kind == "regressao_linear");
      for (const Value& r : treino) {
        const Value* c = r.map->find(alvo);
        if (!c || c->kind == ValueKind::Nulo) {
          throw std::runtime_error("alvo '" + alvo + "' com valor ausente/nulo no treino");
        }
        if (regr) {
          if (!c->is_number()) {
            throw std::runtime_error("regressao_linear preve numero; alvo '" + alvo + "' e " +
                                     std::string(c->type_name()) + " (use regressao_logistica | knn)");
          }
          ytr.push_back(c->as_number());
        } else {
          const std::string k = chave_valor(*c);
          int id = -1;
          for (std::size_t j = 0; j < m.classes.size(); ++j) {
            if (chave_valor(m.classes[j]) == k) {
              id = static_cast<int>(j);
              break;
            }
          }
          if (id < 0) {
            id = static_cast<int>(m.classes.size());
            m.classes.push_back(*c);
          }
          yt.push_back(id);
        }
      }
      if (!regr && kind == "regressao_logistica" && m.classes.size() != 2) {
        throw std::runtime_error("regressao_logistica e binaria (" +
                                 std::to_string(m.classes.size()) + " classes no alvo; use knn)");
      }
      m.classificacao = !regr;
    }

    // ---- ajuste por modelo
    if (kind == "regressao_linear") {
      const std::size_t f = largura;
      std::vector<std::vector<double>> a(f + 1, std::vector<double>(f + 1, 0.0));
      std::vector<double> b(f + 1, 0.0);
      for (std::size_t i = 0; i < xt.size(); ++i) {
        for (std::size_t j = 0; j <= f; ++j) {
          const double vj = (j < f) ? xt[i][j] : 1.0;
          b[j] += vj * ytr[i];
          for (std::size_t k = 0; k <= f; ++k) a[j][k] += vj * ((k < f) ? xt[i][k] : 1.0);
        }
      }
      for (std::size_t j = 0; j <= f; ++j) a[j][j] += 1e-8;  // crista: estabilidade
      m.pesos = gauss(std::move(a), std::move(b));
    } else if (kind == "regressao_logistica") {
      exp_std_ajustar(m, xt);
      const double taxa = hnum("taxa", hnum("taxa_aprendizado", 0.5));
      const int epocas = hint("epocas", 500);
      if (!(taxa > 0.0) || epocas < 1) throw std::runtime_error("'taxa' > 0 e 'epocas' >= 1");
      const std::size_t f = largura;
      m.pesos.assign(f + 1, 0.0);
      std::vector<std::vector<double>> z;
      z.reserve(xt.size());
      for (const auto& r : xt) z.push_back(exp_std_aplicar(m, r));
      for (int e = 0; e < epocas; ++e) {
        std::vector<double> g(f + 1, 0.0);
        for (std::size_t i = 0; i < z.size(); ++i) {
          double s = m.pesos[f];
          for (std::size_t j = 0; j < f; ++j) s += m.pesos[j] * z[i][j];
          const double p = sigmoide(s);
          const double err = p - static_cast<double>(yt[i]);
          for (std::size_t j = 0; j < f; ++j) g[j] += err * z[i][j];
          g[f] += err;
        }
        const double inv = taxa / static_cast<double>(z.size());
        for (std::size_t j = 0; j <= f; ++j) m.pesos[j] -= inv * g[j];
      }
    } else if (kind == "knn") {
      exp_std_ajustar(m, xt);
      m.vizinhos = hint("vizinhos", 5);
      if (m.vizinhos < 1) throw std::runtime_error("'vizinhos' >= 1");
      if (static_cast<std::size_t>(m.vizinhos) > xt.size()) {
        throw std::runtime_error("'vizinhos' (" + std::to_string(m.vizinhos) + ") maior que o treino (" +
                                 std::to_string(xt.size()) + " linhas)");
      }
      m.base_x.reserve(xt.size());
      for (const auto& r : xt) m.base_x.push_back(exp_std_aplicar(m, r));
      m.base_y = yt;
      m.base_yr = ytr;
    } else {  // kmeans
      const int k = hint("grupos", -1);
      if (k < 1) throw std::runtime_error("kmeans exige 'grupos:' (numero de grupos >= 1)");
      if (static_cast<std::size_t>(k) > xt.size()) {
        throw std::runtime_error("'grupos' (" + std::to_string(k) + ") maior que o treino (" +
                                 std::to_string(xt.size()) + " linhas)");
      }
      std::vector<std::size_t> ordem(xt.size());
      for (std::size_t i = 0; i < ordem.size(); ++i) ordem[i] = i;
      std::shuffle(ordem.begin(), ordem.end(), rng);
      m.centroides.clear();
      for (int c = 0; c < k; ++c) m.centroides.push_back(xt[ordem[static_cast<std::size_t>(c)]]);
      std::vector<int> atrib(xt.size(), -1);
      for (int it = 0; it < 100; ++it) {
        bool mudou = false;
        for (std::size_t i = 0; i < xt.size(); ++i) {
          int melhor = 0;
          double bd = 0.0;
          for (std::size_t j = 0; j < xt[i].size(); ++j) {
            const double d = xt[i][j] - m.centroides[0][j];
            bd += d * d;
          }
          for (int c = 1; c < k; ++c) {
            double d2 = 0.0;
            for (std::size_t j = 0; j < xt[i].size(); ++j) {
              const double d = xt[i][j] - m.centroides[static_cast<std::size_t>(c)][j];
              d2 += d * d;
            }
            if (d2 < bd) {
              bd = d2;
              melhor = c;
            }
          }
          if (atrib[i] != melhor) {
            atrib[i] = melhor;
            mudou = true;
          }
        }
        if (!mudou) break;
        std::vector<std::vector<double>> soma(static_cast<std::size_t>(k),
                                              std::vector<double>(largura, 0.0));
        std::vector<std::size_t> cont(static_cast<std::size_t>(k), 0);
        for (std::size_t i = 0; i < xt.size(); ++i) {
          const int c = atrib[i] < 0 ? 0 : atrib[i];
          for (std::size_t j = 0; j < largura; ++j) soma[static_cast<std::size_t>(c)][j] += xt[i][j];
          cont[static_cast<std::size_t>(c)] += 1;
        }
        for (int c = 0; c < k; ++c) {
          if (cont[static_cast<std::size_t>(c)] == 0) continue;  // grupo vazio: mantem
          for (std::size_t j = 0; j < largura; ++j) {
            m.centroides[static_cast<std::size_t>(c)][j] =
                soma[static_cast<std::size_t>(c)][j] /
                static_cast<double>(cont[static_cast<std::size_t>(c)]);
          }
        }
      }
    }

    // ---- metricas
    std::vector<std::string> pedidas;
    if (const Item* fmet = find_field(cfg, "metricas"); fmet && fmet->value) {
      pedidas = nomes_crus(*fmet->value, "'metricas'");
    }
    auto prever_idx = [&](const std::vector<double>& xraw, double& proba) -> int {
      if (kind == "regressao_logistica") {
        const std::vector<double> z = exp_std_aplicar(m, xraw);
        double s = m.pesos[largura];
        for (std::size_t j = 0; j < largura; ++j) s += m.pesos[j] * z[j];
        proba = sigmoide(s);
        return proba >= 0.5 ? 1 : 0;
      }
      // knn
      std::vector<std::pair<double, std::size_t>> dist;
      const std::vector<double> z = exp_std_aplicar(m, xraw);
      for (std::size_t i = 0; i < m.base_x.size(); ++i) {
        double d2 = 0.0;
        for (std::size_t j = 0; j < largura; ++j) {
          const double d = z[j] - m.base_x[i][j];
          d2 += d * d;
        }
        dist.emplace_back(d2, i);
      }
      std::sort(dist.begin(), dist.end());
      if (m.classificacao) {
        std::vector<int> votos(m.classes.size(), 0);
        for (int v = 0; v < m.vizinhos; ++v) votos[static_cast<std::size_t>(m.base_y[dist[static_cast<std::size_t>(v)].second])] += 1;
        int melhor = 0;
        for (std::size_t c = 1; c < votos.size(); ++c) {
          if (votos[c] > votos[static_cast<std::size_t>(melhor)]) melhor = static_cast<int>(c);
        }
        proba = static_cast<double>(votos[static_cast<std::size_t>(melhor)]) /
                static_cast<double>(m.vizinhos);
        return melhor;
      }
      proba = 0.0;
      return -1;  // regressao: ver prever_num
    };
    auto prever_num = [&](const std::vector<double>& xraw) -> double {
      if (kind == "regressao_linear") {
        double s = m.pesos[largura];
        for (std::size_t j = 0; j < largura; ++j) s += m.pesos[j] * xraw[j];
        return s;
      }
      // knn regressao: media dos vizinhos
      const std::vector<double> z = exp_std_aplicar(m, xraw);
      std::vector<std::pair<double, std::size_t>> dist;
      for (std::size_t i = 0; i < m.base_x.size(); ++i) {
        double d2 = 0.0;
        for (std::size_t j = 0; j < largura; ++j) {
          const double d = z[j] - m.base_x[i][j];
          d2 += d * d;
        }
        dist.emplace_back(d2, i);
      }
      std::sort(dist.begin(), dist.end());
      double soma = 0.0;
      for (int v = 0; v < m.vizinhos; ++v) soma += m.base_yr[dist[static_cast<std::size_t>(v)].second];
      return soma / static_cast<double>(m.vizinhos);
    };

    if (kind == "kmeans") {
      if (!pedidas.empty()) {
        throw std::runtime_error("kmeans reporta inercia automaticamente; remova 'metricas:'");
      }
      auto avalia_grupo = [&](const std::vector<std::vector<double>>& xx, const char* rot) {
        std::vector<std::size_t> tam(m.centroides.size(), 0);
        double iner = 0.0;
        for (const auto& r : xx) {
          std::size_t melhor = 0;
          double bd = 0.0;
          for (std::size_t j = 0; j < r.size(); ++j) {
            const double d = r[j] - m.centroides[0][j];
            bd += d * d;
          }
          for (std::size_t c = 1; c < m.centroides.size(); ++c) {
            double d2 = 0.0;
            for (std::size_t j = 0; j < r.size(); ++j) {
              const double d = r[j] - m.centroides[c][j];
              d2 += d * d;
            }
            if (d2 < bd) {
              bd = d2;
              melhor = c;
            }
          }
          tam[melhor] += 1;
          iner += bd;
        }
        std::string t = "inercia: " + fmt4(iner);
        std::string g = "grupos:";
        for (std::size_t c = 0; c < tam.size(); ++c) g += " " + std::to_string(tam[c]);
        relatorio.push_back(std::string(rot) + t);
        relatorio.push_back(std::string(rot) + g);
      };
      avalia_grupo(xs, "teste ");
      if (!xv.empty()) avalia_grupo(xv, "validacao ");
    } else if (m.classificacao) {
      if (pedidas.empty()) pedidas = {"acuracia"};
      for (const std::string& p : pedidas) {
        if (p != "acuracia" && p != "f1" && p != "auc" && p != "matriz_confusao") {
          throw std::runtime_error("metrica '" + p +
                                   "' invalida p/ classificacao (use acuracia | f1 | auc | matriz_confusao)");
        }
      }
      auto avalia_classe = [&](const std::vector<std::vector<double>>& xx,
                               const std::vector<Value>& rs, const char* rot) {
        const std::size_t K = m.classes.size();
        std::vector<std::size_t> ac(K, 0), tot(K, 0), previstos(K, 0);
        std::vector<std::vector<std::size_t>> mat(K, std::vector<std::size_t>(K, 0));
        std::size_t ok = 0;
        std::vector<std::pair<double, int>> ranking;  // (proba da classe 1, rotulo 0/1)
        for (std::size_t i = 0; i < xx.size(); ++i) {
          const Value* c = rs[i].map->find(alvo);
          int real = -1;
          const std::string k = chave_valor(*c);
          for (std::size_t j = 0; j < K; ++j) {
            if (chave_valor(m.classes[j]) == k) {
              real = static_cast<int>(j);
              break;
            }
          }
          if (real < 0) throw std::runtime_error("classe nova no teste/validacao (so vale o que apareceu no treino)");
          double proba = 0.0;
          const int pred = prever_idx(xx[i], proba);
          if (pred == real) ++ok;
          ac[static_cast<std::size_t>(pred)] += (pred == real ? 1 : 0);
          tot[static_cast<std::size_t>(real)] += 1;
          previstos[static_cast<std::size_t>(pred)] += 1;
          mat[static_cast<std::size_t>(real)][static_cast<std::size_t>(pred)] += 1;
          if (K == 2) ranking.emplace_back(proba, real == 1 ? 1 : 0);
        }
        const double n = static_cast<double>(xx.size());
        for (const std::string& p : pedidas) {
          if (p == "acuracia") {
            relatorio.push_back(std::string(rot) + "acuracia: " + fmt4(static_cast<double>(ok) / n));
          } else if (p == "f1") {
            // F1 ponderado pelo suporte (padrao intuitivo: classes ausentes
            // no teste nao derrubam a media).
            double soma = 0.0, sup = 0.0;
            for (std::size_t c = 0; c < K; ++c) {
              const double prec = previstos[c] ? static_cast<double>(ac[c]) / previstos[c] : 0.0;
              const double rec = tot[c] ? static_cast<double>(ac[c]) / tot[c] : 0.0;
              const double f1c = (prec + rec > 0.0) ? 2.0 * prec * rec / (prec + rec) : 0.0;
              soma += f1c * static_cast<double>(tot[c]);
              sup += static_cast<double>(tot[c]);
            }
            relatorio.push_back(std::string(rot) + "f1: " + fmt4(sup > 0.0 ? soma / sup : 0.0));
          } else if (p == "auc") {
            if (K != 2) {
              relatorio.push_back(std::string(rot) + "auc: (exige 2 classes; nota)");
              continue;
            }
            std::sort(ranking.begin(), ranking.end());
            double auc = 0.0;
            std::size_t npos = 0, nneg = 0;
            for (const auto& pr : ranking) {
              if (pr.second == 1) ++npos;
              else ++nneg;
            }
            if (npos == 0 || nneg == 0) {
              relatorio.push_back(std::string(rot) +
                                  "auc: (exige exemplos das 2 classes no teste; nota)");
              continue;
            }
            std::size_t neg_antes = 0;
            std::size_t i = 0;
            // Mann-Whitney com empate = 0.5.
            while (i < ranking.size()) {
              std::size_t j = i;
              while (j < ranking.size() && ranking[j].first == ranking[i].first) ++j;
              std::size_t pos_g = 0;
              for (std::size_t t = i; t < j; ++t) {
                if (ranking[t].second == 1) ++pos_g;
              }
              auc += static_cast<double>(pos_g) * (static_cast<double>(neg_antes) +
                                                   static_cast<double>((j - i - pos_g)) * 0.5);
              for (std::size_t t = i; t < j; ++t) {
                if (ranking[t].second == 0) ++neg_antes;
              }
              i = j;
            }
            auc /= (static_cast<double>(npos) * static_cast<double>(nneg));
            relatorio.push_back(std::string(rot) + "auc: " + fmt4(auc));
          } else if (p == "matriz_confusao") {
            std::string bloco = std::string(rot) + "matriz_confusao (linhas=true, colunas=previsto):";
            std::string cab = std::string(14, ' ');
            for (std::size_t c = 0; c < K; ++c) {
              std::string r = rotulo_valor(m.classes[c]);
              cab += std::string(12 - std::min<std::size_t>(12, r.size()), ' ') + r;
            }
            bloco += "\n" + cab;
            for (std::size_t r = 0; r < K; ++r) {
              std::string lin = rotulo_valor(m.classes[r]);
              lin += std::string(14 - std::min<std::size_t>(14, lin.size()), ' ');
              for (std::size_t c = 0; c < K; ++c) {
                const std::string n = std::to_string(mat[r][c]);
                lin += std::string(12 - std::min<std::size_t>(12, n.size()), ' ') + n;
              }
              bloco += "\n" + lin;
            }
            relatorio.push_back(bloco);
          }
        }
      };
      avalia_classe(xs, teste, "teste ");
      if (!xv.empty()) avalia_classe(xv, valid, "validacao ");
    } else {
      if (pedidas.empty()) pedidas = {"rmse"};
      for (const std::string& p : pedidas) {
        if (p != "rmse" && p != "r2") {
          throw std::runtime_error("metrica '" + p + "' invalida p/ regressao (use rmse | r2)");
        }
      }
      auto avalia_regr = [&](const std::vector<std::vector<double>>& xx,
                             const std::vector<Value>& rs, const char* rot) {
        double ss_res = 0.0, soma = 0.0;
        for (std::size_t i = 0; i < xx.size(); ++i) {
          const double real = rs[i].map->find(alvo)->as_number();
          const double d = real - prever_num(xx[i]);
          ss_res += d * d;
          soma += real;
        }
        const double n = static_cast<double>(xx.size());
        const double media = soma / n;
        double ss_tot = 0.0;
        for (std::size_t i = 0; i < xx.size(); ++i) {
          const double d = rs[i].map->find(alvo)->as_number() - media;
          ss_tot += d * d;
        }
        for (const std::string& p : pedidas) {
          if (p == "rmse") {
            relatorio.push_back(std::string(rot) + "rmse: " + fmt4(std::sqrt(ss_res / n)));
          } else {
            const double r2 = ss_tot > 0.0 ? 1.0 - ss_res / ss_tot : (ss_res == 0.0 ? 1.0 : 0.0);
            relatorio.push_back(std::string(rot) + "r2: " + fmt4(r2));
          }
        }
      };
      // Reconstroi y do teste/validacao a partir das linhas (numeros ja validados).
      avalia_regr(xs, teste, "teste ");
      if (!xv.empty()) avalia_regr(xv, valid, "validacao ");
    }

    // ---- registrar_em (mlflow:// -> JSON local; REST fica p/ depois)
    if (const Item* fr = find_field(cfg, "registrar_em"); fr && fr->value) {
      Value rv = eval(*fr->value, root_);
      if (rv.kind != ValueKind::Texto) throw std::runtime_error("'registrar_em' deve ser texto (ex.: \"mlflow://host/experimento\")");
      if (rv.s.rfind("mlflow://", 0) != 0) {
        throw std::runtime_error("'registrar_em' suporta 'mlflow://...' (1a passada grava o run em JSON local)");
      }
      Value doc = Value::mapa();
      doc.map->set("experimento", Value::texto(name));
      doc.map->set("modelo", Value::texto(kind));
      doc.map->set("linhas", Value::inteiro(static_cast<std::int64_t>(total)));
      doc.map->set("semente", Value::inteiro(semente));
      Value mets = Value::mapa();
      for (const std::string& lin : relatorio) {
        const std::size_t p = lin.find(':');
        if (p == std::string::npos) continue;
        mets.map->set(lin.substr(0, p), Value::texto(lin.substr(p + 2)));
      }
      doc.map->set("metricas", mets);
      const std::string caminho = "experimento_" + name + "_run.json";
      std::ofstream out(caminho, std::ios::trunc);
      if (!out) throw std::runtime_error("nao foi possivel gravar '" + caminho + "'");
      out << rt::json_dump(doc) << "\n";
      relatorio.push_back("run salvo em " + caminho + " (mlflow REST: 1a passada grava JSON local)");
    }

    // ---- saida
    out_ << "== experimento " << name << " ==\n";
    out_ << "modelo: " << kind << " | linhas: " << total << " (treino " << n_tr;
    if (n_va) out_ << ", validacao " << n_va;
    out_ << ", teste " << n_te << ") | atributos: " << largura << "\n";
    for (const std::string& lin : relatorio) out_ << lin << "\n";
  } catch (const std::exception& e) {
    fail(decl.span, "experimento " + name + ": " + e.what());
  }

  {
    std::lock_guard<std::mutex> lk(experimentos_mutex_);
    experimentos_[name] = std::move(m);
  }
}

// Previsao com modelo ajustado: {classe, probabilidade} | {valor} | {grupo}.
Value Interpreter::experimento_prever(const std::string& nome, const Value& entrada, Span span) {
  if (entrada.kind != ValueKind::Mapa || !entrada.map) {
    fail(span, "prever espera um mapa {atributo: valor} (sem a coluna alvo)");
  }
  ExpModel m;
  {
    std::lock_guard<std::mutex> lk(experimentos_mutex_);
    auto it = experimentos_.find(nome);
    if (it == experimentos_.end()) {
      fail(span, "experimento '" + nome + "' nao foi executado (rode o programa antes de prever)");
    }
    m = it->second;
  }
  std::vector<double> x;
  try {
    x = exp_vetor(m, entrada);
  } catch (const std::exception& e) {
    fail(span, std::string(e.what()));
  }
  Value out = Value::mapa();
  if (m.kind == "kmeans") {
    std::size_t melhor = 0;
    double bd = 0.0;
    for (std::size_t j = 0; j < x.size(); ++j) {
      const double d = x[j] - m.centroides[0][j];
      bd += d * d;
    }
    for (std::size_t c = 1; c < m.centroides.size(); ++c) {
      double d2 = 0.0;
      for (std::size_t j = 0; j < x.size(); ++j) {
        const double d = x[j] - m.centroides[c][j];
        d2 += d * d;
      }
      if (d2 < bd) {
        bd = d2;
        melhor = c;
      }
    }
    out.map->set("grupo", Value::inteiro(static_cast<std::int64_t>(melhor)));
    return out;
  }
  if (m.classificacao) {
    double proba = 0.0;
    int pred = 0;
    if (m.kind == "regressao_logistica") {
      const std::vector<double> z = exp_std_aplicar(m, x);
      double s = m.pesos[x.size()];
      for (std::size_t j = 0; j < x.size(); ++j) s += m.pesos[j] * z[j];
      const double p1 = sigmoide(s);  // P(classes[1])
      pred = p1 >= 0.5 ? 1 : 0;
      proba = (pred == 1) ? p1 : 1.0 - p1;  // P da classe prevista
    } else {  // knn
      const std::vector<double> z = exp_std_aplicar(m, x);
      std::vector<std::pair<double, std::size_t>> dist;
      for (std::size_t i = 0; i < m.base_x.size(); ++i) {
        double d2 = 0.0;
        for (std::size_t j = 0; j < x.size(); ++j) {
          const double d = z[j] - m.base_x[i][j];
          d2 += d * d;
        }
        dist.emplace_back(d2, i);
      }
      std::sort(dist.begin(), dist.end());
      std::vector<int> votos(m.classes.size(), 0);
      for (int v = 0; v < m.vizinhos; ++v) {
        votos[static_cast<std::size_t>(m.base_y[dist[static_cast<std::size_t>(v)].second])] += 1;
      }
      for (std::size_t c = 1; c < votos.size(); ++c) {
        if (votos[c] > votos[static_cast<std::size_t>(pred)]) pred = static_cast<int>(c);
      }
      proba = static_cast<double>(votos[static_cast<std::size_t>(pred)]) /
              static_cast<double>(m.vizinhos);
    }
    out.map->set("classe", m.classes[static_cast<std::size_t>(pred)]);
    out.map->set("probabilidade", Value::decimal(proba));
    return out;
  }
  double v = 0.0;
  if (m.kind == "regressao_linear") {
    v = m.pesos[x.size()];
    for (std::size_t j = 0; j < x.size(); ++j) v += m.pesos[j] * x[j];
  } else {  // knn regressao
    const std::vector<double> z = exp_std_aplicar(m, x);
    std::vector<std::pair<double, std::size_t>> dist;
    for (std::size_t i = 0; i < m.base_x.size(); ++i) {
      double d2 = 0.0;
      for (std::size_t j = 0; j < x.size(); ++j) {
        const double d = z[j] - m.base_x[i][j];
        d2 += d * d;
      }
      dist.emplace_back(d2, i);
    }
    std::sort(dist.begin(), dist.end());
    for (int k = 0; k < m.vizinhos; ++k) v += m.base_yr[dist[static_cast<std::size_t>(k)].second];
    v /= static_cast<double>(m.vizinhos);
  }
  out.map->set("valor", Value::decimal(v));
  return out;
}

// Forma `experimento Nome.prever <mapa>` (espelha `modelo Nome.executar`).
Value Interpreter::eval_experimento_call(const Expr& call, Env& env) {
  if (call.args.size() != 1 || call.args[0].value->kind != ExprKind::Call) {
    fail(call.span, "uso: experimento <Nome>.prever <mapa>");
  }
  const Expr& inner = *call.args[0].value;
  if (!inner.lhs || inner.lhs->kind != ExprKind::Member || !inner.lhs->lhs ||
      inner.lhs->lhs->kind != ExprKind::Name) {
    fail(inner.span, "uso: experimento <Nome>.prever <mapa>");
  }
  const std::string ename = inner.lhs->lhs->text;
  const std::string method = inner.lhs->text;
  auto it = entities_.find(ename);
  if (it == entities_.end() || it->second->key != "experimento") {
    fail(inner.span, "'" + ename + "' nao e um experimento declarado");
  }
  if (method != "prever") {
    fail(inner.span, "metodo de experimento '" + method + "' desconhecido (use prever)");
  }
  if (inner.args.empty()) fail(inner.span, "prever precisa de um mapa {atributo: valor}");
  Value entrada = eval(*inner.args[0].value, env);
  return experimento_prever(ename, entrada, inner.span);
}

// ------------------------------------------------------------------ LLM + RAG

namespace {

std::string field_str(const ast::Block& block, std::string_view key) {
  const Item* f = find_field(block, key);
  return (f && f->value && f->value->kind == ExprKind::TextLit) ? f->value->text : std::string();
}

std::string field_env_or_text(const ast::Block& block, std::string_view key) {
  const Item* f = find_field(block, key);
  if (!f || !f->value) return {};
  const Expr* v = f->value.get();
  if (v->kind == ExprKind::TextLit || v->kind == ExprKind::Name) return v->text;
  if (v->kind == ExprKind::Call && v->lhs && v->lhs->kind == ExprKind::Name &&
      v->lhs->text == "env" && !v->args.empty() && v->args[0].value->kind == ExprKind::TextLit) {
    const char* e = std::getenv(v->args[0].value->text.c_str());
    return e ? std::string(e) : std::string();
  }
  return {};
}

std::string row_text(const Value& item) {
  if (item.kind == ValueKind::Mapa && item.map) {
    for (const char* key : {"texto", "conteudo", "text", "trecho", "content"}) {
      if (const Value* v = item.map->find(key); v && v->kind == ValueKind::Texto) return v->s;
    }
  }
  return item.kind == ValueKind::Texto ? item.s : to_display(item);
}

Value default_for_type(const Expr* type_expr) {
  if (!type_expr) return Value::nulo();
  if (type_expr->kind == ExprKind::Name) {
    const std::string& w = type_expr->text;
    if (w == "texto") return Value::texto("exemplo");
    if (w == "inteiro") return Value::inteiro(0);
    if (w == "decimal") return Value::decimal(0.0);
    if (w == "logico") return Value::logico(false);
    return Value::nulo();
  }
  if (type_expr->kind == ExprKind::TextLit) return Value::texto(type_expr->text);
  if (type_expr->kind == ExprKind::Binary && type_expr->text == "|") {
    const Expr* e = type_expr;
    while (e && e->kind == ExprKind::Binary) e = e->lhs.get();
    return Value::texto(e && e->kind == ExprKind::TextLit ? e->text : "");
  }
  if (type_expr->kind == ExprKind::Index && type_expr->lhs &&
      type_expr->lhs->kind == ExprKind::Name && type_expr->lhs->text == "lista") {
    return Value::lista();
  }
  return Value::nulo();
}

}  // namespace

rt::LlmConfig Interpreter::llm_config(const std::string& name, Span span) {
  auto it = entities_.find(name);
  if (it == entities_.end() || it->second->key != "llm" || !it->second->block) {
    fail(span, "'" + name + "' nao e um 'llm' declarado");
  }
  const ast::Block& b = *it->second->block;
  rt::LlmConfig cfg;
  cfg.provider = field_word(b, "provedor", "anthropic");
  cfg.model = field_str(b, "modelo");
  cfg.api_key = field_env_or_text(b, "chave");
  cfg.base_url = field_env_or_text(b, "base_url");
  cfg.temperature = field_num(b, "temperatura", 0.2);
  cfg.max_tokens = field_int(b, "max_tokens", 1024);
  return cfg;
}

rt::Value Interpreter::eval_perguntar(const Expr& call, Env& env) {
  std::string llm_name;
  if (!call.args.empty() && call.args[0].name.empty()) {
    Value v = eval(*call.args[0].value, env);
    llm_name = v.kind == ValueKind::Texto ? v.s : "";
  }

  rt::ValueMap kw;
  for (const auto& a : call.args) {
    if (!a.name.empty()) kw.set(a.name, eval(*a.value, env));
  }
  if (call.block) {
    for (const auto& it : call.block->items) {
      if (it && it->kind == ItemKind::Field && it->value) kw.set(it->key, eval(*it->value, env));
    }
  }

  auto text_of = [](const Value* v) { return v && v->kind == ValueKind::Texto ? v->s : std::string(); };
  std::string system = text_of(kw.find("sistema"));
  std::string user = text_of(kw.find("usuario"));
  if (user.empty()) user = text_of(kw.find("prompt"));

  rt::LlmConfig cfg = llm_config(llm_name, call.span);
  std::string raw;
  try {
    raw = rt::llm_chat(cfg, system, user);
  } catch (const std::exception& e) {
    fail(call.span, std::string("LLM: ") + e.what());
  }

  if (const Value* fmt = kw.find("formato"); fmt && fmt->kind == ValueKind::Texto) {
    return structured_from_tipo(fmt->s, raw, call.span);
  }
  Value out = Value::mapa();
  out.map->set("texto", Value::texto(raw));
  out.map->set("modelo", Value::texto(cfg.model));
  return out;
}

rt::Value Interpreter::structured_from_tipo(const std::string& tipo_name, const std::string& raw,
                                           Span span) {
  auto it = entities_.find(tipo_name);
  if (it == entities_.end() || it->second->key != "tipo" || !it->second->block) {
    fail(span, "formato: '" + tipo_name + "' nao e um 'tipo' declarado");
  }
  Value parsed;
  bool have_parsed = false;
  if (!rt::llm_is_mock()) {
    try {
      parsed = rt::json_parse(raw);
      have_parsed = parsed.kind == ValueKind::Mapa;
    } catch (...) {
      fail(span, "a resposta do LLM nao e um JSON valido para o tipo '" + tipo_name + "'");
    }
  }

  Value out = Value::mapa();
  for (const auto& f : it->second->block->items) {
    if (!f || f->kind != ItemKind::Field) continue;
    Value v;
    if (have_parsed && parsed.map) {
      const Value* got = parsed.map->find(f->key);
      v = got ? *got : default_for_type(f->value.get());
    } else {
      v = default_for_type(f->value.get());
    }
    out.map->set(f->key, std::move(v));
  }
  return out;
}

rt::Value Interpreter::eval_indice_method(const std::string& indice_name, const std::string& method,
                                         const Expr& call, Env& env) {
  auto it = entities_.find(indice_name);
  const ast::Block& b = *it->second->block;
  const std::string armazenamento = field_str(b, "armazenamento");
  // Formatos: "qdrant://host:porta/colecao" (ver run-indice-armazenamento),
  // "pgvector://colecao" (Postgres + extensao pgvector; connection string no
  // campo "url", como em fonte postgres), "weaviate://host:porta/classe"
  // (REST; auth opcional via env WEAVIATE_API_KEY),
  // "pinecone://host-do-indice/namespace" (data plane hospedado, sempre
  // HTTPS; auth obrigatoria via env PINECONE_API_KEY) e
  // "chroma://host[:porta]/colecao" (HTTP; porta default 8000; sem auth).
  std::string qdrant_base, qdrant_col;
  std::string pgv_table, pgv_url;
  std::string weaviate_base, weaviate_classe;
  std::string pinecone_base, pinecone_ns;
  std::string chroma_base, chroma_col;
  const bool qdrant = armazenamento.rfind("qdrant://", 0) == 0;
  const bool pgvector = armazenamento.rfind("pgvector://", 0) == 0;
  const bool weaviate = armazenamento.rfind("weaviate://", 0) == 0;
  const bool pinecone = armazenamento.rfind("pinecone://", 0) == 0;
  const bool chroma = armazenamento.rfind("chroma://", 0) == 0;
  if (!armazenamento.empty() && armazenamento != "memoria" && !qdrant && !pgvector && !weaviate &&
      !pinecone && !chroma) {
    fail(call.span, "indice '" + indice_name + "': armazenamento '" + armazenamento +
                        "' nao implementado; use \"memoria\", \"qdrant://host:porta/colecao\", "
                        "\"pgvector://colecao\" (com campo \"url\"), "
                        "\"weaviate://host:porta/classe\", \"pinecone://host/namespace\" "
                        "ou \"chroma://host[:porta]/colecao\"",
         DiagCode::ConnectorNotImplemented);
  }
  if (qdrant) {
    const std::string rest = armazenamento.substr(9);  // depois de qdrant://
    const std::size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash == rest.size() - 1) {
      fail(call.span, "indice '" + indice_name +
                          "': armazenamento qdrant deve ser 'qdrant://host:porta/colecao'");
    }
    qdrant_base = "http://" + rest.substr(0, slash);
    qdrant_col = rest.substr(slash + 1);
  }
  if (weaviate) {
    const std::string rest = armazenamento.substr(11);  // depois de weaviate://
    const std::size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash == rest.size() - 1) {
      fail(call.span, "indice '" + indice_name +
                          "': armazenamento weaviate deve ser 'weaviate://host:porta/classe'");
    }
    weaviate_base = "http://" + rest.substr(0, slash);
    weaviate_classe = rest.substr(slash + 1);
  }
  if (pinecone) {
    const std::string rest = armazenamento.substr(11);  // depois de pinecone://
    const std::size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash == rest.size() - 1) {
      fail(call.span, "indice '" + indice_name +
                          "': armazenamento pinecone deve ser 'pinecone://host/namespace'");
    }
    pinecone_base = "https://" + rest.substr(0, slash);  // API hospedada: sempre HTTPS
    pinecone_ns = rest.substr(slash + 1);
  }
  if (chroma) {
    const std::string rest = armazenamento.substr(9);  // depois de chroma://
    const std::size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash == rest.size() - 1) {
      fail(call.span, "indice '" + indice_name +
                          "': armazenamento chroma deve ser 'chroma://host[:porta]/colecao'");
    }
    std::string hostport = rest.substr(0, slash);
    if (hostport.find(':') == std::string::npos) hostport += ":8000";  // porta padrao do Chroma
    chroma_base = "http://" + hostport;
    chroma_col = rest.substr(slash + 1);
  }
  if (pgvector) {
    pgv_table = armazenamento.substr(11);  // depois de pgvector://
    pgv_url = field_str(b, "url");
    if (pgv_url.empty()) {
      fail(call.span, "indice '" + indice_name +
                          "': armazenamento pgvector precisa do campo \"url\" "
                          "(connection string libpq, ex.: \"host=... port=... dbname=... user=...\")");
    }
  }
  const std::string emb_model = field_str(b, "embeddings");
  // Serializa o indice em memoria entre as rotas paralelas. Vive ate o fim
  // da funcao: cobre inserir/buscar e as chamadas de embedding no meio.
  std::unique_lock<std::mutex> index_lk(index_stores_mutex_);
  rt::MemoryIndex& store = index_stores_[indice_name];

  if (method == "inserir") {
    if (call.args.empty()) fail(call.span, "inserir espera uma lista ou tabela");
    Value v = eval(*call.args[0].value, env);
    int added = 0;
    auto add_one = [&](const Value& item) {
      const std::string text = row_text(item);
      std::string id = std::to_string(store.size() + 1);
      if (item.kind == ValueKind::Mapa && item.map) {
        if (const Value* i = item.map->find("id")) id = to_display(*i);
      }
      const std::vector<float> vec = rt::llm_embed(emb_model, text);
      if (qdrant) {
        try {
          rt::qdrant_upsert(qdrant_base, qdrant_col, id, text, vec);
        } catch (const std::exception& e) {
          fail(call.span, std::string(e.what()));
        }
      } else if (pgvector) {
        try {
          rt::pgvector_upsert(pgv_url, pgv_table, id, text, vec);
        } catch (const std::exception& e) {
          fail(call.span, std::string(e.what()));
        }
      } else if (weaviate) {
        try {
          rt::weaviate_upsert(weaviate_base, weaviate_classe, id, text, vec);
        } catch (const std::exception& e) {
          fail(call.span, std::string(e.what()));
        }
      } else if (pinecone) {
        try {
          rt::pinecone_upsert(pinecone_base, pinecone_ns, id, text, vec);
        } catch (const std::exception& e) {
          fail(call.span, std::string(e.what()));
        }
      } else if (chroma) {
        try {
          rt::chroma_upsert(chroma_base, chroma_col, id, text, vec);
        } catch (const std::exception& e) {
          fail(call.span, std::string(e.what()));
        }
      } else {
        store.insert(id, text, vec);
      }
      ++added;
    };
    if ((v.kind == ValueKind::Lista || v.kind == ValueKind::Tabela) && v.list) {
      for (const Value& e : *v.list) add_one(e);
    } else {
      add_one(v);
    }
    return Value::inteiro(added);
  }

  if (method == "buscar") {
    if (call.args.empty()) fail(call.span, "buscar espera um texto de consulta");
    Value q = eval(*call.args[0].value, env);
    rt::ValueMap kw;
    for (const auto& a : call.args) {
      if (!a.name.empty()) kw.set(a.name, eval(*a.value, env));
    }
    std::size_t k = 5;
    if (const Value* tk = kw.find("top_k")) k = static_cast<std::size_t>(tk->as_number());
    const std::string qt = q.kind == ValueKind::Texto ? q.s : to_display(q);
    Value out = Value::lista();
    if (qdrant || pgvector || weaviate || pinecone || chroma) {
      std::vector<std::pair<std::string, double>> hits;
      try {
        if (qdrant) {
          hits = rt::qdrant_search(qdrant_base, qdrant_col, rt::llm_embed(emb_model, qt), k);
        } else if (pgvector) {
          hits = rt::pgvector_search(pgv_url, pgv_table, rt::llm_embed(emb_model, qt), k);
        } else if (weaviate) {
          hits = rt::weaviate_search(weaviate_base, weaviate_classe,
                                     rt::llm_embed(emb_model, qt), k);
        } else if (pinecone) {
          hits = rt::pinecone_search(pinecone_base, pinecone_ns,
                                     rt::llm_embed(emb_model, qt), k);
        } else {
          hits = rt::chroma_search(chroma_base, chroma_col, rt::llm_embed(emb_model, qt), k);
        }
      } catch (const std::exception& e) {
        fail(call.span, std::string(e.what()));
      }
      for (const auto& h : hits) {
        Value row = Value::mapa();
        row.map->set("id", Value::texto(h.first));
        row.map->set("score", Value::decimal(h.second));
        out.list->push_back(std::move(row));
      }
      return out;
    }
    auto hits = store.search(rt::llm_embed(emb_model, qt), k);
    for (const auto& h : hits) {
      Value row = Value::mapa();
      row.map->set("id", Value::texto(h.id));
      row.map->set("texto", Value::texto(h.text));
      row.map->set("score", Value::decimal(h.score));
      out.list->push_back(std::move(row));
    }
    return out;
  }

  fail(call.span, "indice: metodo '" + method + "' desconhecido (use inserir / buscar)");
}

// ------------------------------------------------------------------ agents

rt::Value Interpreter::run_tool(const Item& tool_decl, const rt::ValueMap& args, Span span) {
  Env env;
  env.parent = &root_;
  if (tool_decl.block) {
    if (const Item* entrada = find_field(*tool_decl.block, "entrada"); entrada && entrada->block) {
      for (const auto& f : entrada->block->items) {
        if (f && f->kind == ItemKind::Field) {
          const Value* a = args.find(f->key);
          env.vars[f->key] = a ? *a : Value::nulo();
        }
      }
    }
    if (const Item* exec = find_field(*tool_decl.block, "executar"); exec && exec->block) {
      try {
        exec_block(*exec->block, env);
      } catch (const ReturnSignal& r) {
        return r.value;
      }
    }
  }
  (void)span;
  return Value::nulo();
}

namespace {

struct PlannerAction {
  enum Kind { Tool, Answer } kind = Answer;
  std::string tool;
  rt::ValueMap args;
  std::string answer;
};

std::string trim_copy(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// Protocolo do planner (uma linha por turno):
//   chamar <nome> {<json de argumentos>}  — executa uma ferramenta
//   responder: <resposta final>           — encerra o loop
// Qualquer outro texto vira resposta final (LLMs reais ignoram protocolo com
// frequencia; e' o fallback tolerante).
PlannerAction parse_planner_action(const std::string& raw) {
  PlannerAction a;
  const std::string s = trim_copy(raw);
  if (s.rfind("chamar ", 0) == 0) {
    const std::string rest = trim_copy(s.substr(7));
    const std::size_t brace = rest.find('{');
    PlannerAction tool;
    tool.kind = PlannerAction::Tool;
    tool.tool = trim_copy(rest.substr(0, brace));
    if (brace != std::string::npos) {
      try {
        const Value v = rt::json_parse(rest.substr(brace));
        if (v.kind == ValueKind::Mapa && v.map) {
          for (const auto& [k, val] : v.map->items) tool.args.set(k, val);
        }
      } catch (...) {
        // argumentos malformados: o interpretador completa com best-effort
      }
    }
    if (!tool.tool.empty()) return tool;
  }
  if (s.rfind("responder:", 0) == 0) {
    a.answer = trim_copy(s.substr(10));
  } else {
    a.answer = s;
  }
  return a;
}

Value args_to_value(const rt::ValueMap& args) {
  Value out = Value::mapa();
  for (const auto& [k, v] : args.items) out.map->set(k, v);
  return out;
}

// Entradas best-effort: campos 'texto' recebem a mensagem; os demais, o
// padrao do tipo declarado.
rt::ValueMap best_effort_args(const Item& tool_decl, const std::string& message) {
  rt::ValueMap targs;
  if (tool_decl.block) {
    if (const Item* entrada = find_field(*tool_decl.block, "entrada"); entrada && entrada->block) {
      for (const auto& f : entrada->block->items) {
        if (!f || f->kind != ItemKind::Field) continue;
        if (f->value && f->value->kind == ExprKind::Name && f->value->text == "texto") {
          targs.set(f->key, Value::texto(message));
        } else {
          targs.set(f->key, default_for_type(f->value.get()));
        }
      }
    }
  }
  return targs;
}

std::string tool_params_desc(const Item& tool_decl) {
  std::string out;
  if (tool_decl.block) {
    if (const Item* entrada = find_field(*tool_decl.block, "entrada"); entrada && entrada->block) {
      bool first = true;
      for (const auto& f : entrada->block->items) {
        if (!f || f->kind != ItemKind::Field) continue;
        if (!first) out += ", ";
        out += f->key;
        if (f->value && f->value->kind == ExprKind::Name) out += ": " + f->value->text;
        first = false;
      }
    }
  }
  return out;
}

}  // namespace

rt::Value Interpreter::eval_agente_responder(const std::string& agent_name, const Expr& call,
                                             Env& env) {
  auto it = entities_.find(agent_name);
  const ast::Block& cfg = *it->second->block;

  std::string message;
  if (!call.args.empty()) {
    Value m = eval(*call.args[0].value, env);
    message = m.kind == ValueKind::Texto ? m.s : to_display(m);
  }

  const std::string papel = field_str(cfg, "papel");
  const std::string llm_name = field_word(cfg, "llm", "");
  const std::string memoria = field_word(cfg, "memoria", "nenhuma");
  const int max_passos = field_int(cfg, "max_passos", 6);

  // Ferramentas declaradas, ja resolvidas e validadas.
  std::vector<std::pair<std::string, const Item*>> tools;
  if (const Item* tf = find_field(cfg, "ferramentas")) {
    std::vector<std::string> names;
    if (tf->value && tf->value->kind == ExprKind::ListLit) {
      for (const auto& e : tf->value->elems) {
        if (e && e->kind == ExprKind::Name) names.push_back(e->text);
      }
    }
    if (tf->block) {
      for (const auto& raw : tf->block->items) {
        const Item* c = (raw && raw->kind == ItemKind::ListEntry && raw->child) ? raw->child.get()
                                                                                : raw.get();
        if (!c) continue;
        if (c->kind == ItemKind::Stmt && c->stmt && c->stmt->a &&
            c->stmt->a->kind == ExprKind::Name) {
          names.push_back(c->stmt->a->text);
        } else if (c->kind == ItemKind::Field) {
          names.push_back(c->key);
        }
      }
    }
    for (const std::string& name : names) {
      auto tit = entities_.find(name);
      if (tit == entities_.end() || tit->second->key != "ferramenta") {
        fail(call.span, "agente '" + agent_name + "': ferramenta '" + name + "' nao declarada");
      }
      tools.emplace_back(name, tit->second);
    }
  }

  std::string prompt = message;
  if (memoria == "conversa") {
    std::string mem;
    {
      std::lock_guard<std::mutex> lk(agent_memory_mutex_);
      mem = agent_memory_[agent_name];
    }
    if (!mem.empty()) prompt = mem + "\n" + message;
  }

  Value rastro = Value::lista();
  std::string answer;

  if (llm_name.empty()) {
    // Sem LLM: cada ferramenta roda uma vez com entradas best-effort e a
    // resposta e' local.
    int step = 0;
    for (const auto& [tname, tdecl] : tools) {
      if (step >= max_passos) break;
      rt::ValueMap targs = best_effort_args(*tdecl, prompt);
      Value obs = run_tool(*tdecl, targs, call.span);
      ++step;
      Value entry = Value::mapa();
      entry.map->set("passo", Value::inteiro(step));
      entry.map->set("ferramenta", Value::texto(tname));
      entry.map->set("argumentos", args_to_value(targs));
      entry.map->set("observacao", Value::texto(to_display(obs)));
      rastro.list->push_back(std::move(entry));
    }
    answer = "[sem llm] " + message;
  } else {
    rt::LlmConfig lc = llm_config(llm_name, call.span);
    std::string system = papel;

    if (tools.empty()) {
      try {
        answer = rt::llm_chat(lc, system, prompt);
      } catch (const std::exception& e) {
        fail(call.span, std::string("agente '") + agent_name + "': LLM: " + e.what());
      }
    } else {
      // Planner iterativo (M9.2): a cada passo o LLM escolhe a proxima acao
      // — chamar uma ferramenta (com argumentos em JSON) ou responder.
      // O bloco "Ferramentas disponiveis:" seguido de linhas "- <nome>:"
      // tambem e' o que o modo mock usa para simular o planner.
      system += "\n\nFerramentas disponiveis:\n";
      for (const auto& [tname, tdecl] : tools) {
        system += "- " + tname + ": " + field_str(*tdecl->block, "descricao");
        const std::string params = tool_params_desc(*tdecl);
        if (!params.empty()) system += " (parametros: " + params + ")";
        system += "\n";
      }
      system += "\nResponda EXATAMENTE uma linha por turno:\n";
      system += "chamar <nome> {<json de argumentos>}  — usa uma ferramenta\n";
      system += "responder: <resposta final>           — quando nao precisar mais de ferramentas\n";

      std::string observations;
      for (int step = 1; step <= max_passos && answer.empty(); ++step) {
        std::string user = "Pedido do usuario: " + prompt + "\n";
        if (!observations.empty()) user += "\nObservacoes ate agora:\n" + observations;
        std::string raw;
        try {
          raw = rt::llm_chat(lc, system, user);
        } catch (const std::exception& e) {
          fail(call.span, std::string("agente '") + agent_name + "': LLM: " + e.what());
        }
        const PlannerAction action = parse_planner_action(raw);
        if (action.kind == PlannerAction::Answer) {
          answer = action.answer;
          break;
        }
        auto tit = entities_.find(action.tool);
        if (tit == entities_.end() || tit->second->key != "ferramenta") {
          fail(call.span, "agente '" + agent_name + "': o LLM pediu a ferramenta '" +
                              action.tool + "', que nao esta declarada");
        }
        rt::ValueMap targs = best_effort_args(*tit->second, prompt);
        for (const auto& [k, v] : action.args.items) targs.set(k, v);  // JSON sobrescreve
        Value obs = run_tool(*tit->second, targs, call.span);

        Value entry = Value::mapa();
        entry.map->set("passo", Value::inteiro(step));
        entry.map->set("ferramenta", Value::texto(action.tool));
        entry.map->set("argumentos", args_to_value(targs));
        entry.map->set("observacao", Value::texto(to_display(obs)));
        rastro.list->push_back(std::move(entry));
        observations += "- " + action.tool + ": " + to_display(obs) + "\n";
      }

      if (answer.empty()) {
        // Estourou max_passos sem resposta final: uma ultima chamada pede a
        // sintese com o que foi observado.
        try {
          const std::string user = "Pedido do usuario: " + prompt + "\n\nObservacoes ate agora:\n" +
                                   observations;
          const std::string raw = rt::llm_chat(
              lc, system + "\n\nLimite de passos atingido. Responda agora no formato responder: <sintese>.",
              user);
          const PlannerAction action = parse_planner_action(raw);
          answer = action.kind == PlannerAction::Answer
                       ? action.answer
                       : "[agente] limite de passos atingido sem resposta final";
        } catch (...) {
          answer = "[agente] limite de passos atingido sem resposta final";
        }
      }
    }
  }

  if (memoria == "conversa") {
    std::lock_guard<std::mutex> lk(agent_memory_mutex_);
    std::string& mem = agent_memory_[agent_name];
    mem += (mem.empty() ? "" : "\n") + ("usuario: " + message) + "\nagente: " + answer;
  }

  Value out = Value::mapa();
  out.map->set("texto", Value::texto(answer));
  out.map->set("rastro", std::move(rastro));
  return out;
}

rt::Value Interpreter::eval_equipe_call(const std::string& team_name, const Expr& call, Env& env) {
  auto it = entities_.find(team_name);
  const ast::Block& cfg = *it->second->block;
  const std::string estrategia = field_word(cfg, "estrategia", "sequencial");

  std::vector<std::pair<std::string, std::string>> members;  // (rotulo, agente)
  if (const Item* af = find_field(cfg, "agentes"); af && af->block) {
    for (const auto& raw : af->block->items) {
      const Item* c = (raw && raw->kind == ItemKind::ListEntry && raw->child) ? raw->child.get()
                                                                              : raw.get();
      if (c && c->kind == ItemKind::Field && c->value && c->value->kind == ExprKind::Name) {
        members.emplace_back(c->key, c->value->text);
      } else if (c && c->kind == ItemKind::Stmt && c->stmt && c->stmt->a &&
                 c->stmt->a->kind == ExprKind::Name) {
        members.emplace_back(c->stmt->a->text, c->stmt->a->text);
      }
    }
  }

  std::string message;
  if (!call.args.empty()) {
    Value m = eval(*call.args[0].value, env);
    message = m.kind == ValueKind::Texto ? m.s : to_display(m);
  }

  if (estrategia == "supervisor") {
    // Supervisor (M9.2): um LLM orquestra, delegando tarefas aos agentes por
    // rotulo ate decidir responder.
    const std::string sup_name = field_word(cfg, "supervisor", "");
    if (sup_name.empty()) {
      fail(call.span, "equipe '" + team_name + "': estrategia 'supervisor' exige 'supervisor: <llm>'");
    }
    const std::string objetivo = field_str(cfg, "objetivo");
    const int max_passos = field_int(cfg, "max_passos", 6);
    rt::LlmConfig lc = llm_config(sup_name, call.span);

    std::string system = objetivo;
    system += "\n\nAgentes disponiveis:\n";
    for (const auto& [rotulo, agente] : members) {
      std::string papel_membro;
      if (auto ait = entities_.find(agente); ait != entities_.end() && ait->second->block) {
        papel_membro = field_str(*ait->second->block, "papel");
      }
      system += "- " + rotulo + ": " + agente;
      if (!papel_membro.empty()) system += " — " + papel_membro;
      system += "\n";
    }
    system += "\nResponda EXATAMENTE uma linha por turno:\n";
    system += "delegar <rotulo> <tarefa>  — delega a tarefa ao agente\n";
    system += "responder: <resposta final>\n";

    Value sup_rastro = Value::lista();
    std::string history;
    std::string final_text;
    std::string last_raw;
    for (int step = 1; step <= max_passos; ++step) {
      std::string user = "Pedido: " + message + "\n";
      if (!history.empty()) user += "\nResultados ate agora:\n" + history;
      std::string raw;
      try {
        raw = rt::llm_chat(lc, system, user);
      } catch (const std::exception& e) {
        fail(call.span, std::string("equipe '") + team_name + "': supervisor: " + e.what());
      }
      last_raw = raw;
      const std::string line = trim_copy(raw);
      if (line.rfind("delegar ", 0) == 0) {
        const std::string rest = trim_copy(line.substr(8));
        const std::size_t sp = rest.find(' ');
        const std::string rotulo = rest.substr(0, sp);
        const std::string tarefa = sp == std::string::npos ? message : trim_copy(rest.substr(sp + 1));
        auto mit = std::find_if(members.begin(), members.end(),
                                [&](const auto& m) { return m.first == rotulo; });
        if (mit == members.end()) {
          fail(call.span, "equipe '" + team_name + "': o supervisor delegou para o rotulo '" +
                              rotulo + "', que nao esta em 'agentes:'");
        }
        Expr fake;  // synthesize a `<agente>.responder <tarefa>` call
        fake.kind = ExprKind::Call;
        fake.span = call.span;
        ast::Arg arg;
        arg.value = std::make_unique<Expr>();
        arg.value->kind = ExprKind::TextLit;
        arg.value->text = tarefa;
        fake.args.push_back(std::move(arg));

        Value r = eval_agente_responder(mit->second, fake, env);
        std::string texto = (r.kind == ValueKind::Mapa && r.map && r.map->find("texto"))
                                ? r.map->find("texto")->s
                                : "";
        Value entry = Value::mapa();
        entry.map->set("agente", Value::texto(rotulo));
        entry.map->set("tarefa", Value::texto(tarefa));
        entry.map->set("texto", Value::texto(texto));
        sup_rastro.list->push_back(std::move(entry));
        history += "- " + rotulo + ": " + texto + "\n";
        continue;
      }
      if (line.rfind("responder:", 0) == 0) {
        final_text = trim_copy(line.substr(10));
      } else {
        final_text = line;  // resposta nao estruturada: usa o texto cru
      }
      break;
    }
    if (final_text.empty()) {
      final_text = last_raw.empty() ? "[supervisor] nenhuma resposta" : last_raw;
    }

    Value out = Value::mapa();
    out.map->set("texto", Value::texto(final_text));
    out.map->set("rastro", std::move(sup_rastro));
    return out;
  }

  Value rastro = Value::lista();
  std::string current = message;
  std::string combined;

  for (const auto& [rotulo, agente] : members) {
    Expr fake;  // synthesize a `<agente>.responder <texto>` call
    fake.kind = ExprKind::Call;
    fake.span = call.span;
    ast::Arg arg;
    arg.value = std::make_unique<Expr>();
    arg.value->kind = ExprKind::TextLit;
    arg.value->text = (estrategia == "paralelo") ? message : current;
    fake.args.push_back(std::move(arg));

    Value r = eval_agente_responder(agente, fake, env);
    std::string texto = (r.kind == ValueKind::Mapa && r.map && r.map->find("texto"))
                            ? r.map->find("texto")->s
                            : "";
    Value entry = Value::mapa();
    entry.map->set("agente", Value::texto(rotulo));
    entry.map->set("texto", Value::texto(texto));
    rastro.list->push_back(std::move(entry));

    current = texto;
    combined += rotulo + ": " + texto + "\n";
  }

  Value out = Value::mapa();
  out.map->set("texto", Value::texto(estrategia == "paralelo" ? combined : current));
  out.map->set("rastro", std::move(rastro));
  return out;
}

// ------------------------------------------------------------------ HTTP service

namespace {

struct Route {
  std::string method;
  std::string path;
  const Item* field = nullptr;  // the `rota` Field (has `entrada:` / `passos:`)
};

std::vector<Route> collect_routes(const ast::Block& block) {
  std::vector<Route> routes;
  for (const auto& it : block.items) {
    if (!it || it->kind != ItemKind::Field || it->key != "rota") continue;
    Route r;
    r.field = it.get();
    if (it->header.size() >= 1 && it->header[0] && it->header[0]->kind == ExprKind::Name) {
      r.method = it->header[0]->text;
      for (char& c : r.method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (it->header.size() >= 2 && it->header[1] && it->header[1]->kind == ExprKind::TextLit) {
      r.path = it->header[1]->text;
    }
    routes.push_back(std::move(r));
  }
  return routes;
}

}  // namespace

int Interpreter::serve(int port_override, int max_requests, int threads) {
  register_decls();

  // Experimentos ajustam uma vez na subida (rotas com prever leem o cache).
  for (const auto& item : program_.items) {
    if (item && item->kind == ItemKind::Decl && item->key == "experimento") run_experimento(*item);
  }

  const Item* svc = nullptr;
  for (const auto& item : program_.items) {
    if (item && item->kind == ItemKind::Decl && item->key == "servico") {
      svc = item.get();
      break;
    }
  }
  if (!svc || !svc->block) {
    out_ << "nenhum 'servico' declarado\n";
    return 1;
  }

  int port = port_override > 0 ? port_override : field_int(*svc->block, "porta", 8080);
  const std::vector<Route> routes = collect_routes(*svc->block);
  // Middleware: todos os blocos `meio:` do servico, em ordem de declaracao.
  // Cada um roda (no Env da rota casada) antes dos `passos:`; se algum
  // executar `responder:`, a resposta dele vale e a rota nao executa.
  std::vector<const Item*> meios;
  for (const auto& it : svc->block->items) {
    if (it && it->kind == ItemKind::Field && it->key == "meio") meios.push_back(it.get());
  }

  rt::HttpServer server;
  const std::string err = server.listen_on("127.0.0.1", port);
  if (!err.empty()) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = DiagCode::RuntimeError;
    d.span = svc->span;
    d.message = "servico " + decl_name(*svc) + ": " + err;
    diag_.report(std::move(d));
    return 1;
  }
  out_ << "servico " << decl_name(*svc) << ": escutando 127.0.0.1:" << port << "\n" << std::flush;

  // O tratamento das rotas roda num pool de workers (padrao: ate 4); os
  // estados mutaveis compartilhados do interpretador foram tornados
  // reentrantes (thread_locals + mutexes nos caches).
  if (threads <= 0) {
    const unsigned hc = std::thread::hardware_concurrency();
    threads = hc > 0 ? static_cast<int>(std::min(4u, hc)) : 1;
  }
  const int served = server.run(
      [&](const rt::HttpRequest& req) -> rt::HttpResponse {
        rt::HttpResponse resp;
        const Route* match = nullptr;
        for (const Route& r : routes) {
          if (r.method == req.method && r.path == req.path) {
            match = &r;
            break;
          }
        }

        if (!match) {
          resp.status = 404;
          resp.body = R"({"erro":"rota nao encontrada"})";
        } else {
          Value parsed = Value::mapa();
          bool bad = false;
          if (!req.body.empty()) {
            try {
              parsed = rt::json_parse(req.body);
            } catch (...) {
              bad = true;
            }
          }
          const Item* entrada = match->field->block ? find_field(*match->field->block, "entrada") : nullptr;
          if (!bad && entrada && entrada->value && entrada->value->kind == ExprKind::Name) {
            if (auto t = entities_.find(entrada->value->text);
                t != entities_.end() && t->second->key == "tipo" && t->second->block &&
                parsed.kind == ValueKind::Mapa) {
              for (const auto& f : t->second->block->items) {
                if (f && f->kind == ItemKind::Field && parsed.map && !parsed.map->find(f->key)) {
                  resp.status = 400;
                  resp.body = R"({"erro":"campo ')" + f->key + R"(' ausente"})";
                  bad = true;
                  break;
                }
              }
            }
          }
          if (bad && resp.status != 400) {
            resp.status = 400;
            resp.body = R"({"erro":"corpo JSON invalido"})";
          }

          if (!bad) {
            RouteResponse rr;
            route_resp_ = &rr;
            Env env;
            env.parent = &root_;
            env.vars["entrada"] = parsed;
            const Item* passos = match->field->block ? find_field(*match->field->block, "passos") : nullptr;
            try {
              bool abortado = false;
              for (const Item* meio : meios) {
                if (!meio->block) continue;
                exec_block(*meio->block, env);
                if (rr.set) {  // meio respondeu (ex.: recusa de autenticacao)
                  abortado = true;
                  break;
                }
              }
              if (!abortado && passos && passos->block) exec_block(*passos->block, env);
              resp.status = rr.set ? rr.status : 200;
              resp.body = json_dump(rr.dados.kind == ValueKind::Nulo ? Value::mapa() : rr.dados);
            } catch (const RuntimeAbort& a) {
              resp.status = 500;
              resp.body = R"({"erro":)" + std::string("\"") + a.message + "\"}";
            }
            route_resp_ = nullptr;
          }
        }

        {
          std::lock_guard<std::mutex> lk(log_mutex_);
          out_ << req.method << " " << req.path << " -> " << resp.status << "\n" << std::flush;
        }
        return resp;
      },
      max_requests, threads);
  return served < 0 ? 1 : 0;
}

// ------------------------------------------------------------------ statements

void Interpreter::exec_block(const ast::Block& block, Env& env) {
  // Desembrulha item de lista (`- x`) ate o conteudo.
  auto unwrap = [](const Item* it) -> const Item* {
    while (it && it->kind == ItemKind::ListEntry) {
      it = it->child ? it->child.get()
                     : (it->block && !it->block->items.empty() ? it->block->items[0].get()
                                                               : nullptr);
    }
    return it;
  };
  const auto& items = block.items;
  for (std::size_t k = 0; k < items.size(); ++k) {
    const Item* it = items[k].get();
    if (!it) continue;
    // `- senao:` solto em 'passos:' (o parser nao o anexa ao 'se' quando ele
    // vem como item de lista separado): trata como o 'senao' do 'se' imediato
    // anterior — mas so se esse 'se' nao pegou nenhum ramo. Fora desse par,
    // mantem o legado: executa o bloco incondicionalmente.
    const Item* bare = unwrap(it);
    const Item* prev = k > 0 ? unwrap(items[k - 1].get()) : nullptr;
    const bool prev_if_bare =
        prev && prev->kind == ItemKind::Stmt && prev->stmt &&
        prev->stmt->kind == StmtKind::If && !prev->stmt->else_body;
    if (bare && bare->kind == ItemKind::Field && bare->key == "senao" &&
        bare->header.empty() && bare->block) {
      if (prev_if_bare) {
        if (!last_if_taken_) {
          Env inner;
          inner.parent = &env;
          exec_block(*bare->block, inner);
        }
      } else {
        Env inner;
        inner.parent = &env;
        exec_block(*bare->block, inner);
      }
      continue;
    }
    exec_item(*it, env);
  }
}

void Interpreter::exec_item(const Item& item, Env& env) {
  switch (item.kind) {
    case ItemKind::Stmt:
      if (item.stmt) exec_stmt(*item.stmt, env);
      return;
    case ItemKind::ListEntry:
      if (item.block) exec_block(*item.block, env);
      if (item.child) exec_item(*item.child, env);
      return;
    case ItemKind::Field:
      if (item.key == "verificar") {
        run_verificar(item, env);
        return;
      }
      if ((item.key == "responder" || item.key == "responder_em_fluxo") && route_resp_ &&
          item.block) {
        for (const auto& sub : item.block->items) {
          if (!sub || sub->kind != ItemKind::Field) continue;
          if (sub->key == "status" && sub->value) {
            route_resp_->status = static_cast<int>(eval(*sub->value, env).as_number());
          } else if (sub->key == "dados") {
            if (sub->block) {
              Value m = Value::mapa();
              for (const auto& d : sub->block->items) {
                if (d && d->kind == ItemKind::Field && d->value) {
                  m.map->set(d->key, eval(*d->value, env));
                }
              }
              route_resp_->dados = std::move(m);
            } else if (sub->value) {
              route_resp_->dados = eval(*sub->value, env);
            }
          }
        }
        route_resp_->set = true;
        return;
      }
      if (item.value) eval(*item.value, env);
      if (item.block) {
        // e.g. `- responder:` inside `passos:` — evaluate nested field values
        Env inner;
        inner.parent = &env;
        exec_block(*item.block, inner);
      }
      return;
    case ItemKind::Decl:
      return;
  }
}

void Interpreter::exec_stmt(const Stmt& stmt, Env& env) {
  switch (stmt.kind) {
    case StmtKind::Expr:
      if (stmt.a) eval(*stmt.a, env);
      return;
    case StmtKind::Assign: {
      Value v = stmt.b ? eval(*stmt.b, env) : Value::nulo();
      if (stmt.a && stmt.a->kind == ExprKind::Name) {
        env.set(stmt.a->text, std::move(v));
      } else if (stmt.a) {
        eval(*stmt.a, env);  // evaluate target for side effects; member assign unsupported
      }
      return;
    }
    case StmtKind::Return:
      throw ReturnSignal{stmt.a ? eval(*stmt.a, env) : Value::nulo()};
    case StmtKind::If: {
      if (stmt.a && eval(*stmt.a, env).truthy()) {
        Env inner;
        inner.parent = &env;
        exec_block(stmt.body, inner);
        last_if_taken_ = true;
        return;
      }
      for (const auto& ei : stmt.elifs) {
        if (ei.cond && eval(*ei.cond, env).truthy()) {
          Env inner;
          inner.parent = &env;
          exec_block(ei.body, inner);
          last_if_taken_ = true;
          return;
        }
      }
      last_if_taken_ = false;
      if (stmt.else_body) {
        Env inner;
        inner.parent = &env;
        exec_block(*stmt.else_body, inner);
      }
      return;
    }
    case StmtKind::ForEach: {
      Value seq = stmt.a ? eval(*stmt.a, env) : Value::nulo();
      if (seq.kind != ValueKind::Lista && seq.kind != ValueKind::Tabela) {
        fail(stmt.span, std::string("'para cada' espera uma lista, recebeu ") + seq.type_name());
      }
      if (seq.list) {
        for (const Value& element : *seq.list) {
          Env inner;
          inner.parent = &env;
          inner.vars[stmt.name] = element;
          exec_block(stmt.body, inner);
        }
      }
      return;
    }
    case StmtKind::While: {
      std::int64_t guard = 0;
      while (stmt.a && eval(*stmt.a, env).truthy()) {
        if (++guard > kLoopGuard) fail(stmt.span, "laco 'enquanto' excedeu o limite de iteracoes");
        Env inner;
        inner.parent = &env;
        exec_block(stmt.body, inner);
      }
      return;
    }
    case StmtKind::Try: {
      try {
        Env inner;
        inner.parent = &env;
        exec_block(stmt.body, inner);
      } catch (const RuntimeAbort& a) {
        if (!stmt.catch_body) return;
        Env inner;
        inner.parent = &env;
        if (!stmt.name.empty()) inner.vars[stmt.name] = Value::texto(a.message);
        exec_block(*stmt.catch_body, inner);
      }
      return;
    }
  }
}

// ------------------------------------------------------------------ expressions

std::string Interpreter::interpolate(const std::string& text, Env& env) {
  std::string out;
  for (std::size_t k = 0; k < text.size();) {
    if (k + 1 < text.size() && text[k] == '{' && text[k + 1] == '{') {
      std::size_t end = text.find("}}", k + 2);
      if (end != std::string::npos) {
        std::string name = text.substr(k + 2, end - (k + 2));
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (Value* v = env.lookup(name)) {
          out += to_display(*v);
        } else {
          out += "{{" + name + "}}";
        }
        k = end + 2;
        continue;
      }
    }
    out += text[k++];
  }
  return out;
}

Value Interpreter::eval(const Expr& expr, Env& env) {
  switch (expr.kind) {
    case ExprKind::IntLit:
      return Value::inteiro(std::strtoll(expr.text.c_str(), nullptr, 10));
    case ExprKind::DecimalLit:
      return Value::decimal(std::strtod(expr.text.c_str(), nullptr));
    case ExprKind::TextLit:
      return Value::texto(interpolate(expr.text, env));
    case ExprKind::BoolLit:
      return Value::logico(expr.boolean);
    case ExprKind::NullLit:
      return Value::nulo();
    case ExprKind::Name: {
      if (expr.text == "_") {
        // C2: curinga de dimensao simbolica — so vale em anotacao/checagem
        // (`tensor[f32, _, N]`, `reformar [_, N]`); aqui virou valor.
        fail(expr.span,
             "'_' e dimensao simbolica (vale em anotacao e no `tilt checar`); "
             "informe o tamanho aqui ou use `reformar` com um '_' inferido pela contagem");
      }
      if (Value* v = env.lookup(expr.text)) return *v;
      if (auto mit = modules_.find(expr.text); mit != modules_.end()) {
        fail(expr.span, "'" + expr.text + "' e um modulo; chame " + expr.text + ".<funcao>(...)");
      }
      if (auto it = entities_.find(expr.text); it != entities_.end()) return Value::texto(expr.text);
      fail(expr.span, "nome '" + expr.text + "' nao definido");
    }
    case ExprKind::Member: {
      Value base = eval(*expr.lhs, env);
      if (base.kind == ValueKind::Tensor && base.tensor) {
        const rt::Tensor& t = *base.tensor;
        const std::string& m = expr.text;
        try {
          if (m == "forma") {
            rt::ValueList dims;
            for (std::int64_t d : t.shape) dims.push_back(Value::inteiro(d));
            return Value::lista(std::move(dims));
          }
          if (m == "dados") {
            rt::ValueList vals;
            for (float fv : t.data) vals.push_back(Value::decimal(fv));
            return Value::lista(std::move(vals));
          }
          if (m == "soma") return Value::decimal(rt::sum_all(t));
          if (m == "media") return Value::decimal(rt::mean_all(t));
          if (m == "argmax") return Value::inteiro(rt::argmax_last(t));
          if (m == "transposta") return Value::tensor_de(rt::transpose2d(t));
          if (m == "softmax") return Value::tensor_de(rt::softmax_last(t));
          if (word_in(m, {"relu", "gelu", "silu", "sigmoide", "tanh"})) {
            return Value::tensor_de(rt::apply_unary(t, m));
          }
          if (m == "item") {
            if (t.size() != 1) fail(expr.span, "item espera um tensor de 1 elemento");
            return Value::decimal(t.data[0]);
          }
          if (m == "tamanho") return Value::inteiro(t.size());
        } catch (const std::exception& e) {
          fail(expr.span, std::string(e.what()));
        }
      }
      if ((base.kind == ValueKind::Mapa || base.kind == ValueKind::Tabela) && base.map) {
        if (Value* f = base.map->find(expr.text)) return *f;
      }
      if (base.kind == ValueKind::Mapa && base.map) {
        if (Value* f = base.map->find(expr.text)) return *f;
      }
      if (expr.text == "tamanho") {
        if (base.kind == ValueKind::Lista || base.kind == ValueKind::Tabela) {
          return Value::inteiro(base.list ? static_cast<std::int64_t>(base.list->size()) : 0);
        }
        if (base.kind == ValueKind::Texto) {
          return Value::inteiro(static_cast<std::int64_t>(base.s.size()));
        }
      }
      if (expr.optional) return Value::nulo();
      fail(expr.span, std::string("'") + base.type_name() + "' nao tem o campo '" + expr.text + "'");
    }
    case ExprKind::Index: {
      // `tensor [ ... ]` / `zeros [ ... ]` etc. — constructor sugar, unambiguous
      // because these names are never bound as variables.
      if (expr.lhs->kind == ExprKind::Name &&
          (expr.lhs->text == "tensor" || expr.lhs->text == "zeros" || expr.lhs->text == "uns" ||
           expr.lhs->text == "aleatorio")) {
        Value elems = Value::lista();
        for (const auto& el : expr.elems) elems.list->push_back(eval(*el, env));
        const std::string& n = expr.lhs->text;
        if (n == "tensor") return Value::tensor_de(value_to_tensor(elems, expr.span));
        std::vector<std::int64_t> shape;
        for (const Value& v : *elems.list) shape.push_back(static_cast<std::int64_t>(v.as_number()));
        if (shape.empty()) fail(expr.span, n + " precisa de uma forma");
        if (n == "zeros") return Value::tensor_de(rt::Tensor::zeros(shape));
        if (n == "uns") return Value::tensor_de(rt::Tensor::ones(shape));
        return Value::tensor_de(rt::Tensor::xavier(shape, shape.front(), shape.back(), 42));
      }
      Value base = eval(*expr.lhs, env);
      if (expr.elems.empty()) fail(expr.span, "indice vazio");
      Value idx = eval(*expr.elems[0], env);
      if (base.kind == ValueKind::Lista || base.kind == ValueKind::Tabela) {
        if (!base.list) return Value::nulo();
        std::int64_t n = static_cast<std::int64_t>(base.list->size());
        std::int64_t i = idx.is_number() ? static_cast<std::int64_t>(idx.as_number()) : 0;
        if (i < 0) i += n;
        if (i < 0 || i >= n) fail(expr.span, "indice fora dos limites");
        return (*base.list)[static_cast<std::size_t>(i)];
      }
      if (base.kind == ValueKind::Mapa && base.map) {
        Value* f = base.map->find(idx.kind == ValueKind::Texto ? idx.s : to_display(idx));
        return f ? *f : Value::nulo();
      }
      fail(expr.span, std::string("nao e possivel indexar '") + base.type_name() + "'");
    }
    case ExprKind::Slice: {
      Value base = eval(*expr.lhs, env);
      if (base.kind != ValueKind::Lista || !base.list) {
        fail(expr.span, "fatia so funciona em listas");
      }
      std::int64_t n = static_cast<std::int64_t>(base.list->size());
      std::int64_t lo = expr.rhs ? static_cast<std::int64_t>(eval(*expr.rhs, env).as_number()) : 0;
      std::int64_t hi = expr.extra ? static_cast<std::int64_t>(eval(*expr.extra, env).as_number()) : n;
      lo = std::clamp<std::int64_t>(lo, 0, n);
      hi = std::clamp<std::int64_t>(hi, lo, n);
      rt::ValueList out(base.list->begin() + lo, base.list->begin() + hi);
      return Value::lista(std::move(out));
    }
    case ExprKind::Unary: {
      Value v = eval(*expr.rhs, env);
      if (expr.text == "nao") return Value::logico(!v.truthy());
      if (expr.text == "-") {
        if (v.kind == ValueKind::Inteiro) return Value::inteiro(-v.i);
        return Value::decimal(-v.as_number());
      }
      return v;
    }
    case ExprKind::Binary:
      return eval_binary(expr, env);
    case ExprKind::ListLit: {
      rt::ValueList out;
      out.reserve(expr.elems.size());
      for (const auto& el : expr.elems) out.push_back(eval(*el, env));
      return Value::lista(std::move(out));
    }
    case ExprKind::MapLit: {
      Value m = Value::mapa();
      for (const auto& en : expr.entries) m.map->set(en.key, eval(*en.value, env));
      return m;
    }
    case ExprKind::Assign: {
      Value v = eval(*expr.rhs, env);
      if (expr.lhs && expr.lhs->kind == ExprKind::Name) env.set(expr.lhs->text, v);
      return v;
    }
    case ExprKind::Device:
      return eval(*expr.lhs, env);
    case ExprKind::Call:
      return eval_call(expr, env);
  }
  return Value::nulo();
}

Value Interpreter::eval_binary(const Expr& expr, Env& env) {
  const std::string& op = expr.text;

  if (op == "e") return Value::logico(eval(*expr.lhs, env).truthy() && eval(*expr.rhs, env).truthy());
  if (op == "ou") return Value::logico(eval(*expr.lhs, env).truthy() || eval(*expr.rhs, env).truthy());

  Value a = eval(*expr.lhs, env);
  Value b = eval(*expr.rhs, env);

  if ((a.kind == ValueKind::Tensor || b.kind == ValueKind::Tensor) &&
      (op == "+" || op == "-" || op == "*" || op == "/")) {
    try {
      if (a.kind == ValueKind::Tensor && b.kind == ValueKind::Tensor) {
        const rt::Tensor& x = *a.tensor;
        const rt::Tensor& y = *b.tensor;
        if (op == "+") return Value::tensor_de(rt::add(x, y));
        if (op == "-") return Value::tensor_de(rt::sub(x, y));
        if (op == "*") return Value::tensor_de(rt::mul(x, y));
        return Value::tensor_de(rt::div(x, y));
      }
      const bool tensor_left = a.kind == ValueKind::Tensor;
      const rt::Tensor& t = tensor_left ? *a.tensor : *b.tensor;
      const float s = tensor_left ? static_cast<float>(b.as_number())
                                  : static_cast<float>(a.as_number());
      if (!tensor_left && (op == "-" || op == "/")) {
        // scalar (op) tensor
        rt::Tensor lhs = rt::Tensor::filled(t.shape, s);
        return Value::tensor_de(op == "-" ? rt::sub(lhs, t) : rt::div(lhs, t));
      }
      return Value::tensor_de(rt::scalar_op(t, s, op[0]));
    } catch (const std::exception& e) {
      fail(expr.span, std::string(e.what()));
    }
  }

  if (op == "|") return Value::texto(to_display(a) + " | " + to_display(b));

  bool ok = false;
  Value r = rt::apply_binop(op, a, b, &ok);
  if (!ok) fail(expr.span, "operador desconhecido '" + op + "'");
  return r;
}

std::vector<Value> Interpreter::eval_args(const Expr& call, Env& env) {
  std::vector<Value> out;
  for (const auto& a : call.args) {
    if (a.name.empty()) out.push_back(eval(*a.value, env));
  }
  return out;
}

rt::ValueMap Interpreter::eval_kwargs(const Expr& call, Env& env) {
  rt::ValueMap kw;
  for (const auto& a : call.args) {
    if (!a.name.empty()) kw.set(a.name, eval(*a.value, env));
  }
  if (call.block) {
    for (const auto& it : call.block->items) {
      if (it && it->kind == ItemKind::Field && it->value) {
        kw.set(it->key, eval(*it->value, env));
      }
    }
  }
  return kw;
}

// `particionar_por:` aceita um nome de coluna (texto) ou uma lista de nomes
// para particao composta. Retorna vazio quando o argumento ausente.
std::vector<std::string> Interpreter::parse_particionar_por(const rt::ValueMap& kw,
                                                           const char* builtin,
                                                           const Span& span) {
  std::vector<std::string> cols;
  const Value* p = kw.find("particionar_por");
  if (!p) return cols;
  if (p->kind == ValueKind::Texto) {
    cols.push_back(p->s);
    return cols;
  }
  if (p->kind == ValueKind::Lista && p->list) {
    for (const Value& item : *p->list) {
      if (item.kind != ValueKind::Texto) {
        fail(span, std::string(builtin) +
                       ": 'particionar_por' deve ser texto ou lista de textos "
                       "(ex.: particionar_por: [\"estado\", \"mes\"])");
      }
      cols.push_back(item.s);
    }
    return cols;
  }
  fail(span, std::string(builtin) +
                 ": 'particionar_por' deve ser texto ou lista de textos "
                 "(ex.: particionar_por: [\"estado\", \"mes\"])");
}

Value Interpreter::eval_call(const Expr& expr, Env& env) {
  const Expr& callee = *expr.lhs;

  if (callee.kind == ExprKind::Member) {
    // Chamada de funcao de modulo: `io.ler_json_seguro(...)`.
    if (callee.lhs && callee.lhs->kind == ExprKind::Name) {
      if (auto mit = modules_.find(callee.lhs->text); mit != modules_.end()) {
        auto fit = mit->second->funcs.find(callee.text);
        if (fit == mit->second->funcs.end()) {
          std::string exports;
          for (const auto& kv : mit->second->funcs) {
            exports += (exports.empty() ? "" : ", ") + kv.first;
          }
          fail(expr.span, "modulo '" + mit->first + "' nao tem a funcao '" + callee.text + "'" +
                              (exports.empty() ? "" : " (funcoes: " + exports + ")"));
        }
        return call_function(*fit->second, eval_args(expr, env), expr.span, &mit->second->scope);
      }
    }
    Value receiver = eval(*callee.lhs, env);
    return eval_method(callee.text, std::move(receiver), expr, env);
  }

  if (callee.kind == ExprKind::Name) {
    const std::string& name = callee.text;
    // Funcao de modulo visivel no escopo (irma ou `de ... importar` aninhado):
    // anda na cadeia de envs procurando uma tabela de funcoes de modulo.
    for (Env* e = &env; e; e = e->parent) {
      if (e->funcs) {
        if (auto fit = e->funcs->find(name); fit != e->funcs->end()) {
          return call_function(*fit->second, eval_args(expr, env), expr.span, e);
        }
      }
    }
    if (auto it = functions_.find(name); it != functions_.end()) {
      Env* scope = nullptr;
      if (auto fm = func_module_.find(it->second); fm != func_module_.end()) {
        scope = &fm->second->scope;
      }
      return call_function(*it->second, eval_args(expr, env), expr.span, scope);
    }
    return eval_builtin(name, expr, env);
  }

  fail(expr.span, "chamada invalida");
}

Value Interpreter::call_function(const Item& fn, std::vector<Value> args, Span span,
                                 Env* module_scope) {
  // Funcoes de modulo rodam pela arvore: o subconjunto da VM resolve chamadas
  // por nome apenas contra 'functions_', sem a tabela do modulo (scope.funcs).
  if (module_scope == nullptr) {
  // Try the bytecode VM for functions in its pure subset; fall back otherwise.
  // A compilacao e lazy e cacheada: o mutex so cobre o mapa; o Chunk em si
  // e imutavel durante a execucao e pode ser rodado por varias threads.
  std::shared_ptr<vm::Chunk> chunk;
  {
    std::lock_guard<std::mutex> lk(vm_chunks_mutex_);
    auto cit = vm_chunks_.find(&fn);
    if (cit == vm_chunks_.end()) {
      try {
        std::unordered_set<std::string> names;
        for (const auto& kv : functions_) names.insert(kv.first);
        chunk = std::make_shared<vm::Chunk>(vm::compile_function(fn, names));
      } catch (const vm::NotCompilable&) {
        chunk = nullptr;
      }
      cit = vm_chunks_.emplace(&fn, chunk).first;
      if (std::getenv("TILT_VM_DEBUG") && chunk) {
        const vm::Chunk& c = *chunk;
        std::lock_guard<std::mutex> log_lk(log_mutex_);
        out_ << "; chunk " << decl_name(fn) << " locals=" << c.num_locals << "\n";
        for (std::size_t i = 0; i < c.code.size(); ++i) {
          out_ << ";  " << i << ": op=" << static_cast<int>(c.code[i].op) << " a=" << c.code[i].a
               << " b=" << c.code[i].b << "\n";
        }
      }
    }
    chunk = cit->second;
  }
  if (chunk) {
    vm::Vm machine(out_, [this](const std::string& name, std::vector<Value>& a, bool* handled) {
      auto f = functions_.find(name);
      if (f == functions_.end()) {
        *handled = false;
        return Value::nulo();
      }
      *handled = true;
      Env* scope = nullptr;
      if (auto fm = func_module_.find(f->second); fm != func_module_.end()) {
        scope = &fm->second->scope;
      }
      return call_function(*f->second, std::move(a), Span{}, scope);
    });
    try {
      return machine.run(*chunk, std::move(args));
    } catch (const std::exception& e) {
      fail(fn.span, std::string("VM: ") + e.what());
    }
  }
  }

  Env env;
  env.parent = module_scope ? module_scope : &root_;
  for (std::size_t k = 0; k < fn.params.size(); ++k) {
    env.vars[fn.params[k].name] = k < args.size() ? args[k] : Value::nulo();
  }
  if (!fn.block) return Value::nulo();
  try {
    exec_block(*fn.block, env);
  } catch (const ReturnSignal& r) {
    return r.value;
  }
  (void)span;
  return Value::nulo();
}

// ------------------------------------------------------------------ builtins

Value Interpreter::eval_builtin(const std::string& name, const Expr& call, Env& env) {
  auto args = [&] { return eval_args(call, env); };

  if (name == "imprimir" || name == "imprima" || name == "print") {
    auto a = args();
    for (std::size_t k = 0; k < a.size(); ++k) {
      if (k) out_ << ' ';
      out_ << to_display(a[k]);
    }
    out_ << '\n';
    return Value::nulo();
  }
  if (name == "registrar" || name == "log") {
    auto a = args();
    out_ << "[log]";
    for (const Value& v : a) out_ << ' ' << to_display(v);
    out_ << '\n';
    return Value::nulo();
  }
  if (name == "env") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) return Value::nulo();
    const char* v = std::getenv(a[0].s.c_str());
    return v ? Value::texto(v) : Value::nulo();
  }
  if (name == "tamanho" || name == "contar") {
    auto a = args();
    if (a.empty()) return Value::inteiro(0);
    const Value& v = a[0];
    if (v.kind == ValueKind::Lista || v.kind == ValueKind::Tabela) {
      return Value::inteiro(v.list ? static_cast<std::int64_t>(v.list->size()) : 0);
    }
    if (v.kind == ValueKind::Texto) return Value::inteiro(static_cast<std::int64_t>(v.s.size()));
    if (v.kind == ValueKind::Mapa) {
      return Value::inteiro(v.map ? static_cast<std::int64_t>(v.map->items.size()) : 0);
    }
    return Value::inteiro(0);
  }
  if (name == "somar" || name == "media" || name == "min" || name == "max") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Lista || !a[0].list || a[0].list->empty()) {
      return Value::inteiro(0);
    }
    const auto& xs = *a[0].list;
    double acc = xs[0].as_number();
    for (std::size_t k = 1; k < xs.size(); ++k) {
      double n = xs[k].as_number();
      if (name == "somar" || name == "media") acc += n;
      else if (name == "min") acc = std::min(acc, n);
      else acc = std::max(acc, n);
    }
    if (name == "media") acc /= static_cast<double>(xs.size());
    if (acc == static_cast<double>(static_cast<std::int64_t>(acc)) && name != "media") {
      return Value::inteiro(static_cast<std::int64_t>(acc));
    }
    return Value::decimal(acc);
  }
  if (name == "intervalo" || name == "ate") {
    auto a = args();
    std::int64_t lo = 0;
    std::int64_t hi = 0;
    if (a.size() == 1) hi = static_cast<std::int64_t>(a[0].as_number());
    else if (a.size() >= 2) {
      lo = static_cast<std::int64_t>(a[0].as_number());
      hi = static_cast<std::int64_t>(a[1].as_number());
    }
    rt::ValueList out;
    for (std::int64_t k = lo; k < hi; ++k) out.push_back(Value::inteiro(k));
    return Value::lista(std::move(out));
  }
  if (name == "dividir") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto || a[1].kind != ValueKind::Texto) {
      fail(call.span, "dividir espera (texto, separador)");
    }
    rt::ValueList out;
    const std::string& s = a[0].s;
    const std::string& sep = a[1].s;
    if (sep.empty()) {
      out.push_back(Value::texto(s));
    } else {
      std::size_t start = 0;
      std::size_t pos;
      while ((pos = s.find(sep, start)) != std::string::npos) {
        out.push_back(Value::texto(s.substr(start, pos - start)));
        start = pos + sep.size();
      }
      out.push_back(Value::texto(s.substr(start)));
    }
    return Value::lista(std::move(out));
  }
  if (name == "tensor" || name == "zeros" || name == "uns" || name == "aleatorio") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    auto to_shape = [&](const Value& v) {
      std::vector<std::int64_t> shape;
      if (v.kind == ValueKind::Lista && v.list) {
        for (const Value& e : *v.list) shape.push_back(static_cast<std::int64_t>(e.as_number()));
      }
      return shape;
    };
    if (name == "tensor" && !a.empty() && a[0].kind == ValueKind::Lista) {
      return Value::tensor_de(value_to_tensor(a[0], call.span));
    }
    std::vector<std::int64_t> shape = a.empty() ? std::vector<std::int64_t>{} : to_shape(a[0]);
    if (const Value* f = kw.find("forma")) shape = to_shape(*f);
    if (shape.empty()) fail(call.span, name + " espera uma forma, ex.: " + name + " [2, 3]");
    if (name == "zeros" || name == "tensor") {
      const Value* fill = kw.find("valor");
      return Value::tensor_de(rt::Tensor::filled(shape, fill ? static_cast<float>(fill->as_number()) : 0.0F));
    }
    if (name == "uns") return Value::tensor_de(rt::Tensor::ones(shape));
    const Value* seed = kw.find("semente");
    std::uint64_t s = seed ? static_cast<std::uint64_t>(seed->as_number()) : 42;
    return Value::tensor_de(rt::Tensor::xavier(shape, shape.front(), shape.back(), s));
  }
  if (name == "ler_csv") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) fail(call.span, "ler_csv espera um caminho");
    return read_csv_file(a[0].s, call.span);
  }
  if (name == "ler_parquet") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) fail(call.span, "ler_parquet espera um caminho");
    try {
      Value t = rt::parquet_read(a[0].s);
      t.kind = ValueKind::Tabela;
      return t;
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "ler_delta") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.empty() || a[0].kind != ValueKind::Texto) fail(call.span, "ler_delta espera um diretorio");
    const Value* onde = kw.find("onde");
    if (onde && onde->kind != ValueKind::Mapa) {
      fail(call.span, "ler_delta: 'onde' deve ser um mapa de colunas e valores "
                      "(ex.: onde: { estado: \"SP\" })");
    }
    try {
      Value t = rt::delta_read(a[0].s, onde);
      t.kind = ValueKind::Tabela;
      return t;
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "carregador") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.empty() || a[0].kind != ValueKind::Texto) fail(call.span, "carregador espera um caminho");
    const Value* alvo = kw.find("alvo");
    if (!alvo || alvo->kind != ValueKind::Texto) fail(call.span, "carregador espera 'alvo: \"coluna\"'");
    Value tbl = read_csv_file(a[0].s, call.span);
    const rt::ValueList& rows = tbl.list ? *tbl.list : rt::ValueList{};
    std::vector<std::string> feats;
    if (!rows.empty() && rows[0].map) {
      for (const auto& kv : rows[0].map->items) {
        if (kv.first != alvo->s) feats.push_back(kv.first);
      }
    }
    rt::Tensor x;
    x.shape = {static_cast<std::int64_t>(rows.size()), static_cast<std::int64_t>(feats.size())};
    x.data.reserve(rows.size() * feats.size());
    Value ys = Value::lista();
    for (const Value& row : rows) {
      for (const std::string& c : feats) {
        const Value* cell = row.map ? row.map->find(c) : nullptr;
        x.data.push_back(cell ? static_cast<float>(cell->as_number()) : 0.0F);
      }
      const Value* t = row.map ? row.map->find(alvo->s) : nullptr;
      ys.list->push_back(Value::inteiro(t ? static_cast<std::int64_t>(t->as_number()) : 0));
    }
    Value out = Value::mapa();
    out.map->set("x", Value::tensor_de(std::move(x)));
    out.map->set("y", std::move(ys));
    rt::ValueList fl;
    for (const std::string& c : feats) fl.push_back(Value::texto(c));
    out.map->set("atributos", Value::lista(std::move(fl)));
    return out;
  }
  if (name == "ler_json") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) fail(call.span, "ler_json espera um caminho");
    std::ifstream in(a[0].s);
    if (!in) fail(call.span, "nao foi possivel abrir '" + a[0].s + "'");
    std::ostringstream ss;
    ss << in.rdbuf();
    Value parsed;
    try {
      parsed = rt::json_parse(ss.str());
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    if (parsed.kind == ValueKind::Lista) parsed.kind = ValueKind::Tabela;
    return parsed;
  }
  if (name == "escrever_json") {
    auto a = args();
    if (a.size() < 2 || a[1].kind != ValueKind::Texto) {
      fail(call.span, "escrever_json espera (valor, caminho)");
    }
    std::ofstream out(a[1].s);
    if (!out) fail(call.span, "nao foi possivel escrever '" + a[1].s + "'");
    out << rt::json_dump(a[0]);
    return Value::nulo();
  }
  if (name == "escrever_csv") {
    auto a = args();
    if (a.size() < 2 || (a[0].kind != ValueKind::Tabela && a[0].kind != ValueKind::Lista)) {
      fail(call.span, "escrever espera (tabela, caminho)");
    }
    std::ofstream outf(a[1].s);
    if (!outf) fail(call.span, "nao foi possivel escrever '" + a[1].s + "'");
    const auto& rows = *a[0].list;
    if (!rows.empty() && rows[0].kind == ValueKind::Mapa && rows[0].map) {
      const auto& hdr = rows[0].map->items;
      for (std::size_t k = 0; k < hdr.size(); ++k) outf << (k ? "," : "") << hdr[k].first;
      outf << '\n';
      for (const Value& r : rows) {
        for (std::size_t k = 0; k < hdr.size(); ++k) {
          const Value* c = r.map ? r.map->find(hdr[k].first) : nullptr;
          outf << (k ? "," : "") << (c ? to_display(*c) : "");
        }
        outf << '\n';
      }
    }
    return Value::nulo();
  }
  if (name == "escrever_parquet") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.size() < 2 || (a[0].kind != ValueKind::Tabela && a[0].kind != ValueKind::Lista)) {
      fail(call.span, "escrever_parquet espera (tabela, caminho)");
    }
    if (a[1].kind != ValueKind::Texto) {
      fail(call.span, "escrever_parquet espera (tabela, caminho)");
    }
    rt::ParquetWriteOpts opts;
    const Value* codec = kw.find("codec");
    if (codec) {
      if (codec->kind != ValueKind::Texto) {
        fail(call.span, "escrever_parquet: 'codec' deve ser \"gzip\" ou \"snappy\"");
      }
      if (codec->s == "gzip") {
        opts.codec = 2;
      } else if (codec->s == "snappy") {
        opts.codec = 1;
      } else {
        fail(call.span, "escrever_parquet: codec '" + codec->s +
                            "' invalido (use \"gzip\" ou \"snappy\")");
      }
    }
    const Value* paginas = kw.find("paginas");
    if (paginas) {
      if (paginas->kind != ValueKind::Texto) {
        fail(call.span, "escrever_parquet: 'paginas' deve ser \"v1\" ou \"v2\"");
      }
      if (paginas->s == "v1") {
        opts.paginas_v2 = false;
      } else if (paginas->s == "v2") {
        opts.paginas_v2 = true;
      } else {
        fail(call.span, "escrever_parquet: paginas '" + paginas->s +
                            "' invalido (use \"v1\" ou \"v2\")");
      }
    }
    if (const Value* tipos = kw.find("tipos")) {
      if (tipos->kind != ValueKind::Mapa || !tipos->map) {
        fail(call.span,
             "escrever_parquet: 'tipos' deve ser um mapa coluna -> tipo "
             "(ex.: tipos: { id: \"int32\" })");
      }
      for (const auto& kv : tipos->map->items) {
        if (kv.second.kind != ValueKind::Texto) {
          fail(call.span, "escrever_parquet: tipos['" + kv.first + "'] deve ser texto");
        }
        opts.tipos[kv.first] = kv.second.s;
      }
    }
    if (const Value* dic = kw.find("dicionario")) {
      if (dic->kind != ValueKind::Logico) {
        fail(call.span, "escrever_parquet: 'dicionario' deve ser logico");
      }
      opts.dicionario = dic->b;
    }
    try {
      rt::parquet_write(a[1].s, a[0], nullptr, opts);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "escrever_delta") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.size() < 2 || (a[0].kind != ValueKind::Tabela && a[0].kind != ValueKind::Lista)) {
      fail(call.span, "escrever_delta espera (tabela, diretorio)");
    }
    std::vector<std::string> part_cols = parse_particionar_por(kw, "escrever_delta", call.span);
    try {
      rt::delta_write(a[1].s, a[0], part_cols);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "anexar_delta") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.size() < 2 || (a[0].kind != ValueKind::Tabela && a[0].kind != ValueKind::Lista)) {
      fail(call.span, "anexar_delta espera (tabela, diretorio)");
    }
    std::vector<std::string> part_cols = parse_particionar_por(kw, "anexar_delta", call.span);
    try {
      rt::delta_append(a[1].s, a[0], part_cols);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "ler_iceberg") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.empty() || a[0].kind != ValueKind::Texto) fail(call.span, "ler_iceberg espera um diretorio");
    const Value* onde = kw.find("onde");
    if (onde && onde->kind != ValueKind::Mapa) {
      fail(call.span, "ler_iceberg: 'onde' deve ser um mapa de colunas e valores "
                      "(ex.: onde: { estado: \"SP\" })");
    }
    try {
      Value t = rt::iceberg_read(a[0].s, onde);
      t.kind = ValueKind::Tabela;
      return t;
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "escrever_iceberg") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.size() < 2 || (a[0].kind != ValueKind::Tabela && a[0].kind != ValueKind::Lista)) {
      fail(call.span, "escrever_iceberg espera (tabela, diretorio)");
    }
    std::vector<std::string> part_cols = parse_particionar_por(kw, "escrever_iceberg", call.span);
    try {
      rt::iceberg_write(a[1].s, a[0], part_cols);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "anexar_iceberg") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.size() < 2 || (a[0].kind != ValueKind::Tabela && a[0].kind != ValueKind::Lista)) {
      fail(call.span, "anexar_iceberg espera (tabela, diretorio)");
    }
    std::vector<std::string> part_cols = parse_particionar_por(kw, "anexar_iceberg", call.span);
    try {
      rt::iceberg_append(a[1].s, a[0], part_cols);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "apagar_iceberg") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span, "apagar_iceberg espera (diretorio, onde: {...})");
    }
    const Value* onde = kw.find("onde");
    if (!onde || onde->kind != ValueKind::Mapa) {
      fail(call.span, "apagar_iceberg: 'onde' deve ser um mapa de colunas e valores "
                      "(ex.: onde: { id: 3 })");
    }
    bool igualdade = false;
    if (const Value* mv = kw.find("modo")) {
      if (mv->kind != ValueKind::Texto || (mv->s != "posicao" && mv->s != "igualdade")) {
        fail(call.span, "apagar_iceberg: 'modo' deve ser \"posicao\" ou \"igualdade\"");
      }
      igualdade = mv->s == "igualdade";
    }
    try {
      return Value::inteiro(rt::iceberg_delete(a[0].s, *onde, igualdade));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "responder") {
    rt::ValueMap kw = eval_kwargs(call, env);
    out_ << "resposta:";
    for (const auto& kv : kw.items) out_ << ' ' << kv.first << '=' << to_display(kv.second);
    out_ << '\n';
    return Value::nulo();
  }

  if (name == "perguntar" || name == "perguntar_em_fluxo") {
    return eval_perguntar(call, env);
  }
  if (name == "incorporar") {
    auto a = args();
    const std::string model = !a.empty() && a[0].kind == ValueKind::Texto ? a[0].s : "";
    std::string text;
    if (a.size() > 1) text = a[1].kind == ValueKind::Texto ? a[1].s : to_display(a[1]);
    try {
      std::vector<float> v = rt::llm_embed(model, text);
      rt::Tensor t;
      t.shape = {static_cast<std::int64_t>(v.size())};
      t.data = std::move(v);
      return Value::tensor_de(std::move(t));
    } catch (const std::exception& e) {
      fail(call.span, std::string("incorporar: ") + e.what());
    }
  }
  if (name == "checar_tilt") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span, "checar_tilt espera o caminho de um arquivo .tilt");
    }
    Value result = Value::mapa();
    std::optional<SourceFile> src;
    try {
      src = SourceFile::load(a[0].s);
    } catch (const std::exception& e) {
      result.map->set("ok", Value::logico(false));
      result.map->set("erro", Value::texto(std::string(e.what())));
      return result;
    }
    DiagnosticEngine d(&src.value());
    Lexer lx(src.value(), d);
    std::vector<Token> toks = lx.tokenize();
    Parser ps(toks, d);
    ast::Program prog = ps.parse_program();
    check_program(prog, d);

    result.map->set("ok", Value::logico(!d.has_errors()));
    Value errs = Value::lista();
    for (const Diagnostic& e : d.all()) {
      Value m = Value::mapa();
      m.map->set("codigo", Value::texto(std::string(diag_code_string(e.code))));
      m.map->set("linha", Value::inteiro(e.span.line));
      m.map->set("coluna", Value::inteiro(e.span.column));
      m.map->set("mensagem", Value::texto(e.message));
      Value ns = Value::lista();
      for (const std::string& n : e.notes) ns.list->push_back(Value::texto(n));
      m.map->set("notas", std::move(ns));
      errs.list->push_back(std::move(m));
    }
    result.map->set("erros", std::move(errs));
    return result;
  }
  if (name == "dividir_texto") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    const std::string src = !a.empty() && a[0].kind == ValueKind::Texto ? a[0].s : "";
    std::size_t win = 800;
    std::size_t overlap = 100;
    if (const Value* t = kw.find("tamanho")) win = static_cast<std::size_t>(t->as_number());
    if (const Value* o = kw.find("sobreposicao")) overlap = static_cast<std::size_t>(o->as_number());
    if (win == 0) win = 1;
    const std::size_t step = win > overlap ? win - overlap : 1;
    rt::ValueList chunks;
    for (std::size_t start = 0; start < src.size(); start += step) {
      chunks.push_back(Value::texto(src.substr(start, win)));
      if (start + win >= src.size()) break;
    }
    if (chunks.empty()) chunks.push_back(Value::texto(src));
    return Value::lista(std::move(chunks));
  }
  if (name == "ler") {
    auto a = args();
    if (!a.empty() && a[0].kind == ValueKind::Texto && entities_.count(a[0].s)) {
      return read_fonte(a[0].s, call.span);
    }
    fail(call.span, "ler: esperava uma 'fonte' declarada", DiagCode::ConnectorNotImplemented);
  }
  if (name == "executar_sql") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto || a[1].kind != ValueKind::Texto) {
      fail(call.span,
           "executar_sql espera (url, sql [, params]), ex.: executar_sql "
           "\"postgres://localhost:5432/app\", \"insert into t (nome) values ($1)\", [\"ana\"] "
           "— use '?' como placeholder (vira $N no postgres e {pN} no clickhouse)");
    }
    const std::string& url = a[0].s;
    // Params opcionais (3o arg, lista de escalares; Marco 3 / D1).
    std::vector<rt::SqlParam> params;
    bool com_params = false;
    if (a.size() >= 3) {
      if (a[2].kind != ValueKind::Lista || !a[2].list) {
        fail(call.span, "executar_sql: 'params' deve ser uma lista [v1, v2, ...]");
      }
      com_params = true;
      for (const Value& v : *a[2].list) {
        try {
          params.push_back(rt::param_de_valor(v, "executar_sql"));
        } catch (const std::exception& e) {
          fail(call.span, std::string(e.what()));
        }
      }
    }
    try {
      if (url.rfind("postgres://", 0) == 0 || url.rfind("postgresql://", 0) == 0) {
        if (com_params) {
          rt::postgres_exec_params(url, a[1].s, params);
        } else {
          rt::postgres_exec(url, a[1].s);
        }
      } else if (url.rfind("sqlite://", 0) == 0) {
        if (com_params) {
          rt::sqlite_exec_params(url.substr(9), a[1].s, params);
        } else {
          rt::sqlite_exec(url.substr(9), a[1].s);
        }
      } else if (url.rfind("duckdb://", 0) == 0) {
        if (com_params) {
          rt::duckdb_exec_params(url.substr(9), a[1].s, params);
        } else {
          rt::duckdb_exec(url.substr(9), a[1].s);
        }
      } else if (url.rfind("mysql://", 0) == 0 || url.rfind("mariadb://", 0) == 0) {
        if (com_params) {
          rt::mysql_exec_params(url, a[1].s, params);
        } else {
          rt::mysql_exec(url, a[1].s);
        }
      } else if (url.rfind("clickhouse://", 0) == 0) {
        if (com_params) {
          rt::clickhouse_exec_params(url, a[1].s, params);
        } else {
          rt::clickhouse_exec(url, a[1].s);
        }
      } else {
        fail(call.span,
             "executar_sql: url '" + url +
                 "' invalida (use postgres://, sqlite://, duckdb://, mysql:// ou clickhouse://)");
      }
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "consultar_sql") {
    // SELECT com params (contraparte de leitura do executar_sql com `?`):
    // consultar_sql url, "select ... where id = ?", [42] -> tabela.
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto || a[1].kind != ValueKind::Texto) {
      fail(call.span,
           "consultar_sql espera (url, sql [, params]), ex.: consultar_sql "
           "\"postgres://localhost:5432/app\", \"select * from t where id = ?\", [42] "
           "— use '?' como placeholder (mesma ligacao do executar_sql)");
    }
    const std::string& url = a[0].s;
    std::vector<rt::SqlParam> params;
    bool com_params = false;
    if (a.size() >= 3) {
      if (a[2].kind != ValueKind::Lista || !a[2].list) {
        fail(call.span, "consultar_sql: 'params' deve ser uma lista [v1, v2, ...]");
      }
      com_params = true;
      for (const Value& v : *a[2].list) {
        try {
          params.push_back(rt::param_de_valor(v, "consultar_sql"));
        } catch (const std::exception& e) {
          fail(call.span, std::string(e.what()));
        }
      }
    }
    try {
      if (url.rfind("postgres://", 0) == 0 || url.rfind("postgresql://", 0) == 0) {
        if (com_params) return rt::postgres_query_params(url, a[1].s, params);
        return rt::postgres_query(url, a[1].s);
      }
      if (url.rfind("sqlite://", 0) == 0) {
        if (com_params) return rt::sqlite_query_params(url.substr(9), a[1].s, params);
        return rt::sqlite_query(url.substr(9), a[1].s);
      }
      if (url.rfind("duckdb://", 0) == 0) {
        if (com_params) return rt::duckdb_query_params(url.substr(9), a[1].s, params);
        return rt::duckdb_query(url.substr(9), a[1].s);
      }
      if (url.rfind("mysql://", 0) == 0 || url.rfind("mariadb://", 0) == 0) {
        if (com_params) return rt::mysql_query_params(url, a[1].s, params);
        return rt::mysql_query(url, a[1].s);
      }
      if (url.rfind("clickhouse://", 0) == 0) {
        if (com_params) return rt::clickhouse_query_params(url, a[1].s, params);
        return rt::clickhouse_query(url, a[1].s);
      }
      fail(call.span,
           "consultar_sql: url '" + url +
               "' invalida (use postgres://, sqlite://, duckdb://, mysql:// ou clickhouse://)");
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "transacao") {
    // Marco 3 / D1: passos atomicos numa unica conexao (BEGIN/COMMIT de
    // verdade; ROLLBACK com o indice do passo em caso de falha).
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto || a[1].kind != ValueKind::Lista ||
        !a[1].list) {
      fail(call.span,
           "transacao espera (url, passos), ex.: transacao \"postgres://h/db\", "
           "[{ sql: \"insert ... values (?, ?)\", params: [1, \"ana\"] }] "
           "(clickhouse nao tem transacoes: erro claro)");
    }
    const std::string& url = a[0].s;
    std::vector<std::pair<std::string, std::vector<rt::SqlParam>>> passos;
    for (const Value& item : *a[1].list) {
      if (item.kind != ValueKind::Mapa || !item.map) {
        fail(call.span, "transacao: cada passo deve ser um mapa { sql:, params:? }");
      }
      const Value* sql = item.map->find("sql");
      if (!sql || sql->kind != ValueKind::Texto) {
        fail(call.span, "transacao: cada passo precisa de 'sql' texto");
      }
      std::vector<rt::SqlParam> ps;
      if (const Value* pv = item.map->find("params")) {
        if (pv->kind != ValueKind::Lista || !pv->list) {
          fail(call.span, "transacao: 'params' deve ser uma lista [v1, v2, ...]");
        }
        for (const Value& v : *pv->list) {
          try {
            ps.push_back(rt::param_de_valor(v, "transacao"));
          } catch (const std::exception& e) {
            fail(call.span, std::string(e.what()));
          }
        }
      }
      passos.emplace_back(sql->s, std::move(ps));
    }
    try {
      if (url.rfind("postgres://", 0) == 0 || url.rfind("postgresql://", 0) == 0) {
        rt::postgres_transact(url, passos);
      } else if (url.rfind("sqlite://", 0) == 0) {
        rt::sqlite_transact(url.substr(9), passos);
      } else if (url.rfind("duckdb://", 0) == 0) {
        rt::duckdb_transact(url.substr(9), passos);
      } else if (url.rfind("mysql://", 0) == 0 || url.rfind("mariadb://", 0) == 0) {
        rt::mysql_transact(url, passos);
      } else if (url.rfind("clickhouse://", 0) == 0) {
        rt::clickhouse_transact(url, passos);
      } else {
        fail(call.span,
             "transacao: url '" + url +
                 "' invalida (use postgres://, sqlite://, duckdb://, mysql:// ou clickhouse://)");
      }
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "spark_sql" || name == "spark_executar") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.size() < 2 || a[0].kind != ValueKind::Texto || a[1].kind != ValueKind::Texto) {
      fail(call.span, name + " espera (url, " +
                           std::string(name == "spark_sql" ? "sql" : "codigo") +
                           ", [lingua:, conf:]), ex.: " + name +
                           " \"http://localhost:8998\", \"" +
                           (name == "spark_sql" ? "select * from vendas" : "1 + 1") + "\"");
    }
    std::string lingua = "scala";
    if (const Value* l = kw.find("lingua")) {
      if (l->kind != ValueKind::Texto || (l->s != "scala" && l->s != "pyspark")) {
        fail(call.span, name + ": 'lingua' deve ser \"scala\" ou \"pyspark\"");
      }
      lingua = l->s;
    }
    Value conf = Value::nulo();
    if (const Value* cf = kw.find("conf")) {
      if (cf->kind != ValueKind::Mapa || !cf->map) {
        fail(call.span, name + ": 'conf' deve ser um mapa {...} (conf de sessao Spark, "
                               "ex.: { \"spark.jars.packages\": \"...\" })");
      }
      conf = *cf;
    }
    try {
      if (name == "spark_sql") {
        Value t = rt::livy_sql(a[0].s, a[1].s, lingua,
                               conf.kind == ValueKind::Mapa ? &conf : nullptr);
        t.kind = ValueKind::Tabela;
        return t;
      }
      return rt::livy_executar(a[0].s, a[1].s, lingua,
                               conf.kind == ValueKind::Mapa ? &conf : nullptr);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  // Opcoes {senha:, banco:, tls:} comuns a ler_redis/escrever_redis; senha e
  // banco vencem a URL, tls liga TLS (rediss:// tambem liga).
  auto redis_opts = [&](const std::vector<Value>& a, std::size_t idx) -> rt::RedisOpts {
    rt::RedisOpts opts;
    if (a.size() <= idx) return opts;
    if (a[idx].kind != ValueKind::Mapa || !a[idx].map) {
      fail(call.span, "redis: opcoes devem ser um mapa {senha:, banco:, tls:}");
    }
    if (const Value* sv = a[idx].map->find("senha")) {
      if (sv->kind != ValueKind::Texto) fail(call.span, "redis: 'senha' deve ser texto");
      opts.auth = sv->s;
    }
    if (const Value* bv = a[idx].map->find("banco")) {
      if (bv->kind != ValueKind::Inteiro) fail(call.span, "redis: 'banco' deve ser inteiro");
      opts.db = static_cast<int>(bv->i);
    }
    if (const Value* tv = a[idx].map->find("tls")) {
      if (tv->kind != ValueKind::Logico) fail(call.span, "redis: 'tls' deve ser logico");
      opts.tls = tv->b;
    }
    return opts;
  };
  if (name == "ler_redis") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto || a[1].kind != ValueKind::Texto) {
      fail(call.span,
           "ler_redis espera (url, chave, {senha:, banco:}), ex.: ler_redis "
           "\"redis://:segredo@localhost:6379/2\", \"chave\"");
    }
    try {
      return rt::redis_get(a[0].s, a[1].s, redis_opts(a, 2));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "escrever_redis") {
    auto a = args();
    if (a.size() < 3 || a[0].kind != ValueKind::Texto || a[1].kind != ValueKind::Texto) {
      fail(call.span,
           "escrever_redis espera (url, chave, valor, {senha:, banco:}), ex.: escrever_redis "
           "url, \"chave\", valor");
    }
    try {
      rt::redis_set(a[0].s, a[1].s, a[2], redis_opts(a, 3));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "redis_executar") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto || a[1].kind != ValueKind::Texto) {
      fail(call.span,
           "redis_executar espera (url, comando, [args...]), ex.: redis_executar "
           "url, \"GET\", \"chave\"");
    }
    std::vector<std::string> cmd;
    cmd.reserve(a.size() - 1);
    cmd.push_back(a[1].s);
    for (std::size_t i = 2; i < a.size(); ++i) {
      cmd.push_back(rt::redis_arg_para_texto(a[i]));
    }
    try {
      return rt::redis_executar(a[0].s, cmd);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "redis_lote") {
    auto a = args();
    if (a.size() != 2 || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "redis_lote espera (url, [[comando, args...], ...]), ex.: redis_lote "
           "url, [[\"INCR\", \"contador\"], [\"GET\", \"chave\"]]");
    }
    if (a[1].kind != ValueKind::Lista || !a[1].list) {
      fail(call.span, "redis: o lote deve ser uma lista de listas [[comando, args...], ...]");
    }
    std::vector<std::vector<std::string>> cmds;
    cmds.reserve(a[1].list->size());
    for (const Value& item : *a[1].list) {
      if (item.kind != ValueKind::Lista || !item.list || item.list->empty()) {
        fail(call.span,
             "redis: cada comando do lote deve ser uma lista nao vazia "
             "[comando, args...]");
      }
      std::vector<std::string> cmd;
      cmd.reserve(item.list->size());
      for (const Value& e : *item.list) cmd.push_back(rt::redis_arg_para_texto(e));
      cmds.push_back(std::move(cmd));
    }
    try {
      return rt::redis_lote(a[0].s, cmds);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "ler_kafka") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "ler_kafka espera (topico, {desde:, max:, grupo:, broker:}), ex.: ler_kafka "
           "\"pedidos\", {desde: \"inicio\", max: 100}");
    }
    bool do_fim = false;
    std::int64_t max = 100;
    std::string grupo, broker;
    bool tls = false;
    if (a.size() >= 2) {
      if (a[1].kind != ValueKind::Mapa || !a[1].map) {
        fail(call.span, "ler_kafka: opcoes devem ser um mapa {desde:, max:, grupo:, broker:, tls:}");
      }
      if (const Value* dv = a[1].map->find("desde")) {
        if (dv->kind != ValueKind::Texto || (dv->s != "inicio" && dv->s != "fim")) {
          fail(call.span, "ler_kafka: 'desde' deve ser \"inicio\" ou \"fim\"");
        }
        do_fim = dv->s == "fim";
      }
      if (const Value* mv = a[1].map->find("max")) {
        if (mv->kind != ValueKind::Inteiro) {
          fail(call.span, "ler_kafka: 'max' deve ser inteiro");
        }
        max = mv->i;
      }
      if (const Value* gv = a[1].map->find("grupo")) {
        if (gv->kind != ValueKind::Texto || gv->s.empty()) {
          fail(call.span, "ler_kafka: 'grupo' deve ser texto nao vazio");
        }
        grupo = gv->s;
      }
      if (const Value* bv = a[1].map->find("broker")) {
        if (bv->kind != ValueKind::Texto) {
          fail(call.span, "ler_kafka: 'broker' deve ser texto (ex.: \"host:9092\")");
        }
        broker = bv->s;
      }
      if (const Value* tv = a[1].map->find("tls")) {
        if (tv->kind != ValueKind::Logico) fail(call.span, "ler_kafka: 'tls' deve ser logico");
        tls = tv->b;
      }
    }
    try {
      if (!grupo.empty()) {
        // Consumer group: coordenacao + checkpoint por commit de offset.
        Value out = Value::lista();
        for (const auto& [part, valor] : rt::kafka_consume_group(broker, grupo, a[0].s,
                                                                 static_cast<int>(max), tls)) {
          (void)part;
          out.list->push_back(Value::texto(valor));
        }
        return out;
      }
      return rt::kafka_ler(a[0].s, do_fim, max, broker, tls);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "escrever_kafka") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "escrever_kafka espera (topico, valor, {particao:, chave:, acks:, tentativas:, "
           "idempotente:}), ex.: escrever_kafka \"pedidos\", valor");
    }
    std::int64_t particao = 0;
    bool tls = false;
    rt::ProduceOptions opt;
    if (a.size() >= 3) {
      if (a[2].kind != ValueKind::Mapa || !a[2].map) {
        fail(call.span,
             "escrever_kafka: opcoes devem ser um mapa {particao:, chave:, acks:, tentativas:, "
             "idempotente:, tls:}");
      }
      if (const Value* pv = a[2].map->find("particao")) {
        if (pv->kind != ValueKind::Inteiro) {
          fail(call.span, "escrever_kafka: 'particao' deve ser inteiro");
        }
        particao = pv->i;
      }
      if (const Value* kv = a[2].map->find("chave")) {
        if (kv->kind != ValueKind::Texto) {
          fail(call.span, "escrever_kafka: 'chave' deve ser texto");
        }
        opt.chave = kv->s;
      }
      if (const Value* av = a[2].map->find("acks")) {
        if (av->kind != ValueKind::Inteiro || (av->i != -1 && av->i != 1)) {
          fail(call.span, "escrever_kafka: 'acks' deve ser -1 (all) ou 1 (leader)");
        }
        opt.acks = static_cast<int>(av->i);
      }
      if (const Value* tv2 = a[2].map->find("tentativas")) {
        if (tv2->kind != ValueKind::Inteiro || tv2->i < 1 || tv2->i > 10) {
          fail(call.span, "escrever_kafka: 'tentativas' deve ser inteiro entre 1 e 10");
        }
        opt.tentativas = static_cast<int>(tv2->i);
      }
      if (const Value* iv = a[2].map->find("idempotente")) {
        if (iv->kind != ValueKind::Logico) {
          fail(call.span, "escrever_kafka: 'idempotente' deve ser logico");
        }
        opt.idempotente = iv->b;
      }
      if (const Value* tv = a[2].map->find("tls")) {
        if (tv->kind != ValueKind::Logico) fail(call.span, "escrever_kafka: 'tls' deve ser logico");
        tls = tv->b;
      }
    }
    try {
      const std::string body = a[1].kind == ValueKind::Texto ? a[1].s : rt::json_dump(a[1]);
      rt::kafka_produzir(a[0].s, body, static_cast<std::int32_t>(particao), opt, tls);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "mongo_inserir") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "mongo_inserir espera (colecao, documento, {banco:}), ex.: mongo_inserir \"pedidos\", "
           "{ cliente: \"ana\" }");
    }
    std::string banco;
    if (a.size() >= 3) {
      if (a[2].kind != ValueKind::Mapa || !a[2].map) {
        fail(call.span, "mongo_inserir: opcoes devem ser um mapa {banco: \"x\"}");
      }
      if (const Value* bv = a[2].map->find("banco")) {
        if (bv->kind != ValueKind::Texto) {
          fail(call.span, "mongo_inserir: 'banco' deve ser texto");
        }
        banco = bv->s;
      }
    }
    try {
      rt::mongo_inserir(a[0].s, a[1], banco);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "mongo_buscar") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "mongo_buscar espera (colecao, {filtro:, max:, somente:, lote:, banco:}), ex.: "
           "mongo_buscar \"pedidos\", { filtro: { cliente: \"ana\" } }");
    }
    Value filtro = Value::mapa();
    std::int64_t max = 100;
    std::int64_t lote = 0;
    Value somente = Value::lista();
    std::string banco;
    if (a.size() >= 2) {
      if (a[1].kind != ValueKind::Mapa || !a[1].map) {
        fail(call.span, "mongo_buscar: opcoes devem ser um mapa {filtro:, max:, somente:, lote:, banco:}");
      }
      if (const Value* fv = a[1].map->find("filtro")) {
        if (fv->kind != ValueKind::Mapa || !fv->map) {
          fail(call.span, "mongo_buscar: 'filtro' deve ser um mapa de igualdade");
        }
        filtro = *fv;
      }
      if (const Value* mv = a[1].map->find("max")) {
        if (mv->kind != ValueKind::Inteiro) {
          fail(call.span, "mongo_buscar: 'max' deve ser inteiro");
        }
        max = mv->i;
      }
      if (const Value* sv = a[1].map->find("somente")) {
        if (sv->kind != ValueKind::Lista || !sv->list || sv->list->empty()) {
          fail(call.span, "mongo_buscar: 'somente' deve ser uma lista de textos nao vazia");
        }
        for (const Value& c : *sv->list) {
          if (c.kind != ValueKind::Texto || c.s.empty()) {
            fail(call.span, "mongo_buscar: 'somente' deve ser uma lista de textos nao vazios");
          }
        }
        somente = *sv;
      }
      if (const Value* lv = a[1].map->find("lote")) {
        if (lv->kind != ValueKind::Inteiro || lv->i <= 0) {
          fail(call.span, "mongo_buscar: 'lote' deve ser inteiro > 0");
        }
        lote = lv->i;
      }
      if (const Value* bv = a[1].map->find("banco")) {
        if (bv->kind != ValueKind::Texto) {
          fail(call.span, "mongo_buscar: 'banco' deve ser texto");
        }
        banco = bv->s;
      }
    }
    try {
      return rt::mongo_buscar(a[0].s, filtro, max, banco, somente, lote);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "mongo_atualizar") {
    auto a = args();
    if (a.size() < 3 || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "mongo_atualizar espera (colecao, filtro, mudancas, {banco:, multi:}), ex.: "
           "mongo_atualizar \"pedidos\", {cliente: \"ana\"}, {$set: {valor: 999}, $inc: {acessos: 1}}");
    }
    if (a[1].kind != ValueKind::Mapa || !a[1].map) {
      fail(call.span, "mongo_atualizar: 'filtro' deve ser um mapa de igualdade");
    }
    if (a[2].kind != ValueKind::Mapa || !a[2].map) {
      fail(call.span, "mongo_atualizar: 'mudancas' deve ser um mapa {$set: {...}, $inc: {...}}");
    }
    std::string banco;
    bool multi = false;
    if (a.size() >= 4) {
      if (a[3].kind != ValueKind::Mapa || !a[3].map) {
        fail(call.span, "mongo_atualizar: opcoes devem ser um mapa {banco:, multi:}");
      }
      if (const Value* bv = a[3].map->find("banco")) {
        if (bv->kind != ValueKind::Texto) {
          fail(call.span, "mongo_atualizar: 'banco' deve ser texto");
        }
        banco = bv->s;
      }
      if (const Value* mv = a[3].map->find("multi")) {
        if (mv->kind != ValueKind::Logico) {
          fail(call.span, "mongo_atualizar: 'multi' deve ser logico");
        }
        multi = mv->b;
      }
    }
    try {
      return Value::inteiro(rt::mongo_atualizar(a[0].s, a[1], a[2], multi, banco));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "mongo_deletar") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "mongo_deletar espera (colecao, filtro, {banco:}), ex.: mongo_deletar \"pedidos\", "
           "{cliente: \"bob\"}");
    }
    if (a[1].kind != ValueKind::Mapa || !a[1].map) {
      fail(call.span, "mongo_deletar: 'filtro' deve ser um mapa de igualdade");
    }
    std::string banco;
    if (a.size() >= 3) {
      if (a[2].kind != ValueKind::Mapa || !a[2].map) {
        fail(call.span, "mongo_deletar: opcoes devem ser um mapa {banco: \"x\"}");
      }
      if (const Value* bv = a[2].map->find("banco")) {
        if (bv->kind != ValueKind::Texto) {
          fail(call.span, "mongo_deletar: 'banco' deve ser texto");
        }
        banco = bv->s;
      }
    }
    try {
      return Value::inteiro(rt::mongo_deletar(a[0].s, a[1], banco));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "mongo_criar_indice") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "mongo_criar_indice espera (colecao, {campos:, banco:}), ex.: mongo_criar_indice "
           "\"pedidos\", {campos: [\"cliente\"]}");
    }
    if (a[1].kind != ValueKind::Mapa || !a[1].map) {
      fail(call.span, "mongo_criar_indice: opcoes devem ser um mapa {campos:, banco:}");
    }
    Value campos;
    if (const Value* cv = a[1].map->find("campos")) {
      if (cv->kind != ValueKind::Lista || !cv->list) {
        fail(call.span, "mongo_criar_indice: 'campos' deve ser uma lista de textos");
      }
      campos = *cv;
    } else {
      fail(call.span, "mongo_criar_indice: falta 'campos: [\"a\", ...]'");
    }
    std::string banco;
    if (const Value* bv = a[1].map->find("banco")) {
      if (bv->kind != ValueKind::Texto) {
        fail(call.span, "mongo_criar_indice: 'banco' deve ser texto");
      }
      banco = bv->s;
    }
    try {
      return Value::texto(rt::mongo_criar_indice(a[0].s, campos, banco));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "mongo_agregar") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "mongo_agregar espera (colecao, [etapas], {banco:}), ex.: mongo_agregar \"pedidos\", "
           "[{$group: {_id: \"$cliente\", total: {$sum: 1}}}]");
    }
    if (a[1].kind != ValueKind::Lista || !a[1].list) {
      fail(call.span, "mongo_agregar: 'etapas' deve ser uma lista de mapas [{operador: {...}}]");
    }
    std::string banco;
    if (a.size() >= 3) {
      if (a[2].kind != ValueKind::Mapa || !a[2].map) {
        fail(call.span, "mongo_agregar: opcoes devem ser um mapa {banco: \"x\"}");
      }
      if (const Value* bv = a[2].map->find("banco")) {
        if (bv->kind != ValueKind::Texto) {
          fail(call.span, "mongo_agregar: 'banco' deve ser texto");
        }
        banco = bv->s;
      }
    }
    try {
      return rt::mongo_agregar(a[0].s, a[1], banco);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "http_get_json" || name == "http_post_json") {
    auto a = args();
    rt::ValueMap kw = eval_kwargs(call, env);
    const bool eh_post = name == "http_post_json";
    const std::size_t min = eh_post ? 2 : 1;
    if (a.size() < min || a[0].kind != ValueKind::Texto) {
      fail(call.span, name + " espera (url" + std::string(eh_post ? ", valor" : "") +
                           ", [cabecalhos: {...}]), ex.: " + name +
                           " \"https://api.exemplo.com/dados\"");
    }
    // cabecalhos: aceito nomeado (cabecalhos: {...}) ou como ultimo argumento
    // posicional (mapa); ausente/nulo = sem headers custom.
    const Value* cab = nullptr;
    if (a.size() > min && a[min].kind != ValueKind::Nulo) cab = &a[min];
    if (!cab) {
      if (const Value* k = kw.find("cabecalhos"); k && k->kind != ValueKind::Nulo) cab = k;
    }
    std::vector<std::pair<std::string, std::string>> headers;
    if (cab) {
      if (cab->kind != ValueKind::Mapa || !cab->map) {
        fail(call.span, name + ": 'cabecalhos' deve ser um mapa { \"Nome\": \"valor\" }");
      }
      for (const auto& [k, v] : cab->map->items) {
        if (v.kind != ValueKind::Texto) {
          fail(call.span, name + ": cabecalho '" + k + "' deve ter valor texto");
        }
        headers.emplace_back(k, v.s);
      }
    }
    try {
      if (eh_post) return rt::http_post_json(a[0].s, a[1], headers);
      return rt::http_get_json(a[0].s, headers);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "es_buscar") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto) {
      fail(call.span, "es_buscar espera (url, dsl), ex.: es_buscar "
                      "\"elasticsearch://localhost:9200/meuindice\", "
                      "{query: {match_all: {}}}");
    }
    try {
      return rt::es_query(a[0].s, a[1]);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "es_executar") {
    auto a = args();
    if (a.size() < 3 || a[0].kind != ValueKind::Texto || a[1].kind != ValueKind::Texto ||
        a[2].kind != ValueKind::Texto) {
      fail(call.span, "es_executar espera (url, metodo, caminho, [corpo]), ex.: es_executar "
                      "\"elasticsearch://localhost:9200\", \"PUT\", \"/meuindice\", "
                      "{ \\\"mappings\\\": {} }");
    }
    try {
      return rt::es_exec(a[0].s, a[1].s, a[2].s, a.size() > 3 ? a[3] : Value::nulo());
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "ler_s3") {
    auto a = args();
    if (a.size() < 1 || a[0].kind != ValueKind::Texto) {
      fail(call.span, "ler_s3 espera (url), ex.: ler_s3 \"s3://bucket/chave\"");
    }
    try {
      return Value::texto(rt::s3_get(a[0].s));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "escrever_s3") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto) {
      fail(call.span, "escrever_s3 espera (url, valor), ex.: escrever_s3 \"s3://bucket/chave\", valor");
    }
    try {
      const std::string body =
          a[1].kind == ValueKind::Texto ? a[1].s : rt::json_dump(a[1]);
      rt::s3_put(a[0].s, body);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "listar_s3") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "listar_s3 espera (url_do_bucket, {prefixo:, max:}), ex.: listar_s3 "
           "\"s3://bucket\", {prefixo: \"x\", max: 100}");
    }
    std::string prefixo;
    int max = 1000;
    if (a.size() >= 2) {
      if (a[1].kind != ValueKind::Mapa || !a[1].map) {
        fail(call.span, "listar_s3: opcoes devem ser um mapa {prefixo:, max:}");
      }
      if (const Value* p = a[1].map->find("prefixo")) {
        if (p->kind != ValueKind::Texto) {
          fail(call.span, "listar_s3: 'prefixo' deve ser texto");
        }
        prefixo = p->s;
      }
      if (const Value* m = a[1].map->find("max")) {
        max = static_cast<int>(m->as_number());
      }
    }
    try {
      const auto chaves = rt::s3_list(a[0].s, prefixo, max);
      rt::ValueList lst;
      for (const auto& c : chaves) lst.push_back(Value::texto(c));
      return Value::lista(std::move(lst));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "apagar_s3") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span, "apagar_s3 espera (url), ex.: apagar_s3 \"s3://bucket/chave\"");
    }
    try {
      rt::s3_delete(a[0].s);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "copiar_s3") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto ||
        a[1].kind != ValueKind::Texto) {
      fail(call.span,
           "copiar_s3 espera (url_origem, url_destino), ex.: copiar_s3 "
           "\"s3://bucket/a\", \"s3://outro-bucket/b\"");
    }
    try {
      rt::s3_copiar(a[0].s, a[1].s);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "cabecalho_s3") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "cabecalho_s3 espera (url), ex.: cabecalho_s3 \"s3://bucket/chave\"");
    }
    try {
      Value m = Value::mapa();
      for (const auto& [k, v] : rt::s3_cabecalho(a[0].s)) {
        if (k == "content-length") {
          m.map->set(k, Value::inteiro(std::stoll(v)));
        } else {
          m.map->set(k, Value::texto(v));
        }
      }
      return m;
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "s3_iniciar_upload") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span,
           "s3_iniciar_upload espera (url), ex.: s3_iniciar_upload "
           "\"s3://bucket/chave\"");
    }
    try {
      return Value::texto(rt::s3_multipart_iniciar(a[0].s));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "s3_enviar_parte") {
    auto a = args();
    if (a.size() < 4 || a[0].kind != ValueKind::Texto ||
        a[1].kind != ValueKind::Texto || !a[2].is_number()) {
      fail(call.span,
           "s3_enviar_parte espera (url, upload_id, numero, dados), ex.: "
           "s3_enviar_parte \"s3://bucket/chave\", uid, 1, dados");
    }
    try {
      const std::string dados =
          a[3].kind == ValueKind::Texto ? a[3].s : rt::json_dump(a[3]);
      return Value::texto(rt::s3_multipart_parte(
          a[0].s, a[1].s, static_cast<int>(a[2].as_number()), dados));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "s3_concluir_upload") {
    auto a = args();
    if (a.size() < 3 || a[0].kind != ValueKind::Texto ||
        a[1].kind != ValueKind::Texto || a[2].kind != ValueKind::Lista) {
      fail(call.span,
           "s3_concluir_upload espera (url, upload_id, [[numero, etag], ...]), "
           "ex.: s3_concluir_upload \"s3://bucket/chave\", uid, [[1, e1], [2, "
           "e2]]");
    }
    std::vector<std::pair<int, std::string>> partes;
    for (const Value& item : *a[2].list) {
      if (item.kind != ValueKind::Lista || item.list->size() != 2 ||
          !(*item.list)[0].is_number() ||
          (*item.list)[1].kind != ValueKind::Texto) {
        fail(call.span,
             "s3_concluir_upload: cada parte deve ser [numero, etag]");
      }
      partes.emplace_back(static_cast<int>((*item.list)[0].as_number()),
                          (*item.list)[1].s);
    }
    try {
      return Value::texto(rt::s3_multipart_concluir(a[0].s, a[1].s, partes));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "s3_abortar_upload") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto ||
        a[1].kind != ValueKind::Texto) {
      fail(call.span,
           "s3_abortar_upload espera (url, upload_id), ex.: s3_abortar_upload "
           "\"s3://bucket/chave\", uid");
    }
    try {
      rt::s3_multipart_abortar(a[0].s, a[1].s);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (word_in(name, {"escrever"}) || (name.rfind("ler_", 0) == 0) ||
      (name.rfind("escrever_", 0) == 0)) {
    fail(call.span, "'" + name + "': conector/formato nao implementado (M5.2)",
         DiagCode::ConnectorNotImplemented);
  }
  if (name == "modelo") return eval_modelo_call(call, env);
  if (name == "experimento") return eval_experimento_call(call, env);

  // Direct tool call: `<ferramenta> arg: valor`.
  if (auto tit = entities_.find(name);
      tit != entities_.end() && tit->second->key == "ferramenta") {
    rt::ValueMap targs;
    for (const auto& a : call.args) {
      if (!a.name.empty()) targs.set(a.name, eval(*a.value, env));
    }
    if (call.block) {
      for (const auto& it : call.block->items) {
        if (it && it->kind == ItemKind::Field && it->value) targs.set(it->key, eval(*it->value, env));
      }
    }
    return run_tool(*tit->second, targs, call.span);
  }
  if (word_in(name, {"treinar", "incorporador"})) {
    fail(call.span, "execucao de '" + name + "' nao implementada", DiagCode::NotImplemented);
  }

  fail(call.span, "funcao '" + name + "' nao definida");
}

// ------------------------------------------------------------------ methods

Value Interpreter::eval_method(const std::string& method, Value receiver, const Expr& call,
                               Env& env) {
  if (receiver.kind == ValueKind::Texto) {
    if (auto e = entities_.find(receiver.s); e != entities_.end() && e->second->block) {
      const std::string& ekind = e->second->key;
      if (ekind == "indice") return eval_indice_method(receiver.s, method, call, env);
      if (ekind == "agente" && (method == "responder" || method == "perguntar")) {
        return eval_agente_responder(receiver.s, call, env);
      }
      if (ekind == "equipe" && (method == "executar" || method == "responder")) {
        return eval_equipe_call(receiver.s, call, env);
      }
      if (ekind == "experimento" && method == "prever") {
        const Expr* entrada = nullptr;
        for (const auto& a : call.args) {
          if (a.name.empty() && a.value) {
            entrada = a.value.get();
            break;
          }
        }
        if (!entrada) fail(call.span, "prever precisa de um mapa {atributo: valor}");
        return experimento_prever(receiver.s, eval(*entrada, env), call.span);
      }
      if (ekind == "ferramenta" && method == "executar") {
        rt::ValueMap targs;
        for (const auto& a : call.args) {
          if (!a.name.empty()) targs.set(a.name, eval(*a.value, env));
        }
        if (call.block) {
          for (const auto& it : call.block->items) {
            if (it && it->kind == ItemKind::Field && it->value) {
              targs.set(it->key, eval(*it->value, env));
            }
          }
        }
        return run_tool(*e->second, targs, call.span);
      }
    }
  }

  const bool is_table = receiver.kind == ValueKind::Tabela || receiver.kind == ValueKind::Lista;

  if (is_table && method == "filtrar") {
    if (call.args.empty()) fail(call.span, "filtrar espera uma condicao");
    rt::ValueList kept;
    for (const Value& row : (receiver.list ? *receiver.list : rt::ValueList{})) {
      Env inner;
      inner.parent = &env;
      inner.vars["linha"] = row;
      if (eval(*call.args[0].value, inner).truthy()) kept.push_back(row);
    }
    return Value::tabela(std::move(kept));
  }
  if (is_table && (method == "derivar" || method == "mapear")) {
    if (call.args.empty() || call.args[0].value->kind != ExprKind::MapLit) {
      fail(call.span, method + " espera um mapa { coluna: expressao }");
    }
    const Expr& spec = *call.args[0].value;
    rt::ValueList out;
    for (const Value& row : (receiver.list ? *receiver.list : rt::ValueList{})) {
      Env inner;
      inner.parent = &env;
      inner.vars["linha"] = row;
      Value nr = Value::mapa();
      if (row.map) {
        for (const auto& kv : row.map->items) nr.map->set(kv.first, kv.second);
      }
      for (const auto& en : spec.entries) nr.map->set(en.key, eval(*en.value, inner));
      out.push_back(std::move(nr));
    }
    return Value::tabela(std::move(out));
  }
  if (is_table && method == "agrupar_por") {
    if (call.args.size() < 2) fail(call.span, "agrupar_por espera (coluna, agregacoes)");
    Value key_v = eval(*call.args[0].value, env);
    std::string key_col = key_v.kind == ValueKind::Texto ? key_v.s : "";
    const Expr& aggs = *call.args[1].value;
    if (aggs.kind != ExprKind::MapLit) fail(call.span, "agregacoes devem ser um mapa");

    std::vector<std::string> order;
    std::unordered_map<std::string, rt::ValueList> groups;
    for (const Value& row : (receiver.list ? *receiver.list : rt::ValueList{})) {
      const Value* k = row.map ? row.map->find(key_col) : nullptr;
      std::string gk = k ? to_display(*k) : "";
      if (!groups.count(gk)) order.push_back(gk);
      groups[gk].push_back(row);
    }

    rt::ValueList out;
    for (const std::string& gk : order) {
      Value r = Value::mapa();
      r.map->set(key_col.empty() ? "grupo" : key_col, Value::texto(gk));
      for (const auto& en : aggs.entries) {
        const Expr& spec = *en.value;
        std::string fn = "contar";
        std::string col;
        if (spec.kind == ExprKind::Call && spec.lhs->kind == ExprKind::Name) {
          fn = spec.lhs->text;
          if (!spec.args.empty() && spec.args[0].value->kind == ExprKind::TextLit) {
            col = spec.args[0].value->text;
          }
        } else if (spec.kind == ExprKind::Name) {
          fn = spec.text;
        }
        const auto& rows = groups[gk];
        double acc = 0;
        if (fn == "contar") {
          acc = static_cast<double>(rows.size());
        } else {
          bool init = false;
          for (const Value& rr : rows) {
            const Value* c = rr.map ? rr.map->find(col) : nullptr;
            double n = c ? c->as_number() : 0;
            if (!init) {
              acc = n;
              init = true;
            } else if (fn == "somar" || fn == "media") {
              acc += n;
            } else if (fn == "min") {
              acc = std::min(acc, n);
            } else if (fn == "max") {
              acc = std::max(acc, n);
            }
          }
          if (fn == "media" && !rows.empty()) acc /= static_cast<double>(rows.size());
        }
        if (acc == static_cast<double>(static_cast<std::int64_t>(acc)) && fn != "media") {
          r.map->set(en.key, Value::inteiro(static_cast<std::int64_t>(acc)));
        } else {
          r.map->set(en.key, Value::decimal(acc));
        }
      }
      out.push_back(std::move(r));
    }
    return Value::tabela(std::move(out));
  }
  if (is_table && method == "selecionar") {
    auto cols = eval_args(call, env);
    rt::ValueList out;
    for (const Value& row : (receiver.list ? *receiver.list : rt::ValueList{})) {
      Value nr = Value::mapa();
      for (const Value& c : cols) {
        if (c.kind == ValueKind::Texto && row.map) {
          if (Value* f = row.map->find(c.s)) nr.map->set(c.s, *f);
        }
      }
      out.push_back(std::move(nr));
    }
    return Value::tabela(std::move(out));
  }
  if (is_table && method == "ordenar_por") {
    auto a = eval_args(call, env);
    rt::ValueMap kw = eval_kwargs(call, env);
    if (a.empty() || a[0].kind != ValueKind::Texto) fail(call.span, "ordenar_por espera uma coluna");
    const std::string col = a[0].s;
    const Value* desc = kw.find("desc");
    const bool descending = desc && desc->truthy();
    rt::ValueList out = receiver.list ? *receiver.list : rt::ValueList{};
    std::stable_sort(out.begin(), out.end(), [&](const Value& x, const Value& y) {
      const Value* xa = x.map ? x.map->find(col) : nullptr;
      const Value* ya = y.map ? y.map->find(col) : nullptr;
      bool less;
      if (xa && ya && xa->is_number() && ya->is_number()) {
        less = xa->as_number() < ya->as_number();
      } else {
        less = (xa ? to_display(*xa) : "") < (ya ? to_display(*ya) : "");
      }
      return descending ? !less : less;
    });
    return Value::tabela(std::move(out));
  }
  if (is_table && (method == "limite" || method == "primeiros")) {
    auto a = eval_args(call, env);
    std::int64_t n = a.empty() ? 0 : static_cast<std::int64_t>(a[0].as_number());
    rt::ValueList out;
    if (receiver.list) {
      for (const Value& row : *receiver.list) {
        if (static_cast<std::int64_t>(out.size()) >= n) break;
        out.push_back(row);
      }
    }
    return Value::tabela(std::move(out));
  }
  if (is_table && method == "distinto") {
    auto a = eval_args(call, env);
    const std::string col = (!a.empty() && a[0].kind == ValueKind::Texto) ? a[0].s : "";
    rt::ValueList out;
    std::vector<std::string> seen;
    for (const Value& row : (receiver.list ? *receiver.list : rt::ValueList{})) {
      std::string key;
      if (col.empty()) {
        key = to_display(row);
      } else {
        const Value* c = row.map ? row.map->find(col) : nullptr;
        key = c ? to_display(*c) : "";
      }
      if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
        seen.push_back(key);
        out.push_back(row);
      }
    }
    return Value::tabela(std::move(out));
  }

  if (receiver.kind == ValueKind::Tensor && receiver.tensor) {
    const rt::Tensor& t = *receiver.tensor;
    try {
      if (method == "forma") {
        rt::ValueList dims;
        for (std::int64_t d : t.shape) dims.push_back(Value::inteiro(d));
        return Value::lista(std::move(dims));
      }
      if (method == "dados") {
        rt::ValueList vals;
        for (float fv : t.data) vals.push_back(Value::decimal(fv));
        return Value::lista(std::move(vals));
      }
      if (method == "matmul" || method == "mais") {
        auto a = eval_args(call, env);
        if (a.empty() || a[0].kind != ValueKind::Tensor) {
          fail(call.span, method + " espera outro tensor");
        }
        return Value::tensor_de(method == "matmul" ? rt::matmul(t, *a[0].tensor)
                                                   : rt::add(t, *a[0].tensor));
      }
      if (method == "transposta") return Value::tensor_de(rt::transpose2d(t));
      if (method == "conv2d") {
        auto a = eval_args(call, env);
        if (a.empty() || a[0].kind != ValueKind::Tensor) {
          fail(call.span, "conv2d espera o nucleo (tensor [C_out, C_in, KH, KW])");
        }
        std::int64_t passo = 1;
        const rt::ValueMap kw = eval_kwargs(call, env);
        if (const Value* pv = kw.find("passo")) {
          passo = static_cast<std::int64_t>(pv->as_number());
        }
        return Value::tensor_de(rt::conv2d(t, *a[0].tensor, passo));
      }
      if (method == "norma_lote") {
        auto a = eval_args(call, env);
        if (a.size() < 2 || a[0].kind != ValueKind::Tensor || a[1].kind != ValueKind::Tensor) {
          fail(call.span, "norma_lote espera gama e beta (tensores [C] ou escalares)");
        }
        const rt::ValueMap kw = eval_kwargs(call, env);
        bool em_treino = false;
        float eps = 1e-5F;
        if (const Value* pv = kw.find("em_treino")) {
          em_treino = pv->kind == ValueKind::Logico && pv->b;
        }
        if (const Value* pv = kw.find("eps")) eps = static_cast<float>(pv->as_number());
        rt::Tensor media, var;
        if (a.size() >= 3) media = value_to_tensor(a[2], call.span);
        if (a.size() >= 4) var = value_to_tensor(a[3], call.span);
        if (!em_treino && a.size() < 4) {
          fail(call.span,
               "norma_lote na inferencia precisa de media e variancia (ou use em_treino: verdadeiro)");
        }
        return Value::tensor_de(
            rt::norma_lote(t, *a[0].tensor, *a[1].tensor, media, var, eps, em_treino));
      }
      if (method == "norma_camada") {
        return Value::tensor_de(rt::layer_norm_last(t));
      }
      if (method == "reformar") {
        // C2: um unico '_' e inferido pela contagem (espelha o solver).
        // Detecta na AST antes de avaliar (avaliar '_' puro falha de proposito).
        int ncuringa = 0;
        const ast::Expr* lista_ast = nullptr;
        if (!call.args.empty() && call.args[0].value &&
            call.args[0].value->kind == ExprKind::ListLit) {
          lista_ast = call.args[0].value.get();
          for (const auto& el : lista_ast->elems) {
            if (el && el->kind == ExprKind::Name && el->text == "_") ++ncuringa;
          }
        }
        if (ncuringa > 1) {
          fail(call.span, "reformar: no maximo um '_' (inferido pela contagem)");
        }
        std::vector<std::int64_t> shape;
        if (ncuringa == 1 && lista_ast) {
          std::int64_t conhecidos = 1;
          for (const auto& el : lista_ast->elems) {
            if (el && el->kind == ExprKind::Name && el->text == "_") continue;
            conhecidos *= static_cast<std::int64_t>(eval(*el, env).as_number());
          }
          const std::int64_t total = static_cast<std::int64_t>(t.size());
          if (conhecidos <= 0 || total % conhecidos != 0) {
            fail(call.span, "reformar: '_' nao deduzivel (contagem incompatível)");
          }
          for (const auto& el : lista_ast->elems) {
            if (el && el->kind == ExprKind::Name && el->text == "_") {
              shape.push_back(total / conhecidos);
            } else {
              shape.push_back(static_cast<std::int64_t>(eval(*el, env).as_number()));
            }
          }
        } else {
          auto a = eval_args(call, env);
          if (!a.empty() && a[0].kind == ValueKind::Lista && a[0].list) {
            for (const Value& e : *a[0].list) {
              shape.push_back(static_cast<std::int64_t>(e.as_number()));
            }
          }
        }
        return Value::tensor_de(rt::reshape(t, shape));
      }
      if (word_in(method, {"relu", "gelu", "silu", "sigmoide", "tanh"})) {
        return Value::tensor_de(rt::apply_unary(t, method));
      }
      if (method == "softmax") return Value::tensor_de(rt::softmax_last(t));
      if (method == "soma") return Value::decimal(rt::sum_all(t));
      if (method == "media") return Value::decimal(rt::mean_all(t));
      if (method == "argmax") return Value::inteiro(rt::argmax_last(t));
      if (method == "item") {
        if (t.size() != 1) fail(call.span, "item espera um tensor de 1 elemento");
        return Value::decimal(t.data[0]);
      }
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    fail(call.span, "tensor nao tem o metodo '" + method + "'");
  }

  if (receiver.kind == ValueKind::Texto) {
    if (method == "maiusculas") {
      std::string s = receiver.s;
      for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      return Value::texto(s);
    }
    if (method == "minusculas") {
      std::string s = receiver.s;
      for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return Value::texto(s);
    }
  }

  fail(call.span, std::string("'") + receiver.type_name() + "' nao tem o metodo '" + method + "'");
}

}  // namespace tilt
