#include "runtime/columnar.hpp"

#include <stdexcept>
#include <numeric>
#include <utility>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#include "runtime/tabela_ops.hpp"

namespace tilt::rt {

namespace {

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
__attribute__((target("avx2"))) double soma_avx2(const double* values, std::size_t count) {
  __m256d total = _mm256_setzero_pd();
  std::size_t i = 0;
  for (; i + 4 <= count; i += 4)
    total = _mm256_add_pd(total, _mm256_loadu_pd(values + i));
  alignas(32) double lanes[4];
  _mm256_store_pd(lanes, total);
  double result = lanes[0] + lanes[1] + lanes[2] + lanes[3];
  for (; i < count; ++i) result += values[i];
  return result;
}

__attribute__((target("avx2"))) double soma_quadrados_avx2(const double* values,
                                                            std::size_t count) {
  __m256d total = _mm256_setzero_pd();
  std::size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    const __m256d v = _mm256_loadu_pd(values + i);
    total = _mm256_add_pd(total, _mm256_mul_pd(v, v));
  }
  alignas(32) double lanes[4];
  _mm256_store_pd(lanes, total);
  double result = lanes[0] + lanes[1] + lanes[2] + lanes[3];
  for (; i < count; ++i) result += values[i] * values[i];
  return result;
}

__attribute__((target("avx2"))) void binary_avx2(const double* left, const double* right,
                                                   double scalar, bool has_right, char operation,
                                                   double* out, std::size_t count) {
  const __m256d constant = _mm256_set1_pd(scalar);
  std::size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    const __m256d a = _mm256_loadu_pd(left + i);
    const __m256d b = has_right ? _mm256_loadu_pd(right + i) : constant;
    __m256d value;
    switch (operation) {
      case '+': value = _mm256_add_pd(a, b); break;
      case '-': value = _mm256_sub_pd(a, b); break;
      case '*': value = _mm256_mul_pd(a, b); break;
      default: value = _mm256_div_pd(a, b); break;
    }
    _mm256_storeu_pd(out + i, value);
  }
  for (; i < count; ++i) {
    const double a = left[i];
    const double b = has_right ? right[i] : scalar;
    if (operation == '+') out[i] = a + b;
    else if (operation == '-') out[i] = a - b;
    else if (operation == '*') out[i] = a * b;
    else out[i] = b == 0.0 ? 0.0 : a / b;
  }
}
#endif

ColumnarColumn::Type type_of(const Value& value) {
  switch (value.kind) {
    case ValueKind::Inteiro: return ColumnarColumn::Type::Integer;
    case ValueKind::Decimal: return ColumnarColumn::Type::Decimal;
    case ValueKind::Logico: return ColumnarColumn::Type::Boolean;
    case ValueKind::Texto: return ColumnarColumn::Type::Text;
    case ValueKind::Lista: return ColumnarColumn::Type::List;
    case ValueKind::Mapa: return ColumnarColumn::Type::Struct;
    default: return ColumnarColumn::Type::Mixed;
  }
}

const Value* nested_value(const Value& root, const std::string& path) {
  const Value* current = &root;
  std::size_t begin = 0;
  while (begin <= path.size()) {
    const std::size_t end = path.find('.', begin);
    const std::string part = path.substr(begin, end == std::string::npos ? std::string::npos
                                                                          : end - begin);
    if (!current || current->kind != ValueKind::Mapa || !current->map_ref()) return nullptr;
    current = current->map_ref()->find(part);
    if (end == std::string::npos) return current;
    begin = end + 1;
  }
  return current;
}

bool matches_predicate(const Value& predicate, const Value& row) {
  if (predicate.kind != ValueKind::Mapa || !predicate.map_ref() || predicate.map_ref()->items.empty())
    return true;
  for (const auto& [name, expected] : predicate.map_ref()->items) {
    if ((name == "e" || name == "ou") && expected.kind == ValueKind::Lista && expected.list_ref()) {
      bool result = name == "e";
      for (const Value& part : *expected.list_ref()) {
        const bool current = matches_predicate(part, row);
        result = name == "e" ? result && current : result || current;
      }
      if (!result) return false;
      continue;
    }
    const Value* cell = nested_value(row, name);
    const Value* rhs = &expected;
    std::string op = "==";
    if (expected.kind == ValueKind::Mapa && expected.map_ref() && expected.map_ref()->items.size() == 1) {
      const auto& candidate = expected.map_ref()->items.front();
      if (candidate.first == "==" || candidate.first == "!=" || candidate.first == "<" ||
          candidate.first == "<=" || candidate.first == ">" || candidate.first == ">=") {
        op = candidate.first;
        rhs = &candidate.second;
      }
    }
    bool ok = false;
    if (!apply_binop(op, cell ? *cell : Value::nulo(), *rhs, &ok).truthy()) return false;
  }
  return true;
}

}  // namespace

void ColumnarColumn::append_null() {
    nulls.push_back(1);
    switch (type) {
      case Type::Integer: integers.push_back(0); break;
      case Type::Decimal: decimals.push_back(0.0); break;
      case Type::Boolean: booleans.push_back(0); break;
      case Type::Text: codes.push_back(0); break;
      case Type::TextPlain: texts.emplace_back(); break;
      case Type::List: offsets.push_back(offsets.back()); break;
      case Type::Struct:
        for (auto& field : fields) field->append(Value::nulo());
        break;
      case Type::Mixed: mixed.push_back(Value::nulo()); break;
      case Type::Empty: break;
    }
}

