#include "vm/vm.hpp"

#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace tilt::vm {

using rt::Value;
using rt::ValueKind;

rt::Value Vm::run(const Chunk& chunk, std::vector<rt::Value> args) {
  std::vector<Value> locals(static_cast<std::size_t>(chunk.num_locals));
  for (std::size_t i = 0; i < args.size() && i < locals.size(); ++i) {
    locals[i] = std::move(args[i]);
  }

  std::vector<Value> stack;
  stack.reserve(16);
  auto pop = [&]() -> Value {
    Value v = std::move(stack.back());
    stack.pop_back();
    return v;
  };

  std::int64_t budget = 50'000'000;
  std::size_t ip = 0;
  while (ip < chunk.code.size()) {
    if (--budget < 0) throw std::runtime_error("VM: limite de instrucoes excedido");
    const Instr& in = chunk.code[ip++];
    switch (in.op) {
      case Op::Const:
        stack.push_back(chunk.consts[static_cast<std::size_t>(in.a)]);
        break;
      case Op::LoadLocal:
        stack.push_back(locals[static_cast<std::size_t>(in.a)]);
        break;
      case Op::StoreLocal:
        locals[static_cast<std::size_t>(in.a)] = pop();
        break;
      case Op::Pop:
        stack.pop_back();
        break;
      case Op::Neg: {
        Value v = pop();
        stack.push_back(v.kind == ValueKind::Inteiro ? Value::inteiro(-v.i)
                                                     : Value::decimal(-v.as_number()));
        break;
      }
      case Op::Not:
        stack.push_back(Value::logico(!pop().truthy()));
        break;
      case Op::Truthy:
        stack.push_back(Value::logico(pop().truthy()));
        break;
      case Op::And: {
        bool b = pop().truthy();
        bool a = pop().truthy();
        stack.push_back(Value::logico(a && b));
        break;
      }
      case Op::Or: {
        bool b = pop().truthy();
        bool a = pop().truthy();
        stack.push_back(Value::logico(a || b));
        break;
      }
      case Op::Binop: {
        Value b = pop();
        Value a = pop();
        bool ok = false;
        Value r = rt::apply_binop(chunk.op_names[static_cast<std::size_t>(in.a)], a, b, &ok);
        if (!ok) throw std::runtime_error("VM: operador desconhecido");
        stack.push_back(std::move(r));
        break;
      }
      case Op::Jump:
        ip = static_cast<std::size_t>(in.a);
        break;
      case Op::JumpIfFalse:
        if (!pop().truthy()) ip = static_cast<std::size_t>(in.a);
        break;
      case Op::CallFunc: {
        std::vector<Value> a(static_cast<std::size_t>(in.b));
        for (std::size_t k = a.size(); k-- > 0;) a[k] = pop();
        bool handled = false;
        Value r = call_(chunk.names[static_cast<std::size_t>(in.a)], a, &handled);
        if (!handled) throw std::runtime_error("VM: funcao desconhecida");
        stack.push_back(std::move(r));
        break;
      }
      case Op::Print: {
        std::vector<Value> a(static_cast<std::size_t>(in.b));
        for (std::size_t k = a.size(); k-- > 0;) a[k] = pop();
        for (std::size_t k = 0; k < a.size(); ++k) {
          if (k) out_ << ' ';
          out_ << rt::to_display(a[k]);
        }
        out_ << '\n';
        stack.push_back(Value::nulo());
        break;
      }
      case Op::Len: {
        Value v = pop();
        std::int64_t n = 0;
        if ((v.kind == ValueKind::Lista || v.kind == ValueKind::Tabela) && v.list) {
          n = static_cast<std::int64_t>(v.list->size());
        } else if (v.kind == ValueKind::Texto) {
          n = static_cast<std::int64_t>(v.s.size());
        } else if (v.kind == ValueKind::Mapa && v.map) {
          n = static_cast<std::int64_t>(v.map->items.size());
        }
        stack.push_back(Value::inteiro(n));
        break;
      }
      case Op::Return:
        return pop();
      case Op::ReturnNil:
        return Value::nulo();
    }
  }
  return Value::nulo();
}

}  // namespace tilt::vm
