#include "semantic/checker.hpp"

#include <initializer_list>
#include <memory>
#include <utility>

namespace tilt {

using ast::Expr;
using ast::ExprKind;
using ast::Item;
using ast::ItemKind;
using ast::Stmt;
using ast::StmtKind;
using sema::Type;
using sema::TypeKind;

namespace {

bool word_in(std::string_view w, std::initializer_list<std::string_view> set) {
  for (std::string_view s : set) {
    if (w == s) return true;
  }
  return false;
}

bool is_entity_keyword(std::string_view kw) {
  return word_in(kw, {"fonte", "pipeline", "verificar", "modelo", "treino", "tarefa", "experimento",
                      "llm", "indice", "fluxo", "ferramenta", "agente", "equipe", "servico"});
}

bool is_secret_key(std::string_view key) {
  return word_in(key, {"chave", "token", "senha", "segredo", "api_key", "chave_api", "password",
                       "secret"});
}

bool is_known_dtype(std::string_view d) {
  return word_in(d, {"f32", "f16", "bf16", "f64", "i8", "i16", "i32", "i64", "u8", "bool"});
}

std::string decl_name(const Item& it) {
  if (!it.header.empty() && it.header[0] && it.header[0]->kind == ExprKind::Name) {
    return it.header[0]->text;
  }
  return {};
}

// Pulls the string members out of a `"a" | "b" | "c"` chain.
void collect_union_literals(const Expr& e, std::vector<std::string>& out) {
  if (e.kind == ExprKind::TextLit) {
    out.push_back(e.text);
  } else if (e.kind == ExprKind::Binary && e.text == "|") {
    if (e.lhs) collect_union_literals(*e.lhs, out);
    if (e.rhs) collect_union_literals(*e.rhs, out);
  }
}

}  // namespace

SemanticChecker::SemanticChecker(const ast::Program& program, DiagnosticEngine& diag)
    : program_(program), diag_(diag) {}

void SemanticChecker::run() {
  collect();
  resolve_types();
  audit_blocks();
  check_bodies();
  check_model_shapes();
}

void SemanticChecker::report(DiagCode code, Span span, std::string message,
                             std::vector<std::string> notes) {
  Diagnostic d;
  d.severity = Severity::Error;
  d.code = code;
  d.span = span;
  d.message = std::move(message);
  d.notes = std::move(notes);
  diag_.report(std::move(d));
}

void SemanticChecker::define(const std::string& name, std::string kind, Type type, Span span) {
  if (name.empty()) return;
  auto it = globals_.find(name);
  if (it != globals_.end()) {
    // `treino X` intentionally shares its name with `modelo X`.
    const bool treino_pair = (kind == "treino" && it->second.kind == "modelo") ||
                             (kind == "modelo" && it->second.kind == "treino");
    if (!treino_pair) {
      report(DiagCode::DuplicateDeclaration, span, "'" + name + "' ja foi declarado",
             {"declaracao anterior na linha " + std::to_string(it->second.span.line)});
    }
    return;
  }
  globals_.emplace(name, Symbol{name, std::move(kind), std::move(type), span});
}

const SemanticChecker::Symbol* SemanticChecker::lookup(std::string_view name) const {
  auto it = globals_.find(std::string(name));
  return it == globals_.end() ? nullptr : &it->second;
}

// ------------------------------------------------------------------- pass 1

void SemanticChecker::collect() {
  for (const auto& item : program_.items) {
    if (!item || item->kind != ItemKind::Decl) continue;
    const std::string kw = item->key;
    const std::string name = decl_name(*item);

    if (kw == "importar" || kw == "de") {
      for (const auto& h : item->header) {
        if (h && h->kind == ExprKind::Name && h->text != "importar") {
          define(h->text, "modulo", Type::scalar(TypeKind::Unknown), item->span);
        }
      }
      continue;
    }
    if (kw == "seja" || kw == "constante") {
      define(name, "var", Type::scalar(TypeKind::Unknown), item->span);
      continue;
    }
    if (kw == "funcao") {
      Type ft;
      ft.kind = TypeKind::Funcao;
      define(name, "funcao", std::move(ft), item->span);
      continue;
    }
    if (kw == "tipo") {
      Type rt;
      rt.kind = TypeKind::Registro;
      rt.name = name;
      define(name, "tipo", std::move(rt), item->span);
      continue;
    }
    Type et;
    et.kind = TypeKind::Entidade;
    et.name = name;
    et.entity_kind = kw;
    define(name, kw, std::move(et), item->span);
  }
}

// ------------------------------------------------------------------- pass 2

Type SemanticChecker::resolve_type_expr(const Expr& e) {
  switch (e.kind) {
    case ExprKind::Name: {
      const std::string& w = e.text;
      if (w == "texto") return Type::scalar(TypeKind::Texto);
      if (w == "inteiro") return Type::scalar(TypeKind::Inteiro);
      if (w == "decimal") return Type::scalar(TypeKind::Decimal);
      if (w == "logico") return Type::scalar(TypeKind::Logico);
      if (w == "nulo") return Type::scalar(TypeKind::Nulo);
      if (w == "tabela") return Type::scalar(TypeKind::Tabela);
      if (w == "_") return Type::scalar(TypeKind::Unknown);
      if (const Symbol* s = lookup(w)) {
        if (s->kind == "tipo" || s->kind == "modulo") return s->type;
        return s->type;  // entity used as a type annotation — tolerated
      }
      report(DiagCode::UnknownType, e.span, "tipo desconhecido '" + w + "'",
             {"tipos base: texto, inteiro, decimal, logico, tabela, tensor[...], lista[...]"});
      return Type::scalar(TypeKind::Unknown);
    }
    case ExprKind::TextLit: {
      Type t;
      t.kind = TypeKind::UniaoLiteral;
      t.literals = {e.text};
      return t;
    }
    case ExprKind::Binary: {
      if (e.text == "|") {
        Type t;
        t.kind = TypeKind::UniaoLiteral;
        collect_union_literals(e, t.literals);
        return t;
      }
      break;
    }
    case ExprKind::Device:
      return e.lhs ? resolve_type_expr(*e.lhs) : Type::scalar(TypeKind::Unknown);
    case ExprKind::Index: {
      const std::string base = (e.lhs && e.lhs->kind == ExprKind::Name) ? e.lhs->text : "";
      if (base == "tensor") {
        Type t;
        t.kind = TypeKind::Tensor;
        std::size_t start = 0;
        if (!e.elems.empty() && e.elems[0] && e.elems[0]->kind == ExprKind::Name) {
          t.name = e.elems[0]->text;
          if (!is_known_dtype(t.name)) {
            report(DiagCode::InvalidTensorType, e.elems[0]->span,
                   "dtype de tensor desconhecido '" + t.name + "'",
                   {"use f32, f16, bf16, f64, i8, i32, i64 ou u8"});
          }
          start = 1;
        }
        for (std::size_t i = start; i < e.elems.size(); ++i) {
          const Expr* d = e.elems[i].get();
          if (d && d->kind == ExprKind::IntLit) {
            t.dims.push_back(std::stoll(d->text));
          } else if (d && d->kind == ExprKind::Name && d->text == "_") {
            t.dims.push_back(-1);
          } else {
            report(DiagCode::InvalidTensorType, d ? d->span : e.span,
                   "dimensao de tensor deve ser um inteiro ou '_'");
          }
        }
        return t;
      }
      if (base == "lista" || base == "opcional" || base == "fluxo") {
        Type t;
        t.kind = base == "lista"      ? TypeKind::Lista
                 : base == "opcional" ? TypeKind::Opcional
                                      : TypeKind::Fluxo;
        if (!e.elems.empty() && e.elems[0]) {
          t.elem = std::make_shared<Type>(resolve_type_expr(*e.elems[0]));
        }
        return t;
      }
      if (base == "mapa") {
        Type t;
        t.kind = TypeKind::Mapa;
        if (e.elems.size() > 0 && e.elems[0]) t.key = std::make_shared<Type>(resolve_type_expr(*e.elems[0]));
        if (e.elems.size() > 1 && e.elems[1]) t.elem = std::make_shared<Type>(resolve_type_expr(*e.elems[1]));
        return t;
      }
      report(DiagCode::UnknownType, e.lhs ? e.lhs->span : e.span,
             "construtor de tipo desconhecido '" + base + "'");
      return Type::scalar(TypeKind::Unknown);
    }
    default:
      break;
  }
  report(DiagCode::UnknownType, e.span, "expressao de tipo invalida");
  return Type::scalar(TypeKind::Unknown);
}

void SemanticChecker::resolve_type_annotations(const Item& decl) {
  const std::string kw = decl.key;

  if (kw == "tipo" && decl.block) {
    Symbol* sym = nullptr;
    if (auto it = globals_.find(decl_name(decl)); it != globals_.end()) sym = &it->second;
    for (const auto& f : decl.block->items) {
      if (!f || f->kind != ItemKind::Field || !f->value) continue;
      Type ft = resolve_type_expr(*f->value);
      if (sym) sym->type.fields.emplace_back(f->key, std::make_shared<Type>(std::move(ft)));
    }
    return;
  }

  if (kw == "funcao") {
    for (const auto& p : decl.params) {
      if (p.value) resolve_type_expr(*p.value);
    }
    if (decl.value) resolve_type_expr(*decl.value);
    return;
  }

  // Any `entrada:` / `saida:` block or inline reference elsewhere.
  std::vector<const ast::Block*> stack;
  if (decl.block) stack.push_back(decl.block.get());
  while (!stack.empty()) {
    const ast::Block* b = stack.back();
    stack.pop_back();
    for (const auto& it : b->items) {
      if (!it) continue;
      const Item* target = it.get();
      if (target->kind == ItemKind::ListEntry && target->child) target = target->child.get();
      if (target->kind != ItemKind::Field) {
        if (target->block) stack.push_back(target->block.get());
        continue;
      }
      if ((target->key == "entrada" || target->key == "saida")) {
        if (target->value) {
          resolve_type_expr(*target->value);
        } else if (target->block) {
          for (const auto& sub : target->block->items) {
            if (sub && sub->kind == ItemKind::Field && sub->value) resolve_type_expr(*sub->value);
          }
        }
      }
      if (target->block) stack.push_back(target->block.get());
    }
  }
}

void SemanticChecker::resolve_types() {
  for (const auto& item : program_.items) {
    if (item && item->kind == ItemKind::Decl) resolve_type_annotations(*item);
  }
}

// ------------------------------------------------------------------- pass 3

void SemanticChecker::check_device(const Expr& value) {
  std::string name;
  if (value.kind == ExprKind::TextLit || value.kind == ExprKind::Name) name = value.text;
  if (name.empty()) return;

  if (word_in(name, {"auto", "cpu", "gpu", "cuda", "metal"})) return;
  if (name.rfind("cuda:", 0) == 0) {
    bool digits = name.size() > 5;
    for (std::size_t i = 5; i < name.size(); ++i) {
      if (name[i] < '0' || name[i] > '9') digits = false;
    }
    if (digits) return;
  }
  report(DiagCode::InvalidDevice, value.span, "dispositivo invalido '" + name + "'",
         {"use auto, cpu, gpu, metal ou \"cuda:N\""});
}

void SemanticChecker::check_tool_list(const Item& field) {
  auto check_name = [&](const std::string& n, Span span) {
    if (n.empty()) return;
    const Symbol* s = lookup(n);
    if (!s || s->kind != "ferramenta") {
      report(DiagCode::UnknownReference, span, "ferramenta '" + n + "' nao declarada");
    }
  };

  if (field.value && field.value->kind == ExprKind::ListLit) {
    for (const auto& el : field.value->elems) {
      if (el && el->kind == ExprKind::Name) check_name(el->text, el->span);
    }
  }
  if (field.value && field.value->kind == ExprKind::Name) {
    check_name(field.value->text, field.value->span);
  }
  if (!field.block) return;
  for (const auto& it : field.block->items) {
    if (!it) continue;
    const Item* c = (it->kind == ItemKind::ListEntry && it->child) ? it->child.get() : it.get();
    if (c->kind == ItemKind::Stmt && c->stmt && c->stmt->a && c->stmt->a->kind == ExprKind::Name) {
      check_name(c->stmt->a->text, c->stmt->a->span);
    } else if (c->kind == ItemKind::Field) {
      check_name(c->key, c->span);
    }
  }
}

void SemanticChecker::visit_item(const Item& item, std::string_view entity_kw) {
  switch (item.kind) {
    case ItemKind::Field: {
      if (item.key == "dispositivo" && item.value) check_device(*item.value);
      if (is_secret_key(item.key) && item.value && item.value->kind == ExprKind::TextLit) {
        report(DiagCode::SecretMustUseEnv, item.value->span,
               "segredo em '" + item.key + "' nao deve ser um literal",
               {"use " + item.key + ": env \"NOME_DA_VARIAVEL\""});
      }
      if (entity_kw == "agente" && item.key == "ferramentas") check_tool_list(item);
      if (item.block) walk_block(*item.block, entity_kw);
      break;
    }
    case ItemKind::ListEntry:
      if (item.block) walk_block(*item.block, entity_kw);
      if (item.child) visit_item(*item.child, entity_kw);
      break;
    case ItemKind::Decl:
    case ItemKind::Stmt:
      break;
  }
}

void SemanticChecker::walk_block(const ast::Block& block, std::string_view entity_kw) {
  for (const auto& it : block.items) {
    if (it) visit_item(*it, entity_kw);
  }
}

void SemanticChecker::audit_blocks() {
  for (const auto& item : program_.items) {
    if (!item || item->kind != ItemKind::Decl || !item->block) continue;
    if (is_entity_keyword(item->key)) walk_block(*item->block, item->key);
  }
}

// ------------------------------------------------------------------- pass 4

namespace {

bool is_builtin_name(std::string_view w) {
  return word_in(w, {"imprimir",   "imprima",     "print",       "registrar",  "log",
                     "env",        "tamanho",     "contar",      "somar",      "media",
                     "min",        "max",         "intervalo",   "ate",        "dividir",
                     "dividir_texto", "ler_csv",  "ler_json",    "escrever_csv", "escrever_json",
                     "escrever_parquet", "ler",   "carregador",  "perguntar",  "perguntar_em_fluxo",
                     "incorporar", "responder",   "responder_em_fluxo", "tensor", "zeros",
                     "uns",        "aleatorio",   "checar_tilt", "modelo",     "agente",
                     "ferramenta", "repetir",     "parquet",     "csv",        "json",
                     "postgres",   "kafka",       "s3",          "verdadeiro", "falso",
                     "nulo",       "abortar",     "avisar",      "um_de_n",    "padronizar"});
}

// Implicit bindings introduced by the runtime (row predicates, callbacks, ...).
bool is_magic_name(std::string_view w) {
  return word_in(w, {"linha", "linhas", "entrada", "epoca", "epocas", "metricas", "passo",
                     "resultado"});
}

bool is_lazy_row_method(std::string_view m) {
  return word_in(m, {"filtrar", "derivar", "mapear", "agrupar_por"});
}

// Visits each `camadas:` spec as (key, inline-value) — handles `- densa: N`
// list entries and the folded `- densa: N` / `ativacao: relu` mapping blocks.
template <typename F>
void for_each_layer(const ast::Block& block, const F& emit) {
  for (const auto& raw : block.items) {
    if (!raw) continue;
    if (raw->kind == ItemKind::ListEntry) {
      if (raw->block) {
        for_each_layer(*raw->block, emit);
        continue;
      }
      const Item* c = raw->child.get();
      if (c && c->kind == ItemKind::Field) emit(c->key, c->value.get());
      else if (c && c->kind == ItemKind::Stmt && c->stmt && c->stmt->a &&
               c->stmt->a->kind == ExprKind::Name)
        emit(c->stmt->a->text, static_cast<const ast::Expr*>(nullptr));
    } else if (raw->kind == ItemKind::Field) {
      emit(raw->key, raw->value.get());
    }
  }
}

const Item* find_field(const ast::Block& block, std::string_view key) {
  for (const auto& it : block.items) {
    if (it && it->kind == ItemKind::Field && it->key == key) return it.get();
  }
  return nullptr;
}

// ---------------------------------------------------------------- shape solver
//
// Subconjunto deliberadamente pequeno: so propaga formas 100% conhecidas
// (dimensoes literais ou anotadas). Qualquer dimensao duvidosa torna a forma
// desconhecida — falso negativo aceito em vez de falso positivo.

using TensorShape = std::vector<std::int64_t>;  // mesmo tipo de SemanticChecker::TensorShape

std::optional<TensorShape> nested_list_dims(const std::vector<ast::ExprPtr>& elems) {
  if (elems.empty() || !elems.front()) return std::nullopt;
  if (elems.front()->kind == ExprKind::ListLit) {
    auto inner = nested_list_dims(elems.front()->elems);
    if (!inner) return std::nullopt;
    for (const auto& el : elems) {
      if (!el || el->kind != ExprKind::ListLit) return std::nullopt;
      auto d = nested_list_dims(el->elems);
      if (!d || *d != *inner) return std::nullopt;  // lista aninhada irregular
    }
    TensorShape out;
    out.push_back(static_cast<std::int64_t>(elems.size()));
    out.insert(out.end(), inner->begin(), inner->end());
    return out;
  }
  for (const auto& el : elems) {
    if (!el || (el->kind != ExprKind::IntLit && el->kind != ExprKind::DecimalLit)) {
      return std::nullopt;
    }
  }
  return TensorShape{static_cast<std::int64_t>(elems.size())};
}

// `tensor [1, 2]` / `tensor [[1, 2], [3, 4]]` (Index com lhs Name "tensor").
std::optional<TensorShape> tensor_literal_dims(const Expr& e) {
  if (e.kind != ExprKind::Index || !e.lhs || e.lhs->kind != ExprKind::Name ||
      e.lhs->text != "tensor") {
    return std::nullopt;
  }
  return nested_list_dims(e.elems);
}

// Dimensoes inteiras de uma anotacao `tensor[f32, 1, 3, 32, 32]` (dtype
// opcional). `std::nullopt` se a anotacao nao for essa forma ou tiver
// dimensoes nao literais ('_' incluido).
std::optional<TensorShape> tensor_annotation_dims(const Expr* e) {
  if (!e || e->kind != ExprKind::Index || !e->lhs || e->lhs->kind != ExprKind::Name ||
      e->lhs->text != "tensor" || e->elems.empty()) {
    return std::nullopt;
  }
  std::size_t start = 0;
  if (e->elems[0] && e->elems[0]->kind == ExprKind::Name) start = 1;  // dtype
  TensorShape dims;
  for (std::size_t i = start; i < e->elems.size(); ++i) {
    const Expr* d = e->elems[i].get();
    if (!d || d->kind != ExprKind::IntLit) return std::nullopt;
    dims.push_back(std::stoll(d->text));
  }
  return dims.empty() ? std::nullopt : std::optional<TensorShape>(dims);
}

std::int64_t num_elements(const TensorShape& s) {
  std::int64_t n = 1;
  for (const std::int64_t d : s) {
    if (d <= 0) return -1;
    n *= d;
  }
  return n;
}

std::string shape_str(const TensorShape& s) {
  std::string out = "[";
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (i) out += ", ";
    out += std::to_string(s[i]);
  }
  return out + "]";
}

bool is_scalar_lit(const Expr& e) { return e.kind == ExprKind::IntLit || e.kind == ExprKind::DecimalLit; }

const Expr* first_positional_arg(const Expr& call) {
  for (const auto& a : call.args) {
    if (a.name.empty() && a.value) return a.value.get();
  }
  return nullptr;
}

void collect_entrada_names(const ast::Block& block, std::unordered_set<std::string>& scope) {
  for (const auto& it : block.items) {
    const Item* f = it.get();
    if (f && f->kind == ItemKind::ListEntry && f->child) f = f->child.get();
    if (!f || f->kind != ItemKind::Field) continue;
    if (f->key != "entrada" && f->key != "saida") continue;
    scope.insert("entrada");
    if (f->block) {
      for (const auto& sub : f->block->items) {
        if (sub && sub->kind == ItemKind::Field) scope.insert(sub->key);
      }
    }
  }
}

// Atribuicoes feitas em blocos `meio:` do `servico` sao visiveis nos
// `passos:` das rotas (o meio roda no mesmo Env da rota); coleta os nomes
// para o escopo do checker nao apontar nomes definidos no meio.
void collect_assigned_names(const ast::Block& block, std::unordered_set<std::string>& scope) {
  for (const auto& raw : block.items) {
    const Item* it = raw.get();
    if (!it) continue;
    if (it->kind == ItemKind::ListEntry) {
      if (it->block) collect_assigned_names(*it->block, scope);
      it = it->child.get();
      if (!it) continue;
    }
    if (it->kind == ItemKind::Stmt && it->stmt) {
      const Stmt& s = *it->stmt;
      if (s.kind == StmtKind::Assign && s.a && s.a->kind == ExprKind::Name) {
        scope.insert(s.a->text);
      } else if (s.kind == StmtKind::If) {
        collect_assigned_names(s.body, scope);
        for (const auto& ei : s.elifs) collect_assigned_names(ei.body, scope);
        if (s.else_body) collect_assigned_names(*s.else_body, scope);
      } else if (s.kind == StmtKind::While || s.kind == StmtKind::ForEach) {
        collect_assigned_names(s.body, scope);
      }
    } else if (it->kind == ItemKind::Field && it->block) {
      collect_assigned_names(*it->block, scope);
    }
  }
}

}  // namespace

