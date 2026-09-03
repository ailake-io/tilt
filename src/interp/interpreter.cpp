#include "interp/interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <ostream>
#include <sstream>
#include <string_view>
#include <utility>

#include "runtime/json.hpp"

namespace tilt {

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

int Interpreter::run() {
  try {
    for (const auto& item : program_.items) {
      if (!item || item->kind != ItemKind::Decl) continue;
      const std::string& kw = item->key;
      const std::string name = decl_name(*item);
      if (kw == "seja" || kw == "constante") {
        if (!name.empty()) root_.vars[name] = item->value ? eval(*item->value, root_) : Value::nulo();
      } else if (kw == "funcao") {
        if (!name.empty()) functions_[name] = item.get();
      } else if (kw == "pipeline") {
        pipelines_.push_back(item.get());
      } else if (kw == "treino") {
        // handled by the dedicated training loop; must not shadow `modelo <name>`
      } else if (!name.empty()) {
        entities_[name] = item.get();
      }
    }

    bool did_something = false;
    for (const auto& item : program_.items) {
      if (item && item->kind == ItemKind::Decl && item->key == "treino") {
        run_treino(*item);
        did_something = true;
      }
    }

    if (!pipelines_.empty()) {
      for (const Item* p : pipelines_) run_pipeline(*p);
    } else if (auto it = functions_.find("principal"); it != functions_.end()) {
      call_function(*it->second, {}, it->second->span);
    } else if (!did_something) {
      out_ << "nada para executar: nenhum 'pipeline', 'treino' nem 'funcao principal'\n";
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

// A cron expression is valid here if it has exactly five whitespace fields.
bool valid_cron(const std::string& expr) {
  int fields = 0;
  bool in_field = false;
  for (char c : expr) {
    if (c == ' ' || c == '\t') {
      in_field = false;
    } else if (!in_field) {
      in_field = true;
      ++fields;
    }
  }
  return fields == 5;
}

}  // namespace

void Interpreter::run_pipeline(const Item& pipeline) {
  out_ << "== pipeline " << decl_name(pipeline) << " ==\n";
  if (!pipeline.block) return;

  if (const Item* ag = find_field(*pipeline.block, "agenda");
      ag && ag->value && ag->value->kind == ExprKind::TextLit) {
    const std::string& cron = ag->value->text;
    if (!valid_cron(cron)) {
      fail(ag->value->span, "agenda '" + cron + "' nao e um cron de 5 campos");
    }
    if (schedule_mode_) {
      out_ << "agenda: '" << cron << "' registrada (execucao unica neste modo; "
           << "o loop real chega no M5.2)\n";
    }
  }

  const Item* passos = find_field(*pipeline.block, "passos");
  if (!passos || !passos->block) {
    out_ << "  (sem passos)\n";
    return;
  }

  const int retries = retry_count(pipeline);
  for (int attempt = 0; attempt <= retries; ++attempt) {
    try {
      Env env;
      env.parent = &root_;
      exec_block(*passos->block, env);
      return;
    } catch (const RuntimeAbort& a) {
      if (attempt >= retries) throw;
      out_ << "[retry] passo falhou (" << a.message << "); tentativa " << (attempt + 2) << "/"
           << (retries + 1) << "\n";
    }
  }
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

  auto field_text = [&](std::string_view key) -> std::string {
    const Item* f = find_field(*decl->block, key);
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
  };

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
  fail(span, "fonte '" + name + "': conector '" + (tipo.empty() ? "?" : tipo) +
                 "' nao implementado (M5.2)",
       DiagCode::ConnectorNotImplemented);
}

// ------------------------------------------------------------------ deep learning

namespace {

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
  bool noted_skip = false;

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
        } else if (word_in(key, {"relu", "gelu", "silu", "sigmoide", "tanh"})) {
          Layer l;
          l.kind = Layer::Activation;
          l.act = key;
          layers.push_back(std::move(l));
        } else if (!noted_skip && (key == "norma_lote" || key == "norma_camada" || key == "conv2d")) {
          out_ << "[nota] modelo " << name << ": camada '" << key
               << "' ainda nao suportada na inferencia (M6.2); ignorada\n";
          noted_skip = true;
        }
      });
    }
  }
  return layers;
}

const std::vector<Interpreter::Layer>& Interpreter::build_model(const Item& decl,
                                                               std::int64_t in_dim, Span span) {
  const std::string name = decl_name(decl);
  if (auto it = model_cache_.find(name); it != model_cache_.end()) return it->second;
  (void)span;
  std::vector<Layer> layers = build_layers(decl, in_dim);
  if (decl.block && find_field(*decl.block, "pesos")) {
    out_ << "[nota] modelo " << name
         << ": carga de 'pesos:' ainda nao implementada; usando init Xavier\n";
  }
  return model_cache_.emplace(name, std::move(layers)).first->second;
}