void ColumnarColumn::append_text(std::string value) {
  if (type == Type::Empty) {
    type = Type::Text;
    codes.resize(nulls.size());
  }
  if (type != Type::Text && type != Type::TextPlain) {
    append(Value::texto(std::move(value)));
    return;
  }
  nulls.push_back(0);
  if (type == Type::TextPlain) {
    texts.push_back(std::move(value));
    return;
  }
  auto [it, inserted] = dictionary_index.try_emplace(value,
                                                       static_cast<std::uint32_t>(dictionary.size()));
  if (inserted) dictionary.push_back(std::move(value));
  codes.push_back(it->second);
  if (nulls.size() >= 65536 && dictionary.size() > 16384 &&
      dictionary.size() * 2 > nulls.size()) {
    texts.reserve(nulls.size());
    for (std::size_t row = 0; row < codes.size(); ++row)
      texts.push_back(nulls[row] ? std::string() : dictionary[codes[row]]);
    codes.clear();
    dictionary.clear();
    dictionary_index.clear();
    type = Type::TextPlain;
  }
}

void ColumnarColumn::append(Value value) {
  if (value.kind == ValueKind::Nulo) {
    append_null();
    return;
  }
  const Type incoming = type_of(value);
  if (type == Type::Empty) {
    type = incoming;
    const std::size_t previous = nulls.size();
    if (type == Type::Integer) integers.resize(previous);
    else if (type == Type::Decimal) decimals.resize(previous);
    else if (type == Type::Boolean) booleans.resize(previous);
    else if (type == Type::Text) codes.resize(previous);
    else if (type == Type::List) {
      offsets.assign(previous + 1, 0);
      elements = std::make_unique<ColumnarColumn>();
    }
    else if (type == Type::Struct) {
      if (value.map_ref()) {
        field_names.reserve(value.map_ref()->items.size());
        fields.reserve(value.map_ref()->items.size());
        for (const auto& [name, _] : value.map_ref()->items) {
          field_names.push_back(name);
          fields.push_back(std::make_unique<ColumnarColumn>());
          for (std::size_t i = 0; i < previous; ++i) fields.back()->append(Value::nulo());
        }
      }
    }
    else mixed.resize(previous);
  }
  bool struct_mismatch = false;
  if (type == Type::Struct && incoming == Type::Struct) {
    const std::size_t count = value.map_ref() ? value.map_ref()->items.size() : 0;
    struct_mismatch = count != field_names.size();
    for (std::size_t i = 0; i < count && !struct_mismatch; ++i)
      struct_mismatch = value.map_ref()->items[i].first != field_names[i];
  }
  if (struct_mismatch || (type != incoming && type != Type::Mixed &&
                          !(type == Type::TextPlain && incoming == Type::Text))) {
    std::vector<Value> promoted;
    promoted.reserve(nulls.size() + 1);
    for (std::size_t row = 0; row < nulls.size(); ++row) promoted.push_back(at(row));
    integers.clear();
    decimals.clear();
    booleans.clear();
    codes.clear();
    texts.clear();
    dictionary.clear();
    dictionary_index.clear();
    offsets.clear();
    elements.reset();
    field_names.clear();
    fields.clear();
    mixed = std::move(promoted);
    type = Type::Mixed;
  }
  nulls.push_back(0);
  switch (type) {
    case Type::Integer: integers.push_back(value.i); break;
    case Type::Decimal: decimals.push_back(value.d); break;
    case Type::Boolean: booleans.push_back(value.b ? 1 : 0); break;
    case Type::Text: {
      auto [it, inserted] = dictionary_index.try_emplace(value.s,
                                                           static_cast<std::uint32_t>(dictionary.size()));
      if (inserted) dictionary.push_back(std::move(value.s));
      codes.push_back(it->second);
      // Dicionario so compensa quando ha repeticao suficiente. IDs quase
      // unicos usam texto plano e dispensam o hash e a copia das chaves.
      if (nulls.size() >= 65536 && dictionary.size() > 16384 &&
          dictionary.size() * 2 > nulls.size()) {
        texts.reserve(nulls.size());
        for (std::size_t row = 0; row < codes.size(); ++row)
          texts.push_back(nulls[row] ? std::string() : dictionary[codes[row]]);
        codes.clear();
        dictionary.clear();
        dictionary_index.clear();
        type = Type::TextPlain;
      }
      break;
    }
    case Type::TextPlain: texts.push_back(std::move(value.s)); break;
    case Type::List: {
      if (value.list_ref()) {
        for (const Value& element : *value.list_ref()) elements->append(element);
      }
      offsets.push_back(elements->nulls.size());
      break;
    }
    case Type::Struct:
      for (std::size_t i = 0; i < fields.size(); ++i)
        fields[i]->append(value.map_ref()->items[i].second);
      break;
    case Type::Mixed: mixed.push_back(std::move(value)); break;
    case Type::Empty: break;
  }
}

void ColumnarColumn::append_ref(const Value& value) {
  switch (value.kind) {
    case ValueKind::Nulo: append_null(); break;
    case ValueKind::Inteiro: append_integer(value.i); break;
    case ValueKind::Decimal: append_decimal(value.d); break;
    case ValueKind::Logico: append_boolean(value.b); break;
    case ValueKind::Texto: append_text(value.s); break;
    default: append(value); break;
  }
}