std::optional<SemanticChecker::TensorShape> SemanticChecker::check_reshape(
    Span span, std::optional<TensorShape> in, const TensorShape& to) {
  if (in) {
    const std::int64_t a = num_elements(*in), b = num_elements(to);
    if (a >= 0 && b >= 0 && a != b) {
      report(DiagCode::TensorShapeMismatch, span,
             "reformar: numero de elementos difere (entrada tem " + std::to_string(a) +
                 ", destino tem " + std::to_string(b) + ")",
             {"ajuste as dimensoes para totalizar " + std::to_string(a) + " elementos"});
    }
    return to;
  }
  return std::nullopt;
}

std::optional<SemanticChecker::TensorShape> SemanticChecker::check_conv2d(const Expr& call,
                                                                          const ShapeEnv& shapes) {
  auto in = call.lhs && call.lhs->lhs ? infer_shape(*call.lhs->lhs, shapes) : std::nullopt;
  const Expr* nucleo = first_positional_arg(call);
  auto k = nucleo ? infer_shape(*nucleo, shapes) : std::nullopt;
  std::int64_t passo = 1;
  for (const auto& a : call.args) {
    if (a.name == "passo" && a.value && a.value->kind == ExprKind::IntLit) {
      passo = std::stoll(a.value->text);
    }
  }
  if (!in) return std::nullopt;
  const Span in_span = call.lhs->lhs->span;
  if (in->size() != 4) {
    report(DiagCode::TensorShapeMismatch, in_span,
           "conv2d espera uma entrada [N, C_in, H, W], mas a entrada tem forma " + shape_str(*in),
           {"use um tensor 4D, ex.: uns [1, 3, 32, 32]"});
    return std::nullopt;
  }
  if (passo < 1) {
    report(DiagCode::TensorShapeMismatch, call.span, "conv2d: passo deve ser >= 1",
           {"ajuste `passo:` para um inteiro >= 1"});
    return std::nullopt;
  }
  if (!k) return std::nullopt;
  const Span k_span = nucleo->span;
  if (k->size() != 4) {
    report(DiagCode::TensorShapeMismatch, k_span,
           "conv2d espera um nucleo [C_out, C_in, KH, KW], mas o nucleo tem forma " + shape_str(*k),
           {"use um nucleo 4D, ex.: uns [8, 3, 3, 3]"});
    return std::nullopt;
  }
  const std::int64_t cin = (*in)[1], h = (*in)[2], w = (*in)[3];
  if ((*k)[1] != cin) {
    report(DiagCode::TensorShapeMismatch, k_span,
           "conv2d: nucleo tem " + std::to_string((*k)[1]) +
               " canais de entrada, mas a entrada tem " + std::to_string(cin),
           {"ajuste o eixo C_in do nucleo para " + std::to_string(cin) + " (ex.: uns [" +
                std::to_string((*k)[0]) + ", " + std::to_string(cin) + ", " +
                std::to_string((*k)[2]) + ", " + std::to_string((*k)[3]) + "])"});
    return std::nullopt;
  }
  const std::int64_t kh = (*k)[2], kw = (*k)[3];
  if (kh > h || kw > w) {
    report(DiagCode::TensorShapeMismatch, k_span,
           "conv2d: nucleo " + std::to_string(kh) + "x" + std::to_string(kw) +
               " maior que a entrada " + std::to_string(h) + "x" + std::to_string(w),
           {"reduza o nucleo ou aumente a entrada (padding ainda nao suportado)"});
    return std::nullopt;
  }
  return TensorShape{(*in)[0], (*k)[0], (h - kh) / passo + 1, (w - kw) / passo + 1};
}

