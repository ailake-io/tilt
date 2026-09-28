#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/value.hpp"

namespace tilt::rt {

// Typed, dictionary-encoded columns for large CSV tables. Rows are built only
// when an operation needs row maps; analytical operators can scan the columns.
struct ColumnarColumn {
  enum class Type { Empty, Integer, Decimal, Boolean, Text, TextPlain, List, Struct, Mixed };
  Type type = Type::Empty;
  std::vector<std::uint8_t> nulls;
  std::vector<std::int64_t> integers;
  std::vector<double> decimals;
  std::vector<std::uint8_t> booleans;
  std::vector<std::uint32_t> codes;
  std::vector<std::string> texts;
  std::vector<std::string> dictionary;
  std::unordered_map<std::string, std::uint32_t> dictionary_index;
  std::vector<std::size_t> offsets;  // listas: um offset por linha + sentinela inicial
  std::unique_ptr<ColumnarColumn> elements;
  std::vector<std::string> field_names;
  std::vector<std::unique_ptr<ColumnarColumn>> fields;
  std::vector<Value> mixed;

  void append(Value value);
  // Concatena uma coluna tipada sem materializar cada célula em Value.
  void append_column(const ColumnarColumn& source);
  void append_integer(std::int64_t value);
  void append_decimal(double value);
  void append_boolean(bool value);
  void append_from(const ColumnarColumn& source, std::size_t row);
  Value at(std::size_t row) const;
  // Seleciona linhas preservando o armazenamento tipado. O caminho antigo
  // chamava at() para cada célula, materializando listas e structs inteiros.
  ColumnarColumn take_rows(const std::vector<std::size_t>& positions) const;
  double number_at(std::size_t row) const;
  double sum_numeric() const;
  double sum_squares_numeric() const;
  double sum_numeric_range(std::size_t begin, std::size_t end) const;
  double sum_squares_numeric_range(std::size_t begin, std::size_t end) const;
  std::string key_at(std::size_t row) const;
  std::size_t memory_bytes() const;
};

struct ColumnarTable {
  using LazyLoader = std::function<std::shared_ptr<ColumnarTable>()>;
  struct JoinIndex {
    struct Bucket {
      std::size_t offset = 0;
      std::size_t count = 0;
    };
    std::unordered_map<std::string, Bucket> buckets;
    std::vector<std::size_t> rows;
    std::unordered_map<std::string, std::vector<std::size_t>> pending;

    void reserve(std::size_t n) {
      buckets.reserve(n);
      rows.reserve(n);
    }
    void add(const std::string& key, std::size_t row) {
      pending[key].push_back(row);
    }
    void finalize() {
      buckets.reserve(pending.size());
      std::size_t total = 0;
      for (const auto& [key, values] : pending) total += values.size();
      rows.reserve(total);
      for (const auto& [key, values] : pending) {
        buckets.emplace(key, Bucket{rows.size(), values.size()});
        rows.insert(rows.end(), values.begin(), values.end());
      }
      pending.clear();
      pending.rehash(0);
    }
    void append(const JoinIndex& other) {
      for (const auto& [key, bucket] : other.buckets)
        for (std::size_t i = 0; i < bucket.count; ++i)
          add(key, other.rows[bucket.offset + i]);
    }
    std::size_t bytes() const {
      std::size_t total = rows.capacity() * sizeof(std::size_t);
      total += buckets.size() * (sizeof(Bucket) + sizeof(void*) * 3);
      for (const auto& [key, _] : buckets) total += key.capacity();
      return total;
    }
  };
  std::vector<std::string> names;
  std::vector<ColumnarColumn> columns;
  std::size_t rows = 0;
  // Ordens ascendentes conhecidas, produzidas por ordenar_por. Cada entrada
  // permite evitar uma nova varredura de verificação em operadores posteriores.
  std::vector<std::vector<std::string>> sorted_by;
  mutable std::mutex row_cache_mutex;
  mutable std::shared_ptr<ValueList> row_cache;
  mutable std::mutex join_cache_mutex;
  mutable std::unordered_map<std::string, std::shared_ptr<JoinIndex>> join_cache;
  mutable std::deque<std::string> join_cache_order;
  mutable std::size_t join_cache_bytes = 0;
  mutable std::size_t join_cache_limit_bytes = 64 * 1024 * 1024;
  mutable std::size_t join_cache_hits = 0;
  mutable std::size_t join_cache_misses = 0;
  mutable std::size_t peak_memory_bytes = 0;
  mutable std::size_t peak_row_group_bytes = 0;
  mutable std::mutex lazy_mutex;
  mutable LazyLoader lazy_loader;

  explicit ColumnarTable(std::vector<std::string> column_names);
  void set_lazy_loader(LazyLoader loader);
  void ensure_loaded() const;
  void append(std::vector<Value>& values);
  ValueList materialize() const;
  std::shared_ptr<ValueList> rows_materialized() const;
  const ColumnarColumn* find(const std::string& name) const;
  std::shared_ptr<ColumnarTable> take_rows(const std::vector<std::size_t>& positions) const;
  void convert_types(const Value& types);
  Value join_metrics() const;
  Value memory_metrics() const;
  void registrar_pico_row_group(std::size_t bytes) const { peak_row_group_bytes = std::max(peak_row_group_bytes, bytes); }
};

}  // namespace tilt::rt
