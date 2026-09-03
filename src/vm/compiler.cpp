#include "vm/compiler.hpp"

#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tilt::vm {

using ast::Expr;
using ast::ExprKind;
using ast::Item;
using ast::ItemKind;
using ast::Stmt;
using ast::StmtKind;

namespace {

[[noreturn]] void bail(std::string why) { throw NotCompilable{std::move(why)}; }

struct Builder {
  const std::unordered_set<std::string>& known;
  Chunk chunk;
  std::unordered_map<std::string, int> slots;

  int slot_of(const std::string& name, bool create) {
    auto it = slots.find(name);
    if (it != slots.end()) return it->second;
    if (!create) return -1;
    int s = static_cast<int>(slots.size());
    slots.emplace(name, s);
    return s;
  }

  int emit(Op op, std::int32_t a = 0, std::int32_t b = 0) {
    chunk.code.push_back({op, a, b});
    return static_cast<int>(chunk.code.size()) - 1;
  }

  int const_idx(rt::Value v) {
    chunk.consts.push_back(std::move(v));
    return static_cast<int>(chunk.consts.size()) - 1;
  }

  int op_idx(const std::string& name) {
    for (std::size_t i = 0; i < chunk.op_names.size(); ++i) {
      if (chunk.op_names[i] == name) return static_cast<int>(i);
    }
    chunk.op_names.push_back(name);
    return static_cast<int>(chunk.op_names.size()) - 1;
  }

  int name_idx(const std::string& name) {
    for (std::size_t i = 0; i < chunk.names.size(); ++i) {
      if (chunk.names[i] == name) return static_cast<int>(i);
    }
    chunk.names.push_back(name);
    return static_cast<int>(chunk.names.size()) - 1;
  }

  void expr(const Expr& e) {
    switch (e.kind) {
      case ExprKind::IntLit:
        emit(Op::Const, const_idx(rt::Value::inteiro(std::strtoll(e.text.c_str(), nullptr, 10))));
        return;
      case ExprKind::DecimalLit:
        emit(Op::Const, const_idx(rt::Value::decimal(std::strtod(e.text.c_str(), nullptr))));
        return;
      case ExprKind::TextLit:
        if (e.text.find("{{") != std::string::npos) bail("interpolacao de texto");
        emit(Op::Const, const_idx(rt::Value::texto(e.text)));
        return;
      case ExprKind::BoolLit:
        emit(Op::Const, const_idx(rt::Value::logico(e.boolean)));
        return;
      case ExprKind::NullLit:
        emit(Op::Const, const_idx(rt::Value::nulo()));
        return;
      case ExprKind::Name: {
        int s = slot_of(e.text, false);
        if (s < 0) bail("nome '" + e.text + "' fora do subconjunto do VM");
        emit(Op::LoadLocal, s);
        return;
      }
      case ExprKind::Unary:
        expr(*e.rhs);
        emit(e.text == "-" ? Op::Neg : Op::Not);
        return;
      case ExprKind::Binary: {
        if (e.text == "|") bail("operador '|'");
        if (e.text == "e" || e.text == "ou") {
          expr(*e.lhs);
          emit(Op::Truthy);
          expr(*e.rhs);
          emit(Op::Truthy);
          emit(e.text == "e" ? Op::And : Op::Or);
          return;
        }
        expr(*e.lhs);
        expr(*e.rhs);
        emit(Op::Binop, op_idx(e.text));
        return;
      }
      case ExprKind::Call: {
        if (!e.lhs || e.lhs->kind != ExprKind::Name) bail("chamada nao trivial");
        const std::string& callee = e.lhs->text;
        for (const auto& a : e.args) {
          if (!a.name.empty()) bail("argumento nomeado");
        }
        if (callee == "imprimir" || callee == "imprima" || callee == "print") {
          for (const auto& a : e.args) expr(*a.value);
          emit(Op::Print, 0, static_cast<std::int32_t>(e.args.size()));
          return;
        }
        if ((callee == "tamanho" || callee == "contar") && e.args.size() == 1) {
          expr(*e.args[0].value);
          emit(Op::Len);
          return;
        }
        if (known.count(callee)) {
          for (const auto& a : e.args) expr(*a.value);
          emit(Op::CallFunc, name_idx(callee), static_cast<std::int32_t>(e.args.size()));
          return;
        }
        bail("chamada a '" + callee + "'");
      }
      default:
        bail("expressao fora do subconjunto do VM");
    }
  }

  void block(const ast::Block& b) {
    for (const auto& raw : b.items) {
      if (!raw) continue;
      const Item* it = raw.get();
      if (it->kind == ItemKind::ListEntry && it->child) it = it->child.get();
      if (it->kind != ItemKind::Stmt || !it->stmt) bail("item fora do subconjunto do VM");
      stmt(*it->stmt);
    }
  }

  void stmt(const Stmt& s) {
    switch (s.kind) {
      case StmtKind::Expr:
        expr(*s.a);
        emit(Op::Pop);
        return;
      case StmtKind::Assign: {
        if (!s.a || s.a->kind != ExprKind::Name) bail("atribuicao a alvo nao simples");
        expr(*s.b);
        emit(Op::StoreLocal, slot_of(s.a->text, true));
        return;
      }
      case StmtKind::Return:
        if (s.a) {
          expr(*s.a);
          emit(Op::Return);
        } else {
          emit(Op::ReturnNil);
        }
        return;
      case StmtKind::If: {
        expr(*s.a);
        int j_else = emit(Op::JumpIfFalse);
        block(s.body);
        int j_end = emit(Op::Jump);
        chunk.code[static_cast<std::size_t>(j_else)].a = static_cast<std::int32_t>(chunk.code.size());
        std::vector<int> ends{j_end};
        for (const auto& ei : s.elifs) {
          expr(*ei.cond);
          int je = emit(Op::JumpIfFalse);
          block(ei.body);
          ends.push_back(emit(Op::Jump));
          chunk.code[static_cast<std::size_t>(je)].a = static_cast<std::int32_t>(chunk.code.size());
        }
        if (s.else_body) block(*s.else_body);
        for (int e : ends) {
          chunk.code[static_cast<std::size_t>(e)].a = static_cast<std::int32_t>(chunk.code.size());
        }
        return;
      }
      case StmtKind::While: {
        int start = static_cast<int>(chunk.code.size());
        expr(*s.a);
        int j_end = emit(Op::JumpIfFalse);
        block(s.body);
        emit(Op::Jump, start);
        chunk.code[static_cast<std::size_t>(j_end)].a = static_cast<std::int32_t>(chunk.code.size());
        return;
      }
      default:
        bail("instrucao fora do subconjunto do VM");
    }
  }
};

}  // namespace

Chunk compile_function(const Item& fn, const std::unordered_set<std::string>& known_funcs) {
  Builder b{known_funcs, {}, {}};
  for (const auto& p : fn.params) b.slot_of(p.name, true);
  if (!fn.block) bail("funcao sem corpo");
  b.block(*fn.block);
  b.emit(Op::ReturnNil);
  b.chunk.num_locals = static_cast<int>(b.slots.size());
  return std::move(b.chunk);
}

}  // namespace tilt::vm