std::optional<SemanticChecker::TensorShape> SemanticChecker::infer_shape(const Expr& e,
                                                                         const ShapeEnv& shapes) {
  switch (e.kind) {
    case ExprKind::Name: {
      auto it = shapes.find(e.text);
      if (it != shapes.end()) return it->second;
      return std::nullopt;
    }
    case ExprKind::Index: {
      const std::string base = (e.lhs && e.lhs->kind == ExprKind::Name) ? e.lhs->text : "";
      if (base == "tensor") return tensor_literal_dims(e);
      if (word_in(base, {"uns", "zeros", "aleatorio"})) {
        TensorShape s;
        for (const auto& el : e.elems) {
          if (!el || el->kind != ExprKind::IntLit) return std::nullopt;
          s.push_back(std::stoll(el->text));
        }
        return s;
      }
      // (`x.reformar [d, ...]` em forma de indice nao despacha no runtime —
      // so a forma de chamada `x.reformar([d, ...])` entra no solver.)
      return std::nullopt;
    }
    case ExprKind::Member: {
      // Propriedades elementwise (preservam a forma) e transposta 2D.
      if (!e.lhs) return std::nullopt;
      if (word_in(e.text, {"softmax", "relu", "gelu", "silu", "sigmoide", "tanh", "norma_camada"})) {
        return infer_shape(*e.lhs, shapes);
      }
      if (e.text == "transposta") {
        auto in = infer_shape(*e.lhs, shapes);
        if (in && in->size() != 2) {
          report(DiagCode::TensorShapeMismatch, e.span,
                 "transposta espera um tensor 2D, mas a entrada tem forma " + shape_str(*in),
                 {"use .reformar([d, d]) para ajustar a forma antes, se necessario"});
          return std::nullopt;
        }
        if (in) return TensorShape{(*in)[1], (*in)[0]};
      }
      return std::nullopt;
    }
    case ExprKind::Call: {
      if (!e.lhs || e.lhs->kind != ExprKind::Member || !e.lhs->lhs) return std::nullopt;
      const std::string& m = e.lhs->text;
      if (m == "conv2d") return check_conv2d(e, shapes);
      if (m == "norma_lote") {
        auto in = infer_shape(*e.lhs->lhs, shapes);
        if (in && in->size() < 2) {
          report(DiagCode::TensorShapeMismatch, e.span,
                 "norma_lote espera um tensor [N, C, ...], mas a entrada tem forma " +
                     shape_str(*in),
                 {"adicione os eixos de lote/canal (ex.: reforme para [1, C, ...] com .reformar([1, C, ...]))"});
        }
        return in;  // forma preservada
      }
      if (m == "matmul") {
        auto a = infer_shape(*e.lhs->lhs, shapes);
        const Expr* arg = first_positional_arg(e);
        auto b = arg ? infer_shape(*arg, shapes) : std::nullopt;
        if (a && b && a->size() == 2 && b->size() == 2) {
          if ((*a)[1] != (*b)[0]) {
            report(DiagCode::TensorShapeMismatch, e.span,
                   "matmul: dimensao interna incompativel (" + shape_str(*a) + " x " +
                       shape_str(*b) + ")",
                   {"ajuste para que o ultimo eixo de um coincida com o primeiro do outro"});
            return std::nullopt;
          }
          return TensorShape{(*a)[0], (*b)[1]};
        }
        return std::nullopt;
      }
      if (m == "reformar") {
        // `x.reformar([d, ...])` (forma de chamada).
        const Expr* arg = first_positional_arg(e);
        if (!arg || arg->kind != ExprKind::ListLit) return std::nullopt;
        auto in = infer_shape(*e.lhs->lhs, shapes);
        TensorShape to;
        for (const auto& el : arg->elems) {
          if (!el || el->kind != ExprKind::IntLit) return std::nullopt;
          to.push_back(std::stoll(el->text));
        }
        return check_reshape(e.span, std::move(in), to);
      }
      return std::nullopt;
    }
    case ExprKind::Binary: {
      // Operacoes elementwise: forma preservada entre tensor e escalar, ou
      // entre tensores de mesma forma. Broadcast parcial fica fora do
      // subconjunto (forma desconhecida).
      if (!word_in(e.text, {"+", "-", "*", "/"})) return std::nullopt;
      auto a = e.lhs ? infer_shape(*e.lhs, shapes) : std::nullopt;
      auto b = e.rhs ? infer_shape(*e.rhs, shapes) : std::nullopt;
      if (a && b) return *a == *b ? a : std::nullopt;
      if (a && e.rhs && is_scalar_lit(*e.rhs)) return a;
      if (b && e.lhs && is_scalar_lit(*e.lhs)) return b;
      return std::nullopt;
    }
    default:
      return std::nullopt;
  }
}