rt::Tensor Interpreter::forward_layers(const std::vector<Layer>& layers, rt::Tensor x) {
  for (const Layer& l : layers) {
    switch (l.kind) {
      case Layer::Dense:
        x = rt::add(rt::matmul(x, l.w), l.b);
        break;
      case Layer::Activation:
        x = rt::apply_unary(x, l.act);
        break;
      case Layer::Softmax:
        x = rt::softmax_last(x);
        break;
      case Layer::Dropout:
        break;
    }
  }
  return x;
}

rt::Value Interpreter::model_forward(const Item& decl, const Value& input, Span span) {
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
  fail(inner.span, "metodo de modelo '" + method + "' chega no M8");
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
  return 1.0F;  // gelu et al.: approximated as identity during training (M6.3)
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

  std::vector<int> y;
  const Value* yv = data.map->find("y");
  if (yv->kind == ValueKind::Lista && yv->list) {
    for (const Value& e : *yv->list) y.push_back(static_cast<int>(e.as_number()));
  }
  if (static_cast<std::int64_t>(y.size()) != n) fail(decl.span, "treino: |x| != |y|");
  int classes = 1;
  for (int v : y) classes = std::max(classes, v + 1);

  const std::string perda = field_word(cfg, "perda", "entropia_cruzada");
  const std::string otim = field_word(cfg, "otimizador", "sgd");
  double lr = field_num(cfg, "taxa", field_num(cfg, "taxa_aprendizado", 0.1));
  const int epocas = field_int(cfg, "epocas", 50);
  const Item* vf = find_field(cfg, "verboso");
  const bool verbose = vf && vf->value && vf->value->kind == ExprKind::BoolLit && vf->value->boolean;

  std::vector<Layer> layers = build_layers(*mit->second, f);
  if (layers.empty() || layers.back().kind != Layer::Softmax || perda != "entropia_cruzada") {
    fail(decl.span,
         "treino (M6.2): suportado apenas perda 'entropia_cruzada' com 'softmax' na ultima camada");
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
          case Layer::Dense: cur = rt::add(rt::matmul(cur, l.w), l.b); break;
          case Layer::Activation: cur = rt::apply_unary(cur, l.act); break;
          case Layer::Softmax: cur = rt::softmax_last(cur); break;
          case Layer::Dropout: break;
        }
      }
      const rt::Tensor& probs = cur;  // [N, C]

      // Cross-entropy loss and dL/d(logits) = probs - onehot(y), averaged.
      float loss = 0.0F;
      rt::Tensor grad = probs;  // becomes gradient w.r.t. softmax input
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
  } catch (const std::exception& e) {
    fail(decl.span, std::string("treino ") + name + ": " + e.what());
  }

  model_cache_[name] = std::move(layers);
}

// ------------------------------------------------------------------ statements

