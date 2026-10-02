#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
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

// String compacta com SSO de 22 bytes. O layout ocupa 24 bytes, contra 32
// bytes de std::string na libc++/libstdc++; textos longos mantêm ownership
// próprio e textos curtos não fazem alocação. A interface cobre o subconjunto
// usado pelo runtime para manter `Value::s` compatível.
class CompactString {
  static constexpr std::size_t kInline = 22;
  static_assert(sizeof(std::size_t) == 8, "CompactString requer size_t de 64 bits");
  alignas(8) unsigned char bytes_[24]{};

  bool curta() const { return bytes_[23] <= kInline; }
  std::size_t palavra(std::size_t indice) const {
    std::size_t valor = 0;
    std::memcpy(&valor, bytes_ + indice * sizeof(valor), sizeof(valor));
    return valor;
  }
  void palavra(std::size_t indice, std::size_t valor) {
    std::memcpy(bytes_ + indice * sizeof(valor), &valor, sizeof(valor));
  }
  char* ponteiro_longo() const {
    return reinterpret_cast<char*>(palavra(0));
  }
  void liberar() {
    if (!curta()) delete[] ponteiro_longo();
  }
  void atribuir(std::string_view valor) {
    liberar();
    std::memset(bytes_, 0, sizeof(bytes_));
    if (valor.size() <= kInline) {
      if (!valor.empty()) std::memcpy(bytes_, valor.data(), valor.size());
      bytes_[valor.size()] = '\0';
      bytes_[23] = static_cast<unsigned char>(valor.size());
      return;
    }
    char* copia = new char[valor.size() + 1];
    std::memcpy(copia, valor.data(), valor.size());
    copia[valor.size()] = '\0';
    palavra(0, reinterpret_cast<std::size_t>(copia));
    palavra(1, valor.size());
    // O ultimo byte funciona como marcador independente da ordem dos bytes;
    // os dois primeiros words guardam ponteiro e tamanho.
    bytes_[23] = 0xFF;
  }

 public:
  CompactString() { bytes_[23] = 0; }
  explicit CompactString(const char* valor) : CompactString(std::string_view(valor ? valor : "")) {}
  explicit CompactString(std::string_view valor) { atribuir(valor); }
  explicit CompactString(const std::string& valor) : CompactString(std::string_view(valor)) {}
  explicit CompactString(std::string&& valor) : CompactString(std::string_view(valor)) {}
  CompactString(const CompactString& outro) : CompactString(outro.view()) {}
  CompactString(CompactString&& outro) noexcept {
    std::memcpy(bytes_, outro.bytes_, sizeof(bytes_));
    outro.bytes_[0] = '\0';
    outro.bytes_[23] = 0;
  }
  ~CompactString() { liberar(); }

  CompactString& operator=(const CompactString& outro) {
    if (this != &outro) atribuir(outro.view());
    return *this;
  }
  CompactString& operator=(CompactString&& outro) noexcept {
    if (this != &outro) {
      liberar();
      std::memcpy(bytes_, outro.bytes_, sizeof(bytes_));
      outro.bytes_[0] = '\0';
      outro.bytes_[23] = 0;
    }
    return *this;
  }
  CompactString& operator=(std::string valor) {
    atribuir(valor);
    return *this;
  }
  CompactString& operator=(const char* valor) {
    atribuir(std::string_view(valor ? valor : ""));
    return *this;
  }

  std::size_t size() const { return curta() ? bytes_[23] : palavra(1); }
  bool empty() const { return size() == 0; }
  const char* data() const { return curta() ? reinterpret_cast<const char*>(bytes_) : ponteiro_longo(); }
  const char* c_str() const { return data(); }
  const char* begin() const { return data(); }
  const char* end() const { return data() + size(); }
  char operator[](std::size_t indice) const { return data()[indice]; }
  std::string_view view() const { return std::string_view(data(), size()); }
  std::string str() const { return std::string(view()); }
  operator std::string() const { return str(); }
  // Apaga o conteúdo antes de liberar o buffer; usado para segredos efêmeros.
  void secure_clear() {
    const bool is_short = curta();
    char* long_ptr = is_short ? nullptr : ponteiro_longo();
    const std::size_t long_size = is_short ? 0 : palavra(1);
    if (!is_short && long_ptr) {
      volatile char* secret = long_ptr;
      for (std::size_t i = 0; i < long_size; ++i) secret[i] = 0;
    }
    volatile unsigned char* p = bytes_;
    for (std::size_t i = 0; i < sizeof(bytes_); ++i) p[i] = 0;
    if (!is_short) delete[] long_ptr;
    std::memset(bytes_, 0, sizeof(bytes_));
  }

