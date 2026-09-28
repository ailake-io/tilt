#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tilt::rt {

enum class ValueKind { Nulo, Logico, Inteiro, Decimal, Texto, Lista, Mapa, Tabela, Tensor, Funcao };

struct Value;
struct ValueStorage;
struct Tensor;
struct ColumnarTable;
void columnar_ensure_loaded(ColumnarTable* table);
struct Closure;  // funcao anonima + variaveis capturadas; definida pelo interpretador

// Insertion-ordered string map; keeps interpreter output deterministic.
struct ValueMap {
  std::vector<std::pair<std::string, Value>> items;

  Value* find(const std::string& key);
  const Value* find(const std::string& key) const;
  void set(std::string key, Value value);
};

using ValueList = std::vector<Value>;

// Slots mutuamente exclusivos por kind. O bloco de referências é compartilhado
// pela cópia de Values, preservando a semântica dos objetos apontados.
struct ValueStorage {
  std::shared_ptr<ValueList> list;  // Lista e Tabela materializada
  std::shared_ptr<ValueMap> map;    // Mapa
  std::shared_ptr<Tensor> tensor;   // Tensor
  std::shared_ptr<void> payload;    // Funcao ou tabela colunar
};

struct Value {
  ValueKind kind = ValueKind::Nulo;
  // O kind define qual escalar esta ativo; manter os tres campos separados
  // desperdicava 16 bytes de padding e armazenamento por Value.
  union {
    bool b;
    std::int64_t i;
    double d;
  };
  std::string s;
  // As referências para os objetos de maior porte vivem em um único bloco
  // compartilhado. Isso elimina três shared_ptr independentes e o payload
  // redundante do layout de cada Value; valores escalares continuam sem
  // qualquer alocação.
  std::shared_ptr<ValueStorage> storage;

  Value() : kind(ValueKind::Nulo), b(false) {}

  static Value nulo() { return {}; }
  static Value logico(bool v);
  static Value inteiro(std::int64_t v);
  static Value decimal(double v);
  static Value texto(std::string v);
  static Value lista(ValueList v = {});
  static Value mapa();
  static Value tabela(ValueList rows = {});
  static Value tabela_colunar(std::shared_ptr<ColumnarTable> columns);
  static Value tensor_de(Tensor t);
  static Value funcao(std::shared_ptr<Closure> c);

  // Acesso compatível aos objetos armazenados no bloco compartilhado. As
  // versões const retornam um slot vazio estático quando o Value é escalar;
  // as versões mutáveis criam o bloco sob demanda.
  std::shared_ptr<ValueList>& list_ref() {
    if (!storage) storage = std::make_shared<ValueStorage>();
    return storage->list;
  }
  const std::shared_ptr<ValueList>& list_ref() const {
    static const std::shared_ptr<ValueList> empty;
    return storage ? storage->list : empty;
  }
  std::shared_ptr<ValueMap>& map_ref() {
    if (!storage) storage = std::make_shared<ValueStorage>();
    return storage->map;
  }
  const std::shared_ptr<ValueMap>& map_ref() const {
    static const std::shared_ptr<ValueMap> empty;
    return storage ? storage->map : empty;
  }
  std::shared_ptr<Tensor>& tensor_ref() {
    if (!storage) storage = std::make_shared<ValueStorage>();
    return storage->tensor;
  }
  const std::shared_ptr<Tensor>& tensor_ref() const {
    static const std::shared_ptr<Tensor> empty;
    return storage ? storage->tensor : empty;
  }
  std::shared_ptr<void>& payload_ref() {
    if (!storage) storage = std::make_shared<ValueStorage>();
    return storage->payload;
  }
  const std::shared_ptr<void>& payload_ref() const {
    static const std::shared_ptr<void> empty;
    return storage ? storage->payload : empty;
  }

  bool is_number() const { return kind == ValueKind::Inteiro || kind == ValueKind::Decimal; }
  bool truthy() const;
  double as_number() const;  // Inteiro / Decimal / Logico -> double
  const char* type_name() const;
  Closure* closure() const;
  std::shared_ptr<Closure> closure_shared() const;
  ColumnarTable* columnar() const;
  void materialize_rows();
};

std::string to_display(const Value& v);  // human form used by `imprimir`
bool equals(const Value& a, const Value& b);

// Strict (non-lazy, non-tensor) binary operators shared by the tree interpreter
// and the bytecode VM. `op` is one of + - * / % == != < <= > >= contem.
// Sets *ok = false and returns Nulo if `op` is not one of these.
Value apply_binop(const std::string& op, const Value& a, const Value& b, bool* ok);

}  // namespace tilt::rt