void SemanticChecker::check_expr(const Expr& e, const Scope& scope) {
  switch (e.kind) {
    case ExprKind::Name: {
      const std::string& w = e.text;
      if (scope.count(w) || globals_.count(w) || is_builtin_name(w) || is_magic_name(w)) return;
      report(DiagCode::UndefinedName, e.span, "nome '" + w + "' nao definido");
      return;
    }
    case ExprKind::Member:
      if (e.lhs) check_expr(*e.lhs, scope);
      return;
    case ExprKind::Index:
      if (e.lhs) check_expr(*e.lhs, scope);
      for (const auto& el : e.elems) {
        if (el) check_expr(*el, scope);
      }
      return;
    case ExprKind::Slice:
      if (e.lhs) check_expr(*e.lhs, scope);
      if (e.rhs) check_expr(*e.rhs, scope);
      if (e.extra) check_expr(*e.extra, scope);
      return;
    case ExprKind::Unary:
      if (e.rhs) check_expr(*e.rhs, scope);
      return;
    case ExprKind::Binary:
      if (e.lhs) check_expr(*e.lhs, scope);
      if (e.rhs) check_expr(*e.rhs, scope);
      return;
    case ExprKind::ListLit:
      for (const auto& el : e.elems) {
        if (el) check_expr(*el, scope);
      }
      return;
    case ExprKind::MapLit:
      for (const auto& en : e.entries) {
        if (en.value) check_expr(*en.value, scope);
      }
      return;
    case ExprKind::Device:
      if (e.lhs) check_expr(*e.lhs, scope);
      return;
    case ExprKind::Assign:
      if (e.rhs) check_expr(*e.rhs, scope);
      return;
    case ExprKind::Call: {
      const bool lazy = e.lhs && e.lhs->kind == ExprKind::Member && is_lazy_row_method(e.lhs->text);
      if (e.lhs && e.lhs->kind == ExprKind::Member) {
        check_expr(*e.lhs->lhs, scope);  // the receiver
      }
      // callee that is a bare Name is assumed to be a stdlib function; not flagged.
      for (std::size_t i = 0; i < e.args.size(); ++i) {
        const Expr* v = e.args[i].value.get();
        if (!v) continue;
        if (lazy && i == 0) {
          Scope with_row = scope;
          with_row.insert("linha");
          check_expr(*v, with_row);
        } else {
          check_expr(*v, scope);
        }
      }
      if (e.block) {
        for (const auto& it : e.block->items) {
          if (it && it->kind == ItemKind::Field && it->value) check_expr(*it->value, scope);
        }
      }
      return;
    }
    default:
      return;
  }
}

