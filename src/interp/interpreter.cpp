#include "interp/interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <ostream>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "runtime/checkpoint.hpp"
#include "runtime/cluster.hpp"
#include "runtime/avro.hpp"
#include "runtime/chroma.hpp"
#include "runtime/clickhouse.hpp"
#include "runtime/compat.hpp"
#include "runtime/delta.hpp"
#include "runtime/duckdb.hpp"
#include "runtime/elasticsearch.hpp"
#include "runtime/gguf.hpp"
#include "runtime/gpu_runtime.hpp"
#include "runtime/http_client.hpp"
#include "runtime/http_server.hpp"
#include "runtime/iceberg.hpp"
#include "runtime/json.hpp"
#include "runtime/kafka.hpp"
#include "runtime/leader.hpp"
#include "runtime/livy.hpp"
#include "runtime/llm.hpp"
#include "runtime/mongo.hpp"
#include "runtime/mysql.hpp"
#include "runtime/onnx.hpp"
#include "runtime/parquet.hpp"
#include "runtime/pgvector.hpp"
#include "runtime/pinecone.hpp"
#include "runtime/postgres.hpp"
#include "runtime/qdrant.hpp"
#include "runtime/redis.hpp"
#include "runtime/s3.hpp"
#include "runtime/safetensors.hpp"
#include "runtime/sqlite.hpp"
#include "runtime/vectorstore.hpp"
#include "runtime/weaviate.hpp"
#include "semantic/checker.hpp"
#include "vm/compiler.hpp"
#include "vm/vm.hpp"

namespace tilt {

// Estado por thread do pool de rotas (serve()): cada worker executa uma
// rota de ponta a ponta na sua thread, entao a resposta corrente, o flag de
// 'se' e o dispositivo ativo nunca sao compartilhados entre requisicoes.
thread_local RouteResponse* route_resp_ = nullptr;  // non-null only while handling a request
thread_local const std::unordered_set<std::string>* route_tool_allowlist_ = nullptr;
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

namespace {

struct MlflowTarget {
  std::string base;
  std::string experiment;
};

std::string mlflow_url_encode(const std::string& text) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0f]);
    }
  }
  return out;
}

MlflowTarget mlflow_target(const std::string& uri) {
  constexpr std::string_view prefix = "mlflow://";
  if (uri.rfind(prefix, 0) != 0) {
    throw std::runtime_error("registrar_em espera mlflow://host/experimento");
  }
  const std::string resto = uri.substr(prefix.size());
  const std::size_t barra = resto.find('/');
  if (barra == std::string::npos || barra == 0 || barra + 1 >= resto.size()) {
    throw std::runtime_error("registrar_em espera mlflow://host/experimento");
  }
  MlflowTarget out;
  out.base = "http://" + resto.substr(0, barra);
  out.experiment = resto.substr(barra + 1);
  return out;
}

std::vector<std::pair<std::string, std::string>> mlflow_headers() {
  std::vector<std::pair<std::string, std::string>> headers;
  if (const char* token = std::getenv("MLFLOW_TRACKING_TOKEN"); token && *token) {
    headers.emplace_back("Authorization", std::string("Bearer ") + token);
  }
  if (const char* workspace = std::getenv("MLFLOW_WORKSPACE"); workspace && *workspace) {
    headers.emplace_back("X-MLFLOW-WORKSPACE", workspace);
  }
  return headers;
}

Value mlflow_post(const MlflowTarget& target, const std::string& endpoint, const Value& body) {
  return rt::http_post_json(target.base + "/api/2.0/mlflow/" + endpoint, body, mlflow_headers());
}

std::string mlflow_value_text(const Value& value) {
  if (value.kind == ValueKind::Texto) return value.s;
  if (value.kind == ValueKind::Inteiro) return std::to_string(value.i);
  if (value.kind == ValueKind::Decimal) return std::to_string(value.d);
  return {};
}

std::string mlflow_id(const Value& reply, const std::string& top_key,
                      const std::string& nested_key) {
  if (reply.kind != ValueKind::Mapa || !reply.map) return {};
  const Value* value = reply.map->find(top_key);
  if (value && !nested_key.empty() && value->kind == ValueKind::Mapa && value->map) {
    value = value->map->find(nested_key);
  }
  return value ? mlflow_value_text(*value) : std::string();
}

std::string mlflow_run_id(const Value& reply) {
  if (reply.kind != ValueKind::Mapa || !reply.map) return {};
  const Value* run = reply.map->find("run");
  if (!run || run->kind != ValueKind::Mapa || !run->map) return {};
  const Value* info = run->map->find("info");
  if (!info || info->kind != ValueKind::Mapa || !info->map) return {};
  const Value* id = info->map->find("run_id");
  return id ? mlflow_value_text(*id) : std::string();
}

bool mlflow_metric(const std::string& line, std::string& key, double& value) {
  const std::size_t colon = line.find(':');
  if (colon == std::string::npos) return false;
  key = line.substr(0, colon);
  while (!key.empty() && key.front() == ' ') key.erase(key.begin());
  while (!key.empty() && key.back() == ' ') key.pop_back();
  std::string number = line.substr(colon + 1);
  while (!number.empty() && number.front() == ' ') number.erase(number.begin());
  const std::size_t spread = number.find(" +-");
  if (spread != std::string::npos) number.resize(spread);
  char* end = nullptr;
  value = std::strtod(number.c_str(), &end);
  if (end == number.c_str()) return false;
  while (*end == ' ') ++end;
  if (*end != 0) return false;
  for (char& c : key) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!(std::isalnum(u) || c == '_' || c == '-' || c == '.')) c = '_';
  }
  return !key.empty();
}

std::string mlflow_registrar_experimento(const std::string& uri, const std::string& name,
                                         const std::string& model, std::size_t total,
                                         std::int64_t seed,
                                         const std::vector<std::string>& report) {
  const MlflowTarget target = mlflow_target(uri);
  const auto headers = mlflow_headers();
  Value experiment;
  const std::string get_url = target.base +
                              "/api/2.0/mlflow/experiments/get-by-name?experiment_name=" +
                              mlflow_url_encode(target.experiment);
  const rt::HttpClientResponse found = rt::http_request("GET", get_url, headers);
  if (found.error.empty() && found.status >= 200 && found.status < 300) {
    experiment = rt::json_parse(found.body);
  } else if (found.status == 404) {
    Value request = Value::mapa();
    request.map->set("name", Value::texto(target.experiment));
    experiment = mlflow_post(target, "experiments/create", request);
  } else {
    throw std::runtime_error("MLflow: nao foi possivel localizar o experimento (" +
                             std::to_string(found.status) + ")");
  }
  std::string experiment_id = mlflow_id(experiment, "experiment_id", "");
  if (experiment_id.empty()) experiment_id = mlflow_id(experiment, "experiment", "experiment_id");
  if (experiment_id.empty()) {
    throw std::runtime_error("MLflow: resposta sem experiment_id");
  }

  const auto agora = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  Value run_request = Value::mapa();
  run_request.map->set("experiment_id", Value::texto(experiment_id));
  run_request.map->set("run_name", Value::texto(name));
  run_request.map->set("start_time", Value::inteiro(agora));
  Value tags = Value::lista();
  Value tag = Value::mapa();
  tag.map->set("key", Value::texto("tilt.modelo"));
  tag.map->set("value", Value::texto(model));
  tags.list->push_back(std::move(tag));
  run_request.map->set("tags", std::move(tags));
  const Value run = mlflow_post(target, "runs/create", run_request);
  const std::string run_id = mlflow_run_id(run);
  if (run_id.empty()) throw std::runtime_error("MLflow: resposta sem run_id");

  Value batch = Value::mapa();
  batch.map->set("run_id", Value::texto(run_id));
  Value metrics = Value::lista();
  for (const std::string& line : report) {
    std::string key;
    double value = 0.0;
    if (!mlflow_metric(line, key, value)) continue;
    Value metric = Value::mapa();
    metric.map->set("key", Value::texto(key));
    metric.map->set("value", Value::decimal(value));
    metric.map->set("timestamp", Value::inteiro(agora));
    metric.map->set("step", Value::inteiro(0));
    metrics.list->push_back(std::move(metric));
  }
  batch.map->set("metrics", std::move(metrics));
  Value params = Value::lista();
  auto add_param = [&params](const std::string& key, const std::string& value) {
    Value param = Value::mapa();
    param.map->set("key", Value::texto(key));
    param.map->set("value", Value::texto(value));
    params.list->push_back(std::move(param));
  };
  add_param("tilt.modelo", model);
  add_param("tilt.linhas", std::to_string(total));
  add_param("tilt.semente", std::to_string(seed));
  batch.map->set("params", std::move(params));
  batch.map->set("tags", Value::lista());
  mlflow_post(target, "runs/log-batch", batch);

  Value update = Value::mapa();
  update.map->set("run_id", Value::texto(run_id));
  update.map->set("status", Value::texto("FINISHED"));
  update.map->set("end_time", Value::inteiro(agora));
  mlflow_post(target, "runs/update", update);
  return run_id;
}

}  // namespace

// ------------------------------------------------------------------ setup

std::mutex Interpreter::zumbis_mu_;
std::vector<std::shared_ptr<Interpreter::Env>> Interpreter::zumbis_;

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
    } else if (kw == "treino" || kw == "busca") {
      // handled by the dedicated training loop; must not shadow `modelo <name>`
    } else if (!name.empty()) {
      entities_[name] = item.get();
    }
  }
  tiltc_load();
}

// ------------------------------------------------------------------ tiltc

bool tiltc_off() {
  const char* v = std::getenv("TILT_VM_NOCACHE");
  return v && std::string(v) == "1";
}

void Interpreter::tiltc_note(const char* what) {
  if (std::getenv("TILT_VM_DEBUG")) std::cerr << "[tiltc " << what << "]\n";
}

void Interpreter::tiltc_load() {
  if (tiltc_loaded_ || tiltc_off()) return;
  tiltc_loaded_ = true;
  const SourceFile* src = diag_.source();
  if (!src || src->path().empty()) return;
  tiltc_path_ = vm::tiltc_path_for(src->path());
  vm::CachedProgram prog;
  if (vm::tiltc_load(tiltc_path_, std::string(src->text()), prog)) {
    tiltc_prog_ = std::move(prog);
    tiltc_note("hit");
  } else {
    tiltc_note("miss");
  }
}

