#include <sys/resource.h>

#include <algorithm>
#include <barrier>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "runtime/value.hpp"
using namespace tilt::rt;
using Clock = std::chrono::steady_clock;
int main(int argc, char** argv) {
  if (argc != 6) return 2;
  const int count = std::stoi(argv[1]), size = std::stoi(argv[2]);
  const int threads = std::stoi(argv[3]), repeats = std::stoi(argv[4]);
  const bool handoff = std::string(argv[5]) == "handoff";
  if (count < 1 || size < 0 || threads < 1 || threads > 16 || repeats < 1) return 2;
  std::vector<ValueList> slots(threads);
  std::vector<std::vector<double>> create(threads), destroy(threads);
  std::barrier gate(threads);
  std::vector<std::thread> workers;
  for (int id = 0; id < threads; ++id) {
    create[id].resize(repeats);
    destroy[id].resize(repeats);
    workers.emplace_back([&, id] {
      for (int run = -2; run < repeats; ++run) {
        gate.arrive_and_wait();
        const auto start = Clock::now();
        auto& own = slots[id];
        own.reserve(count);
        for (int i = 0; i < count; ++i) {
          ValueList elements;
          elements.reserve(size);
          for (int j = 0; j < size; ++j) elements.push_back(Value::inteiro(1000000LL * id + i + j));
          own.push_back(Value::lista(std::move(elements)));
        }
        const auto built = Clock::now();
        gate.arrive_and_wait();
        const int owner = handoff ? (id + 1) % threads : id;
        auto& consumed = slots[owner];
        if (consumed.size() != static_cast<std::size_t>(count)) std::abort();
        for (int i = 0; i < count; ++i) {
          const auto& value = consumed[i];
          if (value.kind != ValueKind::Lista || !value.list ||
              value.list->size() != static_cast<std::size_t>(size))
            std::abort();
          for (int j = 0; j < size; ++j)
            if (value.list->at(j).kind != ValueKind::Inteiro ||
                value.list->at(j).i != 1000000LL * owner + i + j)
              std::abort();
        }
        gate.arrive_and_wait();
        const auto destroy_start = Clock::now();
        consumed.clear();
        const auto end = Clock::now();
        if (run >= 0) {
          create[id][run] = std::chrono::duration<double, std::milli>(built - start).count();
          destroy[id][run] = std::chrono::duration<double, std::milli>(end - destroy_start).count();
        }
        gate.arrive_and_wait();
      }
    });
  }
  for (auto& worker : workers) worker.join();
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  for (int run = 0; run < repeats; ++run) {
    double c = 0, d = 0;
    for (int id = 0; id < threads; ++id) {
      c = std::max(c, create[id][run]);
      d = std::max(d, destroy[id][run]);
    }
    std::cout << c << ' ' << d << ' ' << usage.ru_maxrss << '\n';
  }
}
