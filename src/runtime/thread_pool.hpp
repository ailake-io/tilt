#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace tilt::rt {

// Small reusable worker pool for blocking or otherwise IO-heavy operations.
// max_pending == 0 means an unbounded queue; otherwise submit returns false
// when the pending queue is full. Tasks must not call shutdown() on this pool.
class ThreadPool {
 public:
  using Task = std::function<void()>;

  explicit ThreadPool(std::size_t workers, std::size_t max_pending = 0)
      : max_pending_(max_pending) {
    if (workers == 0) workers = 1;
    workers_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  ~ThreadPool() { shutdown(); }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  bool submit(Task task) {
    if (!task) return false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stopping_ || (max_pending_ != 0 && tasks_.size() >= max_pending_)) return false;
      tasks_.push_back(std::move(task));
    }
    ready_.notify_one();
    return true;
  }

  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stopping_) return;
      stopping_ = true;
    }
    ready_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) worker.join();
    }
    workers_.clear();
  }

  std::size_t pending() const {
    std::lock_guard<std::mutex> lock(mu_);
    return tasks_.size();
  }

  std::size_t size() const { return workers_.size(); }

 private:
  void worker_loop() {
    while (true) {
      Task task;
      {
        std::unique_lock<std::mutex> lock(mu_);
        ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty()) {
          if (stopping_) return;
          continue;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      try {
        task();
      } catch (...) {
        // A failed task must not terminate the worker or strand the queue.
      }
    }
  }

  const std::size_t max_pending_;
  mutable std::mutex mu_;
  std::condition_variable ready_;
  std::deque<Task> tasks_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
};

// Pool compartilhado pelos operadores analíticos. A criação única evita o
// custo de criar/destruir threads em cada filtro, join ou agregação grande.
inline ThreadPool& compute_pool() {
  static ThreadPool pool([] {
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    return static_cast<std::size_t>(std::min(hardware, 16u));
  }());
  return pool;
}

// Executa faixas independentes em workers reutilizáveis. O callback recebe
// [begin,end) e deve escrever apenas na memória associada à sua faixa.
template <typename Fn>
void parallel_for(std::size_t count, Fn&& fn, std::size_t grain = 65536) {
  if (count == 0) return;
  grain = std::max<std::size_t>(1, grain);
  ThreadPool& pool = compute_pool();
  const std::size_t parts = (count + grain - 1) / grain;
  if (parts <= 1 || pool.size() <= 1) {
    fn(0, count);
    return;
  }
  auto callback = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
  std::mutex done_mu;
  std::condition_variable done;
  std::size_t remaining = parts;
  std::exception_ptr error;
  for (std::size_t part = 0; part < parts; ++part) {
    const std::size_t begin = part * grain;
    const std::size_t end = std::min(count, begin + grain);
    auto task = [callback, begin, end, &done_mu, &done, &remaining, &error] {
      try {
        (*callback)(begin, end);
      } catch (...) {
        std::lock_guard<std::mutex> lock(done_mu);
        if (!error) error = std::current_exception();
      }
      {
        std::lock_guard<std::mutex> lock(done_mu);
        if (--remaining == 0) done.notify_one();
      }
    };
    if (!pool.submit(std::move(task))) task();
  }
  std::unique_lock<std::mutex> lock(done_mu);
  done.wait(lock, [&] { return remaining == 0; });
  if (error) std::rethrow_exception(error);
}

}  // namespace tilt::rt