void Interpreter::tiltc_flush() {
  if (!tiltc_dirty_ || tiltc_path_.empty() || tiltc_off()) return;
  tiltc_dirty_ = false;
  const SourceFile* src = diag_.source();
  if (!src) return;
  if (vm::tiltc_save(tiltc_path_, std::string(src->text()), tiltc_prog_)) {
    tiltc_note("save");
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

bool parse_duracao_com_zero(const std::string& s, std::time_t& out) {
  if (s == "0s" || s == "0min" || s == "0h") {
    out = 0;
    return true;
  }
  return parse_duracao(s, out);
}

Value janela_literal(const Expr& e, Span span) {
  switch (e.kind) {
    case ExprKind::IntLit:
      return Value::inteiro(std::strtoll(e.text.c_str(), nullptr, 10));
    case ExprKind::DecimalLit:
      return Value::decimal(std::strtod(e.text.c_str(), nullptr));
    case ExprKind::TextLit:
      return Value::texto(e.text);
    case ExprKind::NullLit:
      return Value::nulo();
    default:
      throw std::runtime_error("valor de cursor/backfill deve ser literal (linha " +
                               std::to_string(span.line) + ")");
  }
}

int comparar_janela_cursor(const Value& a, const Value& b, Span span) {
  if (a.is_number() && b.is_number()) {
    const double x = a.as_number();
    const double y = b.as_number();
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  if (a.kind == ValueKind::Texto && b.kind == ValueKind::Texto) {
    return a.s < b.s ? -1 : (a.s > b.s ? 1 : 0);
  }
  throw std::runtime_error(
      "cursor/backfill: valores devem ter o mesmo tipo "
      "(inteiro/decimal ou texto) na linha " +
      std::to_string(span.line));
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

// Especificacao completa de `ao_falhar: repetir N[, espera: "5s"][, backoff: 2]`:
// tentativas extras, espera base entre elas, fator multiplicativo (1 = fixo)
// e jitter adicional aleatorio (0..jitter).
// Valida duracao e fator; joga runtime_error com mensagem acionavel.
struct RetrySpec {
  int tentativas = 0;
  std::time_t espera = 0;
  long backoff = 1;
  std::time_t jitter = 0;
};

RetrySpec retry_spec(const Item& pipeline) {
  RetrySpec spec;
  spec.tentativas = retry_count(pipeline);
  if (spec.tentativas <= 0) return spec;
  const Item* f = find_field(*pipeline.block, "ao_falhar");
  const Expr* v = f && f->value ? f->value.get() : nullptr;
  if (!v || v->kind != ExprKind::Call) return spec;
  for (const auto& a : v->args) {
    if (!a.value || a.name.empty()) continue;
    if (a.name == "espera") {
      if (a.value->kind != ExprKind::TextLit || !parse_duracao(a.value->text, spec.espera)) {
        throw std::runtime_error(
            "ao_falhar: 'espera' deve ser duracao (\"5s\", \"2min\", \"1h\")");
      }
    } else if (a.name == "backoff") {
      if (a.value->kind != ExprKind::IntLit ||
          (spec.backoff = std::strtol(a.value->text.c_str(), nullptr, 10)) < 1) {
        throw std::runtime_error("ao_falhar: 'backoff' deve ser inteiro >= 1 (1 = espera fixa)");
      }
    } else if (a.name == "jitter") {
      if (a.value->kind != ExprKind::TextLit ||
          !parse_duracao_com_zero(a.value->text, spec.jitter)) {
        throw std::runtime_error(
            "ao_falhar: 'jitter' deve ser duracao (\"0s\", \"5s\", \"2min\", \"1h\")");
      }
    }
  }
  return spec;
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
      if (item && item->kind == ItemKind::Decl && item->key == "busca") {
        run_busca(*item);
        did_something = true;
      }
      if (item && item->kind == ItemKind::Decl && item->key == "experimento") {
        run_experimento(*item);
        did_something = true;
      }
      if (item && item->kind == ItemKind::Decl && item->key == "avaliacao") {
        run_avaliacao(*item);
        did_something = true;
      }
    }

    if (!pipelines_.empty()) {
      for (const Item* p : pipelines_) run_pipeline(*p);
      tiltc_flush();
    } else if (auto it = functions_.find("principal"); it != functions_.end()) {
      call_function(*it->second, {}, it->second->span);
      tiltc_flush();
    } else if (!did_something) {
      out_ << "nada para executar: nenhum 'pipeline', 'treino', 'experimento', 'avaliacao' nem 'funcao principal'\n";
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

Value Interpreter::vm_call_hook(const std::string& name, std::vector<Value>& args, bool* handled) {
  // `ler_csv "arq"` compilado pela VM: tabela do runtime, como no interpretador.
  if (name == "ler_csv") {
    if (args.size() == 1 && args[0].kind == ValueKind::Texto) {
      *handled = true;
      return read_csv_file(args[0].s, Span{});
    }
    *handled = false;
    return Value::nulo();
  }
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
  return call_function(*f->second, std::move(args), Span{}, scope);
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
      if (item && item->kind == ItemKind::Decl && item->key == "busca") {
        run_busca(*item);
        did_something = true;
      }
      if (item && item->kind == ItemKind::Decl && item->key == "experimento") {
        run_experimento(*item);
        did_something = true;
      }
      if (item && item->kind == ItemKind::Decl && item->key == "avaliacao") {
        run_avaliacao(*item);
        did_something = true;
      }
    }

    if (!pipelines_.empty()) {
      std::unordered_set<std::string> names;
      for (const auto& kv : functions_) names.insert(kv.first);
      for (const Item* p : pipelines_) {
        // Pipeline no subconjunto -> bytecode VM (.tiltc, senao compila);
        // fora dele -> arvore.
        std::shared_ptr<vm::Chunk> chunk;
        const std::string tkey = "pipeline " + decl_name(*p);
        if (auto it = tiltc_prog_.entries.find(tkey);
            it != tiltc_prog_.entries.end() && it->second.is_pipeline) {
          chunk = std::make_shared<vm::Chunk>(it->second.chunk);
          tiltc_note("hit");
        } else {
          try {
            chunk = std::make_shared<vm::Chunk>(vm::compile_pipeline(*p, names));
          } catch (const vm::NotCompilable&) {
            chunk = nullptr;
          }
          if (chunk) {
            vm::CachedChunk cc;
            cc.is_pipeline = true;
            cc.chunk = *chunk;
            tiltc_prog_.entries[tkey] = std::move(cc);
            tiltc_dirty_ = true;
          }
        }
        if (!chunk) {
          run_pipeline(*p);
          continue;
        }
        out_ << "== pipeline " << decl_name(*p) << " ==\n";
        if (jit_mode_) {
          vm::Jit jit(out_);
          std::string why;
          if (jit.can_compile(*chunk, &why)) {
            if (std::getenv("TILT_JIT_DEBUG"))
              std::cerr << "[jit native] pipeline " << decl_name(*p) << "\n";
            try {
              jit.run(*chunk, {});
              continue;
            } catch (const std::exception& e) {
              fail(p->span, std::string("JIT: ") + e.what());
              continue;
            }
          } else if (std::getenv("TILT_JIT_DEBUG")) {
            std::cerr << "[jit fallback] pipeline " << decl_name(*p) << ": " << why << "\n";
          }
        }
        vm::Vm machine(out_, [this](const std::string& name, std::vector<Value>& a, bool* handled) {
          return vm_call_hook(name, a, handled);
        });
        try {
          machine.run(*chunk, {});
        } catch (const std::exception& e) {
          fail(p->span, std::string("VM: ") + e.what());
        }
      }
      tiltc_flush();
    } else if (auto it = functions_.find("principal"); it != functions_.end()) {
      call_function(*it->second, {}, it->second->span);
      tiltc_flush();
    } else if (!did_something) {
      out_ << "nada para executar: nenhum 'pipeline', 'treino', 'experimento', 'avaliacao' nem 'funcao principal'\n";
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

    // treinos, experimentos, avaliacoes e funcao principal nao entram no loop; rodam uma vez antes
    for (const auto& item : program_.items) {
      if (item && item->kind == ItemKind::Decl && item->key == "treino") run_treino(*item);
      if (item && item->kind == ItemKind::Decl && item->key == "busca") run_busca(*item);
      if (item && item->kind == ItemKind::Decl && item->key == "experimento") run_experimento(*item);
      if (item && item->kind == ItemKind::Decl && item->key == "avaliacao") run_avaliacao(*item);
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
  const auto inicio_pipeline = std::chrono::steady_clock::now();
  const bool log_pipeline_json = [] {
    const char* valor = std::getenv("TILT_PIPELINE_LOG_JSON");
    return valor && std::string(valor) == "1";
  }();
  auto json_escape_pipeline = [](const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    for (const char c : value) {
      switch (c) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += c; break;
      }
    }
    return escaped;
  };
  auto log_pipeline = [&](const char* status, int tentativas, const std::string& erro) {
    if (!log_pipeline_json) return;
    const auto decorrido = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - inicio_pipeline);
    out_ << "{\"evento\":\"pipeline\",\"nome\":\""
         << json_escape_pipeline(decl_name(pipeline)) << "\",\"status\":\"" << status
         << "\",\"tentativas\":" << tentativas << ",\"duracao_us\":"
         << std::max<long long>(1, decorrido.count());
    if (!erro.empty()) {
      out_ << ",\"erro\":\"" << json_escape_pipeline(erro) << "\"";
    }
    out_ << "}\n";
  };
  log_pipeline("iniciado", 0, "");
  std::time_t sla = 0;
  if (const Item* sf = find_field(*pipeline.block, "sla")) {
    if (!sf->value || sf->value->kind != ExprKind::TextLit ||
        !parse_duracao(sf->value->text, sla)) {
      fail(sf->span, "sla: espera duracao (\"30s\", \"5min\", \"1h\")");
    }
  }
  auto alerta_sla = [&]() {
    if (sla <= 0) return;
    const auto decorrido = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - inicio_pipeline);
    const auto limite = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::seconds(sla));
    if (decorrido > limite) {
      out_ << "[sla] alerta: pipeline " << decl_name(pipeline) << " excedeu " << sla
           << "s (decorrido " << decorrido.count() << "ms)\n";
    }
  };

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
    log_pipeline("sem_passos", 0, "");
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
      log_pipeline("janela_pendente", 0, "");
      return;
    }
  }

  RetrySpec retry;
  try {
    retry = retry_spec(pipeline);
  } catch (const std::exception& e) {
    fail(pipeline.span, std::string(e.what()));
  }
  // Timeout por passo (`tempo_limite: "30s"`): cada item de `passos:` com
  // deadline propria; sem o campo, execucao direta sem thread.
  PrazoPasso prazo;
  bool com_prazo = false;
  if (const Item* tl = find_field(*pipeline.block, "tempo_limite")) {
    if (!tl->value || tl->value->kind != ExprKind::TextLit ||
        !parse_duracao(tl->value->text, prazo.segundos)) {
      fail(tl->span, "tempo_limite: espera duracao (\"30s\", \"5min\", \"1h\")");
    }
    com_prazo = true;
  }
  const int retries = retry.tentativas;
  // Quarentena (`quarentena: "arq.jsonl"`): linhas do `para cada` que
  // falham sao desviadas para o arquivo em vez de abortar.
  std::shared_ptr<QuarentenaState> qst;
  if (const Item* qf = find_field(*pipeline.block, "quarentena")) {
    if (!qf->value || qf->value->kind != ExprKind::TextLit || qf->value->text.empty()) {
      fail(qf->span, "quarentena: espera um caminho (\"quarentena.jsonl\")");
    }
    qst = std::make_shared<QuarentenaState>();
    qst->caminho = qf->value->text;
  }
  auto executar_callback = [&](const char* nome, const std::string& erro,
                               int tentativas_executadas) {
    const Item* callback = find_field(*pipeline.block, nome);
    if (!callback) return;
    if (!callback->block) {
      fail(callback->span, std::string(nome) + ": espera um bloco de passos");
    }
    Env env;
    env.parent = &root_;
    if (janela) env.vars["linhas"] = Value::lista(janela_lotes);
    env.vars["pipeline"] = Value::texto(decl_name(pipeline));
    env.vars["tentativas"] = Value::inteiro(tentativas_executadas);
    env.vars["status"] = Value::texto(erro.empty() ? "sucesso" : "falha");
    if (!erro.empty()) env.vars["erro"] = Value::texto(erro);
    env.quarentena = qst;
    exec_block(*callback->block, env);
  };
  std::exception_ptr falha_final;
  std::string mensagem_falha;
  int tentativas_executadas = 0;
  for (int attempt = 0; attempt <= retries; ++attempt) {
    tentativas_executadas = attempt + 1;
    try {
      if (com_prazo) {
        auto senv = std::make_shared<Env>();
        senv->parent = &root_;
        if (janela) senv->vars["linhas"] = Value::lista(janela_lotes);
        senv->quarentena = qst;
        prazo.dono = senv;
        exec_block(*passos->block, *senv, &prazo);
      } else {
        Env env;
        env.parent = &root_;
        if (janela) env.vars["linhas"] = Value::lista(janela_lotes);
        env.quarentena = qst;
        exec_block(*passos->block, env);
      }
      if (qst && qst->n > 0) {
        out_ << "quarentena: " << qst->n << " linha(s) desviadas para " << qst->caminho << "\n";
      }
      break;
    } catch (const RuntimeAbort& a) {
      if (attempt >= retries) {
        falha_final = std::current_exception();
        mensagem_falha = a.message;
        break;
      }
      // Espera com backoff: espera * backoff^attempt, teto de 5min.
      long espera = retry.espera;
      for (int k = 0; k < attempt; ++k) {
        espera *= retry.backoff;
        if (espera > 300) {
          espera = 300;
          break;
        }
      }
      if (retry.jitter > 0) {
        std::random_device rd;
        std::mt19937 gerador(rd());
        std::uniform_int_distribution<long> distribuicao(0, static_cast<long>(retry.jitter));
        espera += distribuicao(gerador);
      }
      espera = std::min<long>(espera, 300);
      if (espera > 0) {
        out_ << "[retry] passo falhou (" << a.message << "); nova tentativa em " << espera
             << "s" << (retry.jitter > 0 ? " (com jitter)" : "") << " ("
             << (attempt + 2) << "/" << (retries + 1) << ")\n";
        std::this_thread::sleep_for(std::chrono::seconds(espera));
      } else {
        out_ << "[retry] passo falhou (" << a.message << "); tentativa " << (attempt + 2) << "/"
             << (retries + 1) << "\n";
      }
    }
  }
  if (falha_final) {
    if (find_field(*pipeline.block, "on_failure")) {
      try {
        executar_callback("on_failure", mensagem_falha, tentativas_executadas);
      } catch (const std::exception& callback_error) {
        out_ << "[callback] on_failure falhou: " << callback_error.what() << "\n";
      }
    }
    log_pipeline("falha", tentativas_executadas, mensagem_falha);
    alerta_sla();
    std::rethrow_exception(falha_final);
  }
  executar_callback("on_success", "", tentativas_executadas);
  log_pipeline("sucesso", tentativas_executadas, "");
  alerta_sla();
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

  std::string cursor_col;
  if (const Item* desde = find_field(*pipeline.block, "desde")) {
    if (!desde->value ||
        (desde->value->kind != ExprKind::Name && desde->value->kind != ExprKind::TextLit) ||
        desde->value->text.empty()) {
      fail(desde->span, "desde: espera o nome da coluna de cursor (ex.: desde: criado_em)");
    }
    cursor_col = desde->value->text;
    if (fonte.empty()) fail(desde->span, "desde: exige 'entrada:' (fonte)");
  }

  bool tem_backfill = false;
  Value backfill_desde;
  Value backfill_ate;
  if (const Item* bf = find_field(*pipeline.block, "backfill")) {
    if (!bf->value || bf->value->kind != ExprKind::MapLit) {
      fail(bf->span, "backfill: espera { desde: valor, ate: valor }");
    }
    const Expr* from = nullptr;
    const Expr* to = nullptr;
    for (const auto& entry : bf->value->entries) {
      if (entry.key == "desde") from = entry.value.get();
      if (entry.key == "ate") to = entry.value.get();
    }
    if (!from || !to) fail(bf->span, "backfill: exige os limites 'desde' e 'ate'");
    try {
      backfill_desde = janela_literal(*from, from->span);
      backfill_ate = janela_literal(*to, to->span);
      if (backfill_desde.kind == ValueKind::Nulo || backfill_ate.kind == ValueKind::Nulo ||
          comparar_janela_cursor(backfill_desde, backfill_ate, bf->span) > 0) {
        fail(bf->span, "backfill: 'desde' deve ser menor ou igual a 'ate'");
      }
    } catch (const std::exception& e) {
      fail(bf->span, e.what());
    }
    tem_backfill = true;
    if (cursor_col.empty()) {
      fail(bf->span, "backfill: exige 'desde: coluna' para definir o cursor");
    }
  }

  const std::string pipe_name = decl_name(pipeline);
  WindowState& st = window_states_[pipe_name];
  st.cursor_active = !cursor_col.empty();
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
    // Le a fonte inteira a cada tick. Cursor e backfill filtram por valor;
    // o modo sem cursor preserva o offset posicional legado.
    Value data = read_fonte(fonte, entrada->span);
    if (data.list) {
      if (st.cursor_active) {
        if (tem_backfill && st.backfill_loaded) {
          // Backfill e uma leitura delimitada de uma vez; o buffer continua
          // podendo fechar em ticks seguintes quando a janela e de contagem.
        } else {
          Value maior_lido = st.cursor_observed_valid
                                 ? st.cursor_observed
                                 : (st.cursor_loaded ? st.cursor_watermark : Value::nulo());
          bool tem_maior = st.cursor_observed_valid || st.cursor_loaded;
          for (const Value& row : *data.list) {
            if (row.kind != ValueKind::Mapa || !row.map) {
              fail(entrada->span, "cursor '" + cursor_col + "' exige linhas em mapas");
            }
            const Value* cursor = row.map->find(cursor_col);
            if (!cursor || cursor->kind == ValueKind::Nulo) {
              fail(entrada->span, "cursor '" + cursor_col + "' ausente ou nulo na fonte");
            }
            bool inclui = true;
            if (tem_backfill) {
              inclui = comparar_janela_cursor(*cursor, backfill_desde, entrada->span) >= 0 &&
                       comparar_janela_cursor(*cursor, backfill_ate, entrada->span) <= 0;
            } else if (tem_maior) {
              inclui = comparar_janela_cursor(*cursor, maior_lido, entrada->span) > 0;
            }
            if (!inclui) continue;
            st.buffer.push_back(row);
            if (!tem_maior || comparar_janela_cursor(*cursor, maior_lido, entrada->span) > 0) {
              maior_lido = *cursor;
              tem_maior = true;
            }
          }
          if (tem_maior) {
            st.cursor_observed = maior_lido;
            st.cursor_observed_valid = true;
          }
          if (tem_backfill) st.backfill_loaded = true;
        }
      } else {
        if (st.offset > data.list->size()) st.offset = data.list->size();  // fonte encolheu
        for (std::size_t i = st.offset; i < data.list->size(); ++i) {
          st.buffer.push_back((*data.list)[i]);
        }
        st.offset = data.list->size();
      }
    }
    // Grava so quando o offset avanca (nada consumido = sem arquivo novo).
    // O buffer pendente tambem precisa ir para o checkpoint: se o processo
    // parar antes de fechar a janela, essas linhas ja foram contabilizadas no
    // offset e seriam perdidas no proximo disparo.
    if (!st.cursor_active && !offset_file.empty() && st.offset > st.persisted_offset) {
      janela_offset_save(st, pipe_name, offset_file, spec.kind != JanelaSpec::Contagem);
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
      if (st.cursor_active) {
        for (long i = 0; i < consome; ++i) {
          const Value& row = batch[static_cast<std::size_t>(i)];
          const Value* cursor = row.map ? row.map->find(cursor_col) : nullptr;
          if (cursor && (!st.cursor_loaded ||
                         comparar_janela_cursor(*cursor, st.cursor_watermark, janela.span) > 0)) {
            st.cursor_watermark = *cursor;
            st.cursor_loaded = true;
          }
        }
      }
      st.buffer.erase(st.buffer.begin(), st.buffer.begin() + consome);
      roda = true;
    }
  } else {
    const bool decorreu = !st.ran_once || (now - st.last_run) >= spec.dur;
    if (fonte.empty()) {
      roda = decorreu;  // throttle: no maximo 1 execucao por duracao
    } else if (decorreu && !st.buffer.empty()) {
      batch = st.buffer;  // janela de tempo: entrega tudo que acumulou e zera
      if (st.cursor_active) {
        for (const Value& row : batch) {
          const Value* cursor = row.map ? row.map->find(cursor_col) : nullptr;
          if (cursor && (!st.cursor_loaded ||
                         comparar_janela_cursor(*cursor, st.cursor_watermark, janela.span) > 0)) {
            st.cursor_watermark = *cursor;
            st.cursor_loaded = true;
          }
        }
      }
      st.buffer.clear();
      roda = true;
    }
  }
  if (roda) {
    st.ran_once = true;
    st.last_run = now;
    // Persiste o relogio para janelas de tempo/throttle entre replicas e
    // confirma a retirada do buffer de contagem (inclusive sobreposicao).
    // Contagem sem pendencias continua no formato legado numero-puro.
    if (!offset_file.empty()) {
      janela_offset_save(st, pipe_name, offset_file, spec.kind != JanelaSpec::Contagem);
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
  if (tipo != "csv" && tipo != "json" && tipo != "parquet") return "";
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
    // {offset, last_run, buffer} — permite retomar janela parcial, throttle e
    // tempo entre replicas.
    if (v->is_number()) {
      st.offset = static_cast<std::size_t>(v->as_number());
      st.persisted_offset = st.offset;
    } else if (v->kind == ValueKind::Mapa && v->map) {
      if (const Value* o = v->map->find("offset"); o && o->is_number()) {
        st.offset = static_cast<std::size_t>(o->as_number());
        st.persisted_offset = st.offset;
      }
      if (const Value* cursor = v->map->find("cursor");
          cursor && cursor->kind != ValueKind::Nulo) {
        st.cursor_watermark = *cursor;
        st.cursor_loaded = true;
        st.cursor_observed = *cursor;
        st.cursor_observed_valid = true;
      }
      if (const Value* lr = v->map->find("last_run"); lr && lr->is_number()) {
        st.last_run = static_cast<std::time_t>(lr->as_number());
        st.ran_once = true;
      }
      if (const Value* b = v->map->find("buffer"); b && b->kind == ValueKind::Lista && b->list) {
        st.buffer.assign(b->list->begin(), b->list->end());
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
  if (st.cursor_active || (com_relogio && st.ran_once) || !st.buffer.empty()) {
    Value entry = Value::mapa();
    entry.map->set("offset", Value::inteiro(static_cast<std::int64_t>(st.offset)));
    if (st.cursor_active && st.cursor_loaded) entry.map->set("cursor", st.cursor_watermark);
    if (com_relogio && st.ran_once) {
      entry.map->set("last_run", Value::inteiro(static_cast<std::int64_t>(st.last_run)));
    }
    if (!st.buffer.empty()) entry.map->set("buffer", Value::lista(st.buffer));
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
    std::string sql = c->value->text;
    std::vector<rt::SqlParam> pushdown_params;
    auto is_sql_identifier = [](const std::string& value) {
      if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_')) {
        return false;
      }
      for (std::size_t i = 1; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (!(std::isalnum(c) || value[i] == '_')) return false;
      }
      return true;
    };
    if (const Item* pf = find_field(*decl->block, "pushdown"); pf) {
      std::vector<std::pair<std::string, const Expr*>> push_entries;
      if (pf->value && pf->value->kind == ExprKind::MapLit) {
        for (const auto& entry : pf->value->entries) {
          push_entries.emplace_back(entry.key, entry.value.get());
        }
      } else if (pf->block) {
        for (const auto& entry : pf->block->items) {
          if (!entry || entry->kind != ItemKind::Field || !entry->value) {
            fail(span, "fonte '" + name + "': 'pushdown' deve conter campos");
          }
          push_entries.emplace_back(entry->key, entry->value.get());
        }
      } else {
        fail(span, "fonte '" + name + "': 'pushdown' deve ser um mapa");
      }
      std::vector<std::string> columns;
      std::vector<std::pair<std::string, Value>> predicates;
      int limit = 0;
      for (const auto& entry : push_entries) {
        if (entry.first == "colunas") {
          Value value = eval(*entry.second, root_);
          if (value.kind != ValueKind::Lista || !value.list || value.list->empty()) {
            fail(span, "fonte '" + name + "': 'pushdown.colunas' deve ser uma lista nao vazia");
          }
          for (const Value& column : *value.list) {
            if (column.kind != ValueKind::Texto || !is_sql_identifier(column.s)) {
              fail(span, "fonte '" + name + "': coluna de pushdown invalida");
            }
            columns.push_back(column.s);
          }
        } else if (entry.first == "onde") {
          Value value = eval(*entry.second, root_);
          if (value.kind != ValueKind::Mapa || !value.map) {
            fail(span, "fonte '" + name + "': 'pushdown.onde' deve ser um mapa");
          }
          for (const auto& predicate : value.map->items) {
            if (!is_sql_identifier(predicate.first)) {
              fail(span, "fonte '" + name + "': coluna de filtro invalida");
            }
            predicates.emplace_back(predicate.first, predicate.second);
          }
        } else if (entry.first == "limite") {
          Value value = eval(*entry.second, root_);
          if (value.kind != ValueKind::Inteiro || value.i < 1 ||
              value.i > std::numeric_limits<int>::max()) {
            fail(span, "fonte '" + name + "': 'pushdown.limite' deve ser inteiro >= 1");
          }
          limit = static_cast<int>(value.i);
        } else {
          fail(span, "fonte '" + name + "': chave '" + entry.first + "' desconhecida em 'pushdown'");
        }
      }
      while (!sql.empty() && std::isspace(static_cast<unsigned char>(sql.back()))) sql.pop_back();
      if (!sql.empty() && sql.back() == ';') {
        sql.pop_back();
        while (!sql.empty() && std::isspace(static_cast<unsigned char>(sql.back()))) sql.pop_back();
      }
      if (sql.find('?') != std::string::npos && !predicates.empty()) {
        fail(span, "fonte '" + name + "': consulta com '?' nao pode receber pushdown.onde");
      }
      std::string pushed = "SELECT ";
      if (columns.empty()) {
        pushed += "*";
      } else {
        for (std::size_t i = 0; i < columns.size(); ++i) {
          if (i) pushed += ", ";
          pushed += columns[i];
        }
      }
      pushed += " FROM (" + sql + ") AS tilt_pushdown";
      for (std::size_t i = 0; i < predicates.size(); ++i) {
        const auto& predicate = predicates[i];
        const rt::SqlParam param = rt::param_de_valor(predicate.second, "pushdown.onde");
        pushed += i == 0 ? " WHERE " : " AND ";
        if (param.tipo == rt::SqlParam::Tipo::Nulo) {
          pushed += predicate.first + " IS NULL";
        } else {
          pushed += predicate.first + " = ?";
          pushdown_params.push_back(param);
        }
      }
      if (limit > 0) pushed += " LIMIT " + std::to_string(limit);
      sql = std::move(pushed);
    }
    auto run_query = [&](const std::string& query) {
      if (tipo == "duckdb") {
        return pushdown_params.empty() ? rt::duckdb_query(path, query)
                                       : rt::duckdb_query_params(path, query, pushdown_params);
      }
      if (tipo == "sqlite") {
        return pushdown_params.empty() ? rt::sqlite_query(path, query)
                                       : rt::sqlite_query_params(path, query, pushdown_params);
      }
      if (tipo == "mysql") {
        return pushdown_params.empty() ? rt::mysql_query(path, query)
                                       : rt::mysql_query_params(path, query, pushdown_params);
      }
      if (tipo == "clickhouse") {
        return pushdown_params.empty() ? rt::clickhouse_query(path, query)
                                       : rt::clickhouse_query_params(path, query, pushdown_params);
      }
      return pushdown_params.empty() ? rt::postgres_query(path, query)
                                     : rt::postgres_query_params(path, query, pushdown_params);
    };
    try {
      Value t = run_query(sql);
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

// Forma da anotacao 'entrada: tensor[...]' sem o lote (quando literal).
// Devolve {} se ausente ou nao literal.
std::vector<std::int64_t> forma_entrada_modelo(const Item& decl) {
  if (!decl.block) return {};
  const Item* ent = find_field(*decl.block, "entrada");
  if (!ent || !ent->value || ent->value->kind != ExprKind::Index || !ent->value->lhs ||
      ent->value->lhs->kind != ExprKind::Name || ent->value->lhs->text != "tensor" ||
      ent->value->elems.empty()) {
    return {};
  }
  std::size_t inicio = 0;
  if (ent->value->elems[0] && ent->value->elems[0]->kind == ExprKind::Name) inicio = 1;  // dtype
  std::vector<std::int64_t> forma;
  for (std::size_t i = inicio; i < ent->value->elems.size(); ++i) {
    const auto& d = ent->value->elems[i];
    if (!d || d->kind != ExprKind::IntLit) return {};
    forma.push_back(std::stoll(d->text));
  }
  return forma;
}

// Dimensao de entrada do modelo: primeiro tenta a anotacao
// 'entrada: tensor[..., N]'; sem anotacao, infere do primeiro
// 'linear: [A, B]' das camadas. Devolve -1 se indeterminavel.
std::int64_t model_in_dim(const Item& decl) {
  if (decl.block) {
    if (const Item* ent = find_field(*decl.block, "entrada");
        ent && ent->value && ent->value->kind == ExprKind::Index && !ent->value->elems.empty()) {
      const Expr* last = ent->value->elems.back().get();
      if (last && last->kind == ExprKind::IntLit) return std::stoll(last->text);
    }
    if (const Item* camadas = find_field(*decl.block, "camadas");
        camadas && camadas->block) {
      std::int64_t found = -1;
      each_layer_spec(*camadas->block, [&](const std::string& key, const ast::Expr* value) {
        if (found >= 0) return;
        if (key == "linear" && value && value->kind == ExprKind::ListLit &&
            value->elems.size() == 2 && value->elems[0]->kind == ExprKind::IntLit) {
          found = std::stoll(value->elems[0]->text);
        }
      });
      if (found >= 0) return found;
    }
  }
  return -1;
}

}  // namespace

bool Interpreter::camada_com_pesos(Interpreter::Layer::Kind kind) {
  return kind == Interpreter::Layer::Dense || kind == Interpreter::Layer::Residual ||
         kind == Interpreter::Layer::Embedding || kind == Interpreter::Layer::Recorrente ||
         kind == Interpreter::Layer::Conv2d || kind == Interpreter::Layer::NormaLote;
}

rt::Tensor Interpreter::value_to_tensor(const Value& v, Span span) {
  if (v.kind == ValueKind::Tensor && v.tensor) return *v.tensor;
  if (v.kind == ValueKind::Lista) {
    std::vector<std::int64_t> shape;
    std::vector<float> data;
    flatten_nested(v, shape, data, 0);
    rt::Tensor t;
    t.shape = shape;
    t.data.assign(data.begin(), data.end());
    if (t.size() != static_cast<std::int64_t>(t.data.size())) {
      fail(span, "lista aninhada irregular; nao forma um tensor");
    }
    return t;
  }
  if (v.is_number()) return rt::Tensor::filled({1}, static_cast<float>(v.as_number()));
  fail(span, std::string("nao e possivel converter '") + v.type_name() + "' em tensor");
}

std::vector<Interpreter::Layer> Interpreter::build_layers(const Item& decl, std::int64_t in_dim,
                                                             std::uint64_t seed_inicial) {
  const std::string name = decl_name(decl);
  std::vector<Layer> layers;
  std::uint64_t seed = seed_inicial;
  // Forma sem lote da entrada: da anotacao literal ou do escalar inferido.
  std::vector<std::int64_t> forma = forma_entrada_modelo(decl);
  if (forma.empty() && in_dim > 0) forma = {in_dim};
  auto largura = [&]() { return forma.empty() ? -1 : forma.back(); };
  auto canais = [&]() {
    if (forma.size() == 3) return forma[0];
    if (forma.size() == 1) return forma[0];
    return static_cast<std::int64_t>(-1);
  };

  if (decl.block) {
    const Item* camadas = find_field(*decl.block, "camadas");
    if (camadas && camadas->block) {
      each_layer_spec(*camadas->block, [&](const std::string& key, const ast::Expr* value) {
        auto ler_inteiro = [&](const ast::Expr* e, const char* oque) {
          if (!e || e->kind != ExprKind::IntLit) {
            fail(decl.span, "modelo '" + name + "': '" + oque + "' espera inteiro");
          }
          return std::strtoll(e->text.c_str(), nullptr, 10);
        };
        if (key == "densa" && value && value->kind == ExprKind::IntLit) {
          if (forma.size() != 1) {
            fail(decl.span, "modelo '" + name + "': 'densa' precisa de entrada 1D; insira 'achatar' antes");
          }
          const std::int64_t atual = largura();
          const std::int64_t n = ler_inteiro(value, "densa");
          if (n <= 0) fail(decl.span, "modelo '" + name + "': 'densa' deve ser >= 1");
          Layer l;
          l.kind = Layer::Dense;
          l.w = rt::Tensor::xavier({atual, n}, atual, n, seed++);
          l.b = rt::Tensor::zeros({n});
          layers.push_back(std::move(l));
          forma = {n};
        } else if (key == "linear" && value && value->kind == ExprKind::ListLit &&
                   value->elems.size() == 2) {
          if (forma.size() != 1) {
            fail(decl.span, "modelo '" + name + "': 'linear' precisa de entrada 1D; insira 'achatar' antes");
          }
          const std::int64_t a = ler_inteiro(value->elems[0].get(), "linear");
          const std::int64_t b = ler_inteiro(value->elems[1].get(), "linear");
          if (a != largura()) {
            fail(decl.span, "modelo '" + name + "': 'linear' espera entrada de " + std::to_string(a) +
                               " mas a camada anterior produz " + std::to_string(largura()));
          }
          Layer l;
          l.kind = Layer::Dense;
          l.w = rt::Tensor::xavier({a, b}, a, b, seed++);
          l.b = rt::Tensor::zeros({b});
          layers.push_back(std::move(l));
          forma = {b};
        } else if (key == "residual") {
          if (forma.size() != 1 || largura() <= 0) {
            fail(decl.span, "modelo " + name + ": residual precisa de entrada 1D conhecida");
          }
          const std::int64_t d = largura();
          Layer l;
          l.kind = Layer::Residual;
          l.w = rt::Tensor::xavier({d, d}, d, d, seed++);
          l.b = rt::Tensor::zeros({d});
          layers.push_back(std::move(l));
        } else if (key == "incorporacao") {
          if (!value || value->kind != ExprKind::ListLit || value->elems.size() != 2) {
            fail(decl.span, "modelo '" + name + "': incorporacao espera [vocabulario, dimensao]");
          }
          const std::int64_t vocabulario = ler_inteiro(value->elems[0].get(), "incorporacao");
          const std::int64_t dimensao = ler_inteiro(value->elems[1].get(), "incorporacao");
          if (vocabulario <= 0 || dimensao <= 0) {
            fail(decl.span, "modelo '" + name + "': vocabulario e dimensao devem ser >= 1");
          }
          if (forma.empty()) {
            fail(decl.span, "modelo '" + name + "': incorporacao precisa de entrada anotada");
          }
          Layer l;
          l.kind = Layer::Embedding;
          l.vocabulario = vocabulario;
          l.dimensao = dimensao;
          l.w = rt::Tensor::xavier({vocabulario, dimensao}, dimensao, dimensao, seed++);
          l.b = rt::Tensor::zeros({0});
          layers.push_back(std::move(l));
          forma.push_back(dimensao);
        } else if (key == "recorrente") {
          if (!value || value->kind != ExprKind::ListLit || value->elems.size() != 2 ||
              (value->elems[0]->kind != ExprKind::Name &&
               value->elems[0]->kind != ExprKind::TextLit)) {
            fail(decl.span, "modelo '" + name + "': recorrente espera [rnn|lstm|gru, oculta]");
          }
          const std::string tipo = value->elems[0]->text;
          if (tipo != "rnn" && tipo != "lstm" && tipo != "gru") {
            fail(decl.span, "modelo '" + name + "': tipo recorrente deve ser rnn, lstm ou gru");
          }
          const std::int64_t h = ler_inteiro(value->elems[1].get(), "recorrente");
          if (h <= 0) fail(decl.span, "modelo '" + name + "': oculta recorrente deve ser >= 1");
          if (forma.size() != 2 || forma[0] <= 0 || forma[1] <= 0) {
            fail(decl.span, "modelo '" + name +
                                "': recorrente precisa de entrada [tempo, atributos] conhecida");
          }
          const std::int64_t f = forma[1];
          const std::int64_t portas = tipo == "rnn" ? 1 : (tipo == "lstm" ? 4 : 3);
          Layer l;
          l.kind = Layer::Recorrente;
          l.recorrente_tipo = tipo;
          l.oculta = h;
          l.w = rt::Tensor::xavier({f, portas * h}, f, portas * h, seed++);
          l.u = rt::Tensor::xavier({h, portas * h}, h, portas * h, seed++);
          l.b = rt::Tensor::zeros({portas * h});
          layers.push_back(std::move(l));
          forma = {h};
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
        } else if (key == "conv2d") {
          // Conv2d: entrada [N, C_in, H, W], nucleo [C_out, C_in, KH, KW].
          // Forma: `conv2d: [C_saida, C_entrada, KH, KW, passo, padding, dilatacao]`;
          // os tres ultimos campos sao opcionais.
          if (!value || value->kind != ExprKind::ListLit ||
              (value->elems.size() < 4 || value->elems.size() > 7)) {
            fail(decl.span,
                 "modelo '" + name +
                     "': 'conv2d' espera [C_saida, C_entrada, KH, KW, passo, padding, dilatacao]");
          }
          const std::int64_t c_saida = ler_inteiro(value->elems[0].get(), "conv2d");
          const std::int64_t c_entrada = ler_inteiro(value->elems[1].get(), "conv2d");
          const std::int64_t kh = ler_inteiro(value->elems[2].get(), "conv2d");
          const std::int64_t kw = ler_inteiro(value->elems[3].get(), "conv2d");
          const std::int64_t passo =
              value->elems.size() >= 5 ? ler_inteiro(value->elems[4].get(), "conv2d") : 1;
          const std::int64_t padding =
              value->elems.size() >= 6 ? ler_inteiro(value->elems[5].get(), "conv2d") : 0;
          const std::int64_t dilatacao =
              value->elems.size() >= 7 ? ler_inteiro(value->elems[6].get(), "conv2d") : 1;
          if (c_saida <= 0 || c_entrada <= 0 || kh <= 0 || kw <= 0 || passo < 1 || padding < 0 ||
              dilatacao < 1) {
            fail(decl.span, "modelo '" + name + "': dimensoes de 'conv2d' devem ser >= 1");
          }
          const std::int64_t conhecido = canais();
          if (conhecido > 0 && c_entrada != conhecido) {
            fail(decl.span, "modelo '" + name + "': 'conv2d' espera C_entrada " +
                                std::to_string(c_entrada) + " mas a camada anterior produz " +
                                std::to_string(conhecido));
          }
          if (forma.size() != 3) {
            fail(decl.span, "modelo '" + name + "': 'conv2d' precisa de entrada 4D [N, C, H, W]");
          }
          const std::int64_t kh_eff = (kh - 1) * dilatacao + 1;
          const std::int64_t kw_eff = (kw - 1) * dilatacao + 1;
          if (kh_eff > forma[1] + 2 * padding || kw_eff > forma[2] + 2 * padding) {
            fail(decl.span, "modelo '" + name + "': nucleo efetivo maior que a entrada com padding");
          }
          Layer l;
          l.kind = Layer::Conv2d;
          l.passo = passo;
          l.padding = padding;
          l.dilatacao = dilatacao;
          l.w = rt::Tensor::xavier({c_saida, c_entrada, kh, kw}, c_entrada * kh * kw,
                                   c_saida * kh * kw, seed++);
          l.b = rt::Tensor::zeros({c_saida});
          layers.push_back(std::move(l));
          const std::int64_t ekh = (kh - 1) * dilatacao + 1;
          const std::int64_t ekw = (kw - 1) * dilatacao + 1;
          forma = {c_saida, (forma[1] + 2 * padding - ekh) / passo + 1,
                   (forma[2] + 2 * padding - ekw) / passo + 1};
        } else if (key == "norma_lote") {
          const std::int64_t c = canais();
          if (c <= 0) {
            fail(decl.span, "modelo '" + name + "': 'norma_lote' precisa de canais conhecidos");
          }
          Layer l;
          l.kind = Layer::NormaLote;
          l.w = rt::Tensor::ones({c});   // gama
          l.b = rt::Tensor::zeros({c});  // beta
          l.media_running = rt::Tensor::zeros({c});
          l.var_running = rt::Tensor::ones({c});
          layers.push_back(std::move(l));
        } else if (key == "achatar") {
          if ((forma.size() != 2 && forma.size() != 3) ||
              std::any_of(forma.begin(), forma.end(), [](std::int64_t d) { return d <= 0; })) {
            fail(decl.span,
                 "modelo '" + name +
                     "': 'achatar' precisa de entrada com pelo menos dois eixos conhecida");
          }
          Layer l;
          l.kind = Layer::Flatten;
          l.plano = 1;
          for (std::int64_t d : forma) l.plano *= d;
          const std::int64_t plano = l.plano;
          layers.push_back(std::move(l));
          forma = {plano};
        } else if (key == "agrupamento_max") {
          if (!value ||
              (value->kind != ExprKind::IntLit &&
               (value->kind != ExprKind::ListLit ||
                (value->elems.size() != 1 && value->elems.size() != 2)))) {
            fail(decl.span, "modelo '" + name + "': 'agrupamento_max' espera [janela] ou [janela, passo]");
          }
          const std::int64_t janela = value->kind == ExprKind::IntLit
                                          ? ler_inteiro(value, "agrupamento_max")
                                          : ler_inteiro(value->elems[0].get(), "agrupamento_max");
          const std::int64_t passo = value->kind == ExprKind::ListLit && value->elems.size() == 2
                                         ? ler_inteiro(value->elems[1].get(), "agrupamento_max")
                                         : janela;
          if (janela < 1 || passo < 1) {
            fail(decl.span, "modelo '" + name + "': 'agrupamento_max' espera janela/passo >= 1");
          }
          if (forma.size() != 3) {
            fail(decl.span, "modelo '" + name + "': 'agrupamento_max' precisa de entrada 4D [N, C, H, W]");
          }
          if (janela > forma[1] || janela > forma[2]) {
            fail(decl.span, "modelo '" + name + "': janela maior que a entrada espacial");
          }
          Layer l;
          l.kind = Layer::MaxPool;
          l.janela = janela;
          l.passo = passo;
          layers.push_back(std::move(l));
          forma = {forma[0], (forma[1] - janela) / passo + 1, (forma[2] - janela) / passo + 1};
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
      if (path.size() >= 12 && path.compare(path.size() - 12, 12, ".safetensors") == 0) {
        std::map<std::string, rt::Tensor> tensors;
        std::map<std::string, std::string> metadata;
        std::string erro;
        if (!rt::safetensors_carregar(path, tensors, metadata, erro))
          fail(span,
               "modelo '" + name + "': arquivo Safetensors invalido '" + path + "' (" + erro + ")");
        std::size_t wi = 0;
        auto get = [&](const std::string& key, const rt::Tensor& esperado) {
          const auto found = tensors.find(key);
          if (found == tensors.end() || found->second.shape != esperado.shape)
            fail(span, "modelo '" + name + "': tensor Safetensors '" + key +
                           "' ausente ou com forma incompativel");
          return found->second;
        };
        for (Layer& l : layers) {
          if (!camada_com_pesos(l.kind)) continue;
          const std::string base = "camada_" + std::to_string(wi++);
          l.w = get(base + ".w", l.w);
          if (l.kind != Layer::Embedding) l.b = get(base + ".b", l.b);
          if (l.kind == Layer::Recorrente) {
            const auto mt = metadata.find(base + ".tipo");
            if (mt != metadata.end() && mt->second != l.recorrente_tipo)
              fail(span, "modelo '" + name + "': tipo recorrente do arquivo difere do modelo");
            l.u = get(base + ".u", l.u);
          } else if (l.kind == Layer::NormaLote) {
            l.media_running = get(base + ".media_running", l.media_running);
            l.var_running = get(base + ".var_running", l.var_running);
          }
        }
      } else {
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
            while (li < layers.size() && !camada_com_pesos(layers[li].kind)) ++li;
            if (li >= layers.size()) {
              fail(span, "modelo '" + name + "': o arquivo '" + path +
                             "' tem mais camadas de pesos do que o modelo");
            }
            Layer& l = layers[li];
            const Value* tipo = c.kind == ValueKind::Mapa && c.map ? c.map->find("tipo") : nullptr;
            std::string t = (tipo && tipo->kind == ValueKind::Texto) ? tipo->s : "densa";
            const std::string esperado =
                l.kind == Layer::Dense
                    ? "densa"
                    : (l.kind == Layer::Residual
                           ? "residual"
                           : (l.kind == Layer::Embedding
                                  ? "incorporacao"
                                  : (l.kind == Layer::Recorrente
                                         ? "recorrente"
                                         : (l.kind == Layer::Conv2d ? "conv2d" : "norma_lote"))));
            if (t != esperado) {
              fail(span, "modelo '" + name + "': camada " + std::to_string(li) + " e '" + esperado +
                             "', mas o arquivo traz '" + t + "'");
            }
            if (t == "densa" || t == "residual" || t == "incorporacao" || t == "recorrente" ||
                t == "conv2d") {
              rt::Tensor w, b;
              const Value* wv = c.kind == ValueKind::Mapa && c.map ? c.map->find("w") : nullptr;
              const Value* bv = c.kind == ValueKind::Mapa && c.map ? c.map->find("b") : nullptr;
              if (!wv || !bv || !tensor_from_json(*wv, w) || !tensor_from_json(*bv, b)) {
                fail(span,
                     "modelo '" + name + "': arquivo de pesos '" + path +
                         "' invalido (cada camada densa/conv2d precisa de 'w' e 'b' com forma "
                         "e dados)");
              }
              if (w.shape != l.w.shape || b.shape != l.b.shape) {
                const std::string rotulo =
                    l.kind == Layer::Dense
                        ? "camada densa "
                        : (l.kind == Layer::Residual
                               ? "camada residual "
                               : (l.kind == Layer::Embedding
                                      ? "camada incorporacao "
                                      : (l.kind == Layer::Conv2d ? "camada conv2d "
                                                                 : "camada norma_lote ")));
                fail(span, "modelo '" + name + "': forma de pesos incompativel na " + rotulo +
                               std::to_string(li) + " (modelo espera w " + l.w.shape_str() + " b " +
                               l.b.shape_str() + ", arquivo tem w " + w.shape_str() + " b " +
                               b.shape_str() + ")");
              }
              l.w = std::move(w);
              l.b = std::move(b);
              if (t == "recorrente") {
                const Value* uv = c.kind == ValueKind::Mapa && c.map ? c.map->find("u") : nullptr;
                const Value* rv =
                    c.kind == ValueKind::Mapa && c.map ? c.map->find("recorrente") : nullptr;
                rt::Tensor u;
                if (!uv || !tensor_from_json(*uv, u) || u.shape != l.u.shape) {
                  fail(span, "modelo '" + name + "': pesos recorrentes com forma U incompativel");
                }
                if (rv && rv->kind == ValueKind::Texto && rv->s != l.recorrente_tipo) {
                  fail(span, "modelo '" + name + "': tipo recorrente do arquivo difere do modelo");
                }
                l.u = std::move(u);
              }
              if (t == "conv2d") {
                const Value* pad =
                    c.kind == ValueKind::Mapa && c.map ? c.map->find("padding") : nullptr;
                const Value* dil =
                    c.kind == ValueKind::Mapa && c.map ? c.map->find("dilatacao") : nullptr;
                if (pad && pad->is_number()) {
                  const std::int64_t padding = static_cast<std::int64_t>(pad->as_number());
                  if (padding != l.padding) {
                    fail(span, "modelo '" + name +
                                   "': 'padding' do arquivo difere da camada conv2d " +
                                   std::to_string(li));
                  }
                }
                if (dil && dil->is_number()) {
                  const std::int64_t dilatacao = static_cast<std::int64_t>(dil->as_number());
                  if (dilatacao != l.dilatacao) {
                    fail(span, "modelo '" + name +
                                   "': 'dilatacao' do arquivo difere da camada conv2d " +
                                   std::to_string(li));
                  }
                }
                const Value* pv =
                    c.kind == ValueKind::Mapa && c.map ? c.map->find("passo") : nullptr;
                if (pv && pv->is_number()) {
                  const std::int64_t passo = static_cast<std::int64_t>(pv->as_number());
                  if (passo != l.passo) {
                    fail(span, "modelo '" + name +
                                   "': 'passo' do arquivo difere da camada conv2d " +
                                   std::to_string(li));
                  }
                }
              }
            } else if (t == "norma_lote") {
              rt::Tensor w, b;
              const Value* wv = c.kind == ValueKind::Mapa && c.map ? c.map->find("w") : nullptr;
              const Value* bv = c.kind == ValueKind::Mapa && c.map ? c.map->find("b") : nullptr;
              const Value* mv =
                  c.kind == ValueKind::Mapa && c.map ? c.map->find("media_running") : nullptr;
              const Value* vv =
                  c.kind == ValueKind::Mapa && c.map ? c.map->find("var_running") : nullptr;
              if (!wv || !bv || !tensor_from_json(*wv, w) || !tensor_from_json(*bv, b)) {
                fail(span, "modelo '" + name + "': arquivo de pesos '" + path +
                               "' invalido (cada camada norma_lote precisa de 'w' e 'b' com forma "
                               "e dados)");
              }
              if (w.shape != l.w.shape || b.shape != l.b.shape) {
                const std::string rotulo =
                    l.kind == Layer::Dense
                        ? "camada densa "
                        : (l.kind == Layer::Residual
                               ? "camada residual "
                               : (l.kind == Layer::Embedding
                                      ? "camada incorporacao "
                                      : (l.kind == Layer::Conv2d ? "camada conv2d "
                                                                 : "camada norma_lote ")));
                fail(span, "modelo '" + name + "': forma de pesos incompativel na " + rotulo +
                               std::to_string(li) + " (modelo espera w " + l.w.shape_str() + " b " +
                               l.b.shape_str() + ", arquivo tem w " + w.shape_str() + " b " +
                               b.shape_str() + ")");
              }
              l.w = std::move(w);
              l.b = std::move(b);
              rt::Tensor media, var;
              if (mv && vv && tensor_from_json(*mv, media) && tensor_from_json(*vv, var) &&
                  media.shape == l.media_running.shape && var.shape == l.var_running.shape) {
                l.media_running = std::move(media);
                l.var_running = std::move(var);
              }
            }
            ++li;
          }
          while (li < layers.size() && !camada_com_pesos(layers[li].kind)) ++li;
          if (li < layers.size()) {
            fail(span, "modelo '" + name + "': o arquivo '" + path +
                           "' tem menos camadas de pesos do que o modelo");
          }
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
      case Layer::Residual: {
        const rt::Tensor skip = x;
        x = rt::add(rt::add(mm(x, l.w), l.b), skip);
        break;
      }
      case Layer::Embedding:
        x = rt::embedding(x, l.w);
        break;
      case Layer::Recorrente:
        x = rt::recorrente(
            x, l.w, l.u, l.b,
            l.recorrente_tipo == "lstm"
                ? rt::RecurrentKind::Lstm
                : (l.recorrente_tipo == "gru" ? rt::RecurrentKind::Gru : rt::RecurrentKind::Rnn));
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
      case Layer::Conv2d:
        x = rt::adicionar_vies_conv(rt::conv2d(x, l.w, l.passo, l.padding, l.dilatacao), l.b);
        break;
      case Layer::NormaLote:
        x = rt::norma_lote(x, l.w, l.b, l.media_running, l.var_running, 1e-5f, false);
        break;
      case Layer::Flatten: {
        if (x.rank() < 2) fail(Span{}, "camada 'achatar' precisa de entrada com lote");
        std::int64_t resto = 1;
        for (std::size_t i = 1; i < x.shape.size(); ++i) resto *= x.shape[i];
        x = rt::reshape(x, {x.shape[0], resto});
        break;
      }
      case Layer::MaxPool:
        x = rt::maxpool2d(x, l.janela, l.passo);
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

  auto make_onnx_layers = [&](const std::vector<Layer>& source) {
    std::vector<rt::OnnxLayer> result;
    for (const Layer& l : source) {
      rt::OnnxLayer o;
      if (l.kind == Layer::Dense) {
        o.kind = rt::OnnxLayer::Dense;
        o.w = l.w;
        o.b = l.b;
      } else if (l.kind == Layer::Residual) {
        o.kind = rt::OnnxLayer::Residual;
        o.w = l.w;
        o.b = l.b;
      } else if (l.kind == Layer::Activation) {
        o.kind = rt::OnnxLayer::Activation;
        o.act = l.act;
      } else if (l.kind == Layer::Softmax) {
        o.kind = rt::OnnxLayer::Softmax;
      } else if (l.kind == Layer::LayerNorm) {
        o.kind = rt::OnnxLayer::LayerNorm;
      } else if (l.kind == Layer::Recorrente) {
        o.kind = rt::OnnxLayer::Recorrente;
        o.w = l.w;
        o.u = l.u;
        o.b = l.b;
        o.recorrente_tipo = l.recorrente_tipo;
      } else if (l.kind == Layer::Embedding) {
        fail(inner.span, "modelo nao exportavel em ONNX: camada incorporacao");
      } else if (l.kind == Layer::Conv2d) {
        o.kind = rt::OnnxLayer::Conv2d;
        o.w = l.w;
        o.b = l.b;
        o.passo = l.passo;
        o.padding = l.padding;
        o.dilatacao = l.dilatacao;
      } else if (l.kind == Layer::NormaLote) {
        o.kind = rt::OnnxLayer::NormaLote;
        o.w = l.w;
        o.b = l.b;
        o.media_running = l.media_running;
        o.var_running = l.var_running;
      } else if (l.kind == Layer::Flatten) {
        o.kind = rt::OnnxLayer::Flatten;
        o.plano = l.plano;
      } else if (l.kind == Layer::MaxPool) {
        o.kind = rt::OnnxLayer::MaxPool;
        o.janela = l.janela;
        o.passo = l.passo;
      } else {
        o.kind = rt::OnnxLayer::Dropout;
      }
      result.push_back(std::move(o));
    }
    return result;
  };

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
    // A dimensao de entrada vem da anotacao 'entrada: tensor[..., N]'
    // (ou do primeiro 'linear: [A, B]' quando sem anotacao).
    const std::int64_t in_dim = model_in_dim(*it->second);
    if (in_dim < 0) {
      fail(inner.span, "modelo '" + mname +
                           "': 'salvar_pesos' precisa de 'entrada: tensor[..., N]' anotado "
                           "para inferir a dimensao de entrada");
    }
    const std::vector<Layer>& layers = build_model(*it->second, in_dim, inner.span);
    if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".onnx") == 0) {
      std::vector<std::int64_t> forma_entrada = forma_entrada_modelo(*it->second);
      if (forma_entrada.empty() && in_dim > 0) forma_entrada = {in_dim};
      if (forma_entrada.empty())
        fail(inner.span, "modelo precisa de forma de entrada conhecida para ONNX");
      std::string erro;
      if (!rt::onnx_salvar(path, make_onnx_layers(layers), forma_entrada, mname, erro))
        fail(inner.span, "modelo nao pode salvar pesos ONNX (" + erro + ")");
      out_ << "modelo " << mname << ": pesos ONNX salvos em " << path << "\n";
      return Value::logico(true);
    }
    if (path.size() >= 12 && path.compare(path.size() - 12, 12, ".safetensors") == 0) {
      std::map<std::string, rt::Tensor> tensors;
      std::map<std::string, std::string> metadata;
      std::size_t wi = 0;
      for (const Layer& l : layers) {
        if (!camada_com_pesos(l.kind)) continue;
        const std::string base = "camada_" + std::to_string(wi++);
        tensors[base + ".w"] = l.w;
        if (l.kind != Layer::Embedding) tensors[base + ".b"] = l.b;
        if (l.kind == Layer::Recorrente) {
          tensors[base + ".u"] = l.u;
          metadata[base + ".tipo"] = l.recorrente_tipo;
        } else if (l.kind == Layer::Residual) {
          metadata[base + ".tipo"] = "residual";
        } else if (l.kind == Layer::Embedding) {
          metadata[base + ".tipo"] = "incorporacao";
        } else if (l.kind == Layer::Conv2d) {
          metadata[base + ".tipo"] = "conv2d";
        } else if (l.kind == Layer::NormaLote) {
          metadata[base + ".tipo"] = "norma_lote";
          tensors[base + ".media_running"] = l.media_running;
          tensors[base + ".var_running"] = l.var_running;
        } else {
          metadata[base + ".tipo"] = "densa";
        }
      }
      std::string erro;
      if (!rt::safetensors_salvar(path, tensors, metadata, erro)) {
        fail(inner.span, "modelo '" + mname + "': nao foi possivel gravar Safetensors '" + path +
                             "' (" + erro + ")");
      }
      out_ << "modelo " << mname << ": pesos Safetensors salvos em " << path << " ("
           << tensors.size() << " tensores)\n";
      return Value::logico(true);
    }
    Value cl = Value::lista();
    for (const Layer& l : layers) {
      if (l.kind != Layer::Dense && l.kind != Layer::Residual && l.kind != Layer::Embedding &&
          l.kind != Layer::Recorrente && l.kind != Layer::Conv2d && l.kind != Layer::NormaLote)
        continue;
      Value c = Value::mapa();
      if (l.kind == Layer::Dense) {
        c.map->set("tipo", Value::texto("densa"));
        c.map->set("w", Value::tensor_de(l.w));
        c.map->set("b", Value::tensor_de(l.b));
      } else if (l.kind == Layer::Residual) {
        c.map->set("tipo", Value::texto("residual"));
        c.map->set("w", Value::tensor_de(l.w));
        c.map->set("b", Value::tensor_de(l.b));
      } else if (l.kind == Layer::Embedding) {
        c.map->set("tipo", Value::texto("incorporacao"));
        c.map->set("w", Value::tensor_de(l.w));
        c.map->set("b", Value::tensor_de(l.b));
      } else if (l.kind == Layer::Recorrente) {
        c.map->set("tipo", Value::texto("recorrente"));
        c.map->set("recorrente", Value::texto(l.recorrente_tipo));
        c.map->set("w", Value::tensor_de(l.w));
        c.map->set("u", Value::tensor_de(l.u));
        c.map->set("b", Value::tensor_de(l.b));
      } else if (l.kind == Layer::Conv2d) {
        c.map->set("tipo", Value::texto("conv2d"));
        c.map->set("w", Value::tensor_de(l.w));
        c.map->set("b", Value::tensor_de(l.b));
        c.map->set("passo", Value::inteiro(l.passo));
        c.map->set("padding", Value::inteiro(l.padding));
        c.map->set("dilatacao", Value::inteiro(l.dilatacao));
      } else if (l.kind == Layer::NormaLote) {
        c.map->set("tipo", Value::texto("norma_lote"));
        c.map->set("w", Value::tensor_de(l.w));
        c.map->set("b", Value::tensor_de(l.b));
        c.map->set("media_running", Value::tensor_de(l.media_running));
        c.map->set("var_running", Value::tensor_de(l.var_running));
      }
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
  if (method == "carregar_pesos") {
    if (inner.args.empty() || inner.args[0].value->kind != ExprKind::TextLit) {
      fail(inner.span, "uso: modelo " + mname + ".carregar_pesos \"caminho\"");
    }
    const std::string& path = inner.args[0].value->text;
    const std::int64_t in_dim = model_in_dim(*it->second);
    if (in_dim < 0) {
      fail(inner.span, "modelo '" + mname +
                           "': 'carregar_pesos' precisa de 'entrada: tensor[..., N]' anotado "
                           "para inferir a dimensao de entrada");
    }
    if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".onnx") == 0) {
      std::vector<rt::OnnxTensor> imported;
      std::string erro;
      if (!rt::onnx_carregar_tensores(path, imported, erro))
        fail(inner.span, "modelo nao pode carregar pesos ONNX (" + erro + ")");
      std::lock_guard<std::mutex> lk(model_cache_mutex_);
      if (model_cache_.find(mname) == model_cache_.end())
        model_cache_.emplace(mname, build_layers(*it->second, in_dim));
      auto& cached = model_cache_[mname];
      std::map<std::string, const rt::OnnxTensor*> by_name;
      for (const rt::OnnxTensor& tensor : imported) by_name[tensor.name] = &tensor;
      std::size_t cursor = 0;
      auto next_named = [&](const std::string& prefix) -> const rt::OnnxTensor* {
        for (; cursor < imported.size(); ++cursor) {
          if (imported[cursor].name.compare(0, prefix.size(), prefix) == 0)
            return &imported[cursor++];
        }
        return nullptr;
      };
      auto next_dense = [&]() -> const rt::OnnxTensor* {
        for (; cursor < imported.size(); ++cursor) {
          const std::string& name = imported[cursor].name;
          if (name.size() > 1 && name.compare(0, 1, "W") == 0 &&
              std::isdigit(static_cast<unsigned char>(name[1])))
            return &imported[cursor++];
        }
        return nullptr;
      };
      auto copy_tensor = [&](const std::string& name, const std::vector<std::int64_t>& shape) {
        const auto found = by_name.find(name);
        if (found == by_name.end() || found->second->shape != shape)
          fail(inner.span, "peso ONNX ausente ou com forma incompativel: " + name);
        rt::Tensor result;
        result.shape = found->second->shape;
        result.data.assign(found->second->data.begin(), found->second->data.end());
        return result;
      };
      auto suffix = [](const std::string& name, const std::string& prefix) {
        return name.substr(prefix.size());
      };
      std::size_t loaded = 0;
      for (Layer& l : cached) {
        if (!camada_com_pesos(l.kind)) continue;
        if (l.kind == Layer::Dense) {
          const rt::OnnxTensor* w = next_dense();
          if (!w) fail(inner.span, "pesos ONNX sem inicializador denso");
          const std::string id = suffix(w->name, "W");
          l.w = copy_tensor(w->name, l.w.shape);
          l.b = copy_tensor("B" + id, l.b.shape);
        } else if (l.kind == Layer::Residual) {
          const rt::OnnxTensor* w = next_named("W_res");
          if (!w) fail(inner.span, "pesos ONNX sem inicializador residual");
          const std::string residual_id = suffix(w->name, "W_res");
          l.w = copy_tensor(w->name, l.w.shape);
          l.b = copy_tensor("B_res" + residual_id, l.b.shape);
        } else if (l.kind == Layer::Recorrente) {
          const rt::OnnxTensor* w = next_named("W_rec");
          if (!w) fail(inner.span, "pesos ONNX sem inicializador recorrente");
          const std::string id = suffix(w->name, "W_rec");
          const std::int64_t input = l.w.shape[0];
          const std::int64_t gates = l.w.shape[1];
          const std::int64_t hidden = l.u.shape[0];
          const std::vector<std::int64_t> onnx_w_shape = {1, input, gates};
          const std::vector<std::int64_t> onnx_u_shape = {1, hidden, gates};
          const rt::Tensor ow = copy_tensor(w->name, onnx_w_shape);
          const rt::Tensor ou = copy_tensor("R_rec" + id, onnx_u_shape);
          const rt::Tensor ob = copy_tensor("B_rec" + id, {1, 2 * gates});
          const int gate_count =
              l.recorrente_tipo == "rnn" ? 1 : (l.recorrente_tipo == "lstm" ? 4 : 3);
          if (gates != static_cast<std::int64_t>(gate_count) * hidden)
            fail(inner.span, "peso ONNX recorrente com quantidade de portas incompativel");
          const std::vector<int> order = l.recorrente_tipo == "lstm" ? std::vector<int>{0, 3, 1, 2}
                                                                     : std::vector<int>{0, 1, 2};
          l.w = rt::Tensor::zeros(l.w.shape);
          l.u = rt::Tensor::zeros(l.u.shape);
          l.b = rt::Tensor::zeros(l.b.shape);
          for (int gate = 0; gate < gate_count; ++gate) {
            const int source = order[static_cast<std::size_t>(gate)];
            for (std::int64_t i = 0; i < input; ++i)
              for (std::int64_t j = 0; j < hidden; ++j)
                l.w.data[static_cast<std::size_t>(i * gates + source * hidden + j)] =
                    ow.data[static_cast<std::size_t>(i * gates + gate * hidden + j)];
            for (std::int64_t i = 0; i < hidden; ++i)
              for (std::int64_t j = 0; j < hidden; ++j)
                l.u.data[static_cast<std::size_t>(i * gates + source * hidden + j)] =
                    ou.data[static_cast<std::size_t>(i * gates + gate * hidden + j)];
            for (std::int64_t j = 0; j < hidden; ++j)
              l.b.data[static_cast<std::size_t>(source * hidden + j)] =
                  ob.data[static_cast<std::size_t>(gate * hidden + j)];
          }
        } else if (l.kind == Layer::Conv2d) {
          const rt::OnnxTensor* w = next_named("W_conv");
          if (!w) fail(inner.span, "pesos ONNX sem inicializador conv2d");
          const std::string id = suffix(w->name, "W_conv");
          l.w = copy_tensor(w->name, l.w.shape);
          l.b = copy_tensor("B_conv" + id, l.b.shape);
        } else if (l.kind == Layer::NormaLote) {
          const rt::OnnxTensor* scale = next_named("BN_scale");
          if (!scale) fail(inner.span, "pesos ONNX sem inicializador norma_lote");
          const std::string id = suffix(scale->name, "BN_scale");
          l.w = copy_tensor(scale->name, l.w.shape);
          l.b = copy_tensor("BN_bias" + id, l.b.shape);
          l.media_running = copy_tensor("BN_mean" + id, l.media_running.shape);
          l.var_running = copy_tensor("BN_var" + id, l.var_running.shape);
        } else {
          fail(inner.span, "camada com pesos nao suportada na importacao ONNX");
        }
        ++loaded;
      }
      out_ << "modelo " << mname << ": pesos ONNX carregados de " << path << " (" << loaded
           << " camadas)\n";
      return Value::logico(true);
    }
    if (path.size() >= 12 && path.compare(path.size() - 12, 12, ".safetensors") == 0) {
      std::map<std::string, rt::Tensor> tensors;
      std::map<std::string, std::string> metadata;
      std::string erro;
      if (!rt::safetensors_carregar(path, tensors, metadata, erro)) {
        fail(inner.span,
             "modelo '" + mname + "': arquivo Safetensors invalido '" + path + "' (" + erro + ")");
      }
      {
        std::lock_guard<std::mutex> lk(model_cache_mutex_);
        if (model_cache_.find(mname) == model_cache_.end())
          model_cache_.emplace(mname, build_layers(*it->second, in_dim));
        auto& cached = model_cache_[mname];
        std::size_t wi = 0;
        auto get = [&](const std::string& key, const rt::Tensor& esperado) {
          const auto found = tensors.find(key);
          if (found == tensors.end() || found->second.shape != esperado.shape) {
            fail(inner.span, "modelo '" + mname + "': tensor Safetensors '" + key +
                                 "' ausente ou com forma incompativel");
          }
          return found->second;
        };
        for (Layer& l : cached) {
          if (!camada_com_pesos(l.kind)) continue;
          const std::string base = "camada_" + std::to_string(wi++);
          l.w = get(base + ".w", l.w);
          if (l.kind != Layer::Embedding) l.b = get(base + ".b", l.b);
          if (l.kind == Layer::Recorrente) {
            const auto mt = metadata.find(base + ".tipo");
            if (mt != metadata.end() && mt->second != l.recorrente_tipo)
              fail(inner.span,
                   "modelo '" + mname + "': tipo recorrente do arquivo difere do modelo");
            l.u = get(base + ".u", l.u);
          } else if (l.kind == Layer::NormaLote) {
            l.media_running = get(base + ".media_running", l.media_running);
            l.var_running = get(base + ".var_running", l.var_running);
          }
        }
        std::size_t expected = 0;
        for (const Layer& l : cached)
          if (camada_com_pesos(l.kind)) ++expected;
        if (wi != expected)
          fail(inner.span,
               "modelo '" + mname + "': quantidade de camadas Safetensors incompativel");
      }
      out_ << "modelo " << mname << ": pesos Safetensors carregados de " << path << "\n";
      return Value::logico(true);
    }
    std::ifstream f(path);
    if (!f)
      fail(inner.span, "modelo '" + mname + "': arquivo de pesos '" + path + "' nao encontrado");
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
      fail(inner.span, "modelo '" + mname + "': arquivo de pesos '" + path +
                           "' invalido (esperado JSON tilt-pesos com 'camadas')");
    }
    std::size_t n = 0;
    {
      std::lock_guard<std::mutex> lk(model_cache_mutex_);
      if (model_cache_.find(mname) == model_cache_.end()) {
        model_cache_.emplace(mname, build_layers(*it->second, in_dim));
      }
    }
    {
      std::lock_guard<std::mutex> lk(model_cache_mutex_);
      std::size_t li = 0;
      for (const Value& c : *camadas->list) {
        while (li < model_cache_[mname].size() && !camada_com_pesos(model_cache_[mname][li].kind)) {
          ++li;
        }
        if (li >= model_cache_[mname].size()) {
          fail(inner.span, "modelo '" + mname + "': o arquivo '" + path +
                               "' tem mais camadas de pesos do que o modelo");
        }
        Layer& l = model_cache_[mname][li];
        const Value* tipo = c.kind == ValueKind::Mapa && c.map ? c.map->find("tipo") : nullptr;
        std::string t = (tipo && tipo->kind == ValueKind::Texto) ? tipo->s : "densa";
        const std::string esperado =
            l.kind == Layer::Dense
                ? "densa"
                : (l.kind == Layer::Residual
                       ? "residual"
                       : (l.kind == Layer::Embedding
                              ? "incorporacao"
                              : (l.kind == Layer::Recorrente
                                     ? "recorrente"
                                     : (l.kind == Layer::Conv2d ? "conv2d" : "norma_lote"))));
        if (t != esperado) {
          fail(inner.span, "modelo '" + mname + "': camada " + std::to_string(li) + " e '" +
                               esperado + "', mas o arquivo traz '" + t + "'");
        }
        if (t == "densa" || t == "residual" || t == "incorporacao" || t == "recorrente" ||
            t == "conv2d") {
          rt::Tensor w, b;
          const Value* wv = c.kind == ValueKind::Mapa && c.map ? c.map->find("w") : nullptr;
          const Value* bv = c.kind == ValueKind::Mapa && c.map ? c.map->find("b") : nullptr;
          if (!wv || !bv || !tensor_from_json(*wv, w) || !tensor_from_json(*bv, b)) {
            fail(
                inner.span,
                "modelo '" + mname + "': arquivo de pesos '" + path +
                    "' invalido (cada camada densa/conv2d precisa de 'w' e 'b' com forma e dados)");
          }
          if (w.shape != l.w.shape || b.shape != l.b.shape) {
            const std::string rotulo =
                l.kind == Layer::Dense
                    ? "camada densa "
                    : (l.kind == Layer::Residual
                           ? "camada residual "
                           : (l.kind == Layer::Embedding
                                  ? "camada incorporacao "
                                  : (l.kind == Layer::Conv2d ? "camada conv2d "
                                                             : "camada norma_lote ")));
            fail(inner.span, "modelo '" + mname + "': forma de pesos incompativel na " + rotulo +
                                 std::to_string(li) + " (modelo espera w " + l.w.shape_str() +
                                 " b " + l.b.shape_str() + ", arquivo tem w " + w.shape_str() +
                                 " b " + b.shape_str() + ")");
          }
          l.w = std::move(w);
          l.b = std::move(b);
          if (t == "recorrente") {
            const Value* uv = c.kind == ValueKind::Mapa && c.map ? c.map->find("u") : nullptr;
            const Value* rv =
                c.kind == ValueKind::Mapa && c.map ? c.map->find("recorrente") : nullptr;
            rt::Tensor u;
            if (!uv || !tensor_from_json(*uv, u) || u.shape != l.u.shape) {
              fail(inner.span,
                   "modelo '" + mname + "': pesos recorrentes com forma U incompativel");
            }
            if (rv && rv->kind == ValueKind::Texto && rv->s != l.recorrente_tipo) {
              fail(inner.span,
                   "modelo '" + mname + "': tipo recorrente do arquivo difere do modelo");
            }
            l.u = std::move(u);
          }
          if (t == "conv2d") {
            const Value* pad =
                c.kind == ValueKind::Mapa && c.map ? c.map->find("padding") : nullptr;
            const Value* dil =
                c.kind == ValueKind::Mapa && c.map ? c.map->find("dilatacao") : nullptr;
            if (pad && pad->is_number()) {
              const std::int64_t padding = static_cast<std::int64_t>(pad->as_number());
              if (padding != l.padding) {
                fail(inner.span, "modelo '" + mname +
                                     "': 'padding' do arquivo difere da camada conv2d " +
                                     std::to_string(li));
              }
            }
            if (dil && dil->is_number()) {
              const std::int64_t dilatacao = static_cast<std::int64_t>(dil->as_number());
              if (dilatacao != l.dilatacao) {
                fail(inner.span, "modelo '" + mname +
                                     "': 'dilatacao' do arquivo difere da camada conv2d " +
                                     std::to_string(li));
              }
            }
            const Value* pv = c.kind == ValueKind::Mapa && c.map ? c.map->find("passo") : nullptr;
            if (pv && pv->is_number()) {
              const std::int64_t passo = static_cast<std::int64_t>(pv->as_number());
              if (passo != l.passo) {
                fail(inner.span, "modelo '" + mname + "': 'passo' do arquivo difere da camada conv2d " +
                                     std::to_string(li));
              }
            }
          }
        } else if (t == "norma_lote") {
          rt::Tensor w, b;
          const Value* wv = c.kind == ValueKind::Mapa && c.map ? c.map->find("w") : nullptr;
          const Value* bv = c.kind == ValueKind::Mapa && c.map ? c.map->find("b") : nullptr;
          const Value* mv = c.kind == ValueKind::Mapa && c.map ? c.map->find("media_running") : nullptr;
          const Value* vv = c.kind == ValueKind::Mapa && c.map ? c.map->find("var_running") : nullptr;
          if (!wv || !bv || !tensor_from_json(*wv, w) || !tensor_from_json(*bv, b)) {
            fail(inner.span, "modelo '" + mname + "': arquivo de pesos '" + path +
                                 "' invalido (cada camada norma_lote precisa de 'w' e 'b' com forma e dados)");
          }
          if (w.shape != l.w.shape || b.shape != l.b.shape) {
            const std::string rotulo = l.kind == Layer::Dense
                                           ? "camada densa "
                                           : (l.kind == Layer::Conv2d ? "camada conv2d "
                                                                      : "camada norma_lote ");
            fail(inner.span, "modelo '" + mname + "': forma de pesos incompativel na " + rotulo +
                                 std::to_string(li) + " (modelo espera w " + l.w.shape_str() +
                                 " b " + l.b.shape_str() + ", arquivo tem w " + w.shape_str() +
                                 " b " + b.shape_str() + ")");
          }
          l.w = std::move(w);
          l.b = std::move(b);
          rt::Tensor media, var;
          if (mv && vv && tensor_from_json(*mv, media) && tensor_from_json(*vv, var) &&
              media.shape == l.media_running.shape && var.shape == l.var_running.shape) {
            l.media_running = std::move(media);
            l.var_running = std::move(var);
          }
        }
        ++li;
        ++n;
      }
      while (li < model_cache_[mname].size() && !camada_com_pesos(model_cache_[mname][li].kind)) {
        ++li;
      }
      if (li < model_cache_[mname].size()) {
        fail(inner.span, "modelo '" + mname + "': o arquivo '" + path + "' tem menos camadas de pesos do que o modelo");
      }
    }
    out_ << "modelo " << mname << ": pesos carregados de " << path << " (" << n << " camadas)\n";
    return Value::logico(true);
  }
  if (method == "exportar_onnx" || method == "exportar") {
    if (inner.args.empty() || inner.args[0].value->kind != ExprKind::TextLit) {
      fail(inner.span, "uso: modelo " + mname + ".exportar_onnx \"caminho.onnx\"");
    }
    const std::string& path = inner.args[0].value->text;
    if (method == "exportar" && path.size() >= 5 &&
        path.compare(path.size() - 5, 5, ".onnx") != 0) {
      fail(inner.span, "modelo '" + mname + "': 'exportar' so suporta destino '.onnx' "
                           "(use modelo " + mname + ".exportar_onnx \"modelo.onnx\")");
    }
    const std::int64_t in_dim = model_in_dim(*it->second);
    if (in_dim < 0) {
      fail(inner.span, "modelo '" + mname +
                           "': 'exportar_onnx' precisa de 'entrada: tensor[..., N]' anotado "
                           "para inferir a dimensao de entrada");
    }
    const std::vector<Layer>& layers = build_model(*it->second, in_dim, inner.span);
    std::vector<std::int64_t> forma_entrada = forma_entrada_modelo(*it->second);
    if (forma_entrada.empty() && in_dim > 0) forma_entrada = {in_dim};
    if (forma_entrada.empty()) {
      fail(inner.span,
           "modelo '" + mname + "': 'exportar_onnx' precisa de forma de entrada conhecida");
    }
    std::vector<rt::OnnxLayer> ol;
    for (const Layer& l : layers) {
      rt::OnnxLayer o;
      if (l.kind == Layer::Dense) {
        o.kind = rt::OnnxLayer::Dense;
        o.w = l.w;
        o.b = l.b;
      } else if (l.kind == Layer::Residual) {
        o.kind = rt::OnnxLayer::Residual;
        o.w = l.w;
        o.b = l.b;
      } else if (l.kind == Layer::Activation) {
        o.kind = rt::OnnxLayer::Activation;
        o.act = l.act;
      } else if (l.kind == Layer::Softmax) {
        o.kind = rt::OnnxLayer::Softmax;
      } else if (l.kind == Layer::LayerNorm) {
        o.kind = rt::OnnxLayer::LayerNorm;
      } else if (l.kind == Layer::Recorrente) {
        o.kind = rt::OnnxLayer::Recorrente;
        o.w = l.w;
        o.u = l.u;
        o.b = l.b;
        o.recorrente_tipo = l.recorrente_tipo;
      } else if (l.kind == Layer::Embedding) {
        fail(inner.span, "modelo '" + mname + "': exportar_onnx ainda nao suporta a camada incorporacao");
      } else if (l.kind == Layer::Conv2d) {
        o.kind = rt::OnnxLayer::Conv2d;
        o.w = l.w;
        o.b = l.b;
        o.passo = l.passo;
        o.padding = l.padding;
        o.dilatacao = l.dilatacao;
      } else if (l.kind == Layer::NormaLote) {
        o.kind = rt::OnnxLayer::NormaLote;
        o.w = l.w;
        o.b = l.b;
        o.media_running = l.media_running;
        o.var_running = l.var_running;
      } else if (l.kind == Layer::Flatten) {
        o.kind = rt::OnnxLayer::Flatten;
        o.plano = l.plano;
      } else if (l.kind == Layer::MaxPool) {
        o.kind = rt::OnnxLayer::MaxPool;
        o.janela = l.janela;
        o.passo = l.passo;
      } else {
        o.kind = rt::OnnxLayer::Dropout;
      }
      ol.push_back(std::move(o));
    }
    std::string err;
    if (!rt::onnx_salvar(path, ol, forma_entrada, mname, err)) {
      fail(inner.span, "modelo '" + mname + "': nao foi possivel exportar ONNX para '" + path +
                           "' (" + err + ")");
    }
    out_ << "modelo " << mname << ": onnx exportado para " << path << "\n";
    return Value::logico(true);
  }
  if (method == "exportar_gguf") {
    if (inner.args.empty() || inner.args[0].value->kind != ExprKind::TextLit) {
      fail(inner.span, "uso: modelo " + mname + ".exportar_gguf \"caminho.gguf\"");
    }
    const std::string& path = inner.args[0].value->text;
    const std::int64_t in_dim = model_in_dim(*it->second);
    if (in_dim < 0) {
      fail(inner.span, "modelo '" + mname +
                           "': 'exportar_gguf' precisa de 'entrada: tensor[..., N]' anotado "
                           "para inferir a dimensao de entrada");
    }
    const std::vector<Layer>& layers = build_model(*it->second, in_dim, inner.span);
    std::vector<rt::GgufTensor> tensores;
    std::size_t estadual = 0;
    for (const Layer& l : layers) {
      if (!camada_com_pesos(l.kind)) continue;
      const std::string base = "camada-" + std::to_string(estadual);
      rt::GgufTensor w, b;
      w.nome = base + ".peso";
      w.forma = l.w.shape;
      w.dados.assign(l.w.data.begin(), l.w.data.end());
      if (l.kind == Layer::Embedding) {
        tensores.push_back(std::move(w));
        ++estadual;
        continue;
      }
      b.nome = base + ".vies";
      b.forma = l.b.shape;
      b.dados.assign(l.b.data.begin(), l.b.data.end());
      tensores.push_back(std::move(w));
      tensores.push_back(std::move(b));
      ++estadual;
    }
    std::string err;
    if (!rt::gguf_salvar(path, tensores, mname, err)) {
      fail(inner.span, "modelo '" + mname + "': nao foi possivel exportar GGUF para '" + path +
                           "' (" + err + ")");
    }
    out_ << "modelo " << mname << ": gguf exportado para " << path << " (" << tensores.size()
         << " tensores)\n";
    return Value::logico(true);
  }
  fail(inner.span, "metodo de modelo '" + method + "' desconhecido (use executar / para_frente / salvar_pesos / carregar_pesos / exportar_onnx / exportar_gguf)");
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

void Interpreter::ler_cfg_treino(const ast::Block& cfg, std::int64_t n, const std::string& ctx,
                                  Span span, TreinoCfg& out) {
  out.perda = field_word(cfg, "perda", "entropia_cruzada");
  out.otim = field_word(cfg, "otimizador", "sgd");
  out.lr = field_num(cfg, "taxa", field_num(cfg, "taxa_aprendizado", 0.1));
  out.epocas = field_int(cfg, "epocas", 50);
  const Item* vf = find_field(cfg, "verboso");
  out.verbose = vf && vf->value && vf->value->kind == ExprKind::BoolLit && vf->value->boolean;
  out.lote = field_int(cfg, "lote", static_cast<int>(n));
  out.embaralhar = true;
  if (const Item* ef = find_field(cfg, "embaralhar"); ef && ef->value) {
    if (ef->value->kind != ExprKind::BoolLit) {
      fail(span, ctx + ": 'embaralhar' deve ser verdadeiro ou falso");
    }
    out.embaralhar = ef->value->boolean;
  }
  if (out.lote < 1) fail(span, ctx + ": 'lote' deve ser >= 1");
  out.num_shards = field_int(cfg, "num_shards", 1);
  out.shard_id = field_int(cfg, "shard_id", 0);
  if (out.num_shards < 1) fail(span, ctx + ": 'num_shards' deve ser >= 1");
  if (out.shard_id < 0 || out.shard_id >= out.num_shards) {
    fail(span, ctx + ": 'shard_id' deve estar em [0, num_shards)");
  }
  if (const Item* cf = find_field(cfg, "cluster"); cf && cf->value) {
    if (cf->value->kind != ExprKind::MapLit) {
      fail(span, ctx + ": 'cluster' deve ser mapa { dir: \"...\", rank: N, mundo: N }");
    }
    for (const auto& e : cf->value->entries) {
      if (e.key == "dir") {
        if (!e.value || e.value->kind != ExprKind::TextLit || e.value->text.empty()) {
          fail(span, ctx + ": 'cluster.dir' deve ser texto nao vazio");
        }
        out.cluster_dir = e.value->text;
      } else if (e.key == "rank") {
        if (!e.value || e.value->kind != ExprKind::IntLit) {
          fail(span, ctx + ": 'cluster.rank' deve ser inteiro");
        }
        out.cluster_rank = static_cast<int>(std::strtol(e.value->text.c_str(), nullptr, 10));
      } else if (e.key == "mundo" || e.key == "world") {
        if (!e.value || e.value->kind != ExprKind::IntLit) {
          fail(span, ctx + ": 'cluster.mundo' deve ser inteiro");
        }
        out.cluster_world = static_cast<int>(std::strtol(e.value->text.c_str(), nullptr, 10));
      } else if (e.key == "timeout") {
        if (!e.value || e.value->kind != ExprKind::IntLit) {
          fail(span, ctx + ": 'cluster.timeout' deve ser inteiro");
        }
        out.cluster_timeout = static_cast<int>(std::strtol(e.value->text.c_str(), nullptr, 10));
      } else {
        fail(span, ctx + ": chave '" + e.key + "' desconhecida em 'cluster'");
      }
    }
    if (out.cluster_world < 2) {
      fail(span, ctx + ": 'cluster.mundo' deve ser >= 2");
    }
    if (out.cluster_dir.empty()) {
      fail(span, ctx + ": 'cluster.dir' e obrigatorio");
    }
    if (out.cluster_rank < 0 || out.cluster_rank >= out.cluster_world) {
      fail(span, ctx + ": 'cluster.rank' deve estar em [0, mundo)");
    }
    if (out.cluster_timeout < 1) {
      fail(span, ctx + ": 'cluster.timeout' deve ser >= 1");
    }
    out.shard_id = out.cluster_rank;
    out.num_shards = out.cluster_world;
  }
  out.seed_init = 0xC1A5;
  out.seed_mistura = 7;
  if (const Item* sf = find_field(cfg, "semente");
      sf && sf->value && sf->value->kind == ExprKind::IntLit) {
    const std::int64_t s = std::stoll(sf->value->text);
    if (s < 0) fail(span, ctx + ": 'semente' deve ser >= 0");
    out.seed_init = static_cast<std::uint64_t>(s);
    out.seed_mistura = static_cast<std::uint64_t>(s);
  }
  out.f_val = 0.0;
  if (const Item* vlf = find_field(cfg, "validacao"); vlf && vlf->value) {
    if (vlf->value->kind != ExprKind::DecimalLit && vlf->value->kind != ExprKind::IntLit) {
      fail(span, ctx + ": 'validacao' deve ser numero em (0, 1)");
    }
    out.f_val = std::strtod(vlf->value->text.c_str(), nullptr);
    if (!(out.f_val > 0.0) || !(out.f_val < 1.0)) {
      fail(span, ctx + ": 'validacao' deve ser numero em (0, 1)");
    }
  }
  out.paciencia = 0;
  out.melhorar_min = 0.0;
  if (const Item* pc = find_field(cfg, "parar_cedo"); pc && pc->value) {
    if (out.f_val <= 0.0) fail(span, ctx + ": 'parar_cedo' exige 'validacao:'");
    if (pc->value->kind == ExprKind::IntLit) {
      out.paciencia = static_cast<int>(std::strtol(pc->value->text.c_str(), nullptr, 10));
    } else if (pc->value->kind == ExprKind::MapLit) {
      for (const auto& e : pc->value->entries) {
        if (e.key == "paciencia") {
          if (!e.value || e.value->kind != ExprKind::IntLit ||
              (out.paciencia = static_cast<int>(std::strtol(e.value->text.c_str(), nullptr, 10))) <
                  1) {
            fail(span, ctx + ": 'parar_cedo.paciencia' deve ser inteiro >= 1");
          }
        } else if (e.key == "melhorar_min") {
          if (!e.value ||
              (e.value->kind != ExprKind::DecimalLit && e.value->kind != ExprKind::IntLit) ||
              (out.melhorar_min = std::strtod(e.value->text.c_str(), nullptr)) < 0.0) {
            fail(span, ctx + ": 'parar_cedo.melhorar_min' deve ser numero >= 0");
          }
        } else {
          fail(span, ctx + ": chave '" + e.key + "' desconhecida em 'parar_cedo'");
        }
      }
      if (out.paciencia < 1) fail(span, ctx + ": 'parar_cedo' exige paciencia >= 1");
    } else {
      fail(span, ctx + ": 'parar_cedo' deve ser N ou { paciencia: N [, melhorar_min: D] }");
    }
  }
  if (const Item* cf = find_field(cfg, "checkpoint");
      cf && cf->value && cf->value->kind == ExprKind::TextLit) {
    out.checkpoint = cf->value->text;
  }
  out.a_cada = field_int(cfg, "a_cada", 0);
  if (out.a_cada < 0) fail(span, ctx + ": 'a_cada' deve ser >= 0");
  out.agenda_tipo = "constante";
  out.degrau_a_cada = 10;
  out.degrau_fator = 0.5;
  if (const Item* ag = find_field(cfg, "agendador"); ag && ag->value) {
    if (ag->value->kind != ExprKind::MapLit) {
      fail(span, ctx + ": 'agendador' deve ser mapa { tipo: cosseno | degrau [, ...] }");
    }
    for (const auto& e : ag->value->entries) {
      if (e.key == "tipo") {
        if (!e.value || e.value->kind != ExprKind::Name ||
            (e.value->text != "cosseno" && e.value->text != "degrau")) {
          fail(span, ctx + ": 'agendador.tipo' deve ser cosseno | degrau");
        }
        out.agenda_tipo = e.value->text;
      } else if (e.key == "a_cada") {
        if (!e.value || e.value->kind != ExprKind::IntLit ||
            (out.degrau_a_cada = static_cast<int>(std::strtol(e.value->text.c_str(), nullptr, 10))) <
                1) {
          fail(span, ctx + ": 'agendador.a_cada' deve ser inteiro >= 1");
        }
      } else if (e.key == "fator") {
        if (!e.value ||
            (e.value->kind != ExprKind::DecimalLit && e.value->kind != ExprKind::IntLit) ||
            (out.degrau_fator = std::strtod(e.value->text.c_str(), nullptr)) <= 0.0 ||
            out.degrau_fator >= 1.0) {
          fail(span, ctx + ": 'agendador.fator' deve ser numero em (0, 1)");
        }
      } else {
        fail(span, ctx + ": chave '" + e.key + "' desconhecida em 'agendador'");
      }
    }
  }
  if (const Item* rf = find_field(cfg, "retomar");
      rf && rf->value && rf->value->kind == ExprKind::TextLit) {
    out.retomar = rf->value->text;
  }
  if (const Item* cb = find_field(cfg, "ao_epoca")) {
    if (!cb->block) {
      fail(span, ctx + ": 'ao_epoca' deve ser um bloco de passos");
    }
    out.ao_epoca = cb->block.get();
  }
  out.bloco = field_int(cfg, "bloco", 1024);
  if (out.bloco < 1) fail(span, ctx + ": 'bloco' deve ser >= 1");
}

namespace {

// Dataloader streaming de CSV (treino em arquivos grandes): o arquivo e
// varrido uma vez (contagem + rotulos) e relido em blocos por epoca — so o
// bloco corrente vive na RAM. Erros via runtime_error (o nucleo converte).
struct FluxoCSV {
  std::string caminho;
  std::string alvo;
  std::vector<std::string> atributos;
  std::vector<std::int64_t> colunas;  // indice CSV de cada atributo
  std::int64_t coluna_alvo = -1;
  std::vector<std::int64_t> desloc;  // byte offset de cada linha de dados
};

// Abre, le o cabecalho e valida o alvo.
void abrir_fluxo_csv(FluxoCSV& fx) {
  std::ifstream in(fx.caminho);
  if (!in) throw std::runtime_error("csv: nao foi possivel abrir '" + fx.caminho + "'");
  std::string linha;
  while (std::getline(in, linha)) {
    if (linha.empty()) continue;
    const auto cab = split_csv_line(linha);
    for (std::size_t k = 0; k < cab.size(); ++k) {
      if (cab[k] == fx.alvo) {
        fx.coluna_alvo = static_cast<std::int64_t>(k);
      } else {
        fx.colunas.push_back(static_cast<std::int64_t>(k));
        fx.atributos.push_back(cab[k]);
      }
    }
    break;
  }
  if (fx.coluna_alvo < 0) {
    throw std::runtime_error("csv: coluna '" + fx.alvo + "' ausente em '" + fx.caminho + "'");
  }
  if (fx.atributos.empty()) {
    throw std::runtime_error("csv: nenhuma coluna de atributo em '" + fx.caminho + "'");
  }
}

// Varredura: deslocamentos + rotulos.
void varrer_fluxo_csv(FluxoCSV& fx, std::vector<double>& rotulos) {
  std::ifstream in(fx.caminho);
  if (!in) throw std::runtime_error("csv: nao foi possivel abrir '" + fx.caminho + "'");
  std::string linha;
  bool cabecalho = true;
  while (true) {
    const std::int64_t pos = static_cast<std::int64_t>(in.tellg());
    if (!std::getline(in, linha)) break;
    if (linha.empty()) continue;
    if (cabecalho) {
      cabecalho = false;
      continue;
    }
    fx.desloc.push_back(pos);
    const auto celulas = split_csv_line(linha);
    const std::string& cel =
        fx.coluna_alvo < static_cast<std::int64_t>(celulas.size())
            ? celulas[static_cast<std::size_t>(fx.coluna_alvo)]
            : std::string();
    rotulos.push_back(cel.empty() ? 0.0 : parse_scalar(cel).as_number());
  }
}

// Le ate `max` linhas a partir da linha `inicio`: tensor [lidas, F] + indices.
void ler_bloco_fluxo(const FluxoCSV& fx, std::int64_t inicio, std::int64_t max, rt::Tensor& saida,
                     std::vector<std::int64_t>& linhas) {
  std::ifstream in(fx.caminho);
  if (!in) throw std::runtime_error("csv: nao foi possivel abrir '" + fx.caminho + "'");
  saida.shape = {0, static_cast<std::int64_t>(fx.atributos.size())};
  saida.data.clear();
  linhas.clear();
  std::string linha;
  const std::int64_t fim =
      std::min<std::int64_t>(inicio + max, static_cast<std::int64_t>(fx.desloc.size()));
  for (std::int64_t r = inicio; r < fim; ++r) {
    in.clear();
    in.seekg(static_cast<std::streampos>(fx.desloc[static_cast<std::size_t>(r)]));
    if (!std::getline(in, linha)) {
      throw std::runtime_error("csv: falha ao ler bloco em '" + fx.caminho + "'");
    }
    const auto celulas = split_csv_line(linha);
    for (std::int64_t c : fx.colunas) {
      const std::string& cel = c < static_cast<std::int64_t>(celulas.size())
                                   ? celulas[static_cast<std::size_t>(c)]
                                   : std::string();
      saida.data.push_back(cel.empty() ? 0.0F : static_cast<float>(parse_scalar(cel).as_number()));
    }
    linhas.push_back(r);
  }
  saida.shape[0] = static_cast<std::int64_t>(linhas.size());
}

}  // namespace

Interpreter::DadosTreino Interpreter::ler_dados_treino(const ast::Expr& expr_dados,
                                                       const std::string& ctx, Span span,
                                                       TreinoCfg& cfg) {
  Value data = eval(expr_dados, root_);
  const Value* fluxo_csv =
      data.kind == ValueKind::Mapa && data.map ? data.map->find("fluxo_csv") : nullptr;
  DadosTreino saida;
  if (fluxo_csv && fluxo_csv->kind == ValueKind::Texto) {
    const Value* alvo = data.map->find("alvo");
    const Value* atributos = data.map->find("atributos");
    if (!alvo || alvo->kind != ValueKind::Texto || !atributos || atributos->kind != ValueKind::Lista ||
        !atributos->list || atributos->list->empty()) {
      fail(span, ctx + ": descritor de fluxo invalido (use carregador com fluxo: verdadeiro)");
    }
    cfg.fluxo_csv = fluxo_csv->s;
    cfg.fluxo_alvo = alvo->s;
    for (const Value& c : *atributos->list) cfg.fluxo_atributos.push_back(c.s);
    cfg.fluxo_parquet = cfg.fluxo_csv.size() >= 8 &&
                        cfg.fluxo_csv.compare(cfg.fluxo_csv.size() - 8, 8, ".parquet") == 0;
    if (cfg.fluxo_parquet) {
      // Parquet: uma passada por row group extraindo so os rotulos.
      try {
        rt::ParquetFluxo pfx = rt::parquet_abrir_fluxo(cfg.fluxo_csv);
        for (std::int64_t g = 0; g < pfx.grupos; ++g) {
          Value tab = rt::parquet_ler_grupo_fluxo(pfx, g);
          if (!tab.list) continue;
          for (const Value& row : *tab.list) {
            const Value* t = row.kind == ValueKind::Mapa && row.map ? row.map->find(cfg.fluxo_alvo)
                                                                    : nullptr;
            saida.yf.push_back(t ? t->as_number() : 0.0);
          }
        }
      } catch (const std::exception& e) {
        fail(span, ctx + ": " + e.what());
      }
      cfg.fluxo_f = static_cast<std::int64_t>(cfg.fluxo_atributos.size());
      cfg.fluxo_n = static_cast<std::int64_t>(saida.yf.size());
      saida.n = cfg.fluxo_n;
      if (saida.n < 1) fail(span, ctx + ": fluxo sem linhas de dados");
      return saida;
    }
    FluxoCSV fx;
    fx.caminho = cfg.fluxo_csv;
    fx.alvo = cfg.fluxo_alvo;
    try {
      abrir_fluxo_csv(fx);
      if (fx.atributos != cfg.fluxo_atributos) {
        throw std::runtime_error("csv: cabecalho divergente em '" + fx.caminho + "'");
      }
      varrer_fluxo_csv(fx, saida.yf);
    } catch (const std::exception& e) {
      fail(span, ctx + ": " + e.what());
    }
    cfg.fluxo_f = static_cast<std::int64_t>(fx.atributos.size());
    cfg.fluxo_desloc = std::move(fx.desloc);
    cfg.fluxo_n = static_cast<std::int64_t>(saida.yf.size());
    saida.n = cfg.fluxo_n;
    if (saida.n < 1) fail(span, ctx + ": fluxo sem linhas de dados");
    return saida;
  }
  if (data.kind != ValueKind::Mapa || !data.map || !data.map->find("x") || !data.map->find("y")) {
    fail(span, ctx + ": 'dados' deve produzir { x: <tensor>, y: <lista> }");
  }
  saida.x = value_to_tensor(*data.map->find("x"), span);
  if (saida.x.rank() != 2 && saida.x.rank() != 3 && saida.x.rank() != 4) {
    fail(span, ctx + ": 'x' deve ser 2D [amostras, atributos] ou 4D [N, C, H, W]");
  }
  const Value* yv = data.map->find("y");
  if (yv->kind == ValueKind::Lista && yv->list) {
    for (const Value& e : *yv->list) saida.yf.push_back(e.as_number());
  }
  saida.n = saida.x.shape[0];
  if (static_cast<std::int64_t>(saida.yf.size()) != saida.n) fail(span, ctx + ": |x| != |y|");
  return saida;
}

Interpreter::TreinoRelato Interpreter::treinar_nucleo(const Item& modelo_decl, rt::Tensor x,
                                                        std::vector<double> yf,
                                                        const TreinoCfg& cfg, const std::string& ctx,
                                                        Span span) {
  const std::string name = decl_name(modelo_decl);
  const bool fluxo = !cfg.fluxo_csv.empty();
  int classes = 1;
  for (double v : yf) classes = std::max(classes, static_cast<int>(v) + 1);
  const std::int64_t n_total = fluxo ? cfg.fluxo_n : x.shape[0];
  if (n_total < 1) fail(span, ctx + ": dados sem linhas");
  std::vector<std::int64_t> fluxo_rows;
  if (fluxo) {
    fluxo_rows.reserve(static_cast<std::size_t>(n_total / cfg.num_shards + 1));
    for (std::int64_t i = 0; i < n_total; ++i) {
      if (i % cfg.num_shards == cfg.shard_id) fluxo_rows.push_back(i);
    }
    if (fluxo_rows.empty()) {
      fail(span, ctx + ": shard_id nao recebeu nenhuma linha");
    }
    std::vector<double> shard_y;
    shard_y.reserve(fluxo_rows.size());
    for (std::int64_t i : fluxo_rows) shard_y.push_back(yf[static_cast<std::size_t>(i)]);
    yf = std::move(shard_y);
  } else if (cfg.num_shards > 1) {
    std::vector<std::int64_t> shard_rows;
    shard_rows.reserve(static_cast<std::size_t>(n_total / cfg.num_shards + 1));
    for (std::int64_t i = 0; i < n_total; ++i) {
      if (i % cfg.num_shards == cfg.shard_id) shard_rows.push_back(i);
    }
    if (shard_rows.empty()) {
      fail(span, ctx + ": shard_id nao recebeu nenhuma linha");
    }
    x = rt::fatiar_lote(x, shard_rows);
    std::vector<double> shard_y;
    shard_y.reserve(shard_rows.size());
    for (std::int64_t i : shard_rows) shard_y.push_back(yf[static_cast<std::size_t>(i)]);
    yf = std::move(shard_y);
  }
  const std::int64_t n = fluxo ? static_cast<std::int64_t>(fluxo_rows.size()) : x.shape[0];
  const std::int64_t f = fluxo ? cfg.fluxo_f : x.shape[1];
  const bool ce = cfg.perda == "entropia_cruzada";
  const bool mse = cfg.perda == "quadratica";
  // For Parquet, map global row numbers from the file to this shard's local labels.
  std::vector<std::int64_t> local_row_for_global;
  if (fluxo && cfg.fluxo_parquet) {
    local_row_for_global.assign(static_cast<std::size_t>(n_total), -1);
    for (std::size_t i = 0; i < fluxo_rows.size(); ++i) {
      local_row_for_global[static_cast<std::size_t>(fluxo_rows[i])] =
          static_cast<std::int64_t>(i);
    }
  }

  std::vector<int> y;
  for (double v : yf) y.push_back(static_cast<int>(v));
  // Divisao treino/validacao (deterministica pela semente).
  rt::Tensor x_tr = x;
  std::vector<int> y_tr = y;
  std::vector<double> yf_tr = yf;
  rt::Tensor x_val;
  std::vector<int> y_val;
  std::vector<double> yf_val;
  std::int64_t n_tr = n;
  // Mascara treino/validacao por linha global (vale para RAM e fluxo).
  std::vector<char> e_treino(static_cast<std::size_t>(n), 1);
  {
    std::vector<std::int64_t> corte(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) corte[static_cast<std::size_t>(i)] = i;
    std::mt19937_64 rng_div(cfg.seed_mistura + 0x9E3779B9ULL);
    std::shuffle(corte.begin(), corte.end(), rng_div);
    std::int64_t n_val = static_cast<std::int64_t>(cfg.f_val * static_cast<double>(n));
    if (cfg.f_val > 0.0) n_val = std::max<std::int64_t>(1, std::min<std::int64_t>(n_val, n - 1));
    std::vector<std::int64_t> idx_tr(corte.begin(), corte.begin() + (n - n_val));
    std::vector<std::int64_t> idx_val(corte.begin() + (n - n_val), corte.end());
    if (cfg.f_val > 0.0) {
      if (!fluxo) {
        x_tr = rt::fatiar_lote(x, idx_tr);
        x_val = rt::fatiar_lote(x, idx_val);
      }
      y_tr.clear();
      yf_tr.clear();
      std::fill(e_treino.begin(), e_treino.end(), 0);
      for (std::int64_t i : idx_tr) {
        y_tr.push_back(y[static_cast<std::size_t>(i)]);
        yf_tr.push_back(yf[static_cast<std::size_t>(i)]);
        e_treino[static_cast<std::size_t>(i)] = 1;
      }
      for (std::int64_t i : idx_val) {
        y_val.push_back(y[static_cast<std::size_t>(i)]);
        yf_val.push_back(yf[static_cast<std::size_t>(i)]);
      }
      n_tr = n - n_val;
    }
  }

  double lr = cfg.lr;
  set_device(modelo_decl);
  std::vector<Layer> layers = build_layers(modelo_decl, f, cfg.seed_init);
  if (fluxo) {
    for (const Layer& l : layers) {
      if (l.kind == Layer::Conv2d || l.kind == Layer::MaxPool || l.kind == Layer::Flatten) {
        fail(span, ctx + ": fluxo CSV so alimenta modelo 2D (denso); sem conv2d/agrupamento_max/achatar");
      }
    }
  }
  if (!ce && !mse) {
    fail(span, ctx + ": perda '" + cfg.perda + "' nao suportada (use 'entropia_cruzada' | 'quadratica')");
  }
  if (layers.empty()) fail(span, ctx + ": modelo sem camadas");
  if (ce && layers.back().kind != Layer::Softmax) {
    fail(span, ctx + ": perda 'entropia_cruzada' exige 'softmax' na ultima camada");
  }
  if (mse) {
    if (layers.back().kind == Layer::Softmax) {
      fail(span, ctx + ": perda 'quadratica' exige ultima camada 'densa'/'linear' sem 'softmax'");
    }
    std::int64_t width = 1;
    for (auto rit = layers.rbegin(); rit != layers.rend(); ++rit) {
      if (rit->kind == Layer::Dense || rit->kind == Layer::Residual) {
        width = rit->w.shape[1];
        break;
      }
    }
    if (width != 1) {
      fail(span, ctx +
                     ": perda 'quadratica' e regressao escalar; a saida do modelo deve ter "
                     "largura 1 (tem " +
                     std::to_string(width) + ")");
    }
  }
  for (Layer& l : layers) {
    if (l.kind != Layer::Dense && l.kind != Layer::Residual && l.kind != Layer::Embedding &&
        l.kind != Layer::Recorrente && l.kind != Layer::Conv2d && l.kind != Layer::NormaLote)
      continue;
    l.m_w = rt::Tensor::zeros(l.w.shape);
    l.v_w = rt::Tensor::zeros(l.w.shape);
    l.m_b = rt::Tensor::zeros(l.b.shape);
    l.v_b = rt::Tensor::zeros(l.b.shape);
    if (l.kind == Layer::Recorrente) {
      l.m_u = rt::Tensor::zeros(l.u.shape);
      l.v_u = rt::Tensor::zeros(l.u.shape);
    }
  }

  // Salva checkpoint de treino: pesos + momentos do Adam + epoca.
  auto salvar_checkpoint = [&](const std::string& caminho, int epoca_feita, int passo_adam) {
    Value cl = Value::lista();
    for (const Layer& l : layers) {
      if (!camada_com_pesos(l.kind)) continue;
      Value c = Value::mapa();
      c.map->set(
          "tipo",
          Value::texto(
              l.kind == Layer::Dense
                  ? "densa"
                  : (l.kind == Layer::Residual
                         ? "residual"
                         : (l.kind == Layer::Embedding
                                ? "incorporacao"
                                : (l.kind == Layer::Recorrente
                                       ? "recorrente"
                                       : (l.kind == Layer::Conv2d ? "conv2d" : "norma_lote"))))));
      c.map->set("w", Value::tensor_de(l.w));
      c.map->set("b", Value::tensor_de(l.b));
      c.map->set("m_w", Value::tensor_de(l.m_w));
      c.map->set("v_w", Value::tensor_de(l.v_w));
      c.map->set("m_b", Value::tensor_de(l.m_b));
      c.map->set("v_b", Value::tensor_de(l.v_b));
      if (l.kind == Layer::Recorrente) {
        c.map->set("u", Value::tensor_de(l.u));
        c.map->set("m_u", Value::tensor_de(l.m_u));
        c.map->set("v_u", Value::tensor_de(l.v_u));
        c.map->set("recorrente", Value::texto(l.recorrente_tipo));
      }
      if (l.kind == Layer::Conv2d) {
        c.map->set("passo", Value::inteiro(l.passo));
        c.map->set("padding", Value::inteiro(l.padding));
        c.map->set("dilatacao", Value::inteiro(l.dilatacao));
      }
      if (l.kind == Layer::NormaLote) {
        c.map->set("media_running", Value::tensor_de(l.media_running));
        c.map->set("var_running", Value::tensor_de(l.var_running));
      }
      cl.list->push_back(std::move(c));
    }
    Value doc = Value::mapa();
    doc.map->set("formato", Value::texto("tilt-checkpoint"));
    doc.map->set("versao", Value::inteiro(1));
    doc.map->set("epoca", Value::inteiro(epoca_feita));
    doc.map->set("adam_t", Value::inteiro(passo_adam));
    doc.map->set("otimizador", Value::texto(cfg.otim));
    doc.map->set("taxa", Value::decimal(cfg.lr));
    doc.map->set("camadas", std::move(cl));
    std::ofstream out(caminho, std::ios::trunc);
    if (!out) fail(span, ctx + ": nao foi possivel gravar '" + caminho + "'");
    out << rt::json_dump(doc) << "\n";
  };

  int adam_t = 0;
  auto cluster_sync = [&](int epoch) {
    if (cfg.cluster_world <= 1) return;
    std::string cluster_error;
    if (!rt::cluster_preparar(cfg.cluster_dir, cluster_error)) {
      fail(span, ctx + ": cluster: " + cluster_error);
    }
    const std::filesystem::path rank_path =
        std::filesystem::path(cfg.cluster_dir) /
        ("rank-" + std::to_string(cfg.cluster_rank) + "-epoch-" + std::to_string(epoch) + ".json");
    salvar_checkpoint(rank_path.string(), epoch, adam_t);
    if (!rt::cluster_barreira(cfg.cluster_dir, cfg.cluster_rank, cfg.cluster_world, epoch * 2,
                              cfg.cluster_timeout, cluster_error)) {
      fail(span, ctx + ": cluster: " + cluster_error);
    }

    const std::filesystem::path aggregate_path =
        std::filesystem::path(cfg.cluster_dir) / ("aggregate-epoch-" + std::to_string(epoch) + ".json");
    std::vector<std::size_t> weight_indices;
    for (std::size_t i = 0; i < layers.size(); ++i) {
      if (camada_com_pesos(layers[i].kind)) weight_indices.push_back(i);
    }
    if (cfg.cluster_rank == 0) {
      std::ifstream first_file(std::filesystem::path(cfg.cluster_dir) /
                               ("rank-0-epoch-" + std::to_string(epoch) + ".json"));
      std::ostringstream first_text;
      first_text << first_file.rdbuf();
      Value aggregate = rt::json_parse(first_text.str());
      const Value* aggregate_layers = aggregate.kind == ValueKind::Mapa && aggregate.map
                                          ? aggregate.map->find("camadas")
                                          : nullptr;
      if (!aggregate_layers || aggregate_layers->kind != ValueKind::Lista || !aggregate_layers->list ||
          aggregate_layers->list->size() != weight_indices.size()) {
        fail(span, ctx + ": cluster: checkpoint agregado invalido");
      }
      std::vector<rt::Tensor> sum_w;
      std::vector<rt::Tensor> sum_b;
      std::vector<rt::Tensor> sum_u;
      sum_w.reserve(weight_indices.size());
      sum_b.reserve(weight_indices.size());
      sum_u.reserve(weight_indices.size());
      for (std::size_t i : weight_indices) {
        const Layer& layer = layers[i];
        sum_w.push_back(rt::Tensor::zeros(layer.w.shape));
        sum_b.push_back(rt::Tensor::zeros(layer.b.shape));
        if (layer.kind == Layer::Recorrente) sum_u.push_back(rt::Tensor::zeros(layer.u.shape));
        else sum_u.emplace_back();
      }
      for (int rank = 0; rank < cfg.cluster_world; ++rank) {
        const std::filesystem::path path = std::filesystem::path(cfg.cluster_dir) /
                                           ("rank-" + std::to_string(rank) + "-epoch-" +
                                            std::to_string(epoch) + ".json");
        std::ifstream input(path);
        if (!input) fail(span, ctx + ": cluster: nao foi possivel ler '" + path.string() + "'");
        std::ostringstream text;
        text << input.rdbuf();
        Value checkpoint = rt::json_parse(text.str());
        const Value* checkpoint_layers = checkpoint.kind == ValueKind::Mapa && checkpoint.map
                                             ? checkpoint.map->find("camadas")
                                             : nullptr;
        if (!checkpoint_layers || checkpoint_layers->kind != ValueKind::Lista ||
            !checkpoint_layers->list || checkpoint_layers->list->size() != weight_indices.size()) {
          fail(span, ctx + ": cluster: quantidade de camadas divergente no rank " +
                       std::to_string(rank));
        }
        for (std::size_t j = 0; j < weight_indices.size(); ++j) {
          const std::size_t i = weight_indices[j];
          const Value& layer = (*checkpoint_layers->list)[j];
          const Value* wv = layer.kind == ValueKind::Mapa && layer.map ? layer.map->find("w") : nullptr;
          const Value* bv = layer.kind == ValueKind::Mapa && layer.map ? layer.map->find("b") : nullptr;
          rt::Tensor w, b, u;
          if (!wv || !bv || !tensor_from_json(*wv, w) || !tensor_from_json(*bv, b) ||
              w.shape != sum_w[j].shape || b.shape != sum_b[j].shape) {
            fail(span, ctx + ": cluster: forma divergente no rank " + std::to_string(rank));
          }
          for (std::size_t k = 0; k < w.data.size(); ++k) sum_w[j].data[k] += w.data[k];
          for (std::size_t k = 0; k < b.data.size(); ++k) sum_b[j].data[k] += b.data[k];
          if (layers[i].kind == Layer::Recorrente) {
            const Value* uv = layer.kind == ValueKind::Mapa && layer.map ? layer.map->find("u") : nullptr;
            if (!uv || !tensor_from_json(*uv, u) || u.shape != sum_u[j].shape) {
              fail(span, ctx + ": cluster: forma recorrente divergente no rank " +
                           std::to_string(rank));
            }
            for (std::size_t k = 0; k < u.data.size(); ++k) sum_u[j].data[k] += u.data[k];
          }
        }
      }
      for (std::size_t j = 0; j < weight_indices.size(); ++j) {
        const std::size_t i = weight_indices[j];
        const float divisor = static_cast<float>(cfg.cluster_world);
        layers[i].w = std::move(sum_w[j]);
        layers[i].b = std::move(sum_b[j]);
        for (float& value : layers[i].w.data) value /= divisor;
        for (float& value : layers[i].b.data) value /= divisor;
        if (layers[i].kind == Layer::Recorrente) {
          layers[i].u = std::move(sum_u[j]);
          for (float& value : layers[i].u.data) value /= divisor;
        }
      }
      salvar_checkpoint(aggregate_path.string(), epoch, adam_t);
    }
    if (!rt::cluster_aguardar(aggregate_path.string(), cfg.cluster_timeout, cluster_error)) {
      fail(span, ctx + ": cluster: " + cluster_error);
    }
    if (cfg.cluster_rank != 0) {
      std::ifstream input(aggregate_path);
      std::ostringstream text;
      text << input.rdbuf();
      Value aggregate = rt::json_parse(text.str());
      const Value* aggregate_layers = aggregate.kind == ValueKind::Mapa && aggregate.map
                                          ? aggregate.map->find("camadas")
                                          : nullptr;
      if (!aggregate_layers || aggregate_layers->kind != ValueKind::Lista || !aggregate_layers->list ||
          aggregate_layers->list->size() != weight_indices.size()) {
        fail(span, ctx + ": cluster: checkpoint agregado invalido");
      }
      for (std::size_t j = 0; j < weight_indices.size(); ++j) {
        const std::size_t i = weight_indices[j];
        const Value& layer = (*aggregate_layers->list)[j];
        if (!layer.map) continue;
        rt::Tensor w, b;
        const Value* wv = layer.map->find("w");
        const Value* bv = layer.map->find("b");
        if (!wv || !bv || !tensor_from_json(*wv, w) || !tensor_from_json(*bv, b) ||
            w.shape != layers[i].w.shape || b.shape != layers[i].b.shape) {
          fail(span, ctx + ": cluster: forma agregada incompativel");
        }
        layers[i].w = std::move(w);
        layers[i].b = std::move(b);
        if (layers[i].kind == Layer::Recorrente) {
          rt::Tensor u;
          const Value* uv = layer.map->find("u");
          if (!uv || !tensor_from_json(*uv, u) || u.shape != layers[i].u.shape) {
            fail(span, ctx + ": cluster: forma recorrente agregada incompativel");
          }
          layers[i].u = std::move(u);
        }
      }
    }
    if (!rt::cluster_barreira(cfg.cluster_dir, cfg.cluster_rank, cfg.cluster_world, epoch * 2 + 1,
                              cfg.cluster_timeout, cluster_error)) {
      fail(span, ctx + ": cluster: " + cluster_error);
    }
  };
  int epoca_inicial = 1;
  int ultimo_ckpt = 0;
  if (!cfg.retomar.empty()) {
    std::ifstream f(cfg.retomar);
    if (!f) fail(span, ctx + ": checkpoint '" + cfg.retomar + "' nao encontrado");
    std::ostringstream ss;
    ss << f.rdbuf();
    Value doc = Value::nulo();
    try {
      doc = rt::json_parse(ss.str());
    } catch (...) {
      doc = Value::nulo();
    }
    const Value* formato = doc.kind == ValueKind::Mapa && doc.map ? doc.map->find("formato") : nullptr;
    const Value* camadas_ckpt =
        doc.kind == ValueKind::Mapa && doc.map ? doc.map->find("camadas") : nullptr;
    if (!formato || formato->kind != ValueKind::Texto || formato->s != "tilt-checkpoint" ||
        !camadas_ckpt || camadas_ckpt->kind != ValueKind::Lista || !camadas_ckpt->list) {
      fail(span, ctx + ": '" + cfg.retomar + "' nao e um checkpoint tilt-checkpoint");
    }
    const Value* otim_ckpt = doc.map->find("otimizador");
    if (otim_ckpt && otim_ckpt->kind == ValueKind::Texto && otim_ckpt->s != cfg.otim) {
      fail(span, ctx + ": checkpoint usa otimizador '" + otim_ckpt->s + "', mas o treino pede '" +
                     cfg.otim + "'");
    }
    std::size_t li = 0;
    for (const Value& c : *camadas_ckpt->list) {
      while (li < layers.size() && !camada_com_pesos(layers[li].kind)) ++li;
      if (li >= layers.size()) {
        fail(span, ctx + ": checkpoint tem mais camadas que o modelo");
      }
      Layer& l = layers[li];
      rt::Tensor w, b, m_w, v_w, m_b, v_b, u, m_u, v_u;
      const Value* wv = c.kind == ValueKind::Mapa && c.map ? c.map->find("w") : nullptr;
      const Value* bv = c.kind == ValueKind::Mapa && c.map ? c.map->find("b") : nullptr;
      const Value* mwv = c.kind == ValueKind::Mapa && c.map ? c.map->find("m_w") : nullptr;
      const Value* vwv = c.kind == ValueKind::Mapa && c.map ? c.map->find("v_w") : nullptr;
      const Value* mbv = c.kind == ValueKind::Mapa && c.map ? c.map->find("m_b") : nullptr;
      const Value* vbv = c.kind == ValueKind::Mapa && c.map ? c.map->find("v_b") : nullptr;
      const Value* uv = c.kind == ValueKind::Mapa && c.map ? c.map->find("u") : nullptr;
      const Value* muv = c.kind == ValueKind::Mapa && c.map ? c.map->find("m_u") : nullptr;
      const Value* vuv = c.kind == ValueKind::Mapa && c.map ? c.map->find("v_u") : nullptr;
      if (!wv || !bv || !mwv || !vwv || !mbv || !vbv || !tensor_from_json(*wv, w) ||
          !tensor_from_json(*bv, b) || !tensor_from_json(*mwv, m_w) ||
          !tensor_from_json(*vwv, v_w) || !tensor_from_json(*mbv, m_b) ||
          !tensor_from_json(*vbv, v_b) ||
          (l.kind == Layer::Recorrente &&
           (!uv || !muv || !vuv || !tensor_from_json(*uv, u) || !tensor_from_json(*muv, m_u) ||
            !tensor_from_json(*vuv, v_u)))) {
        fail(span, ctx + ": checkpoint invalido na camada " + std::to_string(li));
      }
      if (w.shape != l.w.shape || b.shape != l.b.shape || m_w.shape != l.w.shape ||
          v_w.shape != l.w.shape || m_b.shape != l.b.shape || v_b.shape != l.b.shape ||
          (l.kind == Layer::Recorrente &&
           (u.shape != l.u.shape || m_u.shape != l.u.shape || v_u.shape != l.u.shape))) {
        fail(span, ctx + ": forma do checkpoint incompativel na camada " + std::to_string(li));
      }
      if (l.kind == Layer::Conv2d) {
        const Value* pv = c.kind == ValueKind::Mapa && c.map ? c.map->find("passo") : nullptr;
        const Value* pad = c.kind == ValueKind::Mapa && c.map ? c.map->find("padding") : nullptr;
        const Value* dil = c.kind == ValueKind::Mapa && c.map ? c.map->find("dilatacao") : nullptr;
        if (pv && pv->is_number() && static_cast<std::int64_t>(pv->as_number()) != l.passo) {
          fail(span, ctx + ": passo do checkpoint difere da camada conv2d " + std::to_string(li));
        }
        if (pad && pad->is_number() && static_cast<std::int64_t>(pad->as_number()) != l.padding) {
          fail(span, ctx + ": padding do checkpoint difere da camada conv2d " + std::to_string(li));
        }
        if (dil && dil->is_number() && static_cast<std::int64_t>(dil->as_number()) != l.dilatacao) {
          fail(span, ctx + ": dilatacao do checkpoint difere da camada conv2d " + std::to_string(li));
        }
      }
      l.w = std::move(w);
      l.b = std::move(b);
      l.m_w = std::move(m_w);
      l.v_w = std::move(v_w);
      l.m_b = std::move(m_b);
      l.v_b = std::move(v_b);
      if (l.kind == Layer::Recorrente) {
        l.u = std::move(u);
        l.m_u = std::move(m_u);
        l.v_u = std::move(v_u);
      }
      if (l.kind == Layer::NormaLote) {
        rt::Tensor media, var;
        const Value* mv = c.kind == ValueKind::Mapa && c.map ? c.map->find("media_running") : nullptr;
        const Value* vv = c.kind == ValueKind::Mapa && c.map ? c.map->find("var_running") : nullptr;
        if (mv && vv && tensor_from_json(*mv, media) && tensor_from_json(*vv, var) &&
            media.shape == l.media_running.shape && var.shape == l.var_running.shape) {
          l.media_running = std::move(media);
          l.var_running = std::move(var);
        }
      }
      ++li;
    }
    while (li < layers.size() && !camada_com_pesos(layers[li].kind)) ++li;
    if (li < layers.size()) {
      fail(span, ctx + ": checkpoint tem menos camadas que o modelo");
    }
    const Value* epoca_ckpt = doc.map->find("epoca");
    const Value* adam_ckpt = doc.map->find("adam_t");
    const int epoca_feita =
        epoca_ckpt && epoca_ckpt->is_number() ? static_cast<int>(epoca_ckpt->as_number()) : 0;
    adam_t = adam_ckpt && adam_ckpt->is_number() ? static_cast<int>(adam_ckpt->as_number()) : 0;
    epoca_inicial = epoca_feita + 1;
    if (!cfg.silencioso) {
      out_ << "treino " << name << ": retomado de " << cfg.retomar << " (epoca " << epoca_feita
           << ")\n";
    }
    if (cfg.epocas < epoca_inicial) {
      fail(span, ctx + ": 'epocas' (" + std::to_string(cfg.epocas) +
                      ") deve passar a epoca do checkpoint (" + std::to_string(epoca_feita) + ")");
    }
  }

  float first_loss = 0.0F;
  float last_loss = 0.0F;
  double melhor_val = std::numeric_limits<double>::infinity();
  int sem_melhora = 0;
  int melhor_epoca = 0;
  std::vector<Layer> melhores_camadas;
  TreinoRelato relato;
  relato.classificacao = ce;
  relato.total = static_cast<int>(n);

  try {
    // Leitores de fluxo (so quando `fluxo_csv`).
    FluxoCSV fx;
    rt::ParquetFluxo pfx;
    struct VaoParquet {
      std::int64_t g_ini = 0;
      std::int64_t g_fim = 0;
    };
    std::vector<VaoParquet> vaos;
    std::vector<std::int64_t> base_grupo;
    if (fluxo) {
      if (cfg.fluxo_parquet) {
        try {
          pfx = rt::parquet_abrir_fluxo(cfg.fluxo_csv);
        } catch (const std::exception& e) {
          fail(span, ctx + ": " + e.what());
        }
        base_grupo.assign(static_cast<std::size_t>(pfx.grupos + 1), 0);
        for (std::int64_t g = 0; g < pfx.grupos; ++g) {
          base_grupo[static_cast<std::size_t>(g + 1)] =
              base_grupo[static_cast<std::size_t>(g)] +
              pfx.linhas_por_grupo[static_cast<std::size_t>(g)];
        }
        std::int64_t g = 0;
        while (g < pfx.grupos) {
          const std::int64_t g0 = g;
          std::int64_t nvao = 0;
          while (g < pfx.grupos && nvao < cfg.bloco) {
            nvao += pfx.linhas_por_grupo[static_cast<std::size_t>(g)];
            ++g;
          }
          vaos.push_back({g0, g});
        }
      } else {
        fx.caminho = cfg.fluxo_csv;
        fx.alvo = cfg.fluxo_alvo;
        fx.desloc.clear();
        for (std::int64_t global : fluxo_rows) {
          if (global < 0 || global >= static_cast<std::int64_t>(cfg.fluxo_desloc.size())) {
            fail(span, ctx + ": indice de shard fora dos offsets CSV");
          }
          fx.desloc.push_back(cfg.fluxo_desloc[static_cast<std::size_t>(global)]);
        }
        try {
          abrir_fluxo_csv(fx);
        } catch (const std::exception& e) {
          fail(span, ctx + ": " + e.what());
        }
      }
    }
    for (int epoch = epoca_inicial; epoch <= cfg.epocas; ++epoch) {
      // Taxa agendada da epoca (aplicar_grad le `lr` por referencia).
      if (cfg.agenda_tipo == "cosseno") {
        const double total = static_cast<double>(std::max(cfg.epocas - 1, 1));
        const double pi = std::acos(-1.0);
        lr = cfg.lr * 0.5 * (1.0 + std::cos(pi * static_cast<double>(epoch - 1) / total));
      } else if (cfg.agenda_tipo == "degrau") {
        lr = cfg.lr * std::pow(cfg.degrau_fator, (epoch - 1) / cfg.degrau_a_cada);
      } else {
        lr = cfg.lr;
      }
      float perda_epoca = 0.0F;
      double soma_val = 0.0;
      std::int64_t n_val_feito = 0;
      // Perda de validacao sobre um bloco (modo inferencia). Acumula soma.
      auto acumula_val = [&](const rt::Tensor& xvb, const std::vector<int>& yvb,
                             const std::vector<double>& yvfb) {
        const rt::Tensor pv = forward_layers(layers, xvb);
        const std::int64_t nv = xvb.shape[0];
        double soma = 0.0;
        if (ce) {
          for (std::int64_t i = 0; i < nv; ++i) {
            const int label = yvb[static_cast<std::size_t>(i)];
            soma -= std::log(
                std::max(pv.data[static_cast<std::size_t>(i * classes + label)], 1e-9F));
          }
        } else {
          for (std::int64_t i = 0; i < nv; ++i) {
            const double d = static_cast<double>(pv.data[static_cast<std::size_t>(i)]) -
                             yvfb[static_cast<std::size_t>(i)];
            soma += d * d;
          }
        }
        soma_val += soma;
        n_val_feito += nv;
      };
      // Um bloco = fatia de linhas (RAM: o treino todo; CSV: um bloco do arquivo).
      // A ordem dentro do bloco depende so de (semente, sufixo).
      auto processa_bloco = [&](const rt::Tensor& xb_full, const std::vector<int>& y_full,
                                const std::vector<double>& yf_full, std::int64_t n_full,
                                std::uint64_t seed_sufixo) {
      const std::int64_t tamanho_lote = std::min<std::int64_t>(cfg.lote, n_full);
      // Ordem refeita do zero: a permutacao depende so de (semente, sufixo),
      // entao retomar == treino continuo.
      std::vector<std::int64_t> ordem(static_cast<std::size_t>(n_full));
      for (std::int64_t i = 0; i < n_full; ++i) ordem[static_cast<std::size_t>(i)] = i;
      // Embaralhamento deterministico pela semente (so quando ha >1 lote).
      if (cfg.embaralhar && tamanho_lote < n_full) {
        std::mt19937_64 rng(cfg.seed_mistura + seed_sufixo);
        std::shuffle(ordem.begin(), ordem.end(), rng);
      }
      for (std::int64_t b0 = 0; b0 < n_full; b0 += tamanho_lote) {
        const std::int64_t nb = std::min(tamanho_lote, n_full - b0);
        std::vector<std::int64_t> idx;
        idx.reserve(static_cast<std::size_t>(nb));
        for (std::int64_t k = 0; k < nb; ++k)
          idx.push_back(ordem[static_cast<std::size_t>(b0 + k)]);
        const rt::Tensor xb = rt::fatiar_lote(xb_full, idx);
        std::vector<int> yb;
        std::vector<double> yfb;
        yb.reserve(static_cast<std::size_t>(nb));
        yfb.reserve(static_cast<std::size_t>(nb));
        for (std::int64_t i : idx) {
          yb.push_back(y_full[static_cast<std::size_t>(i)]);
          yfb.push_back(yf_full[static_cast<std::size_t>(i)]);
        }
        const float inv_nb = 1.0F / static_cast<float>(nb);
      // Forward with cached inputs per layer.
      std::vector<rt::Tensor> ins;
      ins.reserve(layers.size() + 1);
      std::vector<rt::RecurrentCache> recorrentes(layers.size());
      rt::Tensor cur = xb;
      for (std::size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
        Layer& l = layers[layer_idx];
        ins.push_back(cur);
        switch (l.kind) {
          case Layer::Dense:
            cur = rt::add(mm(cur, l.w), l.b);
            break;
          case Layer::Residual: {
            const rt::Tensor skip = cur;
            cur = rt::add(rt::add(mm(cur, l.w), l.b), skip);
            break;
          }
          case Layer::Embedding:
            cur = rt::embedding(cur, l.w);
            break;
          case Layer::Recorrente:
            cur = rt::recorrente(cur, l.w, l.u, l.b,
                                 l.recorrente_tipo == "lstm"
                                     ? rt::RecurrentKind::Lstm
                                     : (l.recorrente_tipo == "gru" ? rt::RecurrentKind::Gru
                                                                   : rt::RecurrentKind::Rnn),
                                 &recorrentes[layer_idx]);
            break;
          case Layer::Activation:
            cur = l.act == "relu" ? act_relu(cur) : rt::apply_unary(cur, l.act);
            break;
          case Layer::Softmax: cur = rt::softmax_last(cur); break;
          case Layer::LayerNorm: cur = rt::layer_norm_last(cur); break;
          case Layer::Dropout: break;
          case Layer::Conv2d:
            cur = rt::adicionar_vies_conv(rt::conv2d(cur, l.w, l.passo, l.padding, l.dilatacao), l.b);
            break;
          case Layer::NormaLote: {
            rt::norma_lote_estatisticas(cur, l.bn_media, l.bn_var);
            cur = rt::norma_lote(cur, l.w, l.b, l.bn_media, l.bn_var, 1e-5f, true);
            constexpr float momento = 0.1F;
            for (std::size_t k = 0; k < l.media_running.data.size(); ++k) {
              l.media_running.data[k] =
                  (1.0F - momento) * l.media_running.data[k] + momento * l.bn_media.data[k];
              l.var_running.data[k] =
                  (1.0F - momento) * l.var_running.data[k] + momento * l.bn_var.data[k];
            }
            break;
          }
          case Layer::Flatten: {
            if (cur.rank() < 2) fail(span, ctx + ": 'achatar' precisa de entrada com lote");
            std::int64_t resto = 1;
            for (std::size_t i = 1; i < cur.shape.size(); ++i) resto *= cur.shape[i];
            cur = rt::reshape(cur, {cur.shape[0], resto});
            break;
          }
          case Layer::MaxPool:
            cur = rt::maxpool2d(cur, l.janela, l.passo);
            break;
        }
      }
      const rt::Tensor& probs = cur;  // [N, C] (probs no CE, valores no MSE)

      // Perda e gradiente da saida.
      float loss = 0.0F;
      rt::Tensor grad = probs;
      if (ce) {
        // entropia_cruzada + softmax: dL/d(logits) = probs - onehot(y), medio.
        for (std::int64_t i = 0; i < nb; ++i) {
          const int label = yb[static_cast<std::size_t>(i)];
          const auto idx = static_cast<std::size_t>(i * classes + label);
          loss -= std::log(std::max(probs.data[idx], 1e-9F));
          for (std::int64_t c = 0; c < classes; ++c) {
            auto g = static_cast<std::size_t>(i * classes + c);
            grad.data[g] = (grad.data[g] - (c == label ? 1.0F : 0.0F)) * inv_nb;
          }
        }
        loss *= inv_nb;
      } else {
        // quadratica (regressao escalar): saida [N, 1], dL/dy = 2*(pred - alvo)/n.
        for (std::int64_t i = 0; i < nb; ++i) {
          const float d = probs.data[static_cast<std::size_t>(i)] -
                          static_cast<float>(yfb[static_cast<std::size_t>(i)]);
          loss += d * d;
          grad.data[static_cast<std::size_t>(i)] = 2.0F * d * inv_nb;
        }
        loss *= inv_nb;
      }
      perda_epoca += loss * static_cast<float>(nb);

      // Backward: skip the softmax layer (fused above); update Dense/Activation.
      ++adam_t;
      auto aplicar_grad = [&](Layer& destino, const rt::Tensor& dw, const rt::Tensor& db,
                              const rt::Tensor* du = nullptr) {
        if (cfg.otim == "adam") {
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
          step(destino.w, destino.m_w, destino.v_w, dw);
          step(destino.b, destino.m_b, destino.v_b, db);
          if (du) step(destino.u, destino.m_u, destino.v_u, *du);
        } else {
          for (std::size_t k = 0; k < destino.w.data.size(); ++k) {
            destino.w.data[k] -= static_cast<float>(lr) * dw.data[k];
          }
          for (std::size_t k = 0; k < destino.b.data.size(); ++k) {
            destino.b.data[k] -= static_cast<float>(lr) * db.data[k];
          }
          if (du) {
            for (std::size_t k = 0; k < destino.u.data.size(); ++k) {
              destino.u.data[k] -= static_cast<float>(lr) * du->data[k];
            }
          }
        }
      };
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
          const rt::Tensor& out_act =
              ins[static_cast<std::size_t>(li) + 1 < ins.size() ? static_cast<std::size_t>(li) + 1
                                                                : static_cast<std::size_t>(li)];
          for (std::size_t k = 0; k < grad.data.size(); ++k) {
            grad.data[k] *= activation_deriv(l.act, in.data[k],
                                             k < out_act.data.size() ? out_act.data[k] : 0.0F);
          }
          continue;
        }
        if (l.kind == Layer::Flatten) {
          grad = rt::reshape(grad, in.shape);
          continue;
        }
        if (l.kind == Layer::MaxPool) {
          grad = rt::maxpool2d_backward(in, grad, l.janela, l.passo);
          continue;
        }
        if (l.kind == Layer::Residual) {
          rt::Tensor dw = rt::matmul(rt::transpose2d(in), grad);
          rt::Tensor db = col_sum(grad);
          rt::Tensor grad_in = rt::matmul(grad, rt::transpose2d(l.w));
          aplicar_grad(l, dw, db);
          grad = rt::add(grad_in, grad);
          continue;
        }
        if (l.kind == Layer::Recorrente) {
          rt::Tensor grad_x, grad_w, grad_u, grad_b;
          const rt::RecurrentKind rk =
              l.recorrente_tipo == "lstm"
                  ? rt::RecurrentKind::Lstm
                  : (l.recorrente_tipo == "gru" ? rt::RecurrentKind::Gru : rt::RecurrentKind::Rnn);
          rt::recorrente_backward(in, l.w, l.u, l.b, rk, recorrentes[static_cast<std::size_t>(li)],
                                  grad, grad_x, grad_w, grad_u, grad_b);
          aplicar_grad(l, grad_w, grad_b, &grad_u);
          grad = std::move(grad_x);
          continue;
        }
        if (l.kind == Layer::Embedding) {
          rt::Tensor grad_tabela = rt::Tensor::zeros(l.w.shape);
          rt::embedding_backward(in, grad, grad_tabela);
          aplicar_grad(l, grad_tabela, rt::Tensor::zeros({0}));
          grad = rt::Tensor::zeros(in.shape);
          continue;
        }
        if (l.kind == Layer::NormaLote) {
          rt::Tensor grad_x, grad_gama, grad_beta;
          rt::norma_lote_backward(in, grad, l.w, l.bn_media, l.bn_var, 1e-5f, grad_x, grad_gama,
                                  grad_beta);
          aplicar_grad(l, grad_gama, grad_beta);
          grad = std::move(grad_x);
          continue;
        }
        if (l.kind == Layer::Conv2d) {
          rt::Tensor grad_x, grad_nucleo, grad_vies;
          rt::conv2d_backward(in, l.w, grad, l.passo, l.padding, l.dilatacao, grad_x, grad_nucleo, grad_vies);
          aplicar_grad(l, grad_nucleo, grad_vies);
          grad = std::move(grad_x);
          continue;
        }
        // Dense
        rt::Tensor dw = rt::matmul(rt::transpose2d(in), grad);  // [F_in, C]
        rt::Tensor db = col_sum(grad);
        rt::Tensor grad_in = rt::matmul(grad, rt::transpose2d(l.w));

        aplicar_grad(l, dw, db);
        grad = std::move(grad_in);
      }
      }  // mini-lotes do bloco
      };  // processa_bloco
      if (cfg.fluxo_csv.empty()) {
        processa_bloco(x_tr, y_tr, yf_tr, n_tr, static_cast<std::uint64_t>(epoch));
        if (cfg.f_val > 0.0) acumula_val(x_val, y_val, yf_val);
      } else {
        // Particiona um bloco lido entre treino/validacao e despacha.
        auto despacha_span = [&](const rt::Tensor& xb_span,
                                 const std::vector<std::int64_t>& linhas_span,
                                 std::uint64_t seed_sufixo) {
          rt::Tensor xb_tr;
          xb_tr.shape = {0, f};
          std::vector<int> yb_tr;
          std::vector<double> yfb_tr;
          rt::Tensor xb_va;
          xb_va.shape = {0, f};
          std::vector<int> yb_va;
          std::vector<double> yfb_va;
          const std::int64_t larg = xb_span.shape[1];
          for (std::size_t k = 0; k < linhas_span.size(); ++k) {
            const std::int64_t g = linhas_span[k];
            const float* base = xb_span.data.data() + k * static_cast<std::size_t>(larg);
            if (e_treino[static_cast<std::size_t>(g)]) {
              xb_tr.data.insert(xb_tr.data.end(), base, base + larg);
              yb_tr.push_back(y[static_cast<std::size_t>(g)]);
              yfb_tr.push_back(yf[static_cast<std::size_t>(g)]);
            } else {
              xb_va.data.insert(xb_va.data.end(), base, base + larg);
              yb_va.push_back(y[static_cast<std::size_t>(g)]);
              yfb_va.push_back(yf[static_cast<std::size_t>(g)]);
            }
          }
          xb_tr.shape[0] = static_cast<std::int64_t>(yb_tr.size());
          xb_va.shape[0] = static_cast<std::int64_t>(yb_va.size());
          if (!yb_tr.empty()) {
            processa_bloco(xb_tr, yb_tr, yfb_tr, static_cast<std::int64_t>(yb_tr.size()),
                           seed_sufixo);
          }
          if (!yb_va.empty()) acumula_val(xb_va, yb_va, yfb_va);
        };
        if (!cfg.fluxo_parquet) {
          // CSV: blocos em ordem embaralhada por epoca; dentro do bloco,
          // a ordem depende so de (semente, epoca, bloco).
          const std::int64_t n_blocos = (n + cfg.bloco - 1) / cfg.bloco;
          std::vector<std::int64_t> ob(static_cast<std::size_t>(n_blocos));
          for (std::int64_t i = 0; i < n_blocos; ++i) ob[static_cast<std::size_t>(i)] = i;
          if (cfg.embaralhar) {
            std::mt19937_64 rngb(cfg.seed_mistura + static_cast<std::uint64_t>(epoch) +
                                 0xBF58476D1CE4E5B9ULL);
            std::shuffle(ob.begin(), ob.end(), rngb);
          }
          for (std::int64_t bpos : ob) {
            rt::Tensor xb_file;
            std::vector<std::int64_t> linhas;
            try {
              ler_bloco_fluxo(fx, bpos * cfg.bloco, cfg.bloco, xb_file, linhas);
            } catch (const std::exception& e) {
              fail(span, ctx + ": " + e.what());
            }
            despacha_span(xb_file, linhas, static_cast<std::uint64_t>(epoch) * 1000003ULL +
                                                     static_cast<std::uint64_t>(bpos));
          }
        } else {
          // Parquet: vaos de row groups em ordem embaralhada por epoca.
          std::vector<std::int64_t> ov(vaos.size());
          for (std::size_t i = 0; i < vaos.size(); ++i) ov[i] = static_cast<std::int64_t>(i);
          if (cfg.embaralhar) {
            std::mt19937_64 rngv(cfg.seed_mistura + static_cast<std::uint64_t>(epoch) +
                                 0x94D049BB133111EBULL);
            std::shuffle(ov.begin(), ov.end(), rngv);
          }
          for (std::int64_t vpos : ov) {
            const VaoParquet& vao = vaos[static_cast<std::size_t>(vpos)];
            rt::Tensor xb_span;
            xb_span.shape = {0, f};
            std::vector<std::int64_t> linhas_span;
            for (std::int64_t g = vao.g_ini; g < vao.g_fim; ++g) {
              Value tab;
              try {
                tab = rt::parquet_ler_grupo_fluxo(pfx, g);
              } catch (const std::exception& e) {
                fail(span, ctx + ": " + e.what());
              }
              if (!tab.list) continue;
              const std::int64_t base = base_grupo[static_cast<std::size_t>(g)];
              std::int64_t local = 0;
              for (const Value& row : *tab.list) {
                const std::int64_t global = base + local;
                const std::int64_t local_row =
                    global >= 0 && global < static_cast<std::int64_t>(local_row_for_global.size())
                        ? local_row_for_global[static_cast<std::size_t>(global)]
                        : -1;
                ++local;
                if (local_row < 0) continue;
                for (const std::string& c : cfg.fluxo_atributos) {
                  const Value* cell =
                      row.kind == ValueKind::Mapa && row.map ? row.map->find(c) : nullptr;
                  xb_span.data.push_back(cell ? static_cast<float>(cell->as_number()) : 0.0F);
                }
                linhas_span.push_back(local_row);
              }
            }
            xb_span.shape[0] = static_cast<std::int64_t>(linhas_span.size());
            if (!linhas_span.empty()) {
              despacha_span(xb_span, linhas_span, static_cast<std::uint64_t>(epoch) * 1000003ULL +
                                                        static_cast<std::uint64_t>(vpos) +
                                                        0x9E3779B97F4A7C15ULL);
            }
          }
        }
      }
      cluster_sync(epoch);
      perda_epoca /= static_cast<float>(n_tr);
      if (epoch == epoca_inicial) first_loss = perda_epoca;
      last_loss = perda_epoca;
      if (cfg.verbose && !cfg.silencioso &&
          (epoch == epoca_inicial || epoch % std::max(1, cfg.epocas / 10) == 0)) {
        out_ << "  epoca " << epoch << " perda " << perda_epoca << "\n";
      }
      if (!cfg.checkpoint.empty() && cfg.a_cada > 0 && epoch % cfg.a_cada == 0) {
        salvar_checkpoint(cfg.checkpoint, epoch, adam_t);
        ultimo_ckpt = epoch;
        if (!cfg.silencioso) {
          out_ << "treino " << name << ": checkpoint salvo em " << cfg.checkpoint << " (epoca "
               << epoch << ")\n";
        }
      }
      relato.epocas_feitas = epoch;
      double perda_val_epoca = -1.0;
      if (cfg.f_val > 0.0) {
        // Perda de validacao em modo inferencia (usa media/var correntes do BN).
        perda_val_epoca = n_val_feito > 0 ? soma_val / static_cast<double>(n_val_feito) : 0.0;
      }
      if (cfg.ao_epoca) {
        Env callback;
        callback.parent = &root_;
        callback.vars["epoca"] = Value::inteiro(epoch);
        callback.vars["perda"] = Value::decimal(perda_epoca);
        callback.vars["perda_validacao"] =
            perda_val_epoca >= 0.0 ? Value::decimal(perda_val_epoca) : Value::nulo();
        callback.vars["taxa"] = Value::decimal(lr);
        callback.vars["modelo"] = Value::texto(name);
        exec_block(*cfg.ao_epoca, callback);
      }
      if (cfg.f_val > 0.0) {
        const double perda_val = perda_val_epoca;
        if (perda_val < melhor_val - cfg.melhorar_min) {
          melhor_val = perda_val;
          melhor_epoca = epoch;
          sem_melhora = 0;
          melhores_camadas = layers;
        } else if (++sem_melhora >= cfg.paciencia && cfg.paciencia > 0) {
          layers = std::move(melhores_camadas);
          relato.parou_cedo = true;
          relato.melhor_epoca = melhor_epoca;
          if (!cfg.silencioso) {
            out_ << "treino " << name << ": parada cedo na epoca " << epoch << " (melhor: "
                 << melhor_epoca << ")\n";
          }
          break;
        }
      }
    }

    relato.primeira = first_loss;
    relato.ultima = last_loss;
    relato.melhor_epoca = melhor_epoca;
    if (ce) {
      // Acuracia final sobre o treino todo (RAM de uma vez; fluxo em passada extra).
      int correct = 0;
      if (cfg.fluxo_csv.empty()) {
        // Final training-set accuracy.
        rt::Tensor probs = forward_layers(layers, x);
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
      } else {
        auto pontua_bloco = [&](const rt::Tensor& xb_file, const std::vector<std::int64_t>& linhas) {
          if (linhas.empty()) return;
          const rt::Tensor probs = forward_layers(layers, xb_file);
          for (std::size_t k = 0; k < linhas.size(); ++k) {
            std::int64_t best = 0;
            for (std::int64_t c = 1; c < classes; ++c) {
              if (probs.data[k * static_cast<std::size_t>(classes) + static_cast<std::size_t>(c)] >
                  probs.data[k * static_cast<std::size_t>(classes) + static_cast<std::size_t>(best)]) {
                best = c;
              }
            }
            if (best == y[static_cast<std::size_t>(linhas[k])]) ++correct;
          }
        };
        if (!cfg.fluxo_parquet) {
          const std::int64_t n_blocos = (n + cfg.bloco - 1) / cfg.bloco;
          for (std::int64_t b = 0; b < n_blocos; ++b) {
            rt::Tensor xb_file;
            std::vector<std::int64_t> linhas;
            try {
              ler_bloco_fluxo(fx, b * cfg.bloco, cfg.bloco, xb_file, linhas);
            } catch (const std::exception& e) {
              fail(span, ctx + ": " + e.what());
            }
            pontua_bloco(xb_file, linhas);
          }
        } else {
          for (const VaoParquet& vao : vaos) {
            rt::Tensor xb_span;
            xb_span.shape = {0, f};
            std::vector<std::int64_t> linhas_span;
            for (std::int64_t g = vao.g_ini; g < vao.g_fim; ++g) {
              Value tab;
              try {
                tab = rt::parquet_ler_grupo_fluxo(pfx, g);
              } catch (const std::exception& e) {
                fail(span, ctx + ": " + e.what());
              }
              if (!tab.list) continue;
              const std::int64_t base = base_grupo[static_cast<std::size_t>(g)];
              std::int64_t local = 0;
              for (const Value& row : *tab.list) {
                for (const std::string& c : cfg.fluxo_atributos) {
                  const Value* cell =
                      row.kind == ValueKind::Mapa && row.map ? row.map->find(c) : nullptr;
                  xb_span.data.push_back(cell ? static_cast<float>(cell->as_number()) : 0.0F);
                }
                linhas_span.push_back(base + local);
                ++local;
              }
            }
            xb_span.shape[0] = static_cast<std::int64_t>(linhas_span.size());
            pontua_bloco(xb_span, linhas_span);
          }
        }
      }
      relato.acertos = correct;
      if (!cfg.silencioso) {
        out_ << "treino " << name << ": perda caiu " << (last_loss < first_loss ? "sim" : "nao")
             << " | acuracia " << correct << "/" << n << "\n";
      }
    } else {
      char mse_buf[64];
      std::snprintf(mse_buf, sizeof(mse_buf), "%.4f", last_loss);
      if (!cfg.silencioso) {
        out_ << "treino " << name << ": perda caiu " << (last_loss < first_loss ? "sim" : "nao")
             << " | mse " << mse_buf << "\n";
      }
    }
    if (!cfg.checkpoint.empty() && cfg.epocas != ultimo_ckpt) {
      salvar_checkpoint(cfg.checkpoint, cfg.epocas, adam_t);
      if (!cfg.silencioso) {
        out_ << "treino " << name << ": checkpoint salvo em " << cfg.checkpoint << " (epoca "
             << cfg.epocas << ")\n";
      }
    }
  } catch (const std::exception& e) {
    fail(span, ctx + ": " + e.what());
  }

  relato.camadas = std::move(layers);
  return relato;
}

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
  TreinoCfg tcfg;
  DadosTreino dt = ler_dados_treino(*dados->value, "treino '" + name + "'", dados->value->span, tcfg);
  ler_cfg_treino(cfg, dt.n, "treino '" + name + "'", decl.span, tcfg);
  TreinoRelato r = treinar_nucleo(*mit->second, std::move(dt.x), std::move(dt.yf), tcfg,
                                  "treino '" + name + "'", decl.span);
  {
    std::lock_guard<std::mutex> lk(model_cache_mutex_);
    model_cache_[name] = std::move(r.camadas);
  }
}

void Interpreter::run_busca(const Item& decl) {
  const std::string name = decl_name(decl);
  if (!decl.block) fail(decl.span, "busca '" + name + "' sem configuracao");
  const ast::Block& cfg = *decl.block;
  const std::string ctx = "busca '" + name + "'";

  const Item* fm = find_field(cfg, "modelo");
  if (!fm || !fm->value || fm->value->kind != ExprKind::Name) {
    fail(decl.span, ctx + ": falta 'modelo: <Nome>' do modelo declarado");
  }
  auto mit = entities_.find(fm->value->text);
  if (mit == entities_.end() || mit->second->key != "modelo") {
    fail(fm->value->span, ctx + ": nao existe 'modelo " + fm->value->text + "'");
  }
  const Item* dados = find_field(cfg, "dados");
  if (!dados || !dados->value) fail(decl.span, ctx + ": falta 'dados:'");
  TreinoCfg base;
  DadosTreino dt0 = ler_dados_treino(*dados->value, ctx, dados->value->span, base);
  const rt::Tensor& x0 = dt0.x;
  const std::vector<double>& yf0 = dt0.yf;
  const std::int64_t n = dt0.n;

  ler_cfg_treino(cfg, n, ctx, decl.span, base);
  if (base.ao_epoca) {
    fail(decl.span, ctx + ": 'ao_epoca' nao se aplica a busca (use treino)");
  }
  if (!base.checkpoint.empty() || !base.retomar.empty()) {
    fail(decl.span, ctx + ": 'checkpoint'/'retomar' nao se aplicam a busca (use treino)");
  }
  base.silencioso = true;
  std::string criterio = "perda";
  if (const Item* fc = find_field(cfg, "criterio"); fc && fc->value) {
    if (fc->value->kind != ExprKind::Name ||
        (fc->value->text != "perda" && fc->value->text != "acuracia")) {
      fail(decl.span, ctx + ": 'criterio' deve ser perda | acuracia");
    }
    criterio = fc->value->text;
  }
  if (criterio == "acuracia" && base.perda != "entropia_cruzada") {
    fail(decl.span, ctx + ": 'criterio: acuracia' exige perda 'entropia_cruzada'");
  }

  // Grade: mapa ordenado chave -> lista de valores (inline ou em bloco).
  const Item* gd = find_field(cfg, "grade");
  if (!gd || (!gd->value && !gd->block)) {
    fail(decl.span, ctx + ": falta 'grade:' com ao menos uma chave e lista de valores");
  }
  struct Par {
    std::string chave;
    const ast::Expr* valor;
  };
  std::vector<Par> pares;
  if (gd->value && gd->value->kind == ExprKind::MapLit) {
    for (const auto& e : gd->value->entries) pares.push_back({e.key, e.value.get()});
  } else if (gd->block) {
    for (const auto& it : gd->block->items) {
      if (it && it->kind == ItemKind::Field) pares.push_back({it->key, it->value.get()});
    }
  }
  if (pares.empty()) {
    fail(decl.span, ctx + ": falta 'grade:' com ao menos uma chave e lista de valores");
  }
  struct Dimensao {
    std::string chave;
    std::vector<const ast::Expr*> valores;
  };
  std::vector<Dimensao> grade;
  for (const auto& e : pares) {
    if (e.chave != "taxa" && e.chave != "lote" && e.chave != "otimizador" && e.chave != "semente" &&
        e.chave != "epocas") {
      fail(decl.span, ctx + ": chave '" + e.chave + "' desconhecida em 'grade' (use taxa | lote | otimizador | semente | epocas)");
    }
    if (!e.valor || e.valor->kind != ExprKind::ListLit || e.valor->elems.empty()) {
      fail(decl.span, ctx + ": 'grade." + e.chave + "' deve ser lista nao vazia");
    }
    Dimensao d;
    d.chave = e.chave;
    for (const auto& v : e.valor->elems) {
      if (!v) fail(decl.span, ctx + ": 'grade." + e.chave + "' tem valor vazio");
      if (e.chave == "taxa") {
        if (v->kind != ExprKind::DecimalLit && v->kind != ExprKind::IntLit) {
          fail(decl.span, ctx + ": 'grade.taxa' espera numeros");
        }
      } else if (e.chave == "otimizador") {
        if (v->kind != ExprKind::Name || (v->text != "sgd" && v->text != "adam")) {
          fail(decl.span, ctx + ": 'grade.otimizador' espera sgd | adam");
        }
      } else {
        if (v->kind != ExprKind::IntLit) {
          fail(decl.span, ctx + ": 'grade." + e.chave + "' espera inteiros");
        }
      }
      d.valores.push_back(v.get());
    }
    grade.push_back(std::move(d));
  }
  std::size_t total = 1;
  for (const Dimensao& d : grade) total *= d.valores.size();
  if (total > 64) {
    fail(decl.span, ctx + ": grade grande demais (" + std::to_string(total) + " > 64 combinacoes)");
  }

  auto rotulo_valor = [&](const std::string& chave, const ast::Expr* v) {
    if (chave == "otimizador") return v->text;
    if (v->kind == ExprKind::DecimalLit) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.4g", std::strtod(v->text.c_str(), nullptr));
      return std::string(buf);
    }
    return v->text;
  };
  out_ << "busca " << name << ": " << total << " combinacoes\n";
  double melhor_nota = 0.0;
  bool tem_melhor = false;
  std::size_t melhor_i = 0;
  std::string melhor_rotulo;
  TreinoRelato melhor_relato;
  std::vector<std::size_t> pos(grade.size(), 0);
  for (std::size_t comb = 0; comb < total; ++comb) {
    TreinoCfg tentativa = base;
    std::string rotulo;
    for (std::size_t k = 0; k < grade.size(); ++k) {
      const ast::Expr* v = grade[k].valores[pos[k]];
      if (!rotulo.empty()) rotulo += " ";
      rotulo += grade[k].chave + "=" + rotulo_valor(grade[k].chave, v);
      if (grade[k].chave == "taxa") {
        tentativa.lr = std::strtod(v->text.c_str(), nullptr);
      } else if (grade[k].chave == "lote") {
        tentativa.lote = static_cast<int>(std::strtol(v->text.c_str(), nullptr, 10));
        if (tentativa.lote < 1) fail(decl.span, ctx + ": 'grade.lote' deve ser >= 1");
      } else if (grade[k].chave == "otimizador") {
        tentativa.otim = v->text;
      } else if (grade[k].chave == "semente") {
        const std::int64_t s = std::stoll(v->text);
        if (s < 0) fail(decl.span, ctx + ": 'grade.semente' deve ser >= 0");
        tentativa.seed_init = static_cast<std::uint64_t>(s);
        tentativa.seed_mistura = static_cast<std::uint64_t>(s);
      } else if (grade[k].chave == "epocas") {
        tentativa.epocas = static_cast<int>(std::strtol(v->text.c_str(), nullptr, 10));
        if (tentativa.epocas < 1) fail(decl.span, ctx + ": 'grade.epocas' deve ser >= 1");
      }
    }
    TreinoRelato r = treinar_nucleo(*mit->second, x0, yf0, tentativa, ctx, decl.span);
    char perda_buf[32];
    std::snprintf(perda_buf, sizeof perda_buf, "%.4f", r.ultima);
    out_ << "  #" << (comb + 1) << " " << rotulo << " -> perda " << perda_buf;
    double nota = 0.0;
    if (criterio == "acuracia") {
      nota = r.total > 0 ? static_cast<double>(r.acertos) / static_cast<double>(r.total) : 0.0;
      out_ << " (acuracia " << r.acertos << "/" << r.total << ")";
    }
    out_ << "\n";
    const bool ganha =
        !tem_melhor || (criterio == "perda" ? r.ultima < melhor_nota : nota > melhor_nota);
    if (ganha) {
      tem_melhor = true;
      melhor_i = comb;
      melhor_nota = criterio == "perda" ? r.ultima : nota;
      melhor_rotulo = rotulo;
      melhor_relato = std::move(r);
    }
    for (std::size_t k = grade.size(); k-- > 0;) {
      if (++pos[k] < grade[k].valores.size()) break;
      pos[k] = 0;
    }
  }
  {
    std::lock_guard<std::mutex> lk(model_cache_mutex_);
    model_cache_[mit->first] = std::move(melhor_relato.camadas);
  }
  out_ << "busca " << name << ": melhor #" << (melhor_i + 1) << " (" << melhor_rotulo << ") ";
  if (criterio == "perda") {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", melhor_nota);
    out_ << "perda " << buf << "\n";
  } else {
    out_ << "acuracia " << melhor_relato.acertos << "/" << melhor_relato.total << "\n";
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

// Celula numerica com imputacao: ausente/nulo usa a media ajustada quando a
// coluna esta em `imputar:`; sem imputacao, mantem o erro que ensina.
static double exp_celula_num(const Interpreter::ExpModel& m, const Value& row,
                             const std::string& col) {
  const Value* c = row.map->find(col);
  if ((!c || c->kind == ValueKind::Nulo)) {
    auto it = m.imputar_num.find(col);
    if (it != m.imputar_num.end()) return it->second;
  }
  if (!c) throw std::runtime_error("prever: falta o atributo '" + col + "'");
  return celula_num(*c, col);
}

// Chave categorica com imputacao (moda ajustada).
static std::string exp_chave_cat(const Interpreter::ExpModel& m, const Value& row,
                                 const std::string& col) {
  const Value* c = row.map->find(col);
  if (!c || c->kind == ValueKind::Nulo) {
    auto it = m.imputar_cat.find(col);
    if (it != m.imputar_cat.end()) return it->second;
    if (!c) throw std::runtime_error("prever: falta o atributo '" + col + "'");
    throw std::runtime_error("coluna categorica '" + col + "' tem valor ausente/nulo");
  }
  return chave_valor(*c);
}

// Ajusta o pipeline do usuario no treino: imputacao, categorias (ordem de
// aparição) e media/desvio dos atributos em `padronizar:`.
static void exp_ajustar(Interpreter::ExpModel& m, const std::vector<Value>& treino,
                        const std::vector<std::string>& std_cols) {
  // Imputacao primeiro: preenche ausentes/nulos antes das demais etapas.
  m.imputar_num.clear();
  m.imputar_cat.clear();
  for (const std::string& col : m.imputar_cols) {
    const bool quente =
        std::find(m.quentes.begin(), m.quentes.end(), col) != m.quentes.end();
    if (quente) {
      std::map<std::string, std::size_t> cont;
      std::string moda;
      std::size_t melhor = 0;
      for (const Value& r : treino) {
        const Value* c = r.map->find(col);
        if (!c || c->kind == ValueKind::Nulo) continue;
        const std::string k = chave_valor(*c);
        const std::size_t n = ++cont[k];
        if (n > melhor) {
          melhor = n;
          moda = k;
        }
      }
      if (melhor == 0) throw std::runtime_error("imputar: coluna '" + col + "' so tem nulo no treino");
      m.imputar_cat[col] = moda;
    } else {
      double soma = 0.0;
      std::size_t n = 0;
      for (const Value& r : treino) {
        const Value* c = r.map->find(col);
        if (!c || c->kind == ValueKind::Nulo) continue;
        soma += celula_num(*c, col);
        ++n;
      }
      if (n == 0) throw std::runtime_error("imputar: coluna '" + col + "' so tem nulo no treino");
      m.imputar_num[col] = soma / static_cast<double>(n);
    }
  }
  m.categorias.clear();
  for (const std::string& col : m.quentes) {
    std::vector<std::string> cats;
    for (const Value& r : treino) {
      const Value* c = r.map->find(col);
      std::string k;
      if (!c || c->kind == ValueKind::Nulo) {
        auto it = m.imputar_cat.find(col);
        if (it == m.imputar_cat.end()) {
          throw std::runtime_error("coluna categorica '" + col + "' tem valor ausente/nulo");
        }
        k = it->second;
      } else {
        k = chave_valor(*c);
      }
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
    for (const Value& r : treino) soma += exp_celula_num(m, r, m.numericas[j]);
    const double media = soma / static_cast<double>(treino.size());
    double var = 0.0;
    for (const Value& r : treino) {
      const double d = exp_celula_num(m, r, m.numericas[j]) - media;
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
    double v = exp_celula_num(m, row, m.numericas[j]);
    if (m.usa_std[j]) v = (v - m.medias[j]) / m.desvios[j];
    x.push_back(v);
  }
  for (const std::string& col : m.quentes) {
    const std::string k = exp_chave_cat(m, row, col);
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

// ------------------------------------------------------- arvores (CART)

using NoA = Interpreter::ExpModel::NoArvore;
using ArvoreA = Interpreter::ExpModel::Arvore;

// Impureza de um conjunto de rotulos (ids de classe como double): Gini para
// classificacao; soma dos quadrados / n (MSE * n) para regressao.
static double arv_impureza(const std::vector<std::size_t>& idx, const std::vector<double>& y,
                           bool classificacao, int nclasses) {
  if (idx.size() < 2) return 0.0;
  if (!classificacao) {
    double media = 0.0;
    for (std::size_t i : idx) media += y[i];
    media /= static_cast<double>(idx.size());
    double ss = 0.0;
    for (std::size_t i : idx) {
      const double d = y[i] - media;
      ss += d * d;
    }
    return ss;
  }
  std::vector<std::size_t> cont(static_cast<std::size_t>(nclasses), 0);
  for (std::size_t i : idx) cont[static_cast<std::size_t>(y[i])] += 1;
  double gini = 1.0;
  for (std::size_t c : cont) {
    const double p = static_cast<double>(c) / static_cast<double>(idx.size());
    gini -= p * p;
  }
  return gini * static_cast<double>(idx.size());  // ponderado pelo tamanho
}

static double arv_folha(const std::vector<std::size_t>& idx, const std::vector<double>& y,
                        bool classificacao) {
  if (!classificacao) {
    double media = 0.0;
    for (std::size_t i : idx) media += y[i];
    return idx.empty() ? 0.0 : media / static_cast<double>(idx.size());
  }
  std::map<double, std::size_t> votos;
  for (std::size_t i : idx) ++votos[y[i]];
  double melhor = y[idx[0]];
  std::size_t nv = 0;
  for (const auto& [v, n] : votos) {
    if (n > nv) {
      nv = n;
      melhor = v;
    }
  }
  return melhor;
}

// Treina uma arvore CART gulosa (limiares nos pontos medios, ganho ponderado).
// `nteste` = atributos sorteados por no (0 = todos).
static ArvoreA arv_treinar(const std::vector<std::vector<double>>& x, const std::vector<double>& y,
                           bool classificacao, int nclasses, int max_prof, int nteste,
                           std::mt19937& rng) {
  ArvoreA t;
  const std::size_t f = x.empty() ? 0 : x[0].size();
  std::function<int(std::vector<std::size_t>, int)> monta = [&](std::vector<std::size_t> idx,
                                                                int prof) -> int {
    const int id = static_cast<int>(t.nos.size());
    t.nos.push_back(NoA{});
    auto folha = [&](double valor) -> int {
      t.nos[static_cast<std::size_t>(id)].valor = valor;
      return id;
    };
    bool pura = true;
    for (std::size_t k = 1; k < idx.size(); ++k) {
      if (y[idx[k]] != y[idx[0]]) {
        pura = false;
        break;
      }
    }
    if (pura || idx.size() < 2 || (max_prof > 0 && prof >= max_prof) || f == 0) {
      return folha(arv_folha(idx, y, classificacao));
    }
    // Atributos candidatos do no.
    std::vector<std::size_t> atts(f);
    for (std::size_t j = 0; j < f; ++j) atts[j] = j;
    if (nteste > 0 && static_cast<std::size_t>(nteste) < f) {
      std::shuffle(atts.begin(), atts.end(), rng);
      atts.resize(static_cast<std::size_t>(nteste));
    }
    const double base = arv_impureza(idx, y, classificacao, nclasses);
    int melhor_a = -1;
    double melhor_l = 0.0, melhor_ganho = 0.0;
    std::vector<std::size_t> ordem = idx;
    for (std::size_t a : atts) {
      std::sort(ordem.begin(), ordem.end(),
                [&](std::size_t p, std::size_t q) { return x[p][a] < x[q][a]; });
      for (std::size_t k = 1; k < ordem.size(); ++k) {
        const double va = x[ordem[k - 1]][a], vb = x[ordem[k]][a];
        if (va == vb) continue;
        std::vector<std::size_t> esq(ordem.begin(), ordem.begin() + k);
        std::vector<std::size_t> dir(ordem.begin() + k, ordem.end());
        const double ganho =
            base - arv_impureza(esq, y, classificacao, nclasses) -
            arv_impureza(dir, y, classificacao, nclasses);
        if (ganho > melhor_ganho) {
          melhor_ganho = ganho;
          melhor_a = static_cast<int>(a);
          melhor_l = (va + vb) * 0.5;
        }
      }
    }
    if (melhor_a < 0) {
      return folha(arv_folha(idx, y, classificacao));
    }
    std::vector<std::size_t> esq, dir;
    for (std::size_t i : idx) {
      if (x[i][static_cast<std::size_t>(melhor_a)] <= melhor_l) esq.push_back(i);
      else dir.push_back(i);
    }
    // Por indice (sem referencia): a recursao pode realocar `t.nos`.
    t.nos[static_cast<std::size_t>(id)].folha = false;
    t.nos[static_cast<std::size_t>(id)].atributo = melhor_a;
    t.nos[static_cast<std::size_t>(id)].limiar = melhor_l;
    const int e = monta(std::move(esq), prof + 1);
    const int d = monta(std::move(dir), prof + 1);
    t.nos[static_cast<std::size_t>(id)].esq = e;
    t.nos[static_cast<std::size_t>(id)].dir = d;
    return id;
  };
  std::vector<std::size_t> tudo(x.size());
  for (std::size_t i = 0; i < tudo.size(); ++i) tudo[i] = i;
  monta(std::move(tudo), 0);
  return t;
}

static double arv_prever(const ArvoreA& t, const std::vector<double>& x) {
  int id = 0;
  while (id >= 0 && static_cast<std::size_t>(id) < t.nos.size() && !t.nos[static_cast<std::size_t>(id)].folha) {
    const NoA& no = t.nos[static_cast<std::size_t>(id)];
    id = x[static_cast<std::size_t>(no.atributo)] <= no.limiar ? no.esq : no.dir;
  }
  if (id < 0 || static_cast<std::size_t>(id) >= t.nos.size()) return 0.0;
  return t.nos[static_cast<std::size_t>(id)].valor;
}

// Hiperparametros do bloco `modelo:` do experimento.
struct ExpHip {
  double taxa = 0.5;
  int epocas = 500;
  int vizinhos = 5;
  int grupos = -1;
  int arvores = 50;
  int profundidade = 0;  // 0 = padrao do modelo
  double custo = 1.0;    // C do svm
};

// Ajuste por modelo (usado no fluxo principal e na validacao cruzada).
static void exp_treinar_modelo(Interpreter::ExpModel& m, const std::vector<std::vector<double>>& xt,
                               const std::vector<int>& yt, const std::vector<double>& ytr,
                               std::size_t largura, const ExpHip& hp, std::mt19937& rng) {
  using EM = Interpreter::ExpModel;
  const std::string& kind = m.kind;
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
    const std::size_t f = largura;
    const std::size_t K = m.classes.size();
    std::vector<std::vector<double>> z;
    z.reserve(xt.size());
    for (const auto& r : xt) z.push_back(exp_std_aplicar(m, r));
    if (K == 2) {
      m.pesos.assign(f + 1, 0.0);
      for (int e = 0; e < hp.epocas; ++e) {
        std::vector<double> g(f + 1, 0.0);
        for (std::size_t i = 0; i < z.size(); ++i) {
          double s = m.pesos[f];
          for (std::size_t j = 0; j < f; ++j) s += m.pesos[j] * z[i][j];
          const double p = sigmoide(s);
          const double err = p - static_cast<double>(yt[i]);
          for (std::size_t j = 0; j < f; ++j) g[j] += err * z[i][j];
          g[f] += err;
        }
        const double inv = hp.taxa / static_cast<double>(z.size());
        for (std::size_t j = 0; j <= f; ++j) m.pesos[j] -= inv * g[j];
      }
    } else {
      // Multinomial: softmax sobre K saidas (GD em lote).
      m.pesos_multi.assign((f + 1) * K, 0.0);
      auto W = [&](std::size_t c, std::size_t j) -> double& { return m.pesos_multi[c * (f + 1) + j]; };
      std::vector<double> s(K);
      for (int e = 0; e < hp.epocas; ++e) {
        std::vector<double> g((f + 1) * K, 0.0);
        for (std::size_t i = 0; i < z.size(); ++i) {
          double mx = 0.0;
          for (std::size_t c = 0; c < K; ++c) {
            s[c] = W(c, f);
            for (std::size_t j = 0; j < f; ++j) s[c] += W(c, j) * z[i][j];
            if (c == 0 || s[c] > mx) mx = s[c];
          }
          double soma = 0.0;
          for (std::size_t c = 0; c < K; ++c) {
            s[c] = std::exp(s[c] - mx);
            soma += s[c];
          }
          for (std::size_t c = 0; c < K; ++c) {
            const double err = s[c] / soma - (static_cast<int>(c) == yt[i] ? 1.0 : 0.0);
            for (std::size_t j = 0; j < f; ++j) g[c * (f + 1) + j] += err * z[i][j];
            g[c * (f + 1) + f] += err;
          }
        }
        const double inv = hp.taxa / static_cast<double>(z.size());
        for (std::size_t k = 0; k < g.size(); ++k) m.pesos_multi[k] -= inv * g[k];
      }
    }
  } else if (kind == "knn") {
    exp_std_ajustar(m, xt);
    m.vizinhos = hp.vizinhos;
    if (m.vizinhos < 1) throw std::runtime_error("'vizinhos' >= 1");
    if (static_cast<std::size_t>(m.vizinhos) > xt.size()) {
      throw std::runtime_error("'vizinhos' (" + std::to_string(m.vizinhos) + ") maior que o treino (" +
                               std::to_string(xt.size()) + " linhas)");
    }
    m.base_x.reserve(xt.size());
    for (const auto& r : xt) m.base_x.push_back(exp_std_aplicar(m, r));
    m.base_y = yt;
    m.base_yr = ytr;
  } else if (kind == "floresta_aleatoria") {
    if (hp.arvores < 1) throw std::runtime_error("'arvores' >= 1");
    const int prof = hp.profundidade > 0 ? hp.profundidade : 8;
    const std::size_t f = largura;
    const std::size_t nteste = m.classificacao
                                   ? std::max<std::size_t>(1, static_cast<std::size_t>(std::sqrt(static_cast<double>(f))))
                                   : f;
    std::vector<double> yd(xt.size());
    if (m.classificacao) {
      for (std::size_t i = 0; i < xt.size(); ++i) yd[i] = static_cast<double>(yt[i]);
    } else {
      yd = ytr;
    }
    m.arvores.clear();
    std::uniform_int_distribution<std::size_t> bolsa(0, xt.size() - 1);
    for (int t = 0; t < hp.arvores; ++t) {
      std::vector<std::vector<double>> xb;
      std::vector<double> yb;
      xb.reserve(xt.size());
      yb.reserve(xt.size());
      for (std::size_t i = 0; i < xt.size(); ++i) {
        const std::size_t k = bolsa(rng);
        xb.push_back(xt[k]);
        yb.push_back(yd[k]);
      }
      const int nc = m.classificacao ? static_cast<int>(m.classes.size()) : 0;
      m.arvores.push_back(arv_treinar(xb, yb, m.classificacao, nc, prof,
                                      static_cast<int>(nteste), rng));
    }
  } else if (kind == "gradiente_impulsionado") {
    if (hp.arvores < 1) throw std::runtime_error("'arvores' >= 1");
    if (!(hp.taxa > 0.0)) throw std::runtime_error("'taxa' > 0");
    const int prof = hp.profundidade > 0 ? hp.profundidade : 3;
    m.taxa_gbm = hp.taxa;
    const std::size_t n = xt.size();
    m.arvores.clear();
    if (!m.classificacao) {
      double media = 0.0;
      for (double v : ytr) media += v;
      media /= static_cast<double>(n);
      m.pesos = {media};
      std::vector<double> F(n, media), r(n);
      for (int t = 0; t < hp.arvores; ++t) {
        for (std::size_t i = 0; i < n; ++i) r[i] = ytr[i] - F[i];
        EM::Arvore a = arv_treinar(xt, r, false, 0, prof, 0, rng);
        for (std::size_t i = 0; i < n; ++i) F[i] += hp.taxa * arv_prever(a, xt[i]);
        m.arvores.push_back(std::move(a));
      }
    } else {
      if (m.classes.size() != 2) {
        throw std::runtime_error("gradiente_impulsionado e binario (" +
                                 std::to_string(m.classes.size()) + " classes no alvo; use floresta_aleatoria | knn)");
      }
      double p = 0.0;
      for (int v : yt) p += static_cast<double>(v);
      p /= static_cast<double>(n);
      const double f0 = std::log(std::max(p, 1e-9) / std::max(1.0 - p, 1e-9));
      m.pesos = {f0};
      std::vector<double> F(n, f0), r(n);
      for (int t = 0; t < hp.arvores; ++t) {
        for (std::size_t i = 0; i < n; ++i) r[i] = static_cast<double>(yt[i]) - sigmoide(F[i]);
        EM::Arvore a = arv_treinar(xt, r, false, 0, prof, 0, rng);
        for (std::size_t i = 0; i < n; ++i) F[i] += hp.taxa * arv_prever(a, xt[i]);
        m.arvores.push_back(std::move(a));
      }
    }
  } else if (kind == "svm") {
    if (m.classes.size() != 2) {
      throw std::runtime_error("svm e binario (" + std::to_string(m.classes.size()) +
                               " classes no alvo; use floresta_aleatoria | knn)");
    }
    if (!(hp.custo > 0.0)) throw std::runtime_error("'custo' (C) > 0");
    if (!(hp.taxa > 0.0) || hp.epocas < 1) throw std::runtime_error("'taxa' > 0 e 'epocas' >= 1");
    exp_std_ajustar(m, xt);
    const std::size_t f = largura;
    std::vector<std::vector<double>> z;
    z.reserve(xt.size());
    for (const auto& r : xt) z.push_back(exp_std_aplicar(m, r));
    // Pegasos: eta_t = 1/(lambda*t), lambda = 1/(C*n).
    const double lambda = 1.0 / (hp.custo * static_cast<double>(z.size()));
    m.pesos.assign(f + 1, 0.0);
    std::vector<std::size_t> ordem(z.size());
    for (std::size_t i = 0; i < ordem.size(); ++i) ordem[i] = i;
    long t = 0;
    for (int e = 0; e < hp.epocas; ++e) {
      std::shuffle(ordem.begin(), ordem.end(), rng);
      for (std::size_t k = 0; k < ordem.size(); ++k) {
        ++t;
        const std::size_t i = ordem[k];
        const double rot = static_cast<double>(yt[i] == 1 ? 1 : -1);
        double s = m.pesos[f];
        for (std::size_t j = 0; j < f; ++j) s += m.pesos[j] * z[i][j];
        const double eta = 1.0 / (lambda * static_cast<double>(t));
        const double encolhe = 1.0 - eta * lambda;
        for (std::size_t j = 0; j < f; ++j) m.pesos[j] *= encolhe;
        if (rot * s < 1.0) {
          for (std::size_t j = 0; j < f; ++j) m.pesos[j] += eta * rot * z[i][j];
          m.pesos[f] += eta * rot;
        }
      }
    }
  } else {  // kmeans
    const int k = hp.grupos;
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
}

// Predicao de classe (indice) + probabilidade/voto, para qualquer modelo.
static int exp_prever_classe(const Interpreter::ExpModel& m, const std::vector<double>& xraw,
                             std::size_t largura, double& proba) {
  using EM = Interpreter::ExpModel;
  const std::string& kind = m.kind;
  if (kind == "regressao_logistica") {
    const std::vector<double> z = exp_std_aplicar(m, xraw);
    if (m.classes.size() == 2) {
      double s = m.pesos[largura];
      for (std::size_t j = 0; j < largura; ++j) s += m.pesos[j] * z[j];
      proba = sigmoide(s);
      return proba >= 0.5 ? 1 : 0;
    }
    const std::size_t K = m.classes.size(), f = largura;
    double mx = 0.0, soma = 0.0;
    std::vector<double> p(K);
    for (std::size_t c = 0; c < K; ++c) {
      double s = m.pesos_multi[c * (f + 1) + f];
      for (std::size_t j = 0; j < f; ++j) s += m.pesos_multi[c * (f + 1) + j] * z[j];
      p[c] = s;
      if (c == 0 || s > mx) mx = s;
    }
    int melhor = 0;
    for (std::size_t c = 0; c < K; ++c) {
      p[c] = std::exp(p[c] - mx);
      soma += p[c];
    }
    for (std::size_t c = 0; c < K; ++c) {
      p[c] /= soma;
      if (p[c] > p[static_cast<std::size_t>(melhor)]) melhor = static_cast<int>(c);
    }
    proba = p[static_cast<std::size_t>(melhor)];
    return melhor;
  }
  if (kind == "floresta_aleatoria") {
    std::vector<std::size_t> votos(m.classes.size(), 0);
    for (const EM::Arvore& a : m.arvores) {
      const int v = static_cast<int>(std::llround(arv_prever(a, xraw)));
      if (v >= 0 && static_cast<std::size_t>(v) < votos.size()) votos[static_cast<std::size_t>(v)] += 1;
    }
    int melhor = 0;
    for (std::size_t c = 1; c < votos.size(); ++c) {
      if (votos[c] > votos[static_cast<std::size_t>(melhor)]) melhor = static_cast<int>(c);
    }
    proba = m.arvores.empty() ? 0.0
                              : static_cast<double>(votos[static_cast<std::size_t>(melhor)]) /
                                    static_cast<double>(m.arvores.size());
    return melhor;
  }
  if (kind == "gradiente_impulsionado") {
    double F = m.pesos.empty() ? 0.0 : m.pesos[0];
    for (const EM::Arvore& a : m.arvores) F += m.taxa_gbm * arv_prever(a, xraw);
    proba = sigmoide(F);
    return proba >= 0.5 ? 1 : 0;
  }
  if (kind == "svm") {
    const std::vector<double> z = exp_std_aplicar(m, xraw);
    double s = m.pesos[largura];
    for (std::size_t j = 0; j < largura; ++j) s += m.pesos[j] * z[j];
    proba = sigmoide(s);
    return s >= 0.0 ? 1 : 0;
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

// Predicao numerica (regressao), para qualquer modelo.
static double exp_prever_num(const Interpreter::ExpModel& m, const std::vector<double>& xraw,
                             std::size_t largura) {
  using EM = Interpreter::ExpModel;
  const std::string& kind = m.kind;
  if (kind == "regressao_linear") {
    double s = m.pesos[largura];
    for (std::size_t j = 0; j < largura; ++j) s += m.pesos[j] * xraw[j];
    return s;
  }
  if (kind == "floresta_aleatoria") {
    if (m.arvores.empty()) return 0.0;
    double soma = 0.0;
    for (const EM::Arvore& a : m.arvores) soma += arv_prever(a, xraw);
    return soma / static_cast<double>(m.arvores.size());
  }
  if (kind == "gradiente_impulsionado") {
    double F = m.pesos.empty() ? 0.0 : m.pesos[0];
    for (const EM::Arvore& a : m.arvores) F += m.taxa_gbm * arv_prever(a, xraw);
    return F;
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
}

// Mapeia o alvo do treino: classes (classificacao) ou numeros (regressao).
static void exp_mapear_y(Interpreter::ExpModel& m, const std::vector<Value>& treino,
                         const std::string& alvo, bool regr,
                         std::vector<int>& yt, std::vector<double>& ytr) {
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
        kind != "kmeans" && kind != "floresta_aleatoria" && kind != "gradiente_impulsionado" &&
        kind != "svm") {
      throw std::runtime_error("modelo '" + kind +
                               "' desconhecido (use regressao_linear | regressao_logistica | knn | kmeans | floresta_aleatoria | gradiente_impulsionado | svm)");
    }
    m.kind = kind;
    ExpHip hp;
    hp.taxa = hnum("taxa", hnum("taxa_aprendizado", kind == "gradiente_impulsionado" ? 0.1 : 0.5));
    hp.epocas = hint("epocas", 500);
    hp.vizinhos = hint("vizinhos", 5);
    hp.grupos = hint("grupos", -1);
    hp.arvores = hint("arvores", 50);
    hp.profundidade = hint("profundidade", 0);
    hp.custo = hnum("custo", hnum("C", 1.0));

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

    // ---- pre_processar: '- um_de_n: [cols]' / '- padronizar: [cols]' / '- imputar: [cols]'
    std::vector<std::string> std_cols;
    if (const Item* pp = find_field(cfg, "pre_processar"); pp && pp->block) {
      for (const auto& it : pp->block->items) {
        const Item* f = (it && it->kind == ItemKind::ListEntry && it->child) ? it->child.get()
                                                                             : it.get();
        if (!f || f->kind != ItemKind::Field || !f->value ||
            (f->key != "um_de_n" && f->key != "padronizar" && f->key != "imputar")) {
          throw std::runtime_error(
              "pre_processar: use '- um_de_n: [cols]', '- padronizar: [cols]' ou '- imputar: [cols]'");
        }
        std::vector<std::string> cols = nomes_crus(*f->value, "pre_processar");
        for (const std::string& c : cols) {
          if (std::find(m.numericas.begin(), m.numericas.end(), c) == m.numericas.end()) {
            throw std::runtime_error("pre_processar: '" + c + "' nao esta em 'atributos'");
          }
        }
        if (f->key == "um_de_n") {
          m.quentes.insert(m.quentes.end(), cols.begin(), cols.end());
        } else if (f->key == "imputar") {
          m.imputar_cols.insert(m.imputar_cols.end(), cols.begin(), cols.end());
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

    // y do treino / classes (floresta e GBM: alvo numerico = regressao)
    std::vector<int> yt;
    std::vector<double> ytr;
    bool regr = (kind == "regressao_linear");
    if (kind != "kmeans") {
      if (kind == "floresta_aleatoria" || kind == "gradiente_impulsionado") {
        // Tipo do alvo como no sklearn: nao-numerico = classificacao;
        // numerico binario (2 valores distintos) = classificacao; demais = regressao.
        regr = false;
        bool numerico = true;
        std::vector<std::string> distintos;
        for (const Value& r : treino) {
          const Value* c = r.map->find(alvo);
          if (!c || c->kind == ValueKind::Nulo) {
            throw std::runtime_error("alvo '" + alvo + "' com valor ausente/nulo no treino");
          }
          if (!c->is_number()) {
            numerico = false;
            break;
          }
          const std::string k = chave_valor(*c);
          if (std::find(distintos.begin(), distintos.end(), k) == distintos.end()) {
            distintos.push_back(k);
          }
        }
        regr = numerico && distintos.size() > 2;
      }
      exp_mapear_y(m, treino, alvo, regr, yt, ytr);
      if (kind == "svm" && m.classes.size() != 2) {
        throw std::runtime_error("svm e binario (" + std::to_string(m.classes.size()) +
                                 " classes no alvo; use floresta_aleatoria | knn)");
      }
      if (kind == "gradiente_impulsionado" && !regr && m.classes.size() != 2) {
        throw std::runtime_error("gradiente_impulsionado e binario (" +
                                 std::to_string(m.classes.size()) +
                                 " classes no alvo; use floresta_aleatoria | knn)");
      }
      m.classificacao = !regr;
    }

    // ---- ajuste por modelo
    if (kind == "regressao_logistica" && !(hp.taxa > 0.0)) {
      throw std::runtime_error("'taxa' > 0");
    }
    if ((kind == "regressao_logistica" || kind == "svm") && hp.epocas < 1) {
      throw std::runtime_error("'epocas' >= 1");
    }
    exp_treinar_modelo(m, xt, yt, ytr, largura, hp, rng);

    // ---- metricas
    std::vector<std::string> pedidas;
    if (const Item* fmet = find_field(cfg, "metricas"); fmet && fmet->value) {
      pedidas = nomes_crus(*fmet->value, "'metricas'");
    }
    auto prever_idx = [&](const std::vector<double>& xraw, double& proba) -> int {
      return exp_prever_classe(m, xraw, largura, proba);
    };
    auto prever_num = [&](const std::vector<double>& xraw) -> double {
      return exp_prever_num(m, xraw, largura);
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

    // ---- validacao_cruzada: K folds com re-ajuste completo por fold
    if (const Item* fcv = find_field(cfg, "validacao_cruzada"); fcv && fcv->value) {
      Value cvv = eval(*fcv->value, root_);
      if (cvv.kind != ValueKind::Inteiro || cvv.i < 2) {
        throw std::runtime_error("'validacao_cruzada' deve ser inteiro >= 2 (folds)");
      }
      const int K = static_cast<int>(cvv.i);
      if (kind == "kmeans") throw std::runtime_error("'validacao_cruzada' nao vale p/ kmeans");
      if (K > static_cast<int>(total)) {
        throw std::runtime_error("'validacao_cruzada' (" + std::to_string(K) + ") maior que as linhas (" +
                                 std::to_string(total) + ")");
      }
      std::vector<std::size_t> idc(total);
      for (std::size_t i = 0; i < total; ++i) idc[i] = i;
      std::mt19937 rng_cv(static_cast<std::uint32_t>(semente));
      std::shuffle(idc.begin(), idc.end(), rng_cv);
      std::vector<double> notas;
      for (int f = 0; f < K; ++f) {
        std::vector<Value> tr, te;
        for (std::size_t k = 0; k < total; ++k) {
          (k % static_cast<std::size_t>(K) == static_cast<std::size_t>(f) ? te : tr)
              .push_back(linhas[idc[k]]);
        }
        ExpModel mf;
        mf.kind = kind;
        mf.numericas = m.numericas;
        mf.quentes = m.quentes;
        mf.imputar_cols = m.imputar_cols;
        exp_ajustar(mf, tr, std_cols);
        std::vector<std::vector<double>> xtf;
        for (const Value& r : tr) xtf.push_back(exp_vetor(mf, r));
        std::vector<int> ytf;
        std::vector<double> ytrf;
        exp_mapear_y(mf, tr, alvo, regr, ytf, ytrf);
        mf.classificacao = !regr;
        exp_treinar_modelo(mf, xtf, ytf, ytrf, exp_largura(mf), hp, rng_cv);
        if (mf.classificacao) {
          std::size_t ok = 0;
          for (const Value& r : te) {
            const Value* c = r.map->find(alvo);
            int real = -1;
            if (c) {
              const std::string kk = chave_valor(*c);
              for (std::size_t j = 0; j < mf.classes.size(); ++j) {
                if (chave_valor(mf.classes[j]) == kk) {
                  real = static_cast<int>(j);
                  break;
                }
              }
            }
            double proba = 0.0;
            if (exp_prever_classe(mf, exp_vetor(mf, r), exp_largura(mf), proba) == real) ++ok;
          }
          notas.push_back(static_cast<double>(ok) / static_cast<double>(te.size()));
        } else {
          double ss = 0.0;
          for (const Value& r : te) {
            const double d = r.map->find(alvo)->as_number() -
                             exp_prever_num(mf, exp_vetor(mf, r), exp_largura(mf));
            ss += d * d;
          }
          notas.push_back(std::sqrt(ss / static_cast<double>(te.size())));
        }
      }
      double media_cv = 0.0;
      for (double v : notas) media_cv += v;
      media_cv /= static_cast<double>(notas.size());
      double var_cv = 0.0;
      for (double v : notas) var_cv += (v - media_cv) * (v - media_cv);
      var_cv /= static_cast<double>(notas.size());
      relatorio.push_back(std::string("cv ") + (m.classificacao ? "acuracia" : "rmse") + ": " +
                          fmt4(media_cv) + " +- " + fmt4(std::sqrt(var_cv)) + " (" +
                          std::to_string(K) + " folds)");
    }

    // ---- registrar_em (MLflow Tracking REST)
    if (const Item* fr = find_field(cfg, "registrar_em"); fr && fr->value) {
      Value rv = eval(*fr->value, root_);
      if (rv.kind != ValueKind::Texto) {
        throw std::runtime_error("'registrar_em' deve ser texto (ex.: mlflow://host/experimento)");
      }
      const std::string run_id =
          mlflow_registrar_experimento(rv.s, name, kind, total, semente, relatorio);
      relatorio.push_back("run enviado ao MLflow: " + run_id);
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

// Avaliacao (evals): roda `executar:` uma vez por caso de `dados:` (com
// `caso` no escopo e `retornar <saida>` como saida sob teste) e pontua cada
// caso contra `esperado` com todas as `metricas:` pedidas. Abaixo do
// `limiar:`, aborta (T910) ou avisa (`ao_reprovar: avisar`).
void Interpreter::run_avaliacao(const Item& decl) {
  const std::string name = decl_name(decl);
  if (!decl.block) fail(decl.span, "avaliacao '" + name + "' sem configuracao");
  const ast::Block& cfg = *decl.block;
  try {
    // ---- dados (lista inline, bloco de casos, caminho .csv/.parquet/.json ou valor de ler_*)
    const Item* fd = find_field(cfg, "dados");
    if (!fd || (!fd->value && !fd->block)) throw std::runtime_error("falta 'dados:' (lista de mapas com 'esperado', ou caminho .csv/.parquet/.json)");
    Value dados = Value::nulo();
    if (fd->value) {
      dados = eval(*fd->value, root_);
    } else {
      // Bloco:
      //   dados:
      //     - { pergunta: "...", esperado: "..." }
      // ou dobrado:
      //   dados:
      //     - pergunta: "..."
      //       esperado: "..."
      Value lista = Value::lista();
      for (const auto& raw : fd->block->items) {
        if (!raw || raw->kind != ItemKind::ListEntry) {
          throw std::runtime_error("'dados:' em bloco espera uma lista de casos ('- {...}')");
        }
        if (raw->block) {
          Value caso = Value::mapa();
          for (const auto& sub : raw->block->items) {
            const Item* f = sub.get();
            if (f && f->kind == ItemKind::ListEntry && f->child) f = f->child.get();
            if (!f || f->kind != ItemKind::Field || !f->value) {
              throw std::runtime_error("'dados:' em bloco dobrado espera 'chave: valor' por linha");
            }
            caso.map->set(f->key, eval(*f->value, root_));
          }
          lista.list->push_back(std::move(caso));
        } else if (raw->child && raw->child->kind == ItemKind::Stmt && raw->child->stmt &&
                   raw->child->stmt->a) {
          lista.list->push_back(eval(*raw->child->stmt->a, root_));
        } else {
          throw std::runtime_error("'dados:' em bloco espera uma lista de casos ('- {...}')");
        }
      }
      dados = std::move(lista);
    }
    std::vector<Value> casos;
    if (dados.kind == ValueKind::Tabela || dados.kind == ValueKind::Lista) {
      if (!dados.list) throw std::runtime_error("'dados' vazio");
      casos.assign(dados.list->begin(), dados.list->end());
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
      casos.assign(t.list->begin(), t.list->end());
    } else {
      throw std::runtime_error("'dados' deve ser lista de mapas ou caminho (veio " +
                               std::string(dados.type_name()) + ")");
    }
    if (casos.empty()) throw std::runtime_error("'dados' vazio (liste ao menos um caso)");
    for (std::size_t i = 0; i < casos.size(); ++i) {
      if (casos[i].kind != ValueKind::Mapa || !casos[i].map) {
        throw std::runtime_error("caso " + std::to_string(i) + " nao e um mapa");
      }
      if (!casos[i].map->find("esperado")) {
        throw std::runtime_error("caso " + std::to_string(i) + " sem 'esperado:' (toda metrica compara contra ele)");
      }
    }

    // ---- executar (passos por caso; `caso` no escopo; `retornar` = saida)
    const Item* fexec = find_field(cfg, "executar");
    if (!fexec || !fexec->block) {
      throw std::runtime_error("falta 'executar:' com os passos (use 'caso.<campo>' e 'retornar <saida>')");
    }

    // ---- metricas
    std::vector<std::string> metricas = {"exata"};
    if (const Item* fm = find_field(cfg, "metricas"); fm && fm->value) {
      metricas = nomes_crus(*fm->value, "'metricas'");
    }
    for (const std::string& mt : metricas) {
      if (mt != "exata" && mt != "contem" && mt != "regex" && mt != "tolerancia" && mt != "juiz") {
        throw std::runtime_error("metrica '" + mt + "' desconhecida (use exata | contem | regex | tolerancia | juiz)");
      }
    }
    const bool usa_juiz =
        std::find(metricas.begin(), metricas.end(), "juiz") != metricas.end();
    const Item* fjuiz = find_field(cfg, "juiz");
    std::vector<std::string> juiz_llms;
    std::string juiz_consenso = "maioria";
    const Expr* juiz_sistema = nullptr;
    const Expr* juiz_usuario = nullptr;
    if (usa_juiz) {
      if (!fjuiz || !fjuiz->block) {
        throw std::runtime_error("metricas com juiz precisa do bloco juiz");
      }
      const Item* fjl = find_field(*fjuiz->block, "llm");
      const Item* fjc = find_field(*fjuiz->block, "cadeia");
      if (fjl && fjc) {
        throw std::runtime_error("bloco juiz deve usar llm ou cadeia, nao ambos");
      }
      if (fjc) {
        if (!fjc->value || fjc->value->kind != ExprKind::ListLit || fjc->value->elems.empty()) {
          throw std::runtime_error("bloco juiz: cadeia deve ser uma lista nao vazia de LLMs");
        }
        for (const auto& el : fjc->value->elems) {
          if (!el || (el->kind != ExprKind::Name && el->kind != ExprKind::TextLit)) {
            throw std::runtime_error("bloco juiz: cadeia espera nomes de LLM");
          }
          juiz_llms.push_back(el->text);
        }
      } else if (fjl && fjl->value && fjl->value->kind == ExprKind::Name) {
        juiz_llms.push_back(fjl->value->text);
      } else {
        throw std::runtime_error("bloco juiz precisa de llm ou cadeia");
      }
      for (const std::string& juiz : juiz_llms) {
        auto jit = entities_.find(juiz);
        if (jit == entities_.end() || jit->second->key != "llm") {
          throw std::runtime_error("juiz: LLM nao declarado: " + juiz);
        }
      }
      if (const Item* fco = find_field(*fjuiz->block, "consenso"); fco && fco->value) {
        if (fco->value->kind != ExprKind::Name && fco->value->kind != ExprKind::TextLit) {
          throw std::runtime_error("juiz: consenso deve ser maioria ou unanimidade");
        }
        juiz_consenso = fco->value->text;
      }
      if (juiz_consenso != "maioria" && juiz_consenso != "unanimidade") {
        throw std::runtime_error("juiz: consenso deve ser maioria ou unanimidade");
      }
      if (const Item* fjs = find_field(*fjuiz->block, "sistema"); fjs && fjs->value) {
        juiz_sistema = fjs->value.get();
      }
      if (const Item* fju = find_field(*fjuiz->block, "usuario"); fju && fju->value) {
        juiz_usuario = fju->value.get();
      }
    } else if (fjuiz) {
      throw std::runtime_error("bloco juiz sem metrica juiz");
    }
    // ---- tolerancia / limiar / ao_reprovar / verboso
    double tolerancia = 1e-6;
    if (const Item* ft = find_field(cfg, "tolerancia"); ft && ft->value) {
      Value tv = eval(*ft->value, root_);
      if (tv.kind != ValueKind::Inteiro && tv.kind != ValueKind::Decimal) {
        throw std::runtime_error("'tolerancia' deve ser numero (ex.: 0.01)");
      }
      tolerancia = tv.as_number();
      if (!(tolerancia >= 0.0)) throw std::runtime_error("'tolerancia' deve ser >= 0");
    }
    double limiar = 1.0;
    if (const Item* fl = find_field(cfg, "limiar"); fl && fl->value) {
      Value lv = eval(*fl->value, root_);
      if (lv.kind != ValueKind::Inteiro && lv.kind != ValueKind::Decimal) {
        throw std::runtime_error("'limiar' deve ser numero entre 0 e 1 (ex.: 0.8)");
      }
      limiar = lv.as_number();
      if (limiar < 0.0 || limiar > 1.0) {
        throw std::runtime_error("'limiar' deve estar entre 0 e 1 (veio " + std::to_string(limiar) + ")");
      }
    }
    std::string ao_reprovar = "abortar";
    if (const Item* fa = find_field(cfg, "ao_reprovar")) {
      const Expr* v = fa->value.get();
      if (v && v->kind == ExprKind::Name) {
        ao_reprovar = v->text;
      } else if (v && v->kind == ExprKind::TextLit) {
        Value av = eval(*v, root_);
        ao_reprovar = av.s;
      } else {
        throw std::runtime_error("'ao_reprovar' deve ser abortar | avisar");
      }
      if (ao_reprovar != "abortar" && ao_reprovar != "avisar") {
        throw std::runtime_error("'ao_reprovar' deve ser abortar | avisar (veio '" + ao_reprovar + "')");
      }
    }
    const Item* fv = find_field(cfg, "verboso");
    const bool verboso =
        fv && fv->value && fv->value->kind == ExprKind::BoolLit && fv->value->boolean;

    // ---- amostra / semente (subamostragem deterministica)
    std::int64_t amostra = -1;  // -1 = todos os casos, em ordem
    if (const Item* fam = find_field(cfg, "amostra"); fam && fam->value) {
      Value amv = eval(*fam->value, root_);
      if (amv.kind != ValueKind::Inteiro || amv.i < 1) {
        throw std::runtime_error("'amostra' deve ser inteiro >= 1 (no maximo de casos a rodar)");
      }
      amostra = amv.i;
    }
    std::int64_t semente = 7;
    if (const Item* fse = find_field(cfg, "semente"); fse && fse->value) {
      Value sev = eval(*fse->value, root_);
      if (sev.kind != ValueKind::Inteiro) throw std::runtime_error("'semente' deve ser inteiro");
      semente = sev.i;
    }
    std::string estratificar_por;
    if (const Item* fe = find_field(cfg, "estratificar_por"); fe && fe->value) {
      if (fe->value->kind != ExprKind::Name && fe->value->kind != ExprKind::TextLit) {
        throw std::runtime_error("'estratificar_por' deve ser o nome/texto de um campo dos casos");
      }
      estratificar_por = fe->value->text;
      if (amostra < 0) {
        throw std::runtime_error(
            "'estratificar_por' exige 'amostra:' para ativar a amostragem estratificada");
      }
      for (std::size_t k = 0; k < casos.size(); ++k) {
        if (!casos[k].map->find(estratificar_por)) {
          throw std::runtime_error("caso " + std::to_string(k) +
                                   " sem o campo de estratificacao '" + estratificar_por + "'");
        }
      }
    }
    std::vector<std::size_t> ordem;
    for (std::size_t k = 0; k < casos.size(); ++k) ordem.push_back(k);
    std::size_t n_rodar = casos.size();
    const bool amostrada = amostra >= 0 && amostra < static_cast<std::int64_t>(casos.size());
    const bool estratificada = amostrada && !estratificar_por.empty();
    if (amostrada) {
      // xorshift64* — mesmo gerador do init Xavier: deterministico na semente.
      std::uint64_t st = semente ? static_cast<std::uint64_t>(semente) : 0x9E3779B97F4A7C15ULL;
      auto prox = [&]() {
        st ^= st >> 12;
        st ^= st << 25;
        st ^= st >> 27;
        return st * 0x2545F4914F6CDD1DULL;
      };
      auto embaralhar = [&](std::vector<std::size_t>& indices) {
        for (std::size_t k = indices.size(); k > 1; --k) {
          const std::size_t j = static_cast<std::size_t>(prox() % k);
          std::swap(indices[k - 1], indices[j]);
        }
      };
      if (!estratificada) {
        embaralhar(ordem);
      } else {
        struct GrupoAmostra {
          std::string chave;
          std::vector<std::size_t> indices;
          std::size_t alvo = 0;
          std::uint64_t resto = 0;
        };
        std::map<std::string, std::vector<std::size_t>> por_valor;
        for (std::size_t k = 0; k < casos.size(); ++k) {
          por_valor[rt::json_dump(*casos[k].map->find(estratificar_por))].push_back(k);
        }
        std::vector<GrupoAmostra> grupos;
        grupos.reserve(por_valor.size());
        std::size_t alocados = 0;
        const std::uint64_t total = static_cast<std::uint64_t>(casos.size());
        const std::uint64_t desejados = static_cast<std::uint64_t>(amostra);
        for (auto& [chave, indices] : por_valor) {
          const std::uint64_t produto = desejados * static_cast<std::uint64_t>(indices.size());
          GrupoAmostra grupo{chave, std::move(indices), static_cast<std::size_t>(produto / total),
                             produto % total};
          grupo.alvo = std::min(grupo.alvo, grupo.indices.size());
          alocados += grupo.alvo;
          embaralhar(grupo.indices);
          grupos.push_back(std::move(grupo));
        }
        std::size_t restantes = static_cast<std::size_t>(amostra) - alocados;
        while (restantes > 0) {
          auto candidato = grupos.end();
          for (auto it = grupos.begin(); it != grupos.end(); ++it) {
            if (it->alvo >= it->indices.size()) continue;
            if (candidato == grupos.end() || it->resto > candidato->resto) candidato = it;
          }
          if (candidato == grupos.end()) break;
          ++candidato->alvo;
          candidato->resto = 0;
          --restantes;
        }
        ordem.clear();
        ordem.reserve(static_cast<std::size_t>(amostra));
        for (const GrupoAmostra& grupo : grupos) {
          ordem.insert(ordem.end(), grupo.indices.begin(), grupo.indices.begin() + grupo.alvo);
        }
        embaralhar(ordem);
      }
      n_rodar = static_cast<std::size_t>(amostra);
    }

    auto como_texto = [](const Value& v) -> std::string {
      if (v.kind == ValueKind::Texto) return v.s;
      if (v.kind == ValueKind::Inteiro) return std::to_string(v.i);
      if (v.kind == ValueKind::Decimal) return std::to_string(v.d);
      if (v.kind == ValueKind::Logico) return v.b ? "verdadeiro" : "falso";
      if (v.kind == ValueKind::Nulo) return "nulo";
      return rt::json_dump(v);
    };

    auto classificar_juiz = [](const std::string& bruto) {
      std::string veredito = bruto;
      try {
        const Value estruturado = rt::json_parse(bruto);
        if (estruturado.kind == ValueKind::Mapa && estruturado.map) {
          for (const char* campo : {"veredito", "resultado", "verdict"}) {
            if (const Value* v = estruturado.map->find(campo); v && v->kind == ValueKind::Texto) {
              veredito = v->s;
              break;
            }
          }
        }
      } catch (...) {
      }
      for (char& c : veredito) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }
      const std::size_t passa_pos = veredito.find("PASSA");
      const std::size_t falha_pos = veredito.find("FALHA");
      if (passa_pos == std::string::npos && falha_pos == std::string::npos) return -1;
      if (falha_pos == std::string::npos) return 1;
      if (passa_pos == std::string::npos) return 0;
      return passa_pos < falha_pos ? 1 : 0;
    };

    // ---- loop dos casos
    out_ << "== avaliacao " << name << " ==\n";
    struct Registro {
      std::size_t indice;
      bool passou;
      std::string motivo;
      Value saida;
    };
    std::vector<Registro> registros;
    std::size_t passou = 0;
    for (std::size_t k = 0; k < n_rodar; ++k) {
      const std::size_t i = ordem[k];
      Env env;
      env.parent = &root_;
      env.vars["caso"] = casos[i];
      bool retornou = false;
      Value saida = Value::nulo();
      try {
        exec_block(*fexec->block, env);
      } catch (const ReturnSignal& r) {
        retornou = true;
        saida = r.value;
      }
      if (!retornou) {
        throw std::runtime_error("'executar:' do caso " + std::to_string(i) +
                                 " terminou sem 'retornar <saida>'");
      }
      const Value* esperado = casos[i].map->find("esperado");
      bool ok = true;
      std::string motivo;
      for (const std::string& mt : metricas) {
        if (mt == "exata") {
          if (rt::json_dump(saida) != rt::json_dump(*esperado)) {
            ok = false;
            motivo = "exata: saida != esperado";
            break;
          }
        } else if (mt == "contem") {
          if (saida.kind != ValueKind::Texto || esperado->kind != ValueKind::Texto) {
            throw std::runtime_error("metrica 'contem' no caso " + std::to_string(i) +
                                     " precisa de textos (saida e 'esperado')");
          }
          if (saida.s.find(esperado->s) == std::string::npos) {
            ok = false;
            motivo = "contem: saida nao contem o esperado";
            break;
          }
        } else if (mt == "regex") {
          if (saida.kind != ValueKind::Texto || esperado->kind != ValueKind::Texto) {
            throw std::runtime_error("metrica 'regex' no caso " + std::to_string(i) +
                                     " precisa de textos (o 'esperado' e o padrao)");
          }
          std::regex re;
          try {
            re = std::regex(esperado->s);
          } catch (const std::regex_error&) {
            throw std::runtime_error("metrica 'regex' no caso " + std::to_string(i) +
                                     ": padrao invalido '" + esperado->s + "'");
          }
          if (!std::regex_search(saida.s, re)) {
            ok = false;
            motivo = "regex: padrao nao casou";
            break;
          }
        } else if (mt == "juiz") {
          // Um juiz simples usa `llm:`; uma cadeia usa varios LLMs independentes
          // e combina os vereditos por maioria ou unanimidade.
          env.vars["saida"] = Value::texto(como_texto(saida));
          env.vars["esperado"] = Value::texto(como_texto(*esperado));
          std::string sistema = "Voce e um avaliador. Responda PASSA ou FALHA.";
          std::string usuario =
              "Esperado: {{esperado}}\nSaida: {{saida}}\nA saida atende ao esperado? Responda PASSA ou FALHA.";
          if (juiz_llms.size() > 1) {
            sistema +=
                " Responda em JSON: {\"veredito\": \"PASSA\" ou \"FALHA\", \"justificativa\": "
                "\"...\"}.";
          }
          if (juiz_sistema) {
            Value jsv = eval(*juiz_sistema, env);
            if (jsv.kind != ValueKind::Texto) {
              throw std::runtime_error("bloco juiz: sistema deve ser texto");
            }
            sistema = jsv.s;
          }
          if (juiz_usuario) {
            Value juv = eval(*juiz_usuario, env);
            if (juv.kind != ValueKind::Texto) {
              throw std::runtime_error("bloco juiz: usuario deve ser texto");
            }
            usuario = juv.s;
          }
          int votos_passou = 0;
          int votos_falhou = 0;
          int votos_indecisos = 0;
          for (const std::string& juiz : juiz_llms) {
            rt::RespostaLLM rj;
            try {
              rj = rt::llm_chat_cadeia(cadeia_llm(juiz, fjuiz->span), sistema, usuario);
            } catch (const std::exception& e) {
              throw std::runtime_error("juiz (llm " + juiz + "): " + e.what());
            }
            const int voto = classificar_juiz(rj.texto);
            if (voto > 0) {
              ++votos_passou;
            } else if (voto == 0) {
              ++votos_falhou;
            } else {
              ++votos_indecisos;
            }
          }
          const bool consenso_ok =
              juiz_consenso == "unanimidade"
                  ? votos_passou == static_cast<int>(juiz_llms.size())
                  : votos_passou > votos_falhou && votos_passou > votos_indecisos;
          if (consenso_ok) continue;
          ok = false;
          if (juiz_llms.size() == 1 && votos_falhou == 1) {
            motivo = "juiz: FALHA";
          } else if (votos_indecisos > 0 && votos_passou == 0 && votos_falhou == 0) {
            motivo = "juiz indeciso (sem PASSA/FALHA)";
          } else {
            motivo = "juiz: consenso " + juiz_consenso + " (passa " + std::to_string(votos_passou) +
                     ", falha " + std::to_string(votos_falhou) + ", indeciso " +
                     std::to_string(votos_indecisos) + ")";
          }
          break;
        } else {  // tolerancia
          if ((saida.kind != ValueKind::Inteiro && saida.kind != ValueKind::Decimal) ||
              (esperado->kind != ValueKind::Inteiro && esperado->kind != ValueKind::Decimal)) {
            throw std::runtime_error("metrica 'tolerancia' no caso " + std::to_string(i) +
                                     " precisa de numeros (saida e 'esperado')");
          }
          if (std::fabs(saida.as_number() - esperado->as_number()) > tolerancia) {
            ok = false;
            motivo = "tolerancia: |saida - esperado| > " + std::to_string(tolerancia);
            break;
          }
        }
      }
      if (ok) {
        ++passou;
        if (verboso) out_ << "  caso " << i << ": passou\n";
      } else if (verboso) {
        out_ << "  caso " << i << ": falhou (" << motivo + "; saida: " + como_texto(saida) + ")\n";
      }
      registros.push_back({i, ok, motivo, saida});
    }

    const double media = n_rodar == 0 ? 1.0 : static_cast<double>(passou) / static_cast<double>(n_rodar);
    char media_s[32], limiar_s[32];
    std::snprintf(media_s, sizeof media_s, "%.2f", media);
    std::snprintf(limiar_s, sizeof limiar_s, "%.2f", limiar);
    out_ << "avaliacao " << name << ": " << passou << "/" << n_rodar << " passou | media "
         << media_s << " (limiar " << limiar_s << ")";
    if (amostrada) {
      out_ << " (amostra " << n_rodar << "/" << casos.size() << ", semente " << semente;
      if (estratificada) out_ << ", estratificada por " << estratificar_por;
      out_ << ")";
    }
    out_ << "\n";

    // ---- registrar_em (JSON local ou MLflow Tracking REST)
    if (const Item* fr = find_field(cfg, "registrar_em"); fr && fr->value) {
      Value rv = eval(*fr->value, root_);
      if (rv.kind != ValueKind::Texto) {
        throw std::runtime_error(
            "'registrar_em' deve ser texto (caminho .json ou mlflow://host/experimento)");
      }
      if (rv.s.rfind("mlflow://", 0) == 0) {
        std::vector<std::string> report = {
            "media: " + std::string(media_s), "limiar: " + std::string(limiar_s),
            "passou: " + std::to_string(passou), "total: " + std::to_string(n_rodar)};
        const std::string run_id =
            mlflow_registrar_experimento(rv.s, name, "avaliacao", n_rodar, semente, report);
        out_ << "run enviado ao MLflow: " << run_id << "\n";
      } else {
        if (rv.s.size() < 6 || rv.s.compare(rv.s.size() - 5, 5, ".json") != 0) {
          throw std::runtime_error("'registrar_em' deve ser um caminho .json (ex.: \"avaliacao_" +
                                   name + "_run.json\")");
        }
        Value doc = Value::mapa();
        doc.map->set("avaliacao", Value::texto(name));
        doc.map->set("media", Value::decimal(media));
        doc.map->set("limiar", Value::decimal(limiar));
        doc.map->set("passou", Value::inteiro(static_cast<std::int64_t>(passou)));
        doc.map->set("total", Value::inteiro(static_cast<std::int64_t>(n_rodar)));
        Value mets = Value::lista();
        for (const std::string& mt : metricas) mets.list->push_back(Value::texto(mt));
        doc.map->set("metricas", std::move(mets));
        if (amostrada) {
          doc.map->set("amostra", Value::inteiro(static_cast<std::int64_t>(n_rodar)));
          doc.map->set("semente", Value::inteiro(semente));
          if (estratificada) doc.map->set("estratificar_por", Value::texto(estratificar_por));
        }
        Value rcs = Value::lista();
        for (const Registro& r : registros) {
          Value rc = Value::mapa();
          rc.map->set("indice", Value::inteiro(static_cast<std::int64_t>(r.indice)));
          rc.map->set("passou", Value::logico(r.passou));
          if (!r.passou) rc.map->set("motivo", Value::texto(r.motivo));
          rc.map->set("saida", r.saida);
          rcs.list->push_back(std::move(rc));
        }
        doc.map->set("casos", std::move(rcs));
        std::ofstream rout(rv.s, std::ios::trunc);
        if (!rout)
          throw std::runtime_error("nao foi possivel gravar '" + rv.s +
                                   "' (crie o diretorio antes?)");
        rout << rt::json_dump(doc) << "\n";
        out_ << "run salvo em " << rv.s << "\n";
      }
    }

    if (media + 1e-12 < limiar) {
      const std::string msg = "avaliacao " + name + ": media " + media_s + " abaixo do limiar " +
                              limiar_s + " (" + std::to_string(passou) + "/" +
                              std::to_string(n_rodar) + " passou)";
      if (ao_reprovar == "avisar") {
        out_ << "[aviso] " + msg + "\n";
      } else {
        fail(decl.span, msg, DiagCode::DataQualityViolation);
      }
    }
  } catch (const std::exception& e) {
    fail(decl.span, "avaliacao " + name + ": " + e.what());
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
    const std::size_t largura = exp_largura(m);
    const int pred = exp_prever_classe(m, x, largura, proba);
    if (m.kind == "regressao_logistica" && m.classes.size() == 2 && pred == 0) {
      proba = 1.0 - proba;  // compat: 'probabilidade' e P da classe prevista
    }
    out.map->set("classe", m.classes[static_cast<std::size_t>(pred)]);
    out.map->set("probabilidade", Value::decimal(proba));
    return out;
  }
  const std::size_t largura = exp_largura(m);
  out.map->set("valor", Value::decimal(exp_prever_num(m, x, largura)));
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

std::string ferramenta_tipo_texto(const Expr* type_expr) {
  if (!type_expr) return "qualquer";
  if (type_expr->kind == ExprKind::Name) return type_expr->text;
  if (type_expr->kind == ExprKind::TextLit) {
    return std::string(1, '"') + type_expr->text + std::string(1, '"');
  }
  if (type_expr->kind == ExprKind::Binary && type_expr->text == "|") {
    return ferramenta_tipo_texto(type_expr->lhs.get()) + " | " +
           ferramenta_tipo_texto(type_expr->rhs.get());
  }
  if (type_expr->kind == ExprKind::Index && type_expr->lhs) {
    std::string out = ferramenta_tipo_texto(type_expr->lhs.get()) + "[";
    for (std::size_t i = 0; i < type_expr->elems.size(); ++i) {
      if (i) out += ", ";
      out += ferramenta_tipo_texto(type_expr->elems[i].get());
    }
    return out + "]";
  }
  return "tipo";
}

bool ferramenta_valor_compativel(const Expr* type_expr, const Value& value) {
  if (!type_expr) return true;
  if (type_expr->kind == ExprKind::Binary && type_expr->text == "|") {
    return ferramenta_valor_compativel(type_expr->lhs.get(), value) ||
           ferramenta_valor_compativel(type_expr->rhs.get(), value);
  }
  if (type_expr->kind == ExprKind::TextLit) {
    return value.kind == ValueKind::Texto && value.s == type_expr->text;
  }
  if (type_expr->kind == ExprKind::Index && type_expr->lhs &&
      type_expr->lhs->kind == ExprKind::Name) {
    const std::string& base = type_expr->lhs->text;
    if (base == "opcional") {
      return value.kind == ValueKind::Nulo ||
             (type_expr->elems.size() == 1 &&
              ferramenta_valor_compativel(type_expr->elems[0].get(), value));
    }
    if (base == "lista") {
      if (value.kind != ValueKind::Lista || !value.list) return false;
      if (type_expr->elems.empty()) return true;
      for (const Value& item : *value.list) {
        if (!ferramenta_valor_compativel(type_expr->elems[0].get(), item)) return false;
      }
      return true;
    }
    if (base == "tensor") return value.kind == ValueKind::Tensor;
  }
  if (type_expr->kind != ExprKind::Name) return true;
  const std::string& type = type_expr->text;
  if (type == "texto") return value.kind == ValueKind::Texto;
  if (type == "inteiro") return value.kind == ValueKind::Inteiro;
  if (type == "decimal")
    return value.kind == ValueKind::Inteiro || value.kind == ValueKind::Decimal;
  if (type == "logico") return value.kind == ValueKind::Logico;
  if (type == "nulo") return value.kind == ValueKind::Nulo;
  if (type == "lista") return value.kind == ValueKind::Lista;
  if (type == "tabela") return value.kind == ValueKind::Tabela;
  if (type == "mapa") return value.kind == ValueKind::Mapa;
  if (type == "tensor") return value.kind == ValueKind::Tensor;
  if (type == "opcional") return true;
  // Tipos declarados pelo usuario sao registros em runtime.
  return value.kind == ValueKind::Mapa;
}

}  // namespace

rt::LlmConfig Interpreter::llm_config(const std::string& name, Span span) {
  auto it = entities_.find(name);
  if (it == entities_.end() || it->second->key != "llm" || !it->second->block) {
    fail(span, "'" + name + "' nao e um 'llm' declarado");
  }
  const ast::Block& b = *it->second->block;
  rt::LlmConfig cfg;
  cfg.nome = name;
  // 'provedor:' aceita palavra ou texto (a doc usa "openai" entre aspas;
  // field_word so le Name, entao lemos aqui).
  if (const Item* fp = find_field(b, "provedor"); fp && fp->value) {
    if (fp->value->kind == ExprKind::Name || fp->value->kind == ExprKind::TextLit) {
      cfg.provider = fp->value->text;
    }
  }
  if (cfg.provider != "anthropic" && cfg.provider != "openai" && cfg.provider != "local" &&
      cfg.provider != "vllm") {
    fail(span, "llm '" + name + "': provedor '" + cfg.provider +
                   "' desconhecido (use anthropic | openai | local | vllm)");
  }
  cfg.model = field_str(b, "modelo");
  cfg.api_key = field_env_or_text(b, "chave");
  cfg.base_url = field_env_or_text(b, "base_url");
  cfg.temperature = field_num(b, "temperatura", 0.2);
  cfg.max_tokens = field_int(b, "max_tokens", 1024);
  cfg.tempo_limite = field_int(b, "tempo_limite", 60);
  if (cfg.tempo_limite <= 0) {
    fail(span, "llm '" + name + "': 'tempo_limite' deve ser > 0 segundos");
  }
  cfg.tentativas = field_int(b, "tentativas", 3);
  if (cfg.tentativas < 1) fail(span, "llm '" + name + "': 'tentativas' deve ser >= 1");
  cfg.teto_tokens = field_int(b, "teto_tokens", 0);
  if (const Item* fc = find_field(b, "cache"); fc && fc->value) {
    if (fc->value->kind != ExprKind::BoolLit) {
      fail(fc->value->span, "llm cache deve ser logico");
    }
    cfg.cache = fc->value->boolean;
  }
  if (cfg.teto_tokens < 0) fail(span, "llm '" + name + "': 'teto_tokens' deve ser >= 0");
  if (const Item* fr = find_field(b, "reserva"); fr && fr->value) {
    if (fr->value->kind != ExprKind::ListLit) {
      fail(fr->value->span, "llm '" + name + "': 'reserva' deve ser lista [outro_llm, ...]");
    }
    for (const auto& el : fr->value->elems) {
      if (!el || (el->kind != ExprKind::Name && el->kind != ExprKind::TextLit)) {
        fail(fr->value->span, "llm '" + name + "': 'reserva' espera nomes de llm ([eco2, ...])");
      }
      cfg.reserva.push_back(el->text);
    }
  }
  return cfg;
}

// Cadeia primario + reservas (um nivel: reserva de reserva nao e seguida;
// nomes repetidos ou inexistentes falham claro aqui, antes da rede).
std::vector<rt::LlmConfig> Interpreter::cadeia_llm(const std::string& name, Span span) {
  std::vector<rt::LlmConfig> out;
  out.push_back(llm_config(name, span));
  for (const std::string& r : out[0].reserva) {
    if (r == name) fail(span, "llm '" + name + "': 'reserva' nao pode conter a si mesmo");
    auto it = entities_.find(r);
    if (it == entities_.end() || it->second->key != "llm") {
      fail(span, "llm '" + name + "': reserva '" + r + "' nao e um 'llm' declarado");
    }
    for (const auto& c : out) {
      if (c.nome == r) fail(span, "llm '" + name + "': reserva '" + r + "' repetida");
    }
    out.push_back(llm_config(r, span));
  }
  return out;
}

rt::Value Interpreter::eval_perguntar(const Expr& call, Env& env, bool fluxo) {
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
  rt::RespostaLLM resp;
  try {
    resp = fluxo ? rt::llm_chat_fluxo_cadeia(cadeia_llm(llm_name, call.span), system, user)
                 : rt::llm_chat_cadeia(cadeia_llm(llm_name, call.span), system, user);
  } catch (const std::exception& e) {
    fail(call.span, std::string("LLM: ") + e.what());
  }
  const std::string& raw = resp.texto;

  if (const Value* fmt = kw.find("formato"); fmt && fmt->kind == ValueKind::Texto) {
    return structured_from_tipo(fmt->s, raw, call.span);
  }
  Value out = Value::mapa();
  out.map->set("texto", Value::texto(raw));
  out.map->set("modelo", Value::texto(resp.modelo.empty() ? cfg.model : resp.modelo));
  Value toks = Value::mapa();
  toks.map->set("entrada", Value::inteiro(resp.tok_entrada));
  toks.map->set("saida", Value::inteiro(resp.tok_saida));
  out.map->set("tokens", toks);
  return out;
}

rt::Value Interpreter::field_default(const ast::Item& field) {
  if (field.default_value) {
    try {
      return eval(*field.default_value, root_);
    } catch (...) {
      // Padrao nao-avaliavel (ex.: depende de variavel): padrao do tipo.
    }
  }
  return default_for_type(field.value.get());
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
      v = got ? *got : field_default(*f);
    } else {
      v = field_default(*f);
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
    if (pinecone) {
      try {
        rt::pinecone_ensure_namespace(pinecone_base, pinecone_ns);
      } catch (const std::exception& e) {
        fail(call.span, std::string(e.what()));
      }
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
      if (pinecone) {
        try {
          rt::pinecone_ensure_namespace(pinecone_base, pinecone_ns);
        } catch (const std::exception& e) {
          fail(call.span, std::string(e.what()));
        }
      }
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
  const std::string nome = decl_name(tool_decl);
  if (route_tool_allowlist_ && route_tool_allowlist_->find(nome) == route_tool_allowlist_->end()) {
    fail(span, "ferramenta '" + nome + "' nao permitida neste servico HTTP");
  }
  std::vector<const Item*> campos;
  if (tool_decl.block) {
    if (const Item* entrada = find_field(*tool_decl.block, "entrada"); entrada && entrada->block) {
      for (const auto& f : entrada->block->items) {
        if (f && f->kind == ItemKind::Field) campos.push_back(f.get());
      }
    }
  }
  for (const auto& [key, value] : args.items) {
    const Item* campo = nullptr;
    for (const Item* f : campos) {
      if (f->key == key) {
        campo = f;
        break;
      }
    }
    if (!campo) {
      fail(span, "ferramenta '" + nome + "': campo de entrada '" + key + "' nao declarado");
    }
    if (campo->value && !ferramenta_valor_compativel(campo->value.get(), value)) {
      fail(span, "ferramenta '" + nome + "': campo '" + key + "' espera " +
                     ferramenta_tipo_texto(campo->value.get()) + ", recebeu '" + value.type_name() +
                     "'");
    }
  }
  for (const Item* campo : campos) {
    const Value* value = args.find(campo->key);
    if (!value) {
      const bool opcional = campo->value && campo->value->kind == ExprKind::Index &&
                            campo->value->lhs && campo->value->lhs->kind == ExprKind::Name &&
                            campo->value->lhs->text == "opcional";
      if (!opcional) {
        fail(span, "ferramenta '" + nome + "': campo de entrada '" + campo->key + "' obrigatorio");
      }
      env.vars[campo->key] = Value::nulo();
      continue;
    }
    env.vars[campo->key] = *value;
  }
  if (tool_decl.block) {
    if (const Item* exec = find_field(*tool_decl.block, "executar"); exec && exec->block) {
      try {
        exec_block(*exec->block, env);
      } catch (const ReturnSignal& r) {
        return r.value;
      }
    }
  }
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
  // Memoria vetorial: recupera os turnos mais similares e prefixa no prompt
  // (mesmo ponto onde a conversa entraria; os dois modos sao exclusivos).
  // O modelo de embeddings vem de `embeddings:` (default text-embedding-3-small).
  std::vector<float> mem_vec;
  if (memoria == "vetorial") {
    const std::string emb_model = field_str(cfg, "embeddings");
    const std::string& modelo = emb_model.empty() ? "text-embedding-3-small" : emb_model;
    mem_vec = rt::llm_embed(modelo, message);
    std::vector<rt::MemoryIndex::Hit> hits;
    {
      std::lock_guard<std::mutex> lk(agent_memory_mutex_);
      auto it = agent_vector_memory_.find(agent_name);
      if (it != agent_vector_memory_.end()) hits = it->second.search(mem_vec, 3);
    }
    if (!hits.empty()) {
      std::string lembretes = "Lembretes relevantes:";
      for (const auto& h : hits) lembretes += "\n- " + h.text;
      prompt = lembretes + "\n" + message;
    }
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
    const std::vector<rt::LlmConfig> cadeia = cadeia_llm(llm_name, call.span);
    std::string system = papel;

    if (tools.empty()) {
      try {
        answer = rt::llm_chat_cadeia(cadeia, system, prompt).texto;
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
          raw = rt::llm_chat_cadeia(cadeia, system, user).texto;
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
          const std::string raw = rt::llm_chat_cadeia(
                  cadeia,
                  system + "\n\nLimite de passos atingido. Responda agora no formato responder: <sintese>.",
                  user)
                                              .texto;
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
  if (memoria == "vetorial" && !mem_vec.empty()) {
    // Guarda o turno com o embedding da pergunta (o mesmo usado na busca).
    // Sem poda no indice: acima do teto, turnos novos nao entram.
    constexpr std::size_t kMaxMemoriaTurnos = 200;
    std::lock_guard<std::mutex> lk(agent_memory_mutex_);
    rt::MemoryIndex& store = agent_vector_memory_[agent_name];
    if (store.size() < kMaxMemoriaTurnos) {
      store.insert(std::to_string(store.size()), "usuario: " + message + "\nagente: " + answer,
                   mem_vec);
    }
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
    const std::vector<rt::LlmConfig> cadeia = cadeia_llm(sup_name, call.span);

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
        raw = rt::llm_chat_cadeia(cadeia, system, user).texto;
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
  std::string version;
  std::string path;
  const Item* field = nullptr;  // the `rota` Field (has `entrada:` / `passos:`)
};

bool api_version_valida(const std::string& version) {
  if (version.size() < 2 || version[0] != 'v') return false;
  for (std::size_t i = 1; i < version.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(version[i]))) return false;
  }
  return true;
}

std::string caminho_versionado(const std::string& version, const std::string& path) {
  std::string suffix = path;
  if (suffix.empty() || suffix.front() != '/') suffix.insert(suffix.begin(), '/');
  return "/" + version + suffix;
}

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
    if (it->header.size() >= 3 && it->header[1] && it->header[2] &&
        (it->header[1]->kind == ExprKind::Name || it->header[1]->kind == ExprKind::TextLit) &&
        it->header[2]->kind == ExprKind::TextLit) {
      r.version = it->header[1]->text;
      r.path = caminho_versionado(r.version, it->header[2]->text);
    } else if (it->header.size() >= 2 && it->header[1] &&
               it->header[1]->kind == ExprKind::TextLit) {
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
  for (const Route& route : routes) {
    if (!route.version.empty() && !api_version_valida(route.version)) {
      fail(route.field ? route.field->span : svc->span,
           "versao de API invalida '" + route.version + "' (use v1, v2 ou vN)");
    }
  }
  std::unordered_set<std::string> ferramentas_http;
  const Item* fpermitidas = find_field(*svc->block, "ferramentas");
  const bool allowlist_http = fpermitidas != nullptr;
  if (fpermitidas) {
    if (!fpermitidas->value || fpermitidas->value->kind != ExprKind::ListLit) {
      fail(fpermitidas->span,
           "servico '" + decl_name(*svc) + "': 'ferramentas' espera uma lista de nomes");
    }
    for (const auto& item : fpermitidas->value->elems) {
      if (!item || (item->kind != ExprKind::Name && item->kind != ExprKind::TextLit)) {
        fail(item ? item->span : fpermitidas->span,
             "servico '" + decl_name(*svc) + "': 'ferramentas' espera nomes de ferramentas");
      }
      const std::string nome = item->text;
      auto ferramenta = entities_.find(nome);
      if (ferramenta == entities_.end() || ferramenta->second->key != "ferramenta") {
        fail(item->span,
             "servico '" + decl_name(*svc) + "': ferramenta '" + nome + "' nao declarada");
      }
      ferramentas_http.insert(nome);
    }
  }
  // Observabilidade opt-in: GET /saude, GET /metricas e a variante Prometheus
  // sao implicitos (rotas do usuario com o mesmo metodo+caminho vencem).
  auto campo_ligado = [&](const char* nome) {
    const Item* f = find_field(*svc->block, nome);
    return f && f->value && f->value->kind == ExprKind::BoolLit && f->value->boolean;
  };
  const bool tem_saude = campo_ligado("saude");
  const bool tem_metricas = campo_ligado("metricas");
  auto prometheus_escape = [](const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    for (const char c : value) {
      if (c == '\\' || c == '"' || c == '\n') escaped += '\\';
      escaped += c;
    }
    return escaped;
  };
  auto metricas_prometheus = [&]() {
    std::ostringstream out;
    out << "# HELP tilt_http_requests_total Total de requisicoes HTTP.\n"
        << "# TYPE tilt_http_requests_total counter\n"
        << "tilt_http_requests_total{service=\""
        << prometheus_escape(decl_name(*svc)) << "\"} ";
    {
      std::lock_guard<std::mutex> lk(metricas_mutex_);
      out << metricas_.requisicoes << "\n"
          << "# HELP tilt_http_errors_total Total de respostas HTTP 5xx.\n"
          << "# TYPE tilt_http_errors_total counter\n"
          << "tilt_http_errors_total{service=\"" << prometheus_escape(decl_name(*svc))
          << "\"} " << metricas_.erros << "\n"
          << "# HELP tilt_http_request_duration_microseconds_total Soma das latencias HTTP.\n"
          << "# TYPE tilt_http_request_duration_microseconds_total counter\n"
          << "# HELP tilt_http_request_duration_microseconds_max Maior latencia HTTP observada.\n"
          << "# TYPE tilt_http_request_duration_microseconds_max gauge\n";
      for (const auto& kv : metricas_.por_rota) {
        const std::size_t sep = kv.first.find(' ');
        const std::string metodo = sep == std::string::npos ? kv.first : kv.first.substr(0, sep);
        const std::string rota = sep == std::string::npos ? "/" : kv.first.substr(sep + 1);
        const std::string labels = "{service=\"" + prometheus_escape(decl_name(*svc)) +
                                   "\",method=\"" + prometheus_escape(metodo) +
                                   "\",route=\"" + prometheus_escape(rota) + "\"}";
        out << "tilt_http_request_duration_microseconds_total" << labels << ' '
            << kv.second.latencia_total_us << "\n"
            << "tilt_http_request_duration_microseconds_max" << labels << ' '
            << kv.second.latencia_max_us << "\n";
      }
    }
    return out.str();
  };
  {
    const std::time_t agora = std::time(nullptr);
    char iso[32];
    std::strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&agora));
    std::lock_guard<std::mutex> lk(metricas_mutex_);
    metricas_.inicio = iso;
  }
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
        const auto inicio_requisicao = std::chrono::steady_clock::now();
        const std::string trace_id =
            "req-" + std::to_string(proximo_trace_id_.fetch_add(1, std::memory_order_relaxed));
        const Route* match = nullptr;
        for (const Route& r : routes) {
          if (r.method == req.method && r.path == req.path) {
            match = &r;
            break;
          }
        }

        const bool metricas_prometheus_request =
            req.method == "GET" &&
            (req.path == "/metricas/prometheus" || req.path == "/metricas?formato=prometheus" ||
             req.path == "/metricas?format=prometheus");
        // Rotas implicitas de observabilidade (so GET; rota do usuario vence).
        if (!match && req.method == "GET" && req.path == "/saude" && tem_saude) {
          Value corpo = Value::mapa();
          corpo.map->set("status", Value::texto("ok"));
          corpo.map->set("servico", Value::texto(decl_name(*svc)));
          corpo.map->set("rotas", Value::inteiro(static_cast<std::int64_t>(routes.size())));
          resp.status = 200;
          resp.body = json_dump(corpo);
        } else if (!match && metricas_prometheus_request && tem_metricas) {
          resp.status = 200;
          resp.content_type = "text/plain; version=0.0.4; charset=utf-8";
          resp.body = metricas_prometheus();
        } else if (!match && req.method == "GET" && req.path == "/metricas" && tem_metricas) {
          Value corpo = Value::mapa();
          Value rotas = Value::mapa();
          {
            std::lock_guard<std::mutex> lk(metricas_mutex_);
            corpo.map->set("inicio", Value::texto(metricas_.inicio));
            corpo.map->set("requisicoes", Value::inteiro(metricas_.requisicoes));
            corpo.map->set("erros", Value::inteiro(metricas_.erros));
            for (const auto& kv : metricas_.por_rota) {
              Value m = Value::mapa();
              m.map->set("total", Value::inteiro(kv.second.total));
              m.map->set("erros", Value::inteiro(kv.second.erros));
              Value lat = Value::mapa();
              lat.map->set("total", Value::inteiro(kv.second.latencia_total_us));
              const double media = kv.second.total > 0
                                       ? static_cast<double>(kv.second.latencia_total_us) /
                                             static_cast<double>(kv.second.total)
                                       : 0.0;
              lat.map->set("media", Value::decimal(media));
              lat.map->set("max", Value::inteiro(kv.second.latencia_max_us));
              m.map->set("latencia_us", std::move(lat));
              rotas.map->set(kv.first, m);
            }
          }
          corpo.map->set("por_rota", rotas);
          resp.status = 200;
          resp.body = json_dump(corpo);
        } else if (!match) {
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
                if (!f || f->kind != ItemKind::Field || !parsed.map) continue;
                const Value* got = parsed.map->find(f->key);
                if (!got) {
                  // Campo com padrao declarado (`campo: Tipo = padrao`) e
                  // preenchido em vez de rejeitar; sem padrao, 400.
                  if (f->default_value) {
                    parsed.map->set(f->key, field_default(*f));
                    continue;
                  }
                  resp.status = 400;
                  resp.body = R"({"erro":"campo ')" + f->key + R"(' ausente"})";
                  bad = true;
                  break;
                }
                // Tipo do valor presente contra o declarado (so escalares;
                // resto passa sem verificacao). Inteiro alarga para decimal.
                if (f->value && f->value->kind == ExprKind::Name) {
                  const std::string& want = f->value->text;
                  const bool ok = (want == "texto" && got->kind == ValueKind::Texto) ||
                                  (want == "inteiro" && got->kind == ValueKind::Inteiro) ||
                                  (want == "decimal" &&
                                   (got->kind == ValueKind::Decimal || got->kind == ValueKind::Inteiro)) ||
                                  (want == "logico" && got->kind == ValueKind::Logico);
                  const bool checavel =
                      want == "texto" || want == "inteiro" || want == "decimal" || want == "logico";
                  if (checavel && !ok) {
                    resp.status = 400;
                    resp.body = R"({"erro":"campo ')" + f->key + "' deve ser " + want + "\"}";
                    bad = true;
                    break;
                  }
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
            const auto* allowlist_anterior = route_tool_allowlist_;
            route_tool_allowlist_ = allowlist_http ? &ferramentas_http : nullptr;
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
            } catch (...) {
              route_tool_allowlist_ = allowlist_anterior;
              route_resp_ = nullptr;
              throw;
            }
            route_tool_allowlist_ = allowlist_anterior;
            route_resp_ = nullptr;
          }
        }

        const auto duracao = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - inicio_requisicao)
                                 .count();
        const long long latencia_us = std::max<long long>(1, duracao);
        auto json_log_escape = [](const std::string& value) {
          std::string escaped;
          escaped.reserve(value.size() + 2);
          for (const char c : value) {
            switch (c) {
              case '"': escaped += "\\\""; break;
              case '\\': escaped += "\\\\"; break;
              case '\n': escaped += "\\n"; break;
              case '\r': escaped += "\\r"; break;
              case '\t': escaped += "\\t"; break;
              default: escaped += c; break;
            }
          }
          return escaped;
        };
        {
          std::lock_guard<std::mutex> lk(log_mutex_);
          out_ << "{\"trace_id\":\"" << json_log_escape(trace_id)
               << "\",\"metodo\":\"" << json_log_escape(req.method)
               << "\",\"rota\":\"" << json_log_escape(req.path)
               << "\",\"status\":" << resp.status << ",\"latencia_us\":" << latencia_us
               << "}\n"
               << std::flush;
        }
        // Contabilidade (menos as implicitas, para nao poluir).
        if (!((req.method == "GET" && req.path == "/saude" && tem_saude) ||
              (req.method == "GET" && req.path == "/metricas" && tem_metricas))) {
          const auto duracao = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - inicio_requisicao)
                                   .count();
          const long long latencia_us = std::max<long long>(1, duracao);
          std::lock_guard<std::mutex> lk(metricas_mutex_);
          metricas_.requisicoes += 1;
          auto& par = metricas_.por_rota[req.method + " " + req.path];
          par.total += 1;
          par.latencia_total_us += latencia_us;
          par.latencia_max_us = std::max(par.latencia_max_us, latencia_us);
          if (resp.status >= 500) {
            metricas_.erros += 1;
            par.erros += 1;
          }
        }
        return resp;
      },
      max_requests, threads);
  return served < 0 ? 1 : 0;
}

// ------------------------------------------------------------------ statements

void Interpreter::exec_block(const ast::Block& block, Env& env, const PrazoPasso* prazo) {
  // Roda um item de topo com deadline (timeout por passo): worker thread +
  // espera limitada; ao estourar, a thread é destacada (segue sozinha) e o
  // passo falha com T901. Sem prazo, execucao direta.
  auto com_prazo = [&](const Item& it, std::size_t passo) {
    if (!prazo || prazo->segundos <= 0) {
      exec_item(it, env);
      return;
    }
    std::packaged_task<void()> tarefa([&] { exec_item(it, env); });
    std::future<void> fut = tarefa.get_future();
    std::thread th(std::move(tarefa));
    if (fut.wait_for(std::chrono::seconds(prazo->segundos)) == std::future_status::ready) {
      th.join();
      fut.get();  // relança RuntimeAbort do passo
      return;
    }
    // Estourou: o Env passa para a lista de zumbis (vive enquanto a thread
    // destacada precisar) e o passo falha para o retry la de cima.
    if (prazo->dono) {
      std::lock_guard<std::mutex> lk(zumbis_mu_);
      zumbis_.push_back(prazo->dono);
    }
    th.detach();
    throw RuntimeAbort{it.span,
                       "passo " + std::to_string(passo) + " excedeu tempo_limite de " +
                           std::to_string(prazo->segundos) + "s",
                       DiagCode::RuntimeError, {}};
  };
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
    com_prazo(*it, k + 1);
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
        *lookup_lvalue(*stmt.a, env) = std::move(v);
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
      // Quarentena (dead-letter): herdada pela cadeia de envs; a linha que
      // falha vai para o JSONL em vez de abortar o pipeline.
      std::shared_ptr<QuarentenaState> qst;
      for (Env* e = &env; e; e = e->parent) {
        if (e->quarentena) {
          qst = e->quarentena;
          break;
        }
      }
      if (seq.list) {
        for (const Value& element : *seq.list) {
          Env inner;
          inner.parent = &env;
          inner.vars[stmt.name] = element;
          if (!qst) {
            exec_block(stmt.body, inner);
            continue;
          }
          try {
            exec_block(stmt.body, inner);
          } catch (const RuntimeAbort& a) {
            Value doc = Value::mapa();
            doc.map->set("linha", element);
            doc.map->set("erro", Value::texto(a.message));
            std::lock_guard<std::mutex> lk(qst->mu);
            std::ofstream out(qst->caminho, std::ios::app);
            if (out) out << rt::json_dump(doc) << "\n";
            qst->n += 1;
          }
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

// Resolve an assignment target without evaluating it to a temporary. Values
// are returned by value by eval(), so member/index assignment must walk through
// the owning object and return the actual slot in the map/list.
Value* Interpreter::lookup_lvalue(const Expr& target, Env& env) {
  switch (target.kind) {
    case ExprKind::Name: {
      if (Value* slot = env.lookup(target.text)) return slot;
      fail(target.span, "nome '" + target.text + "' nao definido");
      return nullptr;
    }
    case ExprKind::Member: {
      if (!target.lhs) {
        fail(target.span, "alvo de atribuicao invalido");
        return nullptr;
      }
      Value* base = lookup_lvalue(*target.lhs, env);
      if (!base || base->kind != ValueKind::Mapa || !base->map) {
        fail(target.span, std::string("'") + (base ? base->type_name() : "nulo") +
             "' nao permite atribuicao de campo");
        return nullptr;
      }
      if (Value* slot = base->map->find(target.text)) return slot;
      base->map->items.emplace_back(target.text, Value::nulo());
      return &base->map->items.back().second;
    }
    case ExprKind::Index: {
      if (!target.lhs || target.elems.empty()) {
        fail(target.span, "alvo de atribuicao indexado invalido");
        return nullptr;
      }
      Value* base = lookup_lvalue(*target.lhs, env);
      Value idx = eval(*target.elems.front(), env);
      if (base->kind == ValueKind::Lista || base->kind == ValueKind::Tabela) {
        if (!base->list) {
          fail(target.span, "lista sem armazenamento para atribuicao");
          return nullptr;
        }
        const std::int64_t n = static_cast<std::int64_t>(base->list->size());
        std::int64_t i = idx.is_number() ? static_cast<std::int64_t>(idx.as_number()) : 0;
        if (i < 0) i += n;
        if (i < 0 || i >= n) {
          fail(target.span, "indice fora dos limites");
          return nullptr;
        }
        return &(*base->list)[static_cast<std::size_t>(i)];
      }
      if (base->kind == ValueKind::Mapa && base->map) {
        const std::string key = idx.kind == ValueKind::Texto ? idx.s : to_display(idx);
        if (Value* slot = base->map->find(key)) return slot;
        base->map->items.emplace_back(key, Value::nulo());
        return &base->map->items.back().second;
      }
      fail(target.span, std::string("nao e possivel atribuir em '") + base->type_name() + "'");
      return nullptr;
    }
    default:
      fail(target.span, "alvo de atribuicao deve ser um nome, campo ou indice");
      return nullptr;
  }
}

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
      if (expr.lhs) *lookup_lvalue(*expr.lhs, env) = v;
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

namespace {

std::uint64_t z_order_scalar(const Value& v) {
  if (v.kind == ValueKind::Inteiro) return static_cast<std::uint64_t>(v.i) ^ (1ULL << 63);
  if (v.kind == ValueKind::Decimal) {
    const double clamped =
        std::isfinite(v.d) ? std::clamp(v.d, -9000000000000.0, 9000000000000.0) : 0.0;
    const auto scaled = static_cast<std::int64_t>(std::llround(clamped * 1000000.0));
    return static_cast<std::uint64_t>(scaled) ^ (1ULL << 63);
  }
  if (v.kind == ValueKind::Logico) return v.b ? 1ULL : 0ULL;
  const std::string text = rt::to_display(v);
  std::uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

Value aplicar_z_order(const Value& source, const rt::ValueMap& kw, const char* builtin,
                      const Span& span) {
  (void)span;
  const Value* spec = kw.find("z_order");
  if (!spec) return source;
  if (spec->kind != ValueKind::Lista || !spec->list || spec->list->empty() ||
      spec->list->size() > 8) {
    throw std::runtime_error(std::string(builtin) +
                             ": z_order deve ser uma lista de 1 a 8 colunas");
  }
  std::vector<std::string> cols;
  for (const Value& item : *spec->list) {
    if (item.kind != ValueKind::Texto || item.s.empty() ||
        std::find(cols.begin(), cols.end(), item.s) != cols.end()) {
      throw std::runtime_error(std::string(builtin) +
                               ": z_order deve conter nomes de colunas distintos");
    }
    cols.push_back(item.s);
  }
  if ((source.kind != ValueKind::Tabela && source.kind != ValueKind::Lista) || !source.list) {
    throw std::runtime_error(std::string(builtin) + " espera uma tabela para aplicar z_order");
  }
  const int bits = 64 / static_cast<int>(cols.size());
  struct RowKey {
    std::uint64_t key = 0;
    std::size_t index = 0;
  };
  std::vector<std::vector<std::uint64_t>> coordinates;
  coordinates.reserve(source.list->size());
  for (std::size_t row_index = 0; row_index < source.list->size(); ++row_index) {
    const Value& row = (*source.list)[row_index];
    if (row.kind != ValueKind::Mapa || !row.map) {
      throw std::runtime_error(std::string(builtin) + ": z_order exige linhas como mapas");
    }
    std::vector<std::uint64_t> values;
    values.reserve(cols.size());
    for (const std::string& col : cols) {
      const Value* field = row.map->find(col);
      if (!field)
        throw std::runtime_error(std::string(builtin) + ": coluna de z_order ausente: '" + col +
                                 "'");
      values.push_back(z_order_scalar(*field));
    }
    coordinates.push_back(std::move(values));
  }
  std::vector<std::uint64_t> mins(cols.size(), std::numeric_limits<std::uint64_t>::max());
  std::vector<std::uint64_t> maxs(cols.size(), 0);
  for (const auto& values : coordinates) {
    for (std::size_t column = 0; column < values.size(); ++column) {
      mins[column] = std::min(mins[column], values[column]);
      maxs[column] = std::max(maxs[column], values[column]);
    }
  }
  std::vector<RowKey> keys;
  keys.reserve(coordinates.size());
  for (std::size_t row_index = 0; row_index < coordinates.size(); ++row_index) {
    std::uint64_t key = 0;
    for (int bit = bits - 1; bit >= 0; --bit) {
      for (std::size_t column = 0; column < cols.size(); ++column) {
        const std::uint64_t value = coordinates[row_index][column];
        std::uint64_t normalized = 0;
        if (maxs[column] != mins[column]) {
          const long double fraction = static_cast<long double>(value - mins[column]) /
                                       static_cast<long double>(maxs[column] - mins[column]);
          normalized = static_cast<std::uint64_t>(
              fraction * static_cast<long double>(std::numeric_limits<std::uint64_t>::max()));
        }
        const int source_bit = 64 - bits + bit;
        key = (key << 1) | ((normalized >> source_bit) & 1ULL);
      }
    }
    keys.push_back({key, row_index});
  }
  std::stable_sort(keys.begin(), keys.end(),
                   [](const RowKey& a, const RowKey& b) { return a.key < b.key; });
  Value out = source.kind == ValueKind::Tabela ? Value::tabela() : Value::lista();
  for (const RowKey& k : keys) out.list->push_back((*source.list)[k.index]);
  return out;
}

}  // namespace

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

int Interpreter::run_jit() {
  jit_mode_ = true;
  return run_vm();
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
      // Disco antes de compilar (.tiltc; nparams valida contra a assinatura).
      const std::string tkey = "funcao " + decl_name(fn);
      if (auto tit = tiltc_prog_.entries.find(tkey);
          tit != tiltc_prog_.entries.end() && !tit->second.is_pipeline &&
          tit->second.nparams == static_cast<int>(fn.params.size())) {
        chunk = std::make_shared<vm::Chunk>(tit->second.chunk);
        tiltc_note("hit");
      } else {
        try {
          std::unordered_set<std::string> names;
          for (const auto& kv : functions_) names.insert(kv.first);
          chunk = std::make_shared<vm::Chunk>(vm::compile_function(fn, names));
        } catch (const vm::NotCompilable&) {
          chunk = nullptr;
        }
        if (chunk) {
          vm::CachedChunk cc;
          cc.is_pipeline = false;
          cc.nparams = static_cast<int>(fn.params.size());
          cc.chunk = *chunk;
          tiltc_prog_.entries[tkey] = std::move(cc);
          tiltc_dirty_ = true;
        }
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
    if (jit_mode_) {
      bool integer_args = true;
      for (const Value& arg : args) {
        if (arg.kind != ValueKind::Inteiro) {
          integer_args = false;
          break;
        }
      }
      vm::Jit jit(out_);
      std::string why;
      if (integer_args && jit.can_compile(*chunk, &why)) {
        try {
          return jit.run(*chunk, std::move(args));
        } catch (const std::exception& e) {
          fail(fn.span, std::string("JIT: ") + e.what());
          return Value::nulo();
        }
      }
    }
    vm::Vm machine(out_, [this](const std::string& name, std::vector<Value>& a, bool* handled) {
      return vm_call_hook(name, a, handled);
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
    long long versao = -1;
    if (const Value* v = kw.find("versao")) {
      if (v->kind != ValueKind::Inteiro || v->i < 0) {
        fail(call.span, "ler_delta: 'versao' deve ser inteiro >= 0");
      }
      versao = static_cast<long long>(v->i);
    }
    try {
      Value t = rt::delta_read(a[0].s, onde, versao);
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
    const Value* fluxo = kw.find("fluxo");
    const bool e_fluxo =
        fluxo && fluxo->kind == ValueKind::Logico && fluxo->b;
    const bool e_parquet =
        a[0].s.size() >= 8 && a[0].s.compare(a[0].s.size() - 8, 8, ".parquet") == 0;
    if (e_fluxo) {
      // Descritor de streaming: nada e materializado aqui; o treino varre o
      // arquivo (contagem + rotulos) e o rele em blocos por epoca.
      if (e_parquet) {
        rt::ParquetFluxo pfx;
        try {
          pfx = rt::parquet_abrir_fluxo(a[0].s);
        } catch (const std::exception& e) {
          fail(call.span, std::string(e.what()));
        }
        bool tem_alvo = false;
        for (const std::string& c : pfx.colunas) {
          if (c == alvo->s) tem_alvo = true;
        }
        if (!tem_alvo) {
          fail(call.span, "carregador: coluna '" + alvo->s + "' ausente em '" + a[0].s + "'");
        }
        Value out = Value::mapa();
        out.map->set("fluxo_csv", Value::texto(a[0].s));
        out.map->set("alvo", Value::texto(alvo->s));
        rt::ValueList fl;
        for (const std::string& c : pfx.colunas) {
          if (c != alvo->s) fl.push_back(Value::texto(c));
        }
        out.map->set("atributos", Value::lista(std::move(fl)));
        return out;
      }
      FluxoCSV fx;
      fx.caminho = a[0].s;
      fx.alvo = alvo->s;
      try {
        abrir_fluxo_csv(fx);
      } catch (const std::exception& e) {
        fail(call.span, std::string(e.what()));
      }
      Value out = Value::mapa();
      out.map->set("fluxo_csv", Value::texto(fx.caminho));
      out.map->set("alvo", Value::texto(fx.alvo));
      rt::ValueList fl;
      for (const std::string& c : fx.atributos) fl.push_back(Value::texto(c));
      out.map->set("atributos", Value::lista(std::move(fl)));
      return out;
    }
    Value tbl = e_parquet ? rt::parquet_read(a[0].s) : read_csv_file(a[0].s, call.span);
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
        fail(call.span, "escrever_parquet: 'codec' deve ser \"gzip\", \"snappy\" ou \"zstd\"");
      }
      if (codec->s == "gzip") {
        opts.codec = 2;
      } else if (codec->s == "snappy") {
        opts.codec = 1;
      } else if (codec->s == "zstd") {
        opts.codec = 6;
      } else {
        fail(call.span, "escrever_parquet: codec '" + codec->s +
                            "' invalido (use \"gzip\", \"snappy\" ou \"zstd\")");
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
      Value ordenada = aplicar_z_order(a[0], kw, "escrever_delta", call.span);
      rt::delta_write(a[1].s, ordenada, part_cols);
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
      Value ordenada = aplicar_z_order(a[0], kw, "anexar_delta", call.span);
      rt::delta_append(a[1].s, ordenada, part_cols);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "otimizar_delta" || name == "optimize_delta") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span, "otimizar_delta espera (diretorio)");
    }
    try {
      rt::delta_optimize(a[0].s);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "vacuum_delta") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span, "vacuum_delta espera (diretorio)");
    }
    try {
      return Value::inteiro(rt::delta_vacuum(a[0].s));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
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
      Value ordenada = aplicar_z_order(a[0], kw, "escrever_iceberg", call.span);
      rt::iceberg_write(a[1].s, ordenada, part_cols);
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
      Value ordenada = aplicar_z_order(a[0], kw, "anexar_iceberg", call.span);
      rt::iceberg_append(a[1].s, ordenada, part_cols);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "otimizar_iceberg" || name == "optimize_iceberg") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span, "otimizar_iceberg espera (diretorio)");
    }
    try {
      rt::iceberg_optimize(a[0].s);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "vacuum_iceberg") {
    auto a = args();
    if (a.empty() || a[0].kind != ValueKind::Texto) {
      fail(call.span, "vacuum_iceberg espera (diretorio)");
    }
    try {
      return Value::inteiro(rt::iceberg_vacuum(a[0].s));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
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
    return eval_perguntar(call, env, name == "perguntar_em_fluxo");
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
      t.data.assign(v.begin(), v.end());
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
    std::string grupo, broker, formato, schema_avro;
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
      if (const Value* fv = a[1].map->find("formato")) {
        if (fv->kind != ValueKind::Texto || (fv->s != "texto" && fv->s != "avro")) {
          fail(call.span, "ler_kafka: 'formato' deve ser \"texto\" ou \"avro\"");
        }
        formato = fv->s;
      }
      if (const Value* sv = a[1].map->find("schema")) {
        if (sv->kind != ValueKind::Texto) fail(call.span, "ler_kafka: 'schema' deve ser texto");
        schema_avro = sv->s;
      }
    }
    auto decodificar_avro = [&](Value raw) {
      if (formato != "avro") return raw;
      if (schema_avro.empty()) fail(call.span, "ler_kafka: formato avro requer 'schema'");
      Value out = Value::lista();
      for (const Value& payload : *raw.list) {
        try {
          out.list->push_back(rt::avro_confluent_decode(schema_avro, payload.s).value);
        } catch (const std::exception& e) {
          fail(call.span, std::string(e.what()));
        }
      }
      return out;
    };
    try {
      if (!grupo.empty()) {
        // Consumer group: coordenacao + checkpoint por commit de offset.
        Value out = Value::lista();
        for (const auto& [part, valor] : rt::kafka_consume_group(broker, grupo, a[0].s,
                                                                 static_cast<int>(max), tls)) {
          (void)part;
          out.list->push_back(Value::texto(valor));
        }
        return decodificar_avro(std::move(out));
      }
      return decodificar_avro(rt::kafka_ler(a[0].s, do_fim, max, broker, tls));
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
      std::string body;
      if (a.size() >= 3 && a[2].kind == ValueKind::Mapa && a[2].map) {
        const Value* formato = a[2].map->find("formato");
        if (formato && formato->kind != ValueKind::Texto) {
          fail(call.span, "escrever_kafka: 'formato' deve ser texto");
        }
        if (formato && formato->s == "avro") {
          const Value* schema = a[2].map->find("schema");
          const Value* id = a[2].map->find("id_esquema");
          if (!schema || schema->kind != ValueKind::Texto || !id || id->kind != ValueKind::Inteiro) {
            fail(call.span, "escrever_kafka: Avro requer schema e id_esquema");
          }
          body = rt::avro_confluent_encode(schema->s, static_cast<std::int32_t>(id->i), a[1]);
        }
      }
      if (body.empty()) body = a[1].kind == ValueKind::Texto ? a[1].s : rt::json_dump(a[1]);
      rt::kafka_produzir(a[0].s, body, static_cast<std::int32_t>(particao), opt, tls);
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
    return Value::nulo();
  }
  if (name == "avro_codificar") {
    auto a = args();
    if (a.size() != 3 || a[1].kind != ValueKind::Texto || a[2].kind != ValueKind::Inteiro) {
      fail(call.span, "avro_codificar espera (valor, schema_json, id_esquema)");
    }
    try {
      return Value::texto(rt::avro_confluent_encode(
          a[1].s, static_cast<std::int32_t>(a[2].i), a[0]));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "avro_decodificar") {
    auto a = args();
    if (a.size() != 2 || a[0].kind != ValueKind::Texto || a[1].kind != ValueKind::Texto) {
      fail(call.span, "avro_decodificar espera (payload, schema_json)");
    }
    try {
      const rt::AvroConfluentValue decoded = rt::avro_confluent_decode(a[1].s, a[0].s);
      Value out = Value::mapa();
      out.map->set("id_esquema", Value::inteiro(decoded.schema_id));
      out.map->set("valor", decoded.value);
      return out;
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "avro_schema") {
    auto a = args();
    if (a.size() != 2 || a[0].kind != ValueKind::Texto || a[1].kind != ValueKind::Inteiro) {
      fail(call.span, "avro_schema espera (url_registry, id_esquema)");
    }
    try {
      return Value::texto(rt::avro_schema_registry_get(
          a[0].s, static_cast<std::int32_t>(a[1].i)));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "avro_registrar") {
    auto a = args();
    if (a.size() != 3 || a[0].kind != ValueKind::Texto || a[1].kind != ValueKind::Texto ||
        a[2].kind != ValueKind::Texto) {
      fail(call.span, "avro_registrar espera (url_registry, subject, schema_json)");
    }
    try {
      return Value::inteiro(rt::avro_schema_registry_register(a[0].s, a[1].s, a[2].s));
    } catch (const std::exception& e) {
      fail(call.span, std::string(e.what()));
    }
  }
  if (name == "transacao_kafka") {
    auto a = args();
    if (a.size() < 2 || a[0].kind != ValueKind::Texto || a[0].s.empty() ||
        a[1].kind != ValueKind::Lista || !a[1].list) {
      fail(call.span,
           "transacao_kafka espera (id, registros, {broker:, acks:, tentativas:, tls:}), "
           "com registros [{topico:, valor:, particao:, chave:?}]");
    }
    rt::KafkaTransactionOptions opt;
    if (a.size() >= 3) {
      if (a[2].kind != ValueKind::Mapa || !a[2].map) {
        fail(call.span,
             "transacao_kafka: opcoes devem ser um mapa {broker:, acks:, tentativas:, tls:}");
      }
      if (const Value* bv = a[2].map->find("broker")) {
        if (bv->kind != ValueKind::Texto) fail(call.span, "transacao_kafka: broker deve ser texto");
        opt.broker = bv->s;
      }
      if (const Value* av = a[2].map->find("acks")) {
        if (av->kind != ValueKind::Inteiro || (av->i != -1 && av->i != 1)) {
          fail(call.span, "transacao_kafka: acks deve ser -1 (all) ou 1 (leader)");
        }
        opt.acks = static_cast<int>(av->i);
      }
      if (const Value* tv = a[2].map->find("tentativas")) {
        if (tv->kind != ValueKind::Inteiro || tv->i < 1 || tv->i > 10) {
          fail(call.span, "transacao_kafka: tentativas deve ser inteiro entre 1 e 10");
        }
        opt.tentativas = static_cast<int>(tv->i);
      }
      if (const Value* tv = a[2].map->find("tls")) {
        if (tv->kind != ValueKind::Logico) fail(call.span, "transacao_kafka: tls deve ser logico");
        opt.tls = tv->b;
      }
    }
    std::vector<rt::KafkaTransactionRecord> registros;
    registros.reserve(a[1].list->size());
    for (const Value& item : *a[1].list) {
      if (item.kind != ValueKind::Mapa || !item.map) {
        fail(call.span, "transacao_kafka: cada registro deve ser um mapa");
      }
      const Value* tv = item.map->find("topico");
      const Value* vv = item.map->find("valor");
      if (!tv || tv->kind != ValueKind::Texto || tv->s.empty() || !vv) {
        fail(call.span, "transacao_kafka: cada registro precisa de topico e valor");
      }
      rt::KafkaTransactionRecord r;
      r.topico = tv->s;
      r.valor = vv->kind == ValueKind::Texto ? vv->s : rt::json_dump(*vv);
      if (const Value* pv = item.map->find("particao")) {
        if (pv->kind != ValueKind::Inteiro || pv->i < 0 || pv->i > 2147483647) {
          fail(call.span, "transacao_kafka: particao deve ser inteiro >= 0");
        }
        r.particao = static_cast<std::int32_t>(pv->i);
      }
      if (const Value* kv = item.map->find("chave")) {
        if (kv->kind != ValueKind::Texto) fail(call.span, "transacao_kafka: chave deve ser texto");
        r.chave = kv->s;
      }
      registros.push_back(std::move(r));
    }
    try {
      rt::kafka_transacao(a[0].s, registros, opt);
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
        std::int64_t padding = 0;
        std::int64_t dilatacao = 1;
        const rt::ValueMap kw = eval_kwargs(call, env);
        if (const Value* pv = kw.find("passo")) {
          passo = static_cast<std::int64_t>(pv->as_number());
        }
        if (const Value* pv = kw.find("padding")) {
          padding = static_cast<std::int64_t>(pv->as_number());
        }
        if (const Value* pv = kw.find("dilatacao")) {
          dilatacao = static_cast<std::int64_t>(pv->as_number());
        } else if (const Value* pv = kw.find("dilation")) {
          dilatacao = static_cast<std::int64_t>(pv->as_number());
        }
        return Value::tensor_de(rt::conv2d(t, *a[0].tensor, passo, padding, dilatacao));
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