void SemanticChecker::walk_stmt(const Stmt& s, Scope& scope, ShapeEnv& shapes) {
  switch (s.kind) {
    case ast::StmtKind::Assign:
      if (s.b) {
        check_expr(*s.b, scope);
        // Registra (ou invalida) a forma conhecida do nome atribuido.
        if (s.a && s.a->kind == ExprKind::Name) {
          if (auto sh = infer_shape(*s.b, shapes)) {
            shapes[s.a->text] = *sh;
          } else {
            shapes.erase(s.a->text);
          }
        }
      }
      if (s.a && s.a->kind == ExprKind::Name) {
        scope.insert(s.a->text);
      } else if (s.a) {
        check_expr(*s.a, scope);
      }
      return;
    case ast::StmtKind::Expr:
      if (s.a) {
        check_expr(*s.a, scope);
        infer_shape(*s.a, shapes);  // valida dimensoes mesmo sem atribuicao
      }
      return;
    case ast::StmtKind::Return:
      if (s.a) {
        check_expr(*s.a, scope);
        infer_shape(*s.a, shapes);
      }
      return;
    case ast::StmtKind::If: {
      if (s.a) check_expr(*s.a, scope);
      walk_stmt_block(s.body, scope, shapes);
      for (const auto& ei : s.elifs) {
        if (ei.cond) check_expr(*ei.cond, scope);
        walk_stmt_block(ei.body, scope, shapes);
      }
      if (s.else_body) walk_stmt_block(*s.else_body, scope, shapes);
      return;
    }
    case ast::StmtKind::ForEach: {
      if (s.a) check_expr(*s.a, scope);
      Scope inner = scope;
      if (!s.name.empty()) inner.insert(s.name);
      walk_stmt_block(s.body, std::move(inner), shapes);
      return;
    }
    case ast::StmtKind::While:
      if (s.a) check_expr(*s.a, scope);
      walk_stmt_block(s.body, scope, shapes);
      return;
    case ast::StmtKind::Try: {
      walk_stmt_block(s.body, scope, shapes);
      if (s.catch_body) {
        Scope inner = scope;
        if (!s.name.empty()) inner.insert(s.name);
        walk_stmt_block(*s.catch_body, std::move(inner), shapes);
      }
      return;
    }
  }
}