void ColumnarColumn::append_column(const ColumnarColumn& source) {
  if (source.nulls.empty()) return;
  if (type == Type::Empty) {
    type = source.type;
    if (type == Type::List) {
      elements = std::make_unique<ColumnarColumn>();
      offsets.push_back(0);
    } else if (type == Type::Struct) {
      field_names = source.field_names;
      fields.reserve(source.fields.size());
      for (std::size_t i = 0; i < source.fields.size(); ++i)
        fields.push_back(std::make_unique<ColumnarColumn>());
    }
  }
  if (type != source.type || (type == Type::Struct && field_names != source.field_names)) {
    for (std::size_t row = 0; row < source.nulls.size(); ++row)
      append_from(source, row);
    return;
  }
  nulls.insert(nulls.end(), source.nulls.begin(), source.nulls.end());
  switch (type) {
    case Type::Integer:
      integers.insert(integers.end(), source.integers.begin(), source.integers.end());
      break;
    case Type::Decimal:
      decimals.insert(decimals.end(), source.decimals.begin(), source.decimals.end());
      break;
    case Type::Boolean:
      booleans.insert(booleans.end(), source.booleans.begin(), source.booleans.end());
      break;
    case Type::Text:
    case Type::TextPlain:
      for (std::size_t row = 0; row < source.nulls.size(); ++row) {
        if (source.nulls[row]) {
          if (type == Type::Text) codes.push_back(0);
          else texts.emplace_back();
          continue;
        }
        const std::string& value = type == Type::Text
                                       ? source.dictionary[source.codes[row]]
                                       : source.texts[row];
        if (type == Type::Text) {
          auto [it, inserted] =
              dictionary_index.try_emplace(value, static_cast<std::uint32_t>(dictionary.size()));
          if (inserted) dictionary.push_back(value);
          codes.push_back(it->second);
        } else {
          texts.push_back(value);
        }
      }
      break;
    case Type::List: {
      const std::size_t base = offsets.empty() ? 0 : offsets.back();
      for (std::size_t row = 0; row + 1 < source.offsets.size(); ++row)
        offsets.push_back(base + source.offsets[row + 1]);
      if (source.elements) {
        if (!elements) elements = std::make_unique<ColumnarColumn>();
        elements->append_column(*source.elements);
      }
      break;
    }
    case Type::Struct:
      for (std::size_t i = 0; i < source.fields.size(); ++i)
        fields[i]->append_column(*source.fields[i]);
      break;
    case Type::Mixed:
      mixed.insert(mixed.end(), source.mixed.begin(), source.mixed.end());
      break;
    case Type::Empty:
      break;
  }
}

void ColumnarColumn::append_integer(std::int64_t value) {
  if (type == Type::Empty) {
    type = Type::Integer;
    integers.resize(nulls.size());
  }
  if (type != Type::Integer) {
    append(Value::inteiro(value));
    return;
  }
  nulls.push_back(0);
  integers.push_back(value);
}

void ColumnarColumn::append_decimal(double value) {
  if (type == Type::Empty) {
    type = Type::Decimal;
    decimals.resize(nulls.size());
  }
  if (type != Type::Decimal) {
    append(Value::decimal(value));
    return;
  }
  nulls.push_back(0);
  decimals.push_back(value);
}

void ColumnarColumn::append_boolean(bool value) {
  if (type == Type::Empty) {
    type = Type::Boolean;
    booleans.resize(nulls.size());
  }
  if (type != Type::Boolean) {
    append(Value::logico(value));
    return;
  }
  nulls.push_back(0);
  booleans.push_back(value ? 1 : 0);
}

void ColumnarColumn::append_from(const ColumnarColumn& source, std::size_t row) {
  if (row >= source.nulls.size() || source.nulls[row]) {
    append(Value::nulo());
    return;
  }
  if (source.type == Type::Text || source.type == Type::TextPlain ||
      source.type == Type::List || source.type == Type::Struct || source.type == Type::Mixed) {
    append(source.at(row));
    return;
  }
  if (type == Type::Empty) type = source.type;
  if (type != source.type) {
    append(source.at(row));
    return;
  }
  nulls.push_back(0);
  switch (source.type) {
    case Type::Integer: integers.push_back(source.integers[row]); break;
    case Type::Decimal: decimals.push_back(source.decimals[row]); break;
    case Type::Boolean: booleans.push_back(source.booleans[row]); break;
    case Type::Text:
    case Type::TextPlain:
    case Type::List:
    case Type::Struct:
    case Type::Mixed:
      nulls.pop_back();
      append(source.at(row));
      break;
    case Type::Empty:
      nulls.pop_back();
      append(Value::nulo());
      break;
  }
}

Value ColumnarColumn::at(std::size_t row) const {
  if (row >= nulls.size() || nulls[row]) return Value::nulo();
  switch (type) {
    case Type::Integer: return Value::inteiro(integers[row]);
    case Type::Decimal: return Value::decimal(decimals[row]);
    case Type::Boolean: return Value::logico(booleans[row] != 0);
    case Type::Text: return Value::texto(dictionary[codes[row]]);
    case Type::TextPlain: return Value::texto(texts[row]);
    case Type::List: {
      Value result = Value::lista();
      result.list_ref()->reserve(offsets[row + 1] - offsets[row]);
      for (std::size_t i = offsets[row]; i < offsets[row + 1]; ++i)
        result.list_ref()->push_back(elements->at(i));
      return result;
    }
    case Type::Struct: {
      Value result = Value::mapa();
      result.map_ref()->items.reserve(fields.size());
      for (std::size_t i = 0; i < fields.size(); ++i)
        result.map_ref()->items.emplace_back(field_names[i], fields[i]->at(row));
      return result;
    }
    case Type::Mixed: return mixed[row];
    case Type::Empty: return Value::nulo();
  }
  return Value::nulo();
}

bool ColumnarColumn::null_at(std::size_t row) const {
  return row >= nulls.size() || nulls[row] != 0;
}

