#include "runtime/value.hpp"

#include <cmath>
#include <memory_resource>
#include <sstream>
#include <utility>

#include "runtime/tensor.hpp"
#include "runtime/columnar.hpp"

namespace tilt::rt {

namespace {
std::pmr::synchronized_pool_resource value_object_pool;

template <typename T>
std::shared_ptr<T> pooled_object() {
  return std::allocate_shared<T>(std::pmr::polymorphic_allocator<T>(&value_object_pool));
}
}  // namespace

Value* ValueMap::find(const std::string& key) {
  for (auto& kv : items) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

const Value* ValueMap::find(const std::string& key) const {
  for (const auto& kv : items) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

void ValueMap::set(std::string key, Value value) {
  if (Value* slot = find(key)) {
    *slot = std::move(value);
    return;
  }
  items.emplace_back(std::move(key), std::move(value));
}

Value Value::logico(bool v) {
  Value x;
  x.kind = ValueKind::Logico;
  x.b = v;
  return x;
}
Value Value::inteiro(std::int64_t v) {
  Value x;
  x.kind = ValueKind::Inteiro;
  x.i = v;
  return x;
}
Value Value::decimal(double v) {
  Value x;
  x.kind = ValueKind::Decimal;
  x.d = v;
  return x;
}
Value Value::texto(std::string v) {
  Value x;
  x.kind = ValueKind::Texto;
  x.s = std::move(v);
  return x;
}
Value Value::lista(ValueList v) {
  Value x;
  x.kind = ValueKind::Lista;
  // Lists may be released by a different worker. The shared PMR pool adds
  // contention here; make_shared wins the size/concurrency/handoff benchmark.
  x.storage = std::make_shared<ValueStorage>();
  x.storage->list = std::make_shared<ValueList>();
  *x.storage->list = std::move(v);
  return x;
}
Value Value::mapa() {
  Value x;
  x.kind = ValueKind::Mapa;
  x.storage = std::make_shared<ValueStorage>();
  x.storage->map = pooled_object<ValueMap>();
  return x;
}
Value Value::tabela(ValueList rows) {
  Value x;
  x.kind = ValueKind::Tabela;
  x.storage = std::make_shared<ValueStorage>();
  x.storage->list = pooled_object<ValueList>();
  *x.storage->list = std::move(rows);
  return x;
}
Value Value::tabela_colunar(std::shared_ptr<ColumnarTable> columns) {
  Value x;
  x.kind = ValueKind::Tabela;
  x.storage = std::make_shared<ValueStorage>();
  x.storage->payload = std::move(columns);
  return x;
}

Closure* Value::closure() const {
  return kind == ValueKind::Funcao ? static_cast<Closure*>(payload_ref().get()) : nullptr;
}

std::shared_ptr<Closure> Value::closure_shared() const {
  return kind == ValueKind::Funcao ? std::static_pointer_cast<Closure>(payload_ref()) : nullptr;
}

ColumnarTable* Value::columnar(bool carregar) const {
  if (kind != ValueKind::Tabela || !payload_ref()) return nullptr;
  auto* columns = static_cast<ColumnarTable*>(payload_ref().get());
  if (carregar) columnar_ensure_loaded(columns);
  return columns;
}

void Value::materialize_rows() {
  if (ColumnarTable* columns = columnar(); columns && !list_ref()) {
    // Cópias de Value compartilham o bloco para reduzir custo de cópia, mas a
    // materialização troca o ponteiro do slot. Separe somente nesse caso para
    // preservar a semântica anterior de cópia por ponteiro.
    // `shared_ptr::unique()` foi removido no C++20 e não existe no libc++/MSVC.
    // O contador mantém a mesma semântica sem depender da extensão da STL.
    if (storage && storage.use_count() != 1) storage = std::make_shared<ValueStorage>(*storage);
    list_ref() = columns->rows_materialized();
  }
}
Value Value::tensor_de(Tensor t) {
  Value x;
  x.kind = ValueKind::Tensor;
  x.storage = std::make_shared<ValueStorage>();
  x.storage->tensor = std::make_shared<Tensor>(std::move(t));
  return x;
}

Value Value::funcao(std::shared_ptr<Closure> c) {
  Value x;
  x.kind = ValueKind::Funcao;
  x.storage = std::make_shared<ValueStorage>();
  x.storage->payload = std::move(c);
  return x;
}

bool Value::truthy() const {
  switch (kind) {
    case ValueKind::Nulo:
      return false;
    case ValueKind::Logico:
      return b;
    case ValueKind::Inteiro:
      return i != 0;
    case ValueKind::Decimal:
      return d != 0.0;
    case ValueKind::Texto:
      return !s.empty();
    case ValueKind::Lista:
    case ValueKind::Tabela:
      if (list_ref() && !list_ref()->empty()) return true;
      if (ColumnarTable* col = columnar()) {
        col->materialize_view();
        return col->rows > 0;
      }
      return false;
    case ValueKind::Mapa:
      return map_ref() && !map_ref()->items.empty();
    case ValueKind::Tensor:
      return tensor_ref() && tensor_ref()->size() > 0;
    case ValueKind::Funcao:
      return closure() != nullptr;
  }
  return false;
}

double Value::as_number() const {
  if (kind == ValueKind::Inteiro) return static_cast<double>(i);
  if (kind == ValueKind::Decimal) return d;
  if (kind == ValueKind::Logico) return b ? 1.0 : 0.0;
  return 0.0;
}

const char* Value::type_name() const {
  switch (kind) {
    case ValueKind::Nulo:
      return "nulo";
    case ValueKind::Logico:
      return "logico";
    case ValueKind::Inteiro:
      return "inteiro";
    case ValueKind::Decimal:
      return "decimal";
    case ValueKind::Texto:
      return "texto";
    case ValueKind::Lista:
      return "lista";
    case ValueKind::Mapa:
      return "mapa";
    case ValueKind::Tabela:
      return "tabela";
    case ValueKind::Tensor:
      return "tensor";
    case ValueKind::Funcao:
      return "funcao";
  }
  return "?";
}

namespace {

std::string number_to_string(double v) {
  if (std::isfinite(v) && v == static_cast<double>(static_cast<std::int64_t>(v))) {
    return std::to_string(static_cast<std::int64_t>(v));
  }
  std::ostringstream ss;
  ss << v;
  return ss.str();
}

}  // namespace

std::string to_display(const Value& v) {
  switch (v.kind) {
    case ValueKind::Nulo:
      return "nulo";
    case ValueKind::Logico:
      return v.b ? "verdadeiro" : "falso";
    case ValueKind::Inteiro:
      return std::to_string(v.i);
    case ValueKind::Decimal:
      return number_to_string(v.d);
    case ValueKind::Texto:
      return v.s;
    case ValueKind::Lista: {
      std::string r = "[";
      if (v.list_ref()) {
        for (std::size_t k = 0; k < v.list_ref()->size(); ++k) {
          if (k) r += ", ";
          r += to_display((*v.list_ref())[k]);
        }
      }
      return r + "]";
    }
    case ValueKind::Mapa: {
      std::string r = "{";
      if (v.map_ref()) {
        for (std::size_t k = 0; k < v.map_ref()->items.size(); ++k) {
          if (k) r += ", ";
          r += v.map_ref()->items[k].first + ": " + to_display(v.map_ref()->items[k].second);
        }
      }
      return r + "}";
    }
    case ValueKind::Tabela: {
      std::size_t n = v.list_ref() ? v.list_ref()->size() : (v.columnar() ? v.columnar()->rows : 0);
      return "tabela(" + std::to_string(n) + " linha" + (n == 1 ? "" : "s") + ")";
    }
    case ValueKind::Tensor:
      return v.tensor_ref() ? "tensor[" + v.tensor_ref()->shape_str() + "]" : "tensor[]";
    case ValueKind::Funcao:
      return "<funcao>";
  }
  return "?";
}

Value apply_binop(const std::string& op, const Value& a, const Value& b, bool* ok) {
  *ok = true;

  if (op == "==") return Value::logico(equals(a, b));
  if (op == "!=") return Value::logico(!equals(a, b));
  if (op == "contem") {
    if (a.kind == ValueKind::Texto && b.kind == ValueKind::Texto) {
      return Value::logico(a.s.find(b.s) != std::string::npos);
    }
    if (a.kind == ValueKind::Lista && a.list_ref()) {
      for (const Value& el : *a.list_ref()) {
        if (equals(el, b)) return Value::logico(true);
      }
    }
    return Value::logico(false);
  }
  if (op == "+" && a.kind == ValueKind::Lista && b.kind == ValueKind::Lista && a.list_ref() && b.list_ref()) {
    ValueList out = *a.list_ref();
    out.insert(out.end(), b.list_ref()->begin(), b.list_ref()->end());
    return Value::lista(std::move(out));
  }
  if (op == "+" && (a.kind == ValueKind::Texto || b.kind == ValueKind::Texto)) {
    return Value::texto(to_display(a) + to_display(b));
  }

  const bool cmp = op == "<" || op == "<=" || op == ">" || op == ">=";
  if (cmp) {
    double x;
    double y;
    if (a.kind == ValueKind::Texto && b.kind == ValueKind::Texto) {
      x = static_cast<double>(a.s.compare(b.s));
      y = 0.0;
    } else {
      x = a.as_number();
      y = b.as_number();
    }
    if (op == "<") return Value::logico(x < y);
    if (op == "<=") return Value::logico(x <= y);
    if (op == ">") return Value::logico(x > y);
    return Value::logico(x >= y);
  }

  if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
    const double x = a.as_number();
    const double y = b.as_number();
    double r = 0.0;
    if (op == "+") r = x + y;
    else if (op == "-") r = x - y;
    else if (op == "*") r = x * y;
    else if (op == "/") r = y == 0.0 ? 0.0 : x / y;
    else r = y == 0.0 ? 0.0 : std::fmod(x, y);
    const bool both_int = a.kind == ValueKind::Inteiro && b.kind == ValueKind::Inteiro;
    if (both_int && op != "/") return Value::inteiro(static_cast<std::int64_t>(r));
    return Value::decimal(r);
  }

  *ok = false;
  return Value::nulo();
}

bool equals(const Value& a, const Value& b) {
  if (a.is_number() && b.is_number()) return a.as_number() == b.as_number();
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case ValueKind::Nulo:
      return true;
    case ValueKind::Logico:
      return a.b == b.b;
    case ValueKind::Texto:
      return a.s == b.s;
    case ValueKind::Funcao:
      return a.payload_ref() == b.payload_ref();
    case ValueKind::Lista:
    case ValueKind::Tabela: {
      Value left = a;
      Value right = b;
      left.materialize_rows();
      right.materialize_rows();
      if (!left.list_ref() || !right.list_ref() || left.list_ref()->size() != right.list_ref()->size()) return false;
      for (std::size_t k = 0; k < left.list_ref()->size(); ++k) {
        if (!equals((*left.list_ref())[k], (*right.list_ref())[k])) return false;
      }
      return true;
    }
    default:
      return false;
  }
}

}  // namespace tilt::rt