void SemanticChecker::walk_stmt_block(const ast::Block& block, Scope scope, ShapeEnv shapes) {
  for (const auto& raw : block.items) {
    if (!raw) continue;
    const Item* it = raw.get();
    if (it->kind == ItemKind::ListEntry) {
      if (it->block) {
        walk_stmt_block(*it->block, scope, shapes);
        continue;
      }
      it = it->child.get();
      if (!it) continue;
    }
    if (it->kind == ItemKind::Stmt && it->stmt) {
      walk_stmt(*it->stmt, scope, shapes);
    } else if (it->kind == ItemKind::Field) {
      if (it->key == "verificar") continue;  // rule keys are column names, not vars
      if (it->value) check_expr(*it->value, scope);
      if (it->block) walk_stmt_block(*it->block, scope, shapes);
    }
  }
}

void SemanticChecker::scan_for_bodies(const ast::Block& block, Scope scope, ShapeEnv shapes) {
  collect_entrada_names(block, scope);
  // Anotacoes `entrada: tensor[...]` (inline ou em bloco) semeiam as formas
  // conhecidas dos dados de entrada da entidade.
  if (const Item* ent = find_field(block, "entrada")) {
    if (auto dims = tensor_annotation_dims(ent->value.get())) {
      shapes["entrada"] = *dims;
    } else if (ent->block) {
      for (const auto& sub : ent->block->items) {
        if (sub && sub->kind == ItemKind::Field) {
          if (auto d = tensor_annotation_dims(sub->value.get())) shapes[sub->key] = *d;
        }
      }
    }
  }
  // Atribuicoes feitas em blocos `meio:` do `servico` valem para todas as
  // rotas (o meio executa no mesmo Env da rota antes dos `passos:`).
  for (const auto& raw : block.items) {
    const Item* it = raw.get();
    if (it && it->kind == ItemKind::ListEntry && it->child) it = it->child.get();
    if (it && it->kind == ItemKind::Field && it->key == "meio" && it->block) {
      collect_assigned_names(*it->block, scope);
    }
  }
  for (const auto& raw : block.items) {
    if (!raw) continue;
    const Item* it = raw.get();
    if (it->kind == ItemKind::ListEntry && it->child) it = it->child.get();
    if (!it || it->kind != ItemKind::Field || !it->block) continue;
    if (it->key == "passos" || it->key == "executar") {
      walk_stmt_block(*it->block, scope, shapes);
    } else if (it->key != "verificar" && it->key != "camadas") {
      scan_for_bodies(*it->block, scope, shapes);
    }
  }
}

