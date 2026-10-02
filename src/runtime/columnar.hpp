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

  void append_null();
  void append_text(std::string value);
  void append(Value value);
  void append_ref(const Value& value);
  // Concatena uma coluna tipada sem materializar cada célula em Value.
  void append_column(const ColumnarColumn& source);
  void append_integer(std::int64_t value);
  void append_decimal(double value);
  void append_boolean(bool value);
  void append_from(const ColumnarColumn& source, std::size_t row);
  Value at(std::size_t row) const;
  bool null_at(std::size_t row) const;
  // Seleciona linhas preservando o armazenamento tipado. O caminho antigo
  // chamava at() para cada célula, materializando listas e structs inteiros.
  ColumnarColumn take_rows(const std::vector<std::size_t>& positions) const;
  double number_at(std::size_t row) const;
  double sum_numeric() const;
  double sum_squares_numeric() const;
  double sum_numeric_range(std::size_t begin, std::size_t end) const;
  double sum_squares_numeric_range(std::size_t begin, std::size_t end) const;
  // Avalia uma expressão numérica entre duas colunas (ou uma coluna e um
  // escalar) sem materializar células Value. O caminho contínuo usa SIMD
  // quando disponível e preserva inteiros em +, - e *.
  ColumnarColumn binary_numeric(const ColumnarColumn* rhs, double scalar,
                                bool scalar_is_integer, char operation,
                                bool scalar_is_null = false) const;
  std::string key_at(std::size_t row) const;
  std::size_t memory_bytes() const;
};

struct ColumnarTable : std::enable_shared_from_this<ColumnarTable> {
  using LazyLoader = std::function<std::shared_ptr<ColumnarTable>()>;
  // Requisição acumulada enquanto uma fonte ainda não foi lida. A fonte pode
  // usar esses campos para evitar decodificar colunas/linhas que o plano não
  // consumirá. Quando não houver suporte, o carregador pode ignorá-los e o
  // executor nativo preserva a semântica original.
  struct LazyPlan {
    std::vector<std::string> projection;
    std::vector<std::string> predicate_columns;
    Value predicate = Value::nulo();
    std::size_t limit = 0;
    bool limit_set = false;
  };
  using LazyPlanner = std::function<std::shared_ptr<ColumnarTable>(const LazyPlan&)>;
  // Predicado adiado para o plano scan -> filtro -> projecao -> agregacao.
  // O indice recebido e relativo ao parent (que permanece materializavel),
  // permitindo que o agregador aplique o filtro no mesmo passe em que reduz
  // os grupos, sem criar o vetor de linhas selecionadas.
  using RowFilter = std::function<bool(std::size_t)>;
  struct DerivedColumn {
    std::string name;
    std::string left;
    std::string right;
    double scalar = 0.0;
    char operation = '+';
    bool right_is_column = false;
    bool scalar_is_integer = false;
    bool scalar_is_null = false;
  };
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
  mutable LazyPlanner lazy_planner;
  mutable LazyPlan lazy_plan;
  // Visão filtrada: mantém o parent vivo e guarda somente as linhas aceitas.
  // Operadores analíticos podem percorrer a seleção sem copiar as colunas;
  // APIs que exigem buffers próprios chamam materialize_view().
  std::shared_ptr<const ColumnarTable> view_parent;
  std::shared_ptr<std::vector<std::size_t>> view_rows;
  std::shared_ptr<RowFilter> pending_filter;
  std::vector<DerivedColumn> pending_derived;
  // Projeção sem filtro: os índices são identidade e podem usar as reduções
  // do parent sem percorrer um vetor de seleção.
  bool view_identity = false;

  explicit ColumnarTable(std::vector<std::string> column_names);
  void set_lazy_loader(LazyLoader loader);
  void set_lazy_planner(LazyPlanner planner, LazyPlan plan);
  bool is_lazy() const;
  // Retorna falso quando a tabela já foi materializada; nesse caso o chamador
  // deve executar a operação normalmente.
  bool add_lazy_projection(const std::vector<std::string>& names);
  bool add_lazy_predicate(const Value& predicate);
  bool add_lazy_limit(std::size_t limit);
  // Aplica um predicado representado pelo mesmo mapa aceito por `onde:`.
  // Usado como residual quando a fonte não consegue fazer o pushdown.
  std::shared_ptr<ColumnarTable> filter_predicate(const Value& predicate) const;
  std::shared_ptr<ColumnarTable> project_columns(const std::vector<std::string>& names) const;
  void ensure_loaded() const;
  void append(std::vector<Value>& values);
  ValueList materialize() const;
  std::shared_ptr<ValueList> rows_materialized() const;
  const ColumnarColumn* find(const std::string& name) const;
  std::shared_ptr<ColumnarTable> take_rows(const std::vector<std::size_t>& positions) const;
  bool is_view() const { return static_cast<bool>(view_parent); }
  bool has_pending_filter() const { return static_cast<bool>(pending_filter); }
  bool is_identity_view() const { return view_identity; }
  std::size_t physical_row(std::size_t row) const;
  void materialize_view();
  void convert_types(const Value& types);
  Value join_metrics() const;
  Value memory_metrics() const;
  void registrar_pico_row_group(std::size_t bytes) const { peak_row_group_bytes = std::max(peak_row_group_bytes, bytes); }
};

}  // namespace tilt::rt
