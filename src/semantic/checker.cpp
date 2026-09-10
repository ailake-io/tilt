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
    // `importar io` + `de io importar f` no mesmo arquivo carregam o mesmo
    // modulo duas vezes; o simbolo modulo e idempotente.
    const bool modulo_pair = kind == "modulo" && it->second.kind == "modulo";
    if (!treino_pair && !modulo_pair) {
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

    if (kw == "importar") {
      for (const auto& h : item->header) {
        if (h && h->kind == ExprKind::Name && h->text != "importar") {
          define(h->text, "modulo", Type::scalar(TypeKind::Unknown), item->span);
        }
      }
      continue;
    }
    if (kw == "de") {
      // `de <modulo> importar <nome>...`: o primeiro nome e o modulo; os
      // demais passam a ser tratados como funcoes (o runtime resolve e
      // valida as exportacoes ao carregar o arquivo).
      bool first = true;
      for (const auto& h : item->header) {
        if (!h || h->kind != ExprKind::Name || h->text == "importar") continue;
        if (first) {
          define(h->text, "modulo", Type::scalar(TypeKind::Unknown), item->span);
          first = false;
        } else {
          Type ft;
          ft.kind = TypeKind::Funcao;
          define(h->text, "funcao", std::move(ft), item->span);
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
      if (p.value) annotation_cache_[p.value.get()] = resolve_type_expr(*p.value);
    }
    if (decl.value) annotation_cache_[decl.value.get()] = resolve_type_expr(*decl.value);
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
          annotation_cache_[target->value.get()] = resolve_type_expr(*target->value);
        } else if (target->block) {
          for (const auto& sub : target->block->items) {
            if (sub && sub->kind == ItemKind::Field && sub->value) {
              annotation_cache_[sub->value.get()] = resolve_type_expr(*sub->value);
            }
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

// ---------------------------------------------------------- type inference
//
// Conservador como o shape solver: so valida quando os dois lados tem tipo
// conhecido. O runtime da tilt e permissivo (coage valores nao numericos
// para 0.0 via as_number()), entao aqui so se rejeita o que seria lixo
// silencioso em runtime — espelhando o que o runtime aceita de verdade
// (ex.: texto + numero concatena; "a" < "b" compara lexicograficamente).

using TypeKind = sema::TypeKind;

std::string type_kind_name(TypeKind k) {
  switch (k) {
    case TypeKind::Nulo: return "nulo";
    case TypeKind::Texto: return "texto";
    case TypeKind::Inteiro: return "inteiro";
    case TypeKind::Decimal: return "decimal";
    case TypeKind::Logico: return "logico";
    case TypeKind::Lista: return "lista";
    case TypeKind::Mapa: return "mapa";
    case TypeKind::Tabela: return "tabela";
    case TypeKind::Tensor: return "tensor";
    default: return "?";
  }
}

// Numericos no runtime incluem logico (as_number(verdadeiro) == 1).
bool is_number_kind(TypeKind k) {
  return k == TypeKind::Inteiro || k == TypeKind::Decimal || k == TypeKind::Logico;
}

bool kind_in_vec(TypeKind k, const std::vector<TypeKind>& set) {
  for (TypeKind e : set) {
    if (k == e) return true;
  }
  return false;
}

// Assinaturas parciais dos builtins de runtime: aridade minima, tipos
// esperados dos dois primeiros args posicionais (vazio = qualquer) e tipo
// de retorno. So espelha falhas que o runtime ja teria (fail por tipo/arity).
struct BuiltinSig {
  const char* name;
  int min_args;
  std::vector<TypeKind> arg0;
  std::vector<TypeKind> arg1;
  TypeKind ret;
  const char* usage;
};

const BuiltinSig* find_builtin_sig(std::string_view name) {
  static const std::vector<BuiltinSig> kSigs = {
      // saida e log — aceitam qualquer coisa
      {"imprimir", 0, {}, {}, TypeKind::Nulo, nullptr},
      {"imprima", 0, {}, {}, TypeKind::Nulo, nullptr},
      {"print", 0, {}, {}, TypeKind::Nulo, nullptr},
      {"registrar", 0, {}, {}, TypeKind::Nulo, nullptr},
      {"log", 0, {}, {}, TypeKind::Nulo, nullptr},
      // ambiente
      {"env", 1, {TypeKind::Texto}, {}, TypeKind::Texto, "env \"NOME_DA_VARIAVEL\""},
      // tamanhos e agregacoes
      {"tamanho", 0, {}, {}, TypeKind::Inteiro, nullptr},
      {"contar", 0, {}, {}, TypeKind::Inteiro, nullptr},
      {"somar", 1, {TypeKind::Lista}, {}, TypeKind::Unknown, "somar [1, 2, 3]"},
      {"media", 1, {TypeKind::Lista}, {}, TypeKind::Decimal, "media [1, 2, 3]"},
      {"min", 1, {TypeKind::Lista}, {}, TypeKind::Unknown, "min [1, 2, 3]"},
      {"max", 1, {TypeKind::Lista}, {}, TypeKind::Unknown, "max [1, 2, 3]"},
      {"intervalo", 1, {TypeKind::Inteiro, TypeKind::Decimal}, {}, TypeKind::Lista, "intervalo 0, 10"},
      {"ate", 1, {TypeKind::Inteiro, TypeKind::Decimal}, {}, TypeKind::Lista, nullptr},
      {"dividir", 2, {TypeKind::Texto}, {TypeKind::Texto}, TypeKind::Lista, "dividir texto, \",\""},
      {"dividir_texto", 1, {TypeKind::Texto}, {}, TypeKind::Lista, "dividir_texto texto, tamanho: 4"},
      // tensores
      {"tensor", 0, {}, {}, TypeKind::Tensor, nullptr},
      {"zeros", 0, {}, {}, TypeKind::Tensor, nullptr},
      {"uns", 0, {}, {}, TypeKind::Tensor, nullptr},
      {"aleatorio", 0, {}, {}, TypeKind::Tensor, nullptr},
      // leitura
      {"ler_csv", 1, {TypeKind::Texto}, {}, TypeKind::Tabela, "ler_csv \"dados.csv\""},
      {"ler_parquet", 1, {TypeKind::Texto}, {}, TypeKind::Tabela, nullptr},
      {"ler_delta", 1, {TypeKind::Texto}, {}, TypeKind::Tabela, nullptr},
      {"ler_iceberg", 1, {TypeKind::Texto}, {}, TypeKind::Tabela, nullptr},
      {"ler_json", 1, {TypeKind::Texto}, {}, TypeKind::Unknown, nullptr},
      {"carregador", 1, {TypeKind::Texto}, {}, TypeKind::Mapa, "carregador \"d.csv\", alvo: \"col\""},
      // escrita
      {"escrever_csv", 2, {TypeKind::Tabela, TypeKind::Lista}, {TypeKind::Texto}, TypeKind::Nulo,
       "escrever_csv tabela, \"saida.csv\""},
      {"escrever_parquet", 2, {TypeKind::Tabela, TypeKind::Lista}, {TypeKind::Texto}, TypeKind::Nulo,
       nullptr},
      {"escrever_delta", 2, {TypeKind::Tabela, TypeKind::Lista}, {TypeKind::Texto}, TypeKind::Nulo,
       nullptr},
      {"anexar_delta", 2, {TypeKind::Tabela, TypeKind::Lista}, {TypeKind::Texto}, TypeKind::Nulo,
       nullptr},
      {"escrever_iceberg", 2, {TypeKind::Tabela, TypeKind::Lista}, {TypeKind::Texto}, TypeKind::Nulo,
       nullptr},
      {"anexar_iceberg", 2, {TypeKind::Tabela, TypeKind::Lista}, {TypeKind::Texto}, TypeKind::Nulo,
       nullptr},
      {"escrever_json", 2, {}, {TypeKind::Texto}, TypeKind::Nulo, "escrever_json valor, \"saida.json\""},
      // LLM / utilidades
      {"incorporar", 1, {TypeKind::Texto}, {}, TypeKind::Tensor, "incorporar \"modelo\", \"texto\""},
      {"checar_tilt", 1, {TypeKind::Texto}, {}, TypeKind::Mapa, nullptr},
      // conectores
      {"executar_sql", 2, {TypeKind::Texto}, {TypeKind::Texto}, TypeKind::Unknown, nullptr},
      {"escrever_kafka", 2, {TypeKind::Texto}, {}, TypeKind::Nulo, nullptr},
      {"ler_kafka", 1, {TypeKind::Texto}, {}, TypeKind::Unknown, nullptr},
      {"escrever_redis", 3, {TypeKind::Texto}, {TypeKind::Texto}, TypeKind::Nulo, nullptr},
      {"ler_redis", 2, {TypeKind::Texto}, {TypeKind::Texto}, TypeKind::Unknown, nullptr},
      {"redis_executar", 2, {TypeKind::Texto}, {TypeKind::Texto}, TypeKind::Unknown, nullptr},
      {"redis_lote", 2, {TypeKind::Texto}, {}, TypeKind::Unknown, nullptr},
      {"ler_s3", 1, {TypeKind::Texto}, {}, TypeKind::Unknown, nullptr},
      {"escrever_s3", 2, {TypeKind::Texto}, {}, TypeKind::Nulo, nullptr},
      {"apagar_s3", 1, {TypeKind::Texto}, {}, TypeKind::Nulo, nullptr},
      {"copiar_s3", 2, {TypeKind::Texto}, {TypeKind::Texto}, TypeKind::Nulo, nullptr},
      {"listar_s3", 1, {TypeKind::Texto}, {}, TypeKind::Unknown, nullptr},
      {"cabecalho_s3", 1, {TypeKind::Texto}, {}, TypeKind::Unknown, nullptr},
      {"s3_iniciar_upload", 1, {TypeKind::Texto}, {}, TypeKind::Unknown, nullptr},
      {"s3_enviar_parte", 4, {TypeKind::Texto}, {}, TypeKind::Unknown, nullptr},
      {"s3_concluir_upload", 3, {TypeKind::Texto}, {}, TypeKind::Unknown, nullptr},
      {"s3_abortar_upload", 2, {TypeKind::Texto}, {}, TypeKind::Unknown, nullptr},
      {"mongo_inserir", 2, {TypeKind::Texto}, {}, TypeKind::Nulo, nullptr},
      {"mongo_buscar", 1, {TypeKind::Texto}, {}, TypeKind::Unknown, nullptr},
      {"mongo_atualizar", 3, {TypeKind::Texto}, {}, TypeKind::Nulo, nullptr},
      {"mongo_deletar", 2, {TypeKind::Texto}, {}, TypeKind::Nulo, nullptr},
      {"mongo_criar_indice", 2, {TypeKind::Texto}, {}, TypeKind::Nulo, nullptr},
      {"mongo_agregar", 2, {TypeKind::Texto}, {}, TypeKind::Unknown, nullptr},
  };
  for (const auto& s : kSigs) {
    if (name == s.name) return &s;
  }
  return nullptr;
}

// Metodos que o runtime despacha por tipo de receiver (eval_method).
bool is_tensor_method(std::string_view m) {
  return word_in(m, {"matmul", "mais", "conv2d", "norma_lote", "norma_camada", "reformar",
                     "softmax", "relu", "gelu", "silu", "sigmoide", "tanh", "soma", "media",
                     "argmax", "item", "forma", "dados", "transposta", "tamanho"});
}
bool is_table_method(std::string_view m) {
  return word_in(m, {"filtrar", "derivar", "mapear", "agrupar_por", "selecionar", "ordenar_por",
                     "limite", "primeiros", "distinto", "tamanho"});
}
bool is_texto_method(std::string_view m) { return word_in(m, {"maiusculas", "minusculas"}); }
// Metodos resolvidos dinamicamente sobre texto-nome-de-entidade (agente,
// equipe, ferramenta, indice, modelo) — nunca rejeitar esses.
bool is_entity_method(std::string_view m) {
  return word_in(m, {"responder", "perguntar", "executar", "para_frente", "inserir", "buscar",
                     "salvar_pesos"});
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

sema::TypeKind SemanticChecker::infer_type(const Expr& e, const TypeEnv& types) {
  using sema::TypeKind;
  switch (e.kind) {
    case ExprKind::IntLit: return TypeKind::Inteiro;
    case ExprKind::DecimalLit: return TypeKind::Decimal;
    case ExprKind::TextLit: return TypeKind::Texto;
    case ExprKind::BoolLit: return TypeKind::Logico;
    case ExprKind::NullLit: return TypeKind::Nulo;
    case ExprKind::ListLit: return TypeKind::Lista;
    case ExprKind::MapLit: return TypeKind::Mapa;
    case ExprKind::Device:
      return e.lhs ? infer_type(*e.lhs, types) : TypeKind::Unknown;
    case ExprKind::Name: {
      auto it = types.find(e.text);
      if (it != types.end()) return it->second;
      if (const Symbol* s = lookup(e.text)) {
        if (s->type.kind == TypeKind::Entidade) return TypeKind::Entidade;
      }
      return TypeKind::Unknown;
    }
    case ExprKind::Unary: {
      if (e.text == "nao") return TypeKind::Logico;
      if (e.text == "-") {
        const TypeKind t = e.rhs ? infer_type(*e.rhs, types) : TypeKind::Unknown;
        if (t != TypeKind::Unknown && !is_number_kind(t)) {
          report(DiagCode::TypeMismatch, e.span,
                 "'-' espera um numero, encontrou '" + type_kind_name(t) + "'",
                 {"se queria o oposto logico, use 'nao' antes de um valor logico"});
          return TypeKind::Unknown;
        }
        return t;
      }
      return TypeKind::Unknown;
    }
    case ExprKind::Binary: {
      const std::string& op = e.text;
      const TypeKind a = e.lhs ? infer_type(*e.lhs, types) : TypeKind::Unknown;
      const TypeKind b = e.rhs ? infer_type(*e.rhs, types) : TypeKind::Unknown;
      if (op == "e" || op == "ou" || op == "==" || op == "!=" || op == "contem") {
        return TypeKind::Logico;
      }
      if (op == "|") return TypeKind::Texto;  // em valor: display(a) + " | " + display(b)
      const bool cmp = op == "<" || op == "<=" || op == ">" || op == ">=";
      const bool arith = op == "+" || op == "-" || op == "*" || op == "/" || op == "%";
      if (!cmp && !arith) return TypeKind::Unknown;
      if (a != TypeKind::Unknown && b != TypeKind::Unknown) {
        const bool a_bad = kind_in_vec(a, {TypeKind::Lista, TypeKind::Mapa, TypeKind::Tabela});
        const bool b_bad = kind_in_vec(b, {TypeKind::Lista, TypeKind::Mapa, TypeKind::Tabela});
        if (cmp) {
          if (a == TypeKind::Tensor || b == TypeKind::Tensor || a_bad || b_bad) {
            report(DiagCode::TypeMismatch, e.span,
                   "comparacao '" + op + "' nao se aplica a '" + type_kind_name(a_bad ? a : b_bad ? b : TypeKind::Tensor) +
                       "'",
                   {"compare numeros ou textos (strings comparam lexicograficamente)"});
            return TypeKind::Unknown;
          }
          if ((a == TypeKind::Texto) != (b == TypeKind::Texto)) {
            report(DiagCode::TypeMismatch, e.span,
                   "comparacao '" + op + "' mistura '" + type_kind_name(a) + "' e '" +
                       type_kind_name(b) + "'",
                   {"para igualdade entre tipos diferentes use '=='"});
            return TypeKind::Unknown;
          }
        } else {  // aritmetica
          // '+' entre listas concatena (espelha apply_binop); os demais
          // operadores continuam invalidos para lista/mapa/tabela.
          const bool concat = op == "+" && a == TypeKind::Lista && b == TypeKind::Lista;
          if ((a_bad || b_bad) && !concat) {
            report(DiagCode::TypeMismatch, e.span,
                   "operacao '" + op + "' nao se aplica a '" +
                       type_kind_name(a_bad ? a : b) + "': aritmetica espera numeros, texto (so com '+'), lista (so com '+') ou tensor",
                   {"se queria concatenar, use '+' entre textos ou entre listas"});
            return TypeKind::Unknown;
          }
          if ((a == TypeKind::Texto || b == TypeKind::Texto) && op != "+") {
            report(DiagCode::TypeMismatch, e.span,
                   "texto so concatena com '+'; '" + op + "' nao se aplica a '" +
                       type_kind_name(a == TypeKind::Texto ? a : b) + "'",
                   {"converta para numero antes (ex.: valor vindo de ler_csv ja e numerico)"});
            return TypeKind::Unknown;
          }
          if ((a == TypeKind::Tensor) != (b == TypeKind::Tensor)) {
            const TypeKind other = a == TypeKind::Tensor ? b : a;
            if (!is_number_kind(other)) {
              report(DiagCode::TypeMismatch, e.span,
                     "operacao '" + op + "' entre tensor e '" + type_kind_name(other) +
                         "' espera um numero ou outro tensor",
                     {"use um escalar numerico (ex.: x * 2.0) ou um tensor de mesma forma"});
              return TypeKind::Unknown;
            }
          }
        }
      }
      if (cmp) return TypeKind::Logico;
      // tipo resultante da aritmetica (espelhando apply_binop)
      if (a == TypeKind::Texto || b == TypeKind::Texto) return TypeKind::Texto;
      if (a == TypeKind::Lista || b == TypeKind::Lista) return TypeKind::Lista;
      if (a == TypeKind::Tensor || b == TypeKind::Tensor) return TypeKind::Tensor;
      if (a == TypeKind::Inteiro && b == TypeKind::Inteiro && op != "/") return TypeKind::Inteiro;
      return is_number_kind(a) && is_number_kind(b) ? TypeKind::Decimal : TypeKind::Unknown;
    }
    case ExprKind::Index: {
      const std::string base = (e.lhs && e.lhs->kind == ExprKind::Name) ? e.lhs->text : "";
      if (word_in(base, {"tensor", "zeros", "uns", "aleatorio"})) return TypeKind::Tensor;
      return TypeKind::Unknown;  // indexacao e dinamica
    }
    case ExprKind::Member: {
      if (e.optional || !e.lhs) return TypeKind::Unknown;
      const TypeKind base = infer_type(*e.lhs, types);
      if (base == TypeKind::Unknown || base == TypeKind::Entidade) return TypeKind::Unknown;
      const std::string& m = e.text;
      if (base == TypeKind::Tensor) {
        if (m == "forma" || m == "dados") return TypeKind::Lista;
        if (m == "soma" || m == "media" || m == "item") return TypeKind::Decimal;
        if (m == "argmax" || m == "tamanho") return TypeKind::Inteiro;
        if (m == "transposta" || m == "softmax" || word_in(m, {"relu", "gelu", "silu", "sigmoide", "tanh"})) {
          return TypeKind::Tensor;
        }
      } else if (base == TypeKind::Tabela || base == TypeKind::Lista) {
        if (m == "tamanho") return TypeKind::Inteiro;
      } else if (base == TypeKind::Texto) {
        if (m == "tamanho") return TypeKind::Inteiro;
      } else if (base == TypeKind::Mapa || base == TypeKind::Registro) {
        return TypeKind::Unknown;  // campos dinamicos
      }
      // Inteiro/Decimal/Logico/Nulo: sem campos validos; Tabela/Lista so tem
      // 'tamanho' (nome de metodo usado como campo cai no erro abaixo,
      // espelhando o runtime).
      if (base == TypeKind::Tabela && is_table_method(m)) return TypeKind::Unknown;
      report(DiagCode::TypeMismatch, e.span,
             "'" + type_kind_name(base) + "' nao tem o campo '" + m + "'",
             {base == TypeKind::Tensor    ? "campos de tensor: forma, dados, soma, media, argmax, transposta, softmax, item, tamanho"
              : base == TypeKind::Tabela  ? "campos de tabela: tamanho (metodos: filtrar, derivar, agrupar_por, ...)"
              : base == TypeKind::Lista   ? "campos de lista: tamanho"
              : base == TypeKind::Texto   ? "campos de texto: tamanho (metodos: maiusculas, minusculas)"
                                          : "tipos numericos e logicos nao tem campos"});
      return TypeKind::Unknown;
    }
    case ExprKind::Call: {
      if (!e.lhs) return TypeKind::Unknown;
      if (e.lhs->kind == ExprKind::Member) {
        // Metodo com receiver de tipo conhecido.
        if (!e.lhs->lhs) return TypeKind::Unknown;
        const TypeKind base = infer_type(*e.lhs->lhs, types);
        if (base == TypeKind::Unknown || base == TypeKind::Entidade) return TypeKind::Unknown;
        const std::string& m = e.lhs->text;
        if (base == TypeKind::Tabela || base == TypeKind::Lista) {
          if (is_table_method(m) && m != "tamanho") return TypeKind::Tabela;
        } else if (base == TypeKind::Tensor) {
          if (word_in(m, {"matmul", "mais", "conv2d", "norma_lote", "norma_camada", "reformar",
                          "softmax"}) ||
              word_in(m, {"relu", "gelu", "silu", "sigmoide", "tanh", "transposta"})) {
            return TypeKind::Tensor;
          }
          if (m == "soma" || m == "media" || m == "item") return TypeKind::Decimal;
          if (m == "argmax") return TypeKind::Inteiro;
          if (m == "forma" || m == "dados") return TypeKind::Lista;
        } else if (base == TypeKind::Texto) {
          if (is_texto_method(m)) return TypeKind::Texto;
          if (is_entity_method(m)) return TypeKind::Unknown;  // despacho dinamico
        } else if (base == TypeKind::Mapa) {
          if (is_entity_method(m)) return TypeKind::Unknown;
          report(DiagCode::TypeMismatch, e.span, "'mapa' nao tem o metodo '" + m + "'",
                 {"mapas tem campos; metodos como filtrar/mapear so existem em tabelas"});
          return TypeKind::Unknown;
        }
        if ((base == TypeKind::Tabela || base == TypeKind::Lista) && m == "tamanho") {
          report(DiagCode::TypeMismatch, e.span,
                 "'tamanho' e um campo, nao um metodo — use 'tabela.tamanho' sem parenteses",
                 {});
          return TypeKind::Inteiro;
        }
        if ((base == TypeKind::Tabela || base == TypeKind::Lista) && !is_table_method(m) &&
            !is_entity_method(m)) {
          report(DiagCode::TypeMismatch, e.span,
                 "'" + type_kind_name(base) + "' nao tem o metodo '" + m + "'",
                 {"metodos de tabela: filtrar, derivar, mapear, agrupar_por, selecionar, ordenar_por, limite, primeiros, distinto"});
          return TypeKind::Unknown;
        }
        if (base == TypeKind::Tensor && !is_tensor_method(m) && !is_entity_method(m)) {
          report(DiagCode::TypeMismatch, e.span, "'tensor' nao tem o metodo '" + m + "'",
                 {"metodos de tensor: matmul, mais, conv2d, norma_lote, norma_camada, reformar, "
                  "softmax, soma, media, argmax, item"});
          return TypeKind::Unknown;
        }
        if (base == TypeKind::Texto && !is_texto_method(m) && !is_entity_method(m)) {
          report(DiagCode::TypeMismatch, e.span, "'texto' nao tem o metodo '" + m + "'",
                 {"metodos de texto: maiusculas, minusculas"});
          return TypeKind::Unknown;
        }
        if (is_number_kind(base) || base == TypeKind::Nulo) {
          report(DiagCode::TypeMismatch, e.span,
                 "'" + type_kind_name(base) + "' nao tem o metodo '" + m + "'",
                 {"tipos numericos, logicos e nulo nao tem metodos"});
        }
        return TypeKind::Unknown;
      }
      if (e.lhs->kind != ExprKind::Name) return TypeKind::Unknown;
      const std::string& name = e.lhs->text;
      // `funcao` do usuario sobrescreve builtin no runtime.
      if (const Symbol* s = lookup(name); s && s->kind == "funcao") return TypeKind::Unknown;
      const BuiltinSig* sig = find_builtin_sig(name);
      if (!sig) return TypeKind::Unknown;
      // aridade minima (só onde o runtime falha com menos args)
      int npos = 0;
      for (const auto& arg : e.args) {
        if (arg.name.empty()) ++npos;
      }
      if (npos < sig->min_args) {
        report(DiagCode::TypeMismatch, e.span,
               "'" + name + "' espera pelo menos " + std::to_string(sig->min_args) +
                   " argumento(s) posicionais",
               {sig->usage ? "uso: " + std::string(sig->usage)
                           : "veja o guia 03 para a assinatura de '" + name + "'"});
      }
      // tipos dos dois primeiros args posicionais
      auto check_arg = [&](int idx, const std::vector<TypeKind>& expected, const char* what) {
        if (expected.empty()) return;
        int seen = -1;
        const Expr* arg = nullptr;
        for (const auto& ar : e.args) {
          if (ar.name.empty()) {
            ++seen;
            if (seen == idx) arg = ar.value.get();
          }
        }
        if (!arg) return;
        const TypeKind t = infer_type(*arg, types);
        if (t == TypeKind::Unknown || kind_in_vec(t, expected)) return;
        report(DiagCode::TypeMismatch, arg->span,
               "'" + name + "' espera " + what + " (" + type_kind_name(expected.front()) +
                   (expected.size() > 1 ? " ou " + type_kind_name(expected.back()) : "") +
                   "), encontrou '" + type_kind_name(t) + "'",
               {sig->usage ? "uso: " + std::string(sig->usage)
                           : "ajuste o argumento para o tipo esperado"});
      };
      check_arg(0, sig->arg0, "um primeiro argumento");
      check_arg(1, sig->arg1, "um segundo argumento");
      // validacao especifica: tamanho/contar so aceitam colecoes e texto
      if ((name == "tamanho" || name == "contar") && npos >= 1) {
        const Expr* arg = first_positional_arg(e);
        if (arg) {
          const TypeKind t = infer_type(*arg, types);
          if (t != TypeKind::Unknown &&
              !kind_in_vec(t, {TypeKind::Lista, TypeKind::Tabela, TypeKind::Texto, TypeKind::Mapa})) {
            report(DiagCode::TypeMismatch, arg->span,
                   "tamanho espera lista, tabela, texto ou mapa; encontrou '" +
                       type_kind_name(t) + "'",
                   {"numeros nao tem tamanho; para digitos, converta para texto antes"});
          }
        }
      }
      return sig->ret;
    }
    default:
      return TypeKind::Unknown;
  }
}

void SemanticChecker::check_return(const Expr* value, Span span, const TypeEnv& types,
                                   const ShapeEnv& shapes) {
  if (!current_ret_ || current_ret_->kind == sema::TypeKind::Unknown) return;
  sema::Type vt = sema::Type::scalar(value ? infer_type(*value, types) : sema::TypeKind::Nulo);
  // Tensores inferidos carregam a forma conhecida para comparar dimensoes.
  if (vt.kind == sema::TypeKind::Tensor && value) {
    if (auto sh = infer_shape(*value, shapes)) {
      vt.name = "f32";
      vt.dims = *sh;
    }
  }
  if (vt.kind == sema::TypeKind::Unknown) return;
  if (sema::assignable(*current_ret_, vt)) return;
  report(DiagCode::TypeMismatch, span,
         "a funcao declara retorno '" + std::string(sema::type_to_string(*current_ret_)) +
             "' mas retorna '" + sema::type_to_string(vt) + "'",
         {"ajuste o valor retornado ou o tipo da assinatura '-> ...'"});
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

void SemanticChecker::walk_stmt(const Stmt& s, Scope& scope, ShapeEnv& shapes, TypeEnv& types) {
  switch (s.kind) {
    case ast::StmtKind::Assign:
      if (s.b) {
        check_expr(*s.b, scope);
        // Registra (ou invalida) a forma e o tipo conhecidos do nome atribuido.
        if (s.a && s.a->kind == ExprKind::Name) {
          if (auto sh = infer_shape(*s.b, shapes)) {
            shapes[s.a->text] = *sh;
          } else {
            shapes.erase(s.a->text);
          }
          const sema::TypeKind t = infer_type(*s.b, types);
          if (t != sema::TypeKind::Unknown) {
            types[s.a->text] = t;
          } else {
            types.erase(s.a->text);
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
        infer_type(*s.a, types);
      }
      return;
    case ast::StmtKind::Return:
      if (s.a) {
        check_expr(*s.a, scope);
        infer_shape(*s.a, shapes);
        infer_type(*s.a, types);
      }
      check_return(s.a.get(), s.span, types, shapes);
      return;
    case ast::StmtKind::If: {
      if (s.a) check_expr(*s.a, scope);
      walk_stmt_block(s.body, scope, shapes, types);
      for (const auto& ei : s.elifs) {
        if (ei.cond) check_expr(*ei.cond, scope);
        walk_stmt_block(ei.body, scope, shapes, types);
      }
      if (s.else_body) walk_stmt_block(*s.else_body, scope, shapes, types);
      return;
    }
    case ast::StmtKind::ForEach: {
      if (s.a) check_expr(*s.a, scope);
      Scope inner = scope;
      if (!s.name.empty()) inner.insert(s.name);
      walk_stmt_block(s.body, std::move(inner), shapes, types);
      return;
    }
    case ast::StmtKind::While:
      if (s.a) check_expr(*s.a, scope);
      walk_stmt_block(s.body, scope, shapes, types);
      return;
    case ast::StmtKind::Try: {
      walk_stmt_block(s.body, scope, shapes, types);
      if (s.catch_body) {
        Scope inner = scope;
        if (!s.name.empty()) inner.insert(s.name);
        walk_stmt_block(*s.catch_body, std::move(inner), shapes, types);
      }
      return;
    }
  }
}

void SemanticChecker::walk_stmt_block(const ast::Block& block, Scope scope, ShapeEnv shapes,
                                      TypeEnv types) {
  for (const auto& raw : block.items) {
    if (!raw) continue;
    const Item* it = raw.get();
    if (it->kind == ItemKind::ListEntry) {
      if (it->block) {
        walk_stmt_block(*it->block, scope, shapes, types);
        continue;
      }
      it = it->child.get();
      if (!it) continue;
    }
    if (it->kind == ItemKind::Stmt && it->stmt) {
      walk_stmt(*it->stmt, scope, shapes, types);
    } else if (it->kind == ItemKind::Field) {
      if (it->key == "verificar") continue;  // rule keys are column names, not vars
      if (it->value) check_expr(*it->value, scope);
      if (it->block) walk_stmt_block(*it->block, scope, shapes, types);
    }
  }
}

void SemanticChecker::scan_for_bodies(const ast::Block& block, Scope scope, ShapeEnv shapes,
                                      TypeEnv types) {
  collect_entrada_names(block, scope);
  // Anotacoes `entrada: tensor[...]` (inline ou em bloco) semeiam as formas
  // e os tipos conhecidos dos dados de entrada da entidade.
  if (const Item* ent = find_field(block, "entrada")) {
    if (auto dims = tensor_annotation_dims(ent->value.get())) {
      shapes["entrada"] = *dims;
    }
    if (auto t = annotation_cache_.find(ent->value.get()); t != annotation_cache_.end()) {
      types["entrada"] = t->second.kind;
    }
    if (ent->block) {
      for (const auto& sub : ent->block->items) {
        if (sub && sub->kind == ItemKind::Field) {
          if (auto d = tensor_annotation_dims(sub->value.get())) shapes[sub->key] = *d;
          if (auto t = annotation_cache_.find(sub->value.get()); t != annotation_cache_.end()) {
            types[sub->key] = t->second.kind;
          }
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
      walk_stmt_block(*it->block, scope, shapes, types);
    } else if (it->key != "verificar" && it->key != "camadas") {
      scan_for_bodies(*it->block, scope, shapes, types);
    }
  }
}

void SemanticChecker::check_bodies() {
  for (const auto& item : program_.items) {
    if (!item || item->kind != ItemKind::Decl || !item->block) continue;
    if (item->key == "funcao") {
      Scope scope;
      ShapeEnv shapes;
      TypeEnv types;
      for (const auto& p : item->params) {
        scope.insert(p.name);
        // Anotacao `p: tensor[...]` semeia a forma e o tipo do parametro.
        if (auto dims = tensor_annotation_dims(p.value.get())) shapes[p.name] = *dims;
        if (auto t = annotation_cache_.find(p.value.get()); t != annotation_cache_.end()) {
          types[p.name] = t->second.kind;
        }
      }
      // Tipo de retorno anotado (`-> texto` etc.), para checar `retornar`.
      const sema::Type* ret = nullptr;
      if (item->value) {
        if (auto t = annotation_cache_.find(item->value.get()); t != annotation_cache_.end()) {
          ret = &t->second;
        }
      }
      current_ret_ = ret;
      walk_stmt_block(*item->block, std::move(scope), std::move(shapes), std::move(types));
      current_ret_ = nullptr;
    } else if (is_entity_keyword(item->key)) {
      scan_for_bodies(*item->block, {}, {}, {});
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