ColumnarColumn ColumnarColumn::take_rows(
    const std::vector<std::size_t>& positions) const {
  ColumnarColumn out;
  out.type = type;
  out.nulls.reserve(positions.size());
  switch (type) {
    case Type::Integer:
      out.integers.reserve(positions.size());
      for (std::size_t row : positions) {
        const bool nulo = row >= nulls.size() || nulls[row];
        out.nulls.push_back(nulo ? 1 : 0);
        out.integers.push_back(nulo ? 0 : integers[row]);
      }
      break;
    case Type::Decimal:
      out.decimals.reserve(positions.size());
      for (std::size_t row : positions) {
        const bool nulo = row >= nulls.size() || nulls[row];
        out.nulls.push_back(nulo ? 1 : 0);
        out.decimals.push_back(nulo ? 0.0 : decimals[row]);
      }
      break;
    case Type::Boolean:
      out.booleans.reserve(positions.size());
      for (std::size_t row : positions) {
        const bool nulo = row >= nulls.size() || nulls[row];
        out.nulls.push_back(nulo ? 1 : 0);
        out.booleans.push_back(nulo ? 0 : booleans[row]);
      }
      break;
    case Type::Text:
      out.dictionary = dictionary;
      out.dictionary_index = dictionary_index;
      out.codes.reserve(positions.size());
      for (std::size_t row : positions) {
        const bool nulo = row >= nulls.size() || nulls[row];
        out.nulls.push_back(nulo ? 1 : 0);
        out.codes.push_back(nulo ? 0 : codes[row]);
      }
      break;
    case Type::TextPlain:
      out.texts.reserve(positions.size());
      for (std::size_t row : positions) {
        const bool nulo = row >= nulls.size() || nulls[row];
        out.nulls.push_back(nulo ? 1 : 0);
        out.texts.push_back(nulo ? std::string() : texts[row]);
      }
      break;
    case Type::Struct:
      out.field_names = field_names;
      out.fields.reserve(fields.size());
      for (const auto& field : fields) out.fields.push_back(
          std::make_unique<ColumnarColumn>(field->take_rows(positions)));
      for (std::size_t row : positions)
        out.nulls.push_back(row >= nulls.size() || nulls[row] ? 1 : 0);
      break;
    case Type::List:
      {
      // Reindexa o vetor de elementos de uma só vez. Isso evita chamar
      // elements->at() (e criar Value/lista/mapa temporários) para cada item,
      // inclusive quando a lista contém structs ou outra lista.
      std::vector<std::size_t> element_positions;
      std::vector<std::size_t> lengths;
      element_positions.reserve(positions.size());
      lengths.reserve(positions.size());
      for (std::size_t row : positions) {
        const bool nulo = row >= nulls.size() || nulls[row];
        out.nulls.push_back(nulo ? 1 : 0);
        const std::size_t begin = nulo ? 0 : offsets[row];
        const std::size_t end = nulo ? begin : offsets[row + 1];
        lengths.push_back(end - begin);
        for (std::size_t i = begin; i < end; ++i) element_positions.push_back(i);
      }
      if (out.offsets.empty()) out.offsets.push_back(0);
      for (std::size_t length : lengths) out.offsets.push_back(out.offsets.back() + length);
      if (elements) {
        out.elements = std::make_unique<ColumnarColumn>(elements->take_rows(element_positions));
      } else {
        out.elements = std::make_unique<ColumnarColumn>();
      }
      }
      break;
    case Type::Mixed:
      out.mixed.reserve(positions.size());
      for (std::size_t row : positions) {
        const bool nulo = row >= nulls.size() || nulls[row];
        out.nulls.push_back(nulo ? 1 : 0);
        out.mixed.push_back(nulo ? Value::nulo() : mixed[row]);
      }
      break;
    case Type::Empty:
      out.nulls.assign(positions.size(), 1);
      break;
  }
  return out;
}

double ColumnarColumn::number_at(std::size_t row) const {
  if (row >= nulls.size() || nulls[row]) return 0.0;
  switch (type) {
    case Type::Integer: return static_cast<double>(integers[row]);
    case Type::Decimal: return decimals[row];
    case Type::Boolean: return booleans[row] ? 1.0 : 0.0;
    case Type::Mixed: return mixed[row].as_number();
    default: return 0.0;
  }
}

ColumnarColumn ColumnarColumn::binary_numeric(const ColumnarColumn* rhs, double scalar,
                                              bool scalar_is_integer, char operation,
                                              bool scalar_is_null) const {
  ColumnarColumn out;
  const std::size_t count = rhs ? std::min(nulls.size(), rhs->nulls.size()) : nulls.size();
  if (scalar_is_null) {
    out.type = Type::Decimal;
    out.nulls.assign(count, 1);
    out.decimals.assign(count, 0.0);
    return out;
  }
  const bool left_integer = type == Type::Integer;
  const bool right_integer = rhs ? rhs->type == Type::Integer : scalar_is_integer;
  const bool integer_result = operation != '/' && left_integer && right_integer;
  out.type = integer_result ? Type::Integer : Type::Decimal;
  out.nulls.resize(count, 0);
  if (integer_result) out.integers.resize(count, 0);
  else out.decimals.resize(count, 0.0);

  bool dense = true;
  for (std::size_t row = 0; row < count; ++row) {
    if (nulls[row] || (rhs && rhs->nulls[row])) {
      out.nulls[row] = 1;
      dense = false;
    }
  }

  // The common CSV path is Decimal/Decimal (or Decimal/scalar). Keep the
  // loop over raw arrays and let AVX2 evaluate four values per iteration.
  const bool left_decimal = type == Type::Decimal;
  const bool right_decimal = rhs && rhs->type == Type::Decimal;
  const bool scalar_decimal = !rhs && !scalar_is_integer;
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
  if (!integer_result && operation != '/' && dense && left_decimal &&
      (right_decimal || scalar_decimal) &&
      count >= 64 && __builtin_cpu_supports("avx2")) {
    binary_avx2(decimals.data(), right_decimal ? rhs->decimals.data() : nullptr,
                scalar, right_decimal, operation, out.decimals.data(), count);
    return out;
  }
#endif

  for (std::size_t row = 0; row < count; ++row) {
    if (out.nulls[row]) continue;
    const double left = number_at(row);
    const double right = rhs ? rhs->number_at(row) : scalar;
    double value = 0.0;
    if (operation == '+') value = left + right;
    else if (operation == '-') value = left - right;
    else if (operation == '*') value = left * right;
    else value = right == 0.0 ? 0.0 : left / right;
    if (integer_result) out.integers[row] = static_cast<std::int64_t>(value);
    else out.decimals[row] = value;
  }
  return out;
}

