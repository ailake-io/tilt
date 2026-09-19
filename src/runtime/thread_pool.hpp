#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
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

}  // namespace tilt::rt
