#include <sys/resource.h>

#include <barrier>
#include <chrono>
#include <exception>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "runtime/columnar.hpp"
#include "runtime/tabela_ops.hpp"
using namespace tilt::rt;
int worker(int argc, char** argv, std::barrier<>& gate, std::ostream& output) {
  try {
    if (argc != 4) throw std::runtime_error("usage: benchmark CASE ROWS REPEATS");
    const std::string mode = argv[1];
    const int n = std::stoi(argv[2]), repeats = std::stoi(argv[3]);
    if (n < 1 || repeats < 1) throw std::runtime_error("positive counts required");
    if (mode != "materialize" && mode != "join" && mode != "lists")
      throw std::runtime_error("unknown case");
    ColumnarTable columns({"id", "value"});
    Value left, right;
    if (mode != "lists") {
      for (int i = 0; i < n; ++i) {
        std::vector<Value> row{Value::inteiro(i), Value::inteiro(i * 2LL)};
        columns.append(row);
      }
      if (mode == "join") {
        left = Value::tabela(columns.materialize());
        right = Value::tabela(columns.materialize());
      }
    }
    for (int run = -2; run < repeats; ++run) {
      gate.arrive_and_wait();
      const auto start = std::chrono::steady_clock::now();
      Value result;
      if (mode == "materialize")
        result = Value::tabela(columns.materialize());
      else if (mode == "join")
        result = tabela_juntar(left, right, Value::texto("id"), "interna");
      else {
        ValueList lists;
        lists.reserve(n);
        for (int i = 0; i < n; ++i)
          lists.push_back(Value::lista({Value::inteiro(i), Value::inteiro(i * 2LL)}));
        result = Value::lista(std::move(lists));
      }
      const auto finish = std::chrono::steady_clock::now();
      if (!result.list || result.list->size() != static_cast<std::size_t>(n))
        throw std::runtime_error("incorrect output size");
      long long sum = 0;
      for (int i = 0; i < n; ++i) {
        const auto& row = result.list->at(i);
        if (mode == "lists") {
          if (!row.list || row.list->size() != 2 || row.list->at(0).i != i ||
              row.list->at(1).i != 2LL * i)
            throw std::runtime_error("incorrect list");
          sum += row.list->at(1).i;
        } else {
          if (!row.map) throw std::runtime_error("missing map");
          const auto* id = row.map->find("id");
          const auto* value = row.map->find("value");
          if (!id || !value || id->i != i || value->i != 2LL * i)
            throw std::runtime_error("incorrect row");
          if (mode == "join") {
            const auto* other = row.map->find("value_direita");
            if (!other || other->i != value->i) throw std::runtime_error("incorrect join");
          }
          sum += value->i;
        }
      }
      if (sum != 1LL * n * (n - 1)) throw std::runtime_error("incorrect checksum");
      const auto destroy_start = std::chrono::steady_clock::now();
      result = Value{};
      const auto destroy_end = std::chrono::steady_clock::now();
      rusage usage{};
      getrusage(RUSAGE_SELF, &usage);
      if (run >= 0)
        output << std::chrono::duration<double, std::milli>(finish - start).count() << ' '
               << std::chrono::duration<double, std::milli>(destroy_end - destroy_start).count()
               << ' ' << usage.ru_maxrss << ' ' << sum << '\n';
    }
    return 0;
  } catch (const std::exception& e) {
    gate.arrive_and_drop();
    std::cerr << e.what() << '\n';
    return 1;
  }
}

int main(int argc, char** argv) {
  if (argc != 5) {
    std::cerr << "usage: benchmark CASE ROWS REPEATS THREADS\n";
    return 1;
  }
  int threads;
  try {
    threads = std::stoi(argv[4]);
  } catch (...) {
    return 1;
  }
  if (threads < 1 || threads > 64) return 1;
  std::barrier gate(threads);
  std::vector<std::thread> workers;
  std::vector<std::ostringstream> outputs(threads);
  std::vector<int> status(threads);
  for (int i = 0; i < threads; ++i)
    workers.emplace_back([&, i] { status[i] = worker(4, argv, gate, outputs[i]); });
  for (auto& thread : workers) thread.join();
  for (int result : status)
    if (result) return result;
  for (const auto& output : outputs) std::cout << output.str();
}