  std::size_t find(std::string_view valor, std::size_t pos = 0) const {
    return view().find(valor, pos);
  }
  std::size_t find(char valor, std::size_t pos = 0) const {
    return view().find(valor, pos);
  }
  std::size_t find(const CompactString& valor, std::size_t pos = 0) const {
    return view().find(valor.view(), pos);
  }
  std::size_t rfind(std::string_view valor, std::size_t pos = std::string_view::npos) const {
    return view().rfind(valor, pos);
  }
  int compare(std::string_view valor) const {
    return view().compare(valor);
  }
  int compare(const CompactString& valor) const {
    return view().compare(valor.view());
  }
  int compare(std::size_t pos, std::size_t count, std::string_view valor) const {
    return view().compare(pos, count, valor);
  }
  std::string substr(std::size_t pos, std::size_t count = std::string::npos) const {
    return std::string(view().substr(pos, count));
  }

  friend bool operator==(const CompactString& a, const CompactString& b) { return a.view() == b.view(); }
  friend bool operator!=(const CompactString& a, const CompactString& b) { return !(a == b); }
  friend bool operator<(const CompactString& a, const CompactString& b) { return a.view() < b.view(); }
  friend bool operator>(const CompactString& a, const CompactString& b) { return b < a; }
  friend bool operator<(const CompactString& a, const std::string& b) {
    return a.view() < std::string_view(b);
  }
  friend bool operator<(const std::string& a, const CompactString& b) {
    return std::string_view(a) < b.view();
  }
  friend bool operator>(const CompactString& a, const std::string& b) { return b < a; }
  friend bool operator>(const std::string& a, const CompactString& b) { return b < a; }
  friend bool operator<=(const CompactString& a, const std::string& b) { return !(a > b); }
  friend bool operator<=(const std::string& a, const CompactString& b) { return !(a > b); }
  friend bool operator>=(const CompactString& a, const std::string& b) { return !(a < b); }
  friend bool operator>=(const std::string& a, const CompactString& b) { return !(a < b); }
  friend bool operator==(const CompactString& a, std::string_view b) { return a.view() == b; }
  friend bool operator==(std::string_view a, const CompactString& b) { return a == b.view(); }
  friend bool operator!=(const CompactString& a, std::string_view b) { return !(a == b); }
  friend bool operator!=(std::string_view a, const CompactString& b) { return !(a == b); }
  friend std::string operator+(const char* a, const CompactString& b) {
    return std::string(a ? a : "") + b.str();
  }
  friend std::string operator+(const CompactString& a, const char* b) {
    return a.str() + (b ? b : "");
  }
  friend std::string operator+(const std::string& a, const CompactString& b) {
    return a + b.str();
  }
};

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
  CompactString s;
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
  // Por padrão resolve um plano lazy. Operadores que apenas estendem o plano
  // podem pedir o ponteiro sem disparar a leitura.
  ColumnarTable* columnar(bool carregar = true) const;
  void materialize_rows();
};

std::string to_display(const Value& v);  // human form used by `imprimir`
bool equals(const Value& a, const Value& b);

// Strict (non-lazy, non-tensor) binary operators shared by the tree interpreter
// and the bytecode VM. `op` is one of + - * / % == != < <= > >= contem.
// Sets *ok = false and returns Nulo if `op` is not one of these.
Value apply_binop(const std::string& op, const Value& a, const Value& b, bool* ok);

}  // namespace tilt::rt
