#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool {
 public:
  explicit ThreadPool(std::size_t threads) {
    if (threads == 0) threads = 1;
    workers_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { worker_loop(); });
  }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  ~ThreadPool() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    work_cv_.notify_all();
    for (auto& t : workers_) t.join();
  }

  template <class F, class... Args>
  auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
    using R = std::invoke_result_t<F, Args...>;
    auto task = std::make_shared<std::packaged_task<R()>>(
        [fn = std::forward<F>(f), ... as = std::forward<Args>(args)]() mutable -> R {
          return std::invoke(std::move(fn), std::move(as)...);
        });
    auto future = task->get_future();
    {
      std::lock_guard lock(mutex_);
      queue_.emplace_back([task] { (*task)(); });
    }
    work_cv_.notify_one();
    return future;
  }

  void wait_idle() {
    std::unique_lock lock(mutex_);
    idle_cv_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
  }

  std::size_t thread_count() const { return workers_.size(); }

 private:
  void worker_loop() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lock(mutex_);
        work_cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) return;  // stopping and fully drained
        job = std::move(queue_.front());
        queue_.pop_front();
        ++active_;
      }
      job();  // packaged_task captures exceptions into the future
      {
        std::lock_guard lock(mutex_);
        --active_;
        if (queue_.empty() && active_ == 0) idle_cv_.notify_all();
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable work_cv_;
  std::condition_variable idle_cv_;
  std::deque<std::function<void()>> queue_;
  std::vector<std::thread> workers_;
  std::size_t active_ = 0;
  bool stopping_ = false;
};