double ColumnarColumn::sum_numeric() const {
  if (type == Type::Decimal) {
    bool sem_nulos = true;
    for (std::size_t row = 0; row < decimals.size(); ++row)
      if (row >= nulls.size() || nulls[row]) { sem_nulos = false; break; }
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    if (sem_nulos && __builtin_cpu_supports("avx2")) return soma_avx2(decimals.data(), decimals.size());
#endif
  }
  double total = 0.0;
  const std::size_t count = type == Type::Integer ? integers.size()
                             : type == Type::Decimal ? decimals.size()
                             : type == Type::Mixed ? mixed.size() : booleans.size();
  for (std::size_t row = 0; row < count; ++row) total += number_at(row);
  return total;
}

double ColumnarColumn::sum_squares_numeric() const {
  if (type == Type::Decimal) {
    bool sem_nulos = true;
    for (std::size_t row = 0; row < decimals.size(); ++row)
      if (row >= nulls.size() || nulls[row]) { sem_nulos = false; break; }
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    if (sem_nulos && __builtin_cpu_supports("avx2"))
      return soma_quadrados_avx2(decimals.data(), decimals.size());
#endif
  }
  double total = 0.0;
  const std::size_t count = type == Type::Integer ? integers.size()
                             : type == Type::Decimal ? decimals.size()
                             : type == Type::Mixed ? mixed.size() : booleans.size();
  for (std::size_t row = 0; row < count; ++row) {
    const double value = number_at(row);
    total += value * value;
  }
  return total;
}

double ColumnarColumn::sum_numeric_range(std::size_t begin, std::size_t end) const {
  end = std::min(end, nulls.size());
  begin = std::min(begin, end);
  if (type == Type::Decimal) {
    bool sem_nulos = true;
    for (std::size_t row = begin; row < end; ++row)
      if (nulls[row]) { sem_nulos = false; break; }
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    if (sem_nulos && __builtin_cpu_supports("avx2"))
      return soma_avx2(decimals.data() + begin, end - begin);
#endif
  }
  double total = 0.0;
  for (std::size_t row = begin; row < end; ++row) total += number_at(row);
  return total;
}

double ColumnarColumn::sum_squares_numeric_range(std::size_t begin, std::size_t end) const {
  end = std::min(end, nulls.size());
  begin = std::min(begin, end);
  if (type == Type::Decimal) {
    bool sem_nulos = true;
    for (std::size_t row = begin; row < end; ++row)
      if (nulls[row]) { sem_nulos = false; break; }
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    if (sem_nulos && __builtin_cpu_supports("avx2"))
      return soma_quadrados_avx2(decimals.data() + begin, end - begin);
#endif
  }
  double total = 0.0;
  for (std::size_t row = begin; row < end; ++row) {
    const double value = number_at(row);
    total += value * value;
  }
  return total;
}

std::string ColumnarColumn::key_at(std::size_t row) const {
  if (row >= nulls.size() || nulls[row]) return "";
  if (type == Type::Text) return dictionary[codes[row]];
  if (type == Type::TextPlain) return texts[row];
  return to_display(at(row));
}

std::size_t ColumnarColumn::memory_bytes() const {
  std::size_t total = nulls.capacity() * sizeof(std::uint8_t);
  total += integers.capacity() * sizeof(std::int64_t);
  total += decimals.capacity() * sizeof(double);
  total += booleans.capacity() * sizeof(std::uint8_t);
  total += codes.capacity() * sizeof(std::uint32_t);
  total += offsets.capacity() * sizeof(std::size_t);
  total += mixed.capacity() * sizeof(Value);
  total += dictionary_index.size() * (sizeof(void*) * 4 + sizeof(std::uint32_t));
  for (const auto& text : texts) total += text.capacity();
  for (const auto& text : dictionary) total += text.capacity();
  if (elements) total += elements->memory_bytes();
  for (const auto& field : fields)
    if (field) total += field->memory_bytes();
  for (const auto& name : field_names) total += name.capacity();
  return total;
}

ColumnarTable::ColumnarTable(std::vector<std::string> column_names)
    : names(std::move(column_names)), columns(names.size()) {}

void ColumnarTable::set_lazy_loader(LazyLoader loader) {
  std::lock_guard<std::mutex> lock(lazy_mutex);
  lazy_loader = std::move(loader);
  lazy_planner = {};
  lazy_plan = {};
}

void ColumnarTable::set_lazy_planner(LazyPlanner planner, LazyPlan plan) {
  std::lock_guard<std::mutex> lock(lazy_mutex);
  lazy_planner = std::move(planner);
  lazy_plan = std::move(plan);
  lazy_loader = {};
}

bool ColumnarTable::is_lazy() const {
  std::lock_guard<std::mutex> lock(lazy_mutex);
  return static_cast<bool>(lazy_loader) || static_cast<bool>(lazy_planner);
}

