#include "runtime/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

int main() {
  tilt::rt::ThreadPool pool(4, 32);
  std::atomic<int> completed{0};
  std::mutex mu;
  std::condition_variable done;

  for (int i = 0; i < 16; ++i) {
    if (!pool.submit([&] {
          completed.fetch_add(1, std::memory_order_relaxed);
          done.notify_one();
        })) {
      std::fprintf(stderr, "submit unexpectedly rejected a task\n");
      return 1;
    }
  }

  {
    std::unique_lock<std::mutex> lock(mu);
    if (!done.wait_for(lock, std::chrono::seconds(2),
                       [&] { return completed.load(std::memory_order_relaxed) == 16; })) {
      std::fprintf(stderr, "thread pool did not finish all tasks\n");
      return 1;
    }
  }
  if (pool.pending() != 0) {
    std::fprintf(stderr, "thread pool still has pending tasks\n");
    return 1;
  }

  pool.shutdown();
  if (pool.submit([] {})) {
    std::fprintf(stderr, "submit accepted a task after shutdown\n");
    return 1;
  }
  {
    tilt::rt::ThreadPool bounded(1, 1);
    std::mutex gate_mu;
    std::condition_variable gate;
    bool started = false;
    bool release = false;
    if (!bounded.submit([&] {
          std::unique_lock<std::mutex> lock(gate_mu);
          started = true;
          gate.notify_one();
          gate.wait(lock, [&] { return release; });
        })) {
      std::fprintf(stderr, "bounded pool rejected its first task\n");
      return 1;
    }
    {
      std::unique_lock<std::mutex> lock(gate_mu);
      if (!gate.wait_for(lock, std::chrono::seconds(2), [&] { return started; })) {
        std::fprintf(stderr, "bounded pool worker did not start\n");
        return 1;
      }
    }
    if (!bounded.submit([] {})) {
      std::fprintf(stderr, "bounded pool rejected its pending slot\n");
      return 1;
    }
    if (bounded.submit([] {})) {
      std::fprintf(stderr, "bounded pool exceeded its pending limit\n");
      return 1;
    }
    {
      std::lock_guard<std::mutex> lock(gate_mu);
      release = true;
    }
    gate.notify_one();
    bounded.shutdown();
  }

  std::puts("thread_pool_unit ok");
  return 0;
}
