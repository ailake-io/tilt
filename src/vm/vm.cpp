#include "vm/vm.hpp"

#include <cmath>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "runtime/tensor.hpp"

namespace tilt::vm {

using rt::Value;
using rt::ValueKind;

namespace {

// Copia de escalar sem o custo do construtor de copia do Value (string + 4 shared_ptr).
// `v` pode apontar para dentro de `pilha` (LoadLocal): le os campos antes de crescer o vetor.
inline void empilhar_copia(std::vector<Value>& pilha, const Value& v) {
  if (v.kind <= ValueKind::Decimal) {
    const ValueKind k = v.kind;
    const bool b = v.b;
    const std::int64_t i = v.i;
    const double d = v.d;
    pilha.emplace_back();
    Value& r = pilha.back();
    r.kind = k;
    r.b = b;
    r.i = i;
    r.d = d;
  } else {
    pilha.push_back(v);
  }
}

// Recursao maxima VM -> VM (cada nivel usa um frame C++ de executar()).
constexpr int kProfundidadeMaxVm = 4000;

// Caminho rapido de Binop entre dois numeros (inteiro/decimal): resultado no lugar do
// operando esquerdo, sem construir Values novos. Mesma semantica de rt::apply_binop
// (aritmetica em double, `/` e `%` por zero dao 0, inteiro so se ambos inteiros e
// operador != `/`). false = nao tratou (operandos nao numericos ou operador generico).
inline bool binop_numerico(BinOp op, std::vector<Value>& pilha) {
  const std::size_t n = pilha.size();
  Value& b = pilha[n - 1];
  Value& a = pilha[n - 2];
  if (!a.is_number() || !b.is_number()) return false;
  const double x = a.as_number();
  const double y = b.as_number();
  const bool ambos_inteiros = a.kind == ValueKind::Inteiro && b.kind == ValueKind::Inteiro;
  double r = 0.0;
  switch (op) {
    case BinOp::Soma:
      r = x + y;
      break;
    case BinOp::Sub:
      r = x - y;
      break;
    case BinOp::Mul:
      r = x * y;
      break;
    case BinOp::Div:
      r = y == 0.0 ? 0.0 : x / y;
      break;
    case BinOp::Mod:
      r = y == 0.0 ? 0.0 : std::fmod(x, y);
      break;
    case BinOp::Eq:
      a.kind = ValueKind::Logico;
      a.b = x == y;
      pilha.pop_back();
      return true;
    case BinOp::Ne:
      a.kind = ValueKind::Logico;
      a.b = x != y;
      pilha.pop_back();
      return true;
    case BinOp::Lt:
      a.kind = ValueKind::Logico;
      a.b = x < y;
      pilha.pop_back();
      return true;
    case BinOp::Le:
      a.kind = ValueKind::Logico;
      a.b = x <= y;
      pilha.pop_back();
      return true;
    case BinOp::Gt:
      a.kind = ValueKind::Logico;
      a.b = x > y;
      pilha.pop_back();
      return true;
    case BinOp::Ge:
      a.kind = ValueKind::Logico;
      a.b = x >= y;
      pilha.pop_back();
      return true;
    case BinOp::Generico:
      return false;
  }
  if (ambos_inteiros && op != BinOp::Div) {
    a.kind = ValueKind::Inteiro;
    a.i = static_cast<std::int64_t>(r);
  } else {
    a.kind = ValueKind::Decimal;
    a.d = r;
  }
  pilha.pop_back();
  return true;
}

}  // namespace

rt::Value Vm::run(const Chunk& chunk, std::vector<rt::Value> args) {
  const std::size_t base = pilha_.size();
  const std::size_t nl = static_cast<std::size_t>(chunk.num_locals);
  pilha_.resize(base + nl);
  for (std::size_t i = 0; i < args.size() && i < nl; ++i) pilha_[base + i] = std::move(args[i]);
  orcamento_ = 50'000'000;
  profundidade_ = 0;
  rt::Value r = executar(chunk, base);
  pilha_.resize(base);
  return r;
}

const Chunk* Vm::alvo_da_chamada(const Chunk& chunk, std::size_t idx_nome) {
  if (!resolver_) return nullptr;
  // Recursao/laco chamam do mesmo chunk em sequencia: evita o hash a cada chamada.
  Chamadas* alvo_cache = nullptr;
  if (&chunk == ultimo_chunk_) {
    alvo_cache = ultimas_chamadas_;
  } else {
    alvo_cache = &chamadas_[&chunk];  // nos do unordered_map tem endereco estavel
    ultimo_chunk_ = &chunk;
    ultimas_chamadas_ = alvo_cache;
  }
  Chamadas& c = *alvo_cache;
  if (c.alvos.empty()) {
    c.alvos.assign(chunk.names.size(), nullptr);
    c.resolvido.assign(chunk.names.size(), 0);
  }
  if (idx_nome >= c.alvos.size()) return nullptr;
  if (!c.resolvido[idx_nome]) {
    c.alvos[idx_nome] = resolver_(chunk.names[idx_nome]);
    c.resolvido[idx_nome] = 1;
  }
  return c.alvos[idx_nome];
}

rt::Value Vm::executar(const Chunk& chunk, std::size_t base) {
  auto pop = [&]() -> Value {
    Value v = std::move(pilha_.back());
    pilha_.pop_back();
    return v;
  };

  std::size_t ip = 0;
  while (ip < chunk.code.size()) {
    if (--orcamento_ < 0) throw std::runtime_error("VM: limite de instrucoes excedido");
    const Instr& in = chunk.code[ip++];
    switch (in.op) {
      case Op::Const:
        empilhar_copia(pilha_, chunk.consts[static_cast<std::size_t>(in.a)]);
        break;
      case Op::LoadLocal:
        empilhar_copia(pilha_, pilha_[base + static_cast<std::size_t>(in.a)]);
        break;
      case Op::StoreLocal: {
        Value& dst = pilha_[base + static_cast<std::size_t>(in.a)];
        Value& src = pilha_.back();
        if (src.kind <= ValueKind::Decimal && dst.kind <= ValueKind::Decimal) {
          // escalar -> escalar: copia so os 4 campos (evita mover string + 4 shared_ptr)
          dst.kind = src.kind;
          dst.b = src.b;
          dst.i = src.i;
          dst.d = src.d;
        } else {
          dst = std::move(src);
        }
        pilha_.pop_back();
        break;
      }
      case Op::Pop:
        pilha_.pop_back();
        break;
      case Op::Neg: {
        Value v = pop();
        pilha_.push_back(v.kind == ValueKind::Inteiro ? Value::inteiro(-v.i)
                                                      : Value::decimal(-v.as_number()));
        break;
      }
      case Op::Not:
        pilha_.push_back(Value::logico(!pop().truthy()));
        break;
      case Op::Truthy:
        pilha_.push_back(Value::logico(pop().truthy()));
        break;
      case Op::Binop: {
        if (in.b != 0 && pilha_.size() >= base + static_cast<std::size_t>(chunk.num_locals) + 2 &&
            binop_numerico(static_cast<BinOp>(in.b), pilha_)) {
          break;
        }
        Value b = pop();
        Value a = pop();
        bool ok = false;
        Value r = rt::apply_binop(chunk.op_names[static_cast<std::size_t>(in.a)], a, b, &ok);
        if (!ok) throw std::runtime_error("VM: operador desconhecido");
        pilha_.push_back(std::move(r));
        break;
      }
      case Op::Jump:
        ip = static_cast<std::size_t>(in.a);
        break;
      case Op::JumpIfFalse: {
        const bool verdadeiro = pilha_.back().truthy();
        pilha_.pop_back();
        if (!verdadeiro) ip = static_cast<std::size_t>(in.a);
        break;
      }
      case Op::CallFunc: {
        // Funcao de usuario ja compilada: quadro novo na mesma pilha (sem alocar).
        if (const Chunk* alvo = alvo_da_chamada(chunk, static_cast<std::size_t>(in.a))) {
          if (++profundidade_ > kProfundidadeMaxVm) {
            throw std::runtime_error("recursao profunda demais (mais de " +
                                     std::to_string(kProfundidadeMaxVm) + " niveis)");
          }
          const std::size_t argc = static_cast<std::size_t>(in.b);
          const std::size_t novo_base = pilha_.size() - argc;
          // Argumentos viram os primeiros locals; sobrando: descartados; faltando: nulo.
          pilha_.resize(novo_base + static_cast<std::size_t>(alvo->num_locals));
          Value r = executar(*alvo, novo_base);
          pilha_.resize(novo_base);
          --profundidade_;
          pilha_.push_back(std::move(r));
          break;
        }
        std::vector<Value> a(static_cast<std::size_t>(in.b));
        for (std::size_t k = a.size(); k-- > 0;) a[k] = pop();
        bool handled = false;
        Value r = call_(chunk.names[static_cast<std::size_t>(in.a)], a, &handled);
        if (!handled) throw std::runtime_error("VM: funcao desconhecida");
        pilha_.push_back(std::move(r));
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
        pilha_.push_back(Value::nulo());
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
        pilha_.push_back(Value::inteiro(n));
        break;
      }
      case Op::MakeList: {
        rt::ValueList items(static_cast<std::size_t>(in.b));
        for (std::size_t k = items.size(); k-- > 0;) items[k] = pop();
        pilha_.push_back(Value::lista(std::move(items)));
        break;
      }
      case Op::Index: {
        Value idx = pop();
        Value base = pop();
        if ((base.kind != ValueKind::Lista && base.kind != ValueKind::Tabela) || !base.list) {
          throw std::runtime_error("VM: indice espera uma lista");
        }
        const auto i = static_cast<long long>(idx.as_number());
        if (i < 0 || static_cast<std::size_t>(i) >= base.list->size()) {
          throw std::runtime_error("VM: indice fora da faixa");
        }
        pilha_.push_back((*base.list)[static_cast<std::size_t>(i)]);
        break;
      }
      case Op::GetField: {
        Value base = pop();
        const std::string& m = chunk.names[static_cast<std::size_t>(in.a)];
        const bool optional = in.b != 0;
        if (base.kind == ValueKind::Tensor && base.tensor) {
          const rt::Tensor& t = *base.tensor;
          if (m == "forma") {
            rt::ValueList dims;
            for (std::int64_t d : t.shape) dims.push_back(Value::inteiro(d));
            pilha_.push_back(Value::lista(std::move(dims)));
            break;
          }
          if (m == "dados") {
            rt::ValueList vals;
            for (float fv : t.data) vals.push_back(Value::decimal(fv));
            pilha_.push_back(Value::lista(std::move(vals)));
            break;
          }
          if (m == "soma") {
            pilha_.push_back(Value::decimal(rt::sum_all(t)));
            break;
          }
          if (m == "media") {
            pilha_.push_back(Value::decimal(rt::mean_all(t)));
            break;
          }
          if (m == "argmax") {
            pilha_.push_back(Value::inteiro(rt::argmax_last(t)));
            break;
          }
          if (m == "transposta") {
            pilha_.push_back(Value::tensor_de(rt::transpose2d(t)));
            break;
          }
          if (m == "softmax") {
            pilha_.push_back(Value::tensor_de(rt::softmax_last(t)));
            break;
          }
          if (m == "relu" || m == "gelu" || m == "silu" || m == "sigmoide" || m == "tanh") {
            pilha_.push_back(Value::tensor_de(rt::apply_unary(t, m)));
            break;
          }
          if (m == "item") {
            if (t.size() != 1) throw std::runtime_error("item espera um tensor de 1 elemento");
            pilha_.push_back(Value::decimal(t.data[0]));
            break;
          }
          if (m == "tamanho") {
            pilha_.push_back(Value::inteiro(t.size()));
            break;
          }
        }
        if ((base.kind == ValueKind::Mapa || base.kind == ValueKind::Tabela) && base.map) {
          if (Value* f = base.map->find(m)) {
            pilha_.push_back(*f);
            break;
          }
        }
        if (m == "tamanho") {
          if ((base.kind == ValueKind::Lista || base.kind == ValueKind::Tabela) && base.list) {
            pilha_.push_back(Value::inteiro(static_cast<std::int64_t>(base.list->size())));
            break;
          }
          if (base.kind == ValueKind::Texto) {
            pilha_.push_back(Value::inteiro(static_cast<std::int64_t>(base.s.size())));
            break;
          }
        }
        if (optional) {
          pilha_.push_back(Value::nulo());
          break;
        }
        throw std::runtime_error(std::string("'") + base.type_name() + "' nao tem o campo '" + m + "'");
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