bool ColumnarTable::add_lazy_projection(const std::vector<std::string>& requested) {
  if (requested.empty()) return true;
  std::lock_guard<std::mutex> lock(lazy_mutex);
  if (!lazy_planner) return false;
  if (lazy_plan.projection.empty()) {
    lazy_plan.projection = requested;
    return true;
  }
  std::vector<std::string> merged;
  merged.reserve(requested.size());
  for (const std::string& name : requested) {
    if (std::find(lazy_plan.projection.begin(), lazy_plan.projection.end(), name) !=
        lazy_plan.projection.end())
      merged.push_back(name);
  }
  lazy_plan.projection = std::move(merged);
  return true;
}

bool ColumnarTable::add_lazy_predicate(const Value& predicate) {
  if (predicate.kind == ValueKind::Nulo) return true;
  std::lock_guard<std::mutex> lock(lazy_mutex);
  if (!lazy_planner) return false;
  if (lazy_plan.predicate.kind == ValueKind::Nulo) {
    lazy_plan.predicate = predicate;
  } else {
    // Preserva a semântica de filtros encadeados: ambos precisam ser verdadeiros.
    Value combinado = Value::mapa();
    Value partes = Value::lista();
    partes.list_ref()->push_back(lazy_plan.predicate);
    partes.list_ref()->push_back(predicate);
    combinado.map_ref()->set("e", std::move(partes));
    lazy_plan.predicate = std::move(combinado);
  }
  std::function<void(const Value&)> coletar = [&](const Value& item) {
    if (item.kind != ValueKind::Mapa || !item.map_ref()) return;
    for (const auto& [name, value] : item.map_ref()->items) {
      if ((name == "e" || name == "ou") && value.kind == ValueKind::Lista && value.list_ref()) {
        for (const Value& part : *value.list_ref()) coletar(part);
      } else if (std::find(lazy_plan.predicate_columns.begin(), lazy_plan.predicate_columns.end(), name) ==
                 lazy_plan.predicate_columns.end()) {
        lazy_plan.predicate_columns.push_back(name);
      }
    }
  };
  coletar(predicate);
  return true;
}

bool ColumnarTable::add_lazy_limit(std::size_t limit) {
  std::lock_guard<std::mutex> lock(lazy_mutex);
  if (!lazy_planner) return false;
  if (!lazy_plan.limit_set || lazy_plan.limit == 0) lazy_plan.limit = limit;
  else lazy_plan.limit = std::min(lazy_plan.limit, limit);
  lazy_plan.limit_set = true;
  return true;
}

void ColumnarTable::ensure_loaded() const {
  std::lock_guard<std::mutex> lock(lazy_mutex);
  if (!lazy_loader && !lazy_planner) return;
  LazyLoader loader = std::move(lazy_loader);
  LazyPlanner planner = std::move(lazy_planner);
  const LazyPlan plan = lazy_plan;
  std::shared_ptr<ColumnarTable> loaded;
  try {
    loaded = planner ? planner(plan) : loader();
  } catch (...) {
    lazy_loader = std::move(loader);
    lazy_planner = std::move(planner);
    throw;
  }
  if (!loaded) throw std::runtime_error("plano colunar: carregador devolveu tabela nula");
  auto* self = const_cast<ColumnarTable*>(this);
  self->names = std::move(loaded->names);
  self->columns = std::move(loaded->columns);
  self->rows = loaded->rows;
  self->sorted_by = std::move(loaded->sorted_by);
  self->peak_memory_bytes = loaded->peak_memory_bytes;
  self->peak_row_group_bytes = loaded->peak_row_group_bytes;
  self->view_parent = std::move(loaded->view_parent);
  self->view_rows = std::move(loaded->view_rows);
  self->view_identity = loaded->view_identity;
  self->pending_filter = std::move(loaded->pending_filter);
  self->pending_derived = std::move(loaded->pending_derived);
  self->lazy_loader = {};
  self->lazy_planner = {};
  self->lazy_plan = {};
}

std::shared_ptr<ColumnarTable> ColumnarTable::filter_predicate(const Value& predicate) const {
  ensure_loaded();
  std::vector<std::size_t> positions;
  positions.reserve(rows / 2 + 1);
  for (std::size_t row = 0; row < rows; ++row) {
    Value record = Value::mapa();
    record.map_ref()->items.reserve(names.size());
    for (const std::string& name : names) {
      const ColumnarColumn* column = find(name);
      record.map_ref()->items.emplace_back(name, column ? column->at(physical_row(row)) : Value::nulo());
    }
    if (matches_predicate(predicate, record)) positions.push_back(row);
  }
  return take_rows(positions);
}

std::shared_ptr<ColumnarTable> ColumnarTable::project_columns(
    const std::vector<std::string>& requested) const {
  ensure_loaded();
  auto result = std::make_shared<ColumnarTable>(requested);
  result->rows = rows;
  try {
    result->view_parent = shared_from_this();
    result->view_rows = std::make_shared<std::vector<std::size_t>>(rows);
    std::iota(result->view_rows->begin(), result->view_rows->end(), std::size_t{0});
    result->view_identity = true;
    for (const std::string& name : requested)
      if (!find(name)) throw std::runtime_error("projecao colunar: coluna inexistente '" + name + "'");
    return result;
  } catch (const std::bad_weak_ptr&) {
    for (std::size_t i = 0; i < requested.size(); ++i) {
      const ColumnarColumn* source = find(requested[i]);
      if (source) result->columns[i].append_column(*source);
    }
    return result;
  }
}

void columnar_ensure_loaded(ColumnarTable* table) {
  if (table) table->ensure_loaded();
}