void Interpreter::exec_block(const ast::Block& block, Env& env) {
  for (const auto& it : block.items) {
    if (it) exec_item(*it, env);
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
        return;
      }
      for (const auto& ei : stmt.elifs) {
        if (ei.cond && eval(*ei.cond, env).truthy()) {
          Env inner;
          inner.parent = &env;
          exec_block(ei.body, inner);
          return;
        }
      }
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
      if (Value* v = env.lookup(expr.text)) return *v;
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

  if (op == "==") return Value::logico(equals(a, b));
  if (op == "!=") return Value::logico(!equals(a, b));
  if (op == "contem") {
    if (a.kind == ValueKind::Texto && b.kind == ValueKind::Texto) {
      return Value::logico(a.s.find(b.s) != std::string::npos);
    }
    if (a.kind == ValueKind::Lista && a.list) {
      for (const Value& el : *a.list) {
        if (equals(el, b)) return Value::logico(true);
      }
    }
    return Value::logico(false);
  }
  if (op == "|") return Value::texto(to_display(a) + " | " + to_display(b));

  if (op == "+" && (a.kind == ValueKind::Texto || b.kind == ValueKind::Texto)) {
    return Value::texto(to_display(a) + to_display(b));
  }

  const bool comparison = word_in(op, {"LT", "LE", "GT", "GE", "<", "<=", ">", ">="});
  if (comparison) {
    double x = a.as_number();
    double y = b.as_number();
    if (a.kind == ValueKind::Texto && b.kind == ValueKind::Texto) {
      int c = a.s.compare(b.s);
      x = c;
      y = 0;
    }
    if (op == "LT" || op == "<") return Value::logico(x < y);
    if (op == "LE" || op == "<=") return Value::logico(x <= y);
    if (op == "GT" || op == ">") return Value::logico(x > y);
    return Value::logico(x >= y);
  }

  double x = a.as_number();
  double y = b.as_number();
  double r = 0;
  if (op == "+") r = x + y;
  else if (op == "-") r = x - y;
  else if (op == "*") r = x * y;
  else if (op == "/") r = y == 0 ? 0 : x / y;
  else if (op == "%") r = y == 0 ? 0 : std::fmod(x, y);
  else fail(expr.span, "operador desconhecido '" + op + "'");

  const bool both_int = a.kind == ValueKind::Inteiro && b.kind == ValueKind::Inteiro;
  if (both_int && op != "/") return Value::inteiro(static_cast<std::int64_t>(r));
  return Value::decimal(r);
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

Value Interpreter::eval_call(const Expr& expr, Env& env) {
  const Expr& callee = *expr.lhs;

  if (callee.kind == ExprKind::Member) {
    Value receiver = eval(*callee.lhs, env);
    return eval_method(callee.text, std::move(receiver), expr, env);
  }

  if (callee.kind == ExprKind::Name) {
    const std::string& name = callee.text;
    if (auto it = functions_.find(name); it != functions_.end()) {
      return call_function(*it->second, eval_args(expr, env), expr.span);
    }
    return eval_builtin(name, expr, env);
  }

  fail(expr.span, "chamada invalida");
}

Value Interpreter::call_function(const Item& fn, std::vector<Value> args, Span span) {
  Env env;
  env.parent = &root_;
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
  if (name == "escrever_csv" || name == "escrever_parquet") {
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
    if (name == "escrever_parquet") {
      out_ << "[nota] parquet ainda nao implementado (M5); gravado como CSV em " << a[1].s << '\n';
    }
    return Value::nulo();
  }
  if (name == "responder") {
    rt::ValueMap kw = eval_kwargs(call, env);
    out_ << "resposta:";
    for (const auto& kv : kw.items) out_ << ' ' << kv.first << '=' << to_display(kv.second);
    out_ << '\n';
    return Value::nulo();
  }

  if (word_in(name, {"perguntar", "perguntar_em_fluxo", "incorporar"})) {
    const char* mode = std::getenv("TILT_LLM");
    if (mode && std::string_view(mode) == "mock") {
      return name == "incorporar" ? Value::lista({Value::decimal(0.0)})
                                  : Value::texto("[resposta simulada]");
    }
    fail(call.span, "runtime de LLM nao implementado (M8); use TILT_LLM=mock para simular",
         DiagCode::NotImplemented);
  }
  if (name == "ler") {
    auto a = args();
    if (!a.empty() && a[0].kind == ValueKind::Texto && entities_.count(a[0].s)) {
      return read_fonte(a[0].s, call.span);
    }
    fail(call.span, "ler: esperava uma 'fonte' declarada", DiagCode::ConnectorNotImplemented);
  }
  if (word_in(name, {"escrever"}) || name == "dividir_texto" || (name.rfind("ler_", 0) == 0) ||
      (name.rfind("escrever_", 0) == 0)) {
    fail(call.span, "'" + name + "': conector/formato nao implementado (M5.2)",
         DiagCode::ConnectorNotImplemented);
  }
  if (name == "modelo") return eval_modelo_call(call, env);
  if (word_in(name, {"agente", "ferramenta", "treinar", "incorporador"})) {
    fail(call.span, "execucao de '" + name + "' nao implementada (M8-M9)",
         DiagCode::NotImplemented);
  }

  fail(call.span, "funcao '" + name + "' nao definida");
}

// ------------------------------------------------------------------ methods

Value Interpreter::eval_method(const std::string& method, Value receiver, const Expr& call,
                               Env& env) {
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
      if (method == "matmul" || method == "mais") {
        auto a = eval_args(call, env);
        if (a.empty() || a[0].kind != ValueKind::Tensor) {
          fail(call.span, method + " espera outro tensor");
        }
        return Value::tensor_de(method == "matmul" ? rt::matmul(t, *a[0].tensor)
                                                   : rt::add(t, *a[0].tensor));
      }
      if (method == "transposta") return Value::tensor_de(rt::transpose2d(t));
      if (method == "reformar") {
        auto a = eval_args(call, env);
        std::vector<std::int64_t> shape;
        if (!a.empty() && a[0].kind == ValueKind::Lista && a[0].list) {
          for (const Value& e : *a[0].list) shape.push_back(static_cast<std::int64_t>(e.as_number()));
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
