#include "semantic/checker.hpp"

#include <initializer_list>
#include <memory>
#include <utility>

namespace tilt {

using ast::Expr;
using ast::ExprKind;
using ast::Item;
using ast::ItemKind;
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
    report(DiagCode::DuplicateDeclaration, span, "'" + name + "' ja foi declarado",
           {"declaracao anterior na linha " + std::to_string(it->second.span.line)});
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

void check_program(const ast::Program& program, DiagnosticEngine& diag) {
  SemanticChecker(program, diag).run();
}

}  // namespace tilt