void ColumnarTable::append(std::vector<Value>& values) {
  if (values.size() != columns.size()) throw std::runtime_error("largura da tabela colunar invalida");
  for (std::size_t i = 0; i < columns.size(); ++i) columns[i].append(std::move(values[i]));
  ++rows;
  sorted_by.clear();
  std::lock_guard<std::mutex> lock(join_cache_mutex);
  join_cache.clear();
  join_cache_order.clear();
  join_cache_bytes = 0;
}

ValueList ColumnarTable::materialize() const {
  // Consumidores de linhas (impressao, interoperabilidade e APIs legadas)
  // precisam resolver um filtro adiado antes de seguir a cadeia de views.
  if (pending_filter) const_cast<ColumnarTable*>(this)->materialize_view();
  ValueList out;
  out.reserve(rows);
  const auto get_value = [&](const std::string& name, std::size_t row) {
    const ColumnarTable* base = this;
    std::size_t physical = row;
    while (base->view_parent) {
      physical = base->view_parent->physical_row((*base->view_rows)[physical]);
      base = base->view_parent.get();
    }
    const ColumnarColumn* column = base->find(name);
    return column ? column->at(physical) : Value::nulo();
  };
  for (std::size_t row = 0; row < rows; ++row) {
    // Batch materialization allocates and releases all row maps together.
    // The global synchronized object pool regresses this path (see the
    // value-pool lifecycle benchmark); keep normal shared ownership here.
    Value record;
    record.kind = ValueKind::Mapa;
    record.map_ref() = std::make_shared<ValueMap>();
    record.map_ref()->items.reserve(names.size());
    for (std::size_t col = 0; col < names.size(); ++col)
      // As colunas têm nomes únicos por contrato; emplace evita a busca
      // linear de mapa feita por set() para cada célula materializada.
      record.map_ref()->items.emplace_back(names[col], get_value(names[col], row));
    out.push_back(std::move(record));
  }
  return out;
}

std::shared_ptr<ValueList> ColumnarTable::rows_materialized() const {
  std::lock_guard<std::mutex> lock(row_cache_mutex);
  if (!row_cache) row_cache = std::make_shared<ValueList>(materialize());
  return row_cache;
}

const ColumnarColumn* ColumnarTable::find(const std::string& name) const {
  if (view_parent) return view_parent->find(name);
  for (std::size_t i = names.size(); i-- > 0;)
    if (names[i] == name) return &columns[i];
  return nullptr;
}

std::size_t ColumnarTable::physical_row(std::size_t row) const {
  if (!view_parent) return row;
  if (view_identity) return row;
  if (!view_rows || row >= view_rows->size())
    throw std::out_of_range("indice fora da visao colunar");
  return view_parent->physical_row((*view_rows)[row]);
}

void ColumnarTable::materialize_view() {
  // Um filtro adiado só materializa quando uma operação realmente precisa
  // das linhas ou quando o consumidor não pode executar o plano colunar.
  // Mantemos o parent vivo e copiamos apenas as posições aceitas.
  if (pending_filter) {
    if (!view_parent) {
      throw std::runtime_error("filtro colunar sem tabela de origem");
    }
    std::vector<std::size_t> selected;
    selected.reserve(rows / 2 + 1);
    const RowFilter filter = *pending_filter;
    for (std::size_t row = 0; row < view_parent->rows; ++row)
      if (filter(row)) selected.push_back(row);
    std::shared_ptr<const ColumnarTable> parent = std::move(view_parent);
    std::vector<ColumnarColumn> materialized;
    materialized.reserve(names.size());
    for (const std::string& name : names) {
      const ColumnarColumn* source = parent->find(name);
      if (source) {
        materialized.push_back(source->take_rows(selected));
        continue;
      }
      const DerivedColumn* derived = nullptr;
      for (const DerivedColumn& candidate : pending_derived) {
        if (candidate.name == name) {
          derived = &candidate;
          break;
        }
      }
      if (!derived) {
        materialized.push_back(ColumnarColumn{});
        continue;
      }
      const ColumnarColumn* left = parent->find(derived->left);
      if (!left) {
        materialized.push_back(ColumnarColumn{});
        continue;
      }
      ColumnarColumn left_selected = left->take_rows(selected);
      if (derived->right_is_column) {
        const ColumnarColumn* right = parent->find(derived->right);
        if (!right) {
          materialized.push_back(ColumnarColumn{});
          continue;
        }
        ColumnarColumn right_selected = right->take_rows(selected);
        materialized.push_back(left_selected.binary_numeric(
            &right_selected, derived->scalar, derived->scalar_is_integer,
            derived->operation, derived->scalar_is_null));
      } else {
        materialized.push_back(left_selected.binary_numeric(
            nullptr, derived->scalar, derived->scalar_is_integer,
            derived->operation, derived->scalar_is_null));
      }
    }
    columns = std::move(materialized);
    rows = selected.size();
    view_rows.reset();
    view_identity = false;
    pending_filter.reset();
    pending_derived.clear();
    sorted_by = parent->sorted_by;
    return;
  }
  if (!view_parent) return;
  std::shared_ptr<const ColumnarTable> parent = std::move(view_parent);
  std::shared_ptr<std::vector<std::size_t>> selected = std::move(view_rows);
  if (!selected) throw std::runtime_error("visao colunar sem selecao");
  std::vector<std::size_t> physical;
  physical.reserve(selected->size());
  for (std::size_t row : *selected) physical.push_back(parent->physical_row(row));
  std::vector<ColumnarColumn> materialized;
  materialized.reserve(names.size());
  for (const std::string& name : names) {
    const ColumnarColumn* source = parent->find(name);
    materialized.push_back(source ? source->take_rows(physical) : ColumnarColumn{});
  }
  columns = std::move(materialized);
  rows = physical.size();
  view_identity = false;
}

