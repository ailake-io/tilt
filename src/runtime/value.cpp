#include "runtime/value.hpp"

#include <cmath>
#include <sstream>
#include <utility>

#include "runtime/tensor.hpp"

namespace tilt::rt {

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
  x.list = std::make_shared<ValueList>(std::move(v));
  return x;
}
Value Value::mapa() {
  Value x;
  x.kind = ValueKind::Mapa;
  x.map = std::make_shared<ValueMap>();
  return x;
}
Value Value::tabela(ValueList rows) {
  Value x;
  x.kind = ValueKind::Tabela;
  x.list = std::make_shared<ValueList>(std::move(rows));
  return x;
}
Value Value::tensor_de(Tensor t) {
  Value x;
  x.kind = ValueKind::Tensor;
  x.tensor = std::make_shared<Tensor>(std::move(t));
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
      return list && !list->empty();
    case ValueKind::Mapa:
      return map && !map->items.empty();
    case ValueKind::Tensor:
      return tensor && tensor->size() > 0;
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
      if (v.list) {
        for (std::size_t k = 0; k < v.list->size(); ++k) {
          if (k) r += ", ";
          r += to_display((*v.list)[k]);
        }
      }
      return r + "]";
    }
    case ValueKind::Mapa: {
      std::string r = "{";
      if (v.map) {
        for (std::size_t k = 0; k < v.map->items.size(); ++k) {
          if (k) r += ", ";
          r += v.map->items[k].first + ": " + to_display(v.map->items[k].second);
        }
      }
      return r + "}";
    }
    case ValueKind::Tabela: {
      std::size_t n = v.list ? v.list->size() : 0;
      return "tabela(" + std::to_string(n) + " linha" + (n == 1 ? "" : "s") + ")";
    }
    case ValueKind::Tensor:
      return v.tensor ? "tensor[" + v.tensor->shape_str() + "]" : "tensor[]";
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
    if (a.kind == ValueKind::Lista && a.list) {
      for (const Value& el : *a.list) {
        if (equals(el, b)) return Value::logico(true);
      }
    }
    return Value::logico(false);
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
    case ValueKind::Lista:
    case ValueKind::Tabela: {
      if (!a.list || !b.list || a.list->size() != b.list->size()) return false;
      for (std::size_t k = 0; k < a.list->size(); ++k) {
        if (!equals((*a.list)[k], (*b.list)[k])) return false;
      }
      return true;
    }
    default:
      return false;
  }
}

}  // namespace tilt::rt
