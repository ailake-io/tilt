#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tilt::rt {

enum class ValueKind { Nulo, Logico, Inteiro, Decimal, Texto, Lista, Mapa, Tabela, Tensor };

struct Value;
struct Tensor;

// Insertion-ordered string map; keeps interpreter output deterministic.
struct ValueMap {
  std::vector<std::pair<std::string, Value>> items;

  Value* find(const std::string& key);
  const Value* find(const std::string& key) const;
  void set(std::string key, Value value);
};

using ValueList = std::vector<Value>;

struct Value {
  ValueKind kind = ValueKind::Nulo;
  bool b = false;
  std::int64_t i = 0;
  double d = 0.0;
  std::string s;
  std::shared_ptr<ValueList> list;  // Lista, and Tabela (a list of Mapa rows)
  std::shared_ptr<ValueMap> map;    // Mapa
  std::shared_ptr<Tensor> tensor;   // Tensor

  static Value nulo() { return {}; }
  static Value logico(bool v);
  static Value inteiro(std::int64_t v);
  static Value decimal(double v);
  static Value texto(std::string v);
  static Value lista(ValueList v = {});
  static Value mapa();
  static Value tabela(ValueList rows = {});
  static Value tensor_de(Tensor t);

  bool is_number() const { return kind == ValueKind::Inteiro || kind == ValueKind::Decimal; }
  bool truthy() const;
  double as_number() const;  // Inteiro / Decimal / Logico -> double
  const char* type_name() const;
};

std::string to_display(const Value& v);  // human form used by `imprimir`
bool equals(const Value& a, const Value& b);

// Strict (non-lazy, non-tensor) binary operators shared by the tree interpreter
// and the bytecode VM. `op` is one of + - * / % == != < <= > >= contem.
// Sets *ok = false and returns Nulo if `op` is not one of these.
Value apply_binop(const std::string& op, const Value& a, const Value& b, bool* ok);

}  // namespace tilt::rt