std::shared_ptr<ColumnarTable> ColumnarTable::take_rows(
    const std::vector<std::size_t>& positions) const {
  // Filtros que aceitam todas as linhas não precisam criar uma nova visão.
  // Preserva a identidade compartilhada e elimina um objeto intermediário no
  // plano scan -> filtro -> agregação.
  if (positions.size() == rows) {
    bool identidade = true;
    for (std::size_t row = 0; row < positions.size(); ++row) {
      if (positions[row] != row) {
        identidade = false;
        break;
      }
    }
    if (identidade) {
      try {
        return std::const_pointer_cast<ColumnarTable>(shared_from_this());
      } catch (const std::bad_weak_ptr&) {
        // Instâncias de stack continuam no caminho de cópia abaixo.
      }
    }
  }
  if (view_parent || rows > 0) {
    try {
      auto result = std::make_shared<ColumnarTable>(names);
      result->rows = positions.size();
      result->sorted_by = sorted_by;
      result->view_parent = view_parent ? view_parent : shared_from_this();
      result->view_rows = std::make_shared<std::vector<std::size_t>>();
      result->view_identity = false;
      result->view_rows->reserve(positions.size());
      for (std::size_t row : positions)
        result->view_rows->push_back(view_parent ? physical_row(row) : row);
      return result;
    } catch (const std::bad_weak_ptr&) {
      // Instancias criadas na pilha (por código embutido/testes antigos) ainda
      // usam o caminho de cópia seguro.
    }
  }
  auto result = std::make_shared<ColumnarTable>(names);
  result->rows = positions.size();
  result->sorted_by = sorted_by;
  for (std::size_t col = 0; col < columns.size(); ++col)
    result->columns[col] = columns[col].take_rows(positions);
  return result;
}

void ColumnarTable::convert_types(const Value& types) {
  std::lock_guard<std::mutex> lock(row_cache_mutex);
  row_cache.reset();
  sorted_by.clear();
  if (types.kind != ValueKind::Mapa || !types.map_ref())
    throw std::runtime_error("converter espera um mapa de tipos por coluna");
  for (const auto& [name, type] : types.map_ref()->items) {
    if (type.kind != ValueKind::Texto)
      throw std::runtime_error("converter: o tipo de '" + name + "' deve ser texto");
    std::size_t index = names.size();
    for (std::size_t i = names.size(); i-- > 0;)
      if (names[i] == name) { index = i; break; }
    if (index == names.size())
      throw std::runtime_error("converter: a coluna '" + name + "' nao existe");
    const ConversorTabela convert = tabela_conversor(type.s);
    ColumnarColumn converted;
    for (std::size_t row = 0; row < rows; ++row)
      converted.append(convert(columns[index].at(row)));
    columns[index] = std::move(converted);
  }
}

Value ColumnarTable::join_metrics() const {
  std::lock_guard<std::mutex> lock(join_cache_mutex);
  std::size_t bytes = 0;
  std::size_t buckets = 0;
  std::size_t positions = 0;
  for (const auto& [_, index] : join_cache) {
    if (!index) continue;
    bytes += index->bytes();
    buckets += index->buckets.size();
    positions += index->rows.size();
  }
  Value out = Value::mapa();
  out.map_ref()->set("indices", Value::inteiro(static_cast<std::int64_t>(join_cache.size())));
  out.map_ref()->set("buckets", Value::inteiro(static_cast<std::int64_t>(buckets)));
  out.map_ref()->set("posicoes", Value::inteiro(static_cast<std::int64_t>(positions)));
  out.map_ref()->set("bytes", Value::inteiro(static_cast<std::int64_t>(bytes)));
  out.map_ref()->set("limite_bytes", Value::inteiro(static_cast<std::int64_t>(join_cache_limit_bytes)));
  out.map_ref()->set("hits", Value::inteiro(static_cast<std::int64_t>(join_cache_hits)));
  out.map_ref()->set("misses", Value::inteiro(static_cast<std::int64_t>(join_cache_misses)));
  return out;
}

Value ColumnarTable::memory_metrics() const {
  std::size_t current = 0;
  if (view_parent) {
    const Value parent_metrics = view_parent->memory_metrics();
    if (parent_metrics.map_ref()) {
      if (const Value* bytes = parent_metrics.map_ref()->find("bytes");
          bytes && bytes->kind == ValueKind::Inteiro)
        current = static_cast<std::size_t>(std::max<std::int64_t>(0, bytes->i));
    }
    if (view_rows) current += view_rows->capacity() * sizeof(std::size_t);
  } else {
    for (const ColumnarColumn& column : columns) current += column.memory_bytes();
  }
  std::size_t cache = 0;
  {
    std::lock_guard<std::mutex> lock(join_cache_mutex);
    cache = join_cache_bytes;
  }
  current += cache;
  peak_memory_bytes = std::max(peak_memory_bytes, current);
  Value out = Value::mapa();
  out.map_ref()->set("bytes", Value::inteiro(static_cast<std::int64_t>(current)));
  out.map_ref()->set("pico_bytes", Value::inteiro(static_cast<std::int64_t>(peak_memory_bytes)));
  out.map_ref()->set("pico_row_group_bytes",
               Value::inteiro(static_cast<std::int64_t>(peak_row_group_bytes)));
  out.map_ref()->set("linhas", Value::inteiro(static_cast<std::int64_t>(rows)));
  out.map_ref()->set("colunas", Value::inteiro(static_cast<std::int64_t>(columns.size())));
  out.map_ref()->set("cache_join_bytes", Value::inteiro(static_cast<std::int64_t>(cache)));
  return out;
}

}  // namespace tilt::rt