void SemanticChecker::check_bodies() {
  for (const auto& item : program_.items) {
    if (!item || item->kind != ItemKind::Decl || !item->block) continue;
    if (item->key == "funcao") {
      Scope scope;
      ShapeEnv shapes;
      for (const auto& p : item->params) {
        scope.insert(p.name);
        // Anotacao `p: tensor[...]` semeia a forma conhecida do parametro.
        if (auto dims = tensor_annotation_dims(p.value.get())) shapes[p.name] = *dims;
      }
      walk_stmt_block(*item->block, std::move(scope), std::move(shapes));
    } else if (is_entity_keyword(item->key)) {
      scan_for_bodies(*item->block, {}, {});
    }
  }
}

// ------------------------------------------------------------------- pass 5

void SemanticChecker::check_model_shapes() {
  for (const auto& item : program_.items) {
    if (!item || item->kind != ItemKind::Decl || item->key != "modelo" || !item->block) continue;

    std::int64_t cur = -1;  // running feature dimension; -1 = unknown
    if (const Item* ent = find_field(*item->block, "entrada")) {
      // A cadeia densa/linear opera sobre a ultima dimensao (espelha o
      // runtime, que usa shape.back() como dimensao de entrada).
      if (auto dims = tensor_annotation_dims(ent->value.get())) cur = dims->back();
    }

    const Item* camadas = find_field(*item->block, "camadas");
    if (!camadas || !camadas->block) continue;

    for_each_layer(*camadas->block, [&](const std::string& key, const ast::Expr* value) {
      if (key == "densa" && value && value->kind == ExprKind::IntLit) {
        cur = std::stoll(value->text);
      } else if (key == "linear" && value && value->kind == ExprKind::ListLit &&
                 value->elems.size() == 2 && value->elems[0]->kind == ExprKind::IntLit &&
                 value->elems[1]->kind == ExprKind::IntLit) {
        const std::int64_t a = std::stoll(value->elems[0]->text);
        const std::int64_t b = std::stoll(value->elems[1]->text);
        if (cur >= 0 && a != cur) {
          report(DiagCode::TensorShapeMismatch, value->span,
                 "camada 'linear' espera entrada de " + std::to_string(a) +
                     " mas a camada anterior produz " + std::to_string(cur),
                 {"ajuste para linear: [" + std::to_string(cur) + ", " + std::to_string(b) + "]"});
        }
        cur = b;
      }
    });
  }
}

void check_program(const ast::Program& program, DiagnosticEngine& diag) {
  SemanticChecker(program, diag).run();
}

}  // namespace tilt
