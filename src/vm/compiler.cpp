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
  int extra_slots = 0;  // slots anonimos do dessugar (ex.: 'para cada')

  int slot_of(const std::string& name, bool create) {
    auto it = slots.find(name);
    if (it != slots.end()) return it->second;
    if (!create) return -1;
    int s = static_cast<int>(slots.size()) + extra_slots;
    slots.emplace(name, s);
    return s;
  }

  // Slot sem nome, para temporarios de dessugar. Nao desloca os slots
  // nomeados; chunk.num_locals soma os dois contadores no final.
  int fresh_slot() { return static_cast<int>(slots.size()) + extra_slots++; }

  int total_locals() const { return static_cast<int>(slots.size()) + extra_slots; }

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
      case ExprKind::ListLit:
        for (const auto& el : e.elems) expr(*el);
        emit(Op::MakeList, 0, static_cast<std::int32_t>(e.elems.size()));
        return;
      case ExprKind::Index: {
        if (e.lhs && e.lhs->kind == ExprKind::Name &&
            (e.lhs->text == "tensor" || e.lhs->text == "zeros" || e.lhs->text == "uns" ||
             e.lhs->text == "aleatorio")) {
          bail("construtor de tensor");
        }
        if (e.elems.size() != 1) bail("indice multidimensional");
        expr(*e.lhs);
        expr(*e.elems[0]);
        emit(Op::Index);
        return;
      }
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

  // Compila `se ...:` seguido de `- senao:` solto (item de campo em
  // 'passos:'), com a mesma semantica do interpretador: o senao so roda se
  // nenhum ramo do 'se' pegou.
  void if_with_stray_senao(const Stmt& s, const ast::Block& senao) {
    expr(*s.a);
    int j_else = emit(Op::JumpIfFalse);
    block(s.body);
    int j_end = emit(Op::Jump);
    chunk.code[static_cast<std::size_t>(j_else)].a = static_cast<std::int32_t>(chunk.code.size());
    block(senao);
    chunk.code[static_cast<std::size_t>(j_end)].a = static_cast<std::int32_t>(chunk.code.size());
  }

  void block(const ast::Block& b) {
    const auto& items = b.items;
    for (std::size_t k = 0; k < items.size(); ++k) {
      const Item* it = items[k].get();
      if (!it) continue;
      // `- senao:` solto: pareia com um 'se' sem else imediatamente anterior;
      // fora desse par, executa incondicionalmente (legado do interpretador).
      const Item* bare = it;
      if (bare->kind == ItemKind::ListEntry) {
        bare = bare->child ? bare->child.get()
                           : (bare->block && !bare->block->items.empty()
                                  ? bare->block->items[0].get()
                                  : nullptr);
      }
      const Item* next = k + 1 < items.size() ? items[k + 1].get() : nullptr;
      const Item* nbare = next;
      if (nbare && nbare->kind == ItemKind::ListEntry) {
        nbare = nbare->child ? nbare->child.get()
                             : (nbare->block && !nbare->block->items.empty()
                                    ? nbare->block->items[0].get()
                                    : nullptr);
      }
      if (bare && bare->kind == ItemKind::Stmt && bare->stmt &&
          bare->stmt->kind == StmtKind::If && !bare->stmt->else_body && nbare &&
          nbare->kind == ItemKind::Field && nbare->key == "senao" && nbare->header.empty() &&
          nbare->block) {
        if_with_stray_senao(*bare->stmt, *nbare->block);
        ++k;
        continue;
      }
      if (bare && bare->kind == ItemKind::Field && bare->key == "senao" &&
          bare->header.empty() && bare->block) {
        block(*bare->block);  // legado: incondicional
        continue;
      }
      if (it->kind == ItemKind::ListEntry) {
        // `- passo` de linha unica tem 'child'; `- para cada ...:' com corpo
        // indentado traz o stmt dentro de 'block'.
        if (it->block) {
          block(*it->block);
          continue;
        }
        if (it->child) it = it->child.get();
      }
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
      case StmtKind::ForEach: {
        // Dessugar: it = <iteravel>; i = 0; enquanto i < tamanho(it): var =
        // it[i]; <corpo>; i = i + 1
        const int it_slot = b_slot();
        const int i_slot = b_slot();
        const int var_slot = slot_of(s.name, true);
        expr(*s.a);
        emit(Op::StoreLocal, it_slot);
        emit(Op::Const, const_idx(rt::Value::inteiro(0)));
        emit(Op::StoreLocal, i_slot);
        const int start = static_cast<int>(chunk.code.size());
        emit(Op::LoadLocal, i_slot);
        emit(Op::LoadLocal, it_slot);
        emit(Op::Len);
        emit(Op::Binop, op_idx("<"));
        const int j_end = emit(Op::JumpIfFalse);
        emit(Op::LoadLocal, it_slot);
        emit(Op::LoadLocal, i_slot);
        emit(Op::Index);
        emit(Op::StoreLocal, var_slot);
        block(s.body);
        emit(Op::LoadLocal, i_slot);
        emit(Op::Const, const_idx(rt::Value::inteiro(1)));
        emit(Op::Binop, op_idx("+"));
        emit(Op::StoreLocal, i_slot);
        emit(Op::Jump, start);
        chunk.code[static_cast<std::size_t>(j_end)].a = static_cast<std::int32_t>(chunk.code.size());
        return;
      }
      default:
        bail("instrucao fora do subconjunto do VM");
    }
  }

  int b_slot() { return fresh_slot(); }
};

}  // namespace

Chunk compile_function(const Item& fn, const std::unordered_set<std::string>& known_funcs) {
  Builder b{known_funcs, {}, {}};
  for (const auto& p : fn.params) b.slot_of(p.name, true);
  if (!fn.block) bail("funcao sem corpo");
  b.block(*fn.block);
  b.emit(Op::ReturnNil);
  b.chunk.num_locals = b.total_locals();
  return std::move(b.chunk);
}

Chunk compile_pipeline(const Item& pipeline, const std::unordered_set<std::string>& known_funcs) {
  if (!pipeline.block) bail("pipeline sem bloco");
  // agenda/ao_falhar/verificar mudam a semantica de execucao; deixa para o
  // interpretador de arvore.
  for (const char* kw : {"agenda", "ao_falhar"}) {
    for (const auto& it : pipeline.block->items) {
      if (it && it->kind == ItemKind::Field && it->key == kw) {
        bail(std::string("campo '") + kw + "' do pipeline");
      }
    }
  }
  const Item* passos = nullptr;
  for (const auto& it : pipeline.block->items) {
    if (it && it->kind == ItemKind::Field && it->key == "passos") {
      passos = it.get();
      break;
    }
  }
  Builder b{known_funcs, {}, {}};
  if (passos && passos->block) b.block(*passos->block);
  b.emit(Op::ReturnNil);
  b.chunk.num_locals = b.total_locals();
  return std::move(b.chunk);
}

}  // namespace tilt::vm
