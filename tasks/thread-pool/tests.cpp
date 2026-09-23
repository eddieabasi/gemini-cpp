#include "check.hpp"
#include "solution.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace std::chrono_literals;

int main() {
  static_assert(!std::is_copy_constructible_v<ThreadPool>);
  static_assert(!std::is_move_constructible_v<ThreadPool>);

  {  // results come back through futures
    ThreadPool pool(4);
    CHECK_EQ(pool.thread_count(), std::size_t{4});
    std::vector<std::future<long>> futures;
    for (long i = 0; i < 1000; ++i) futures.push_back(pool.submit([i] { return i * i; }));
    long sum = 0;
    for (auto& f : futures) sum += f.get();
    CHECK_EQ(sum, 332833500L);
  }
  {  // arguments, void tasks and exceptions
    ThreadPool pool(2);
    auto add = pool.submit([](int a, int b) { return a + b; }, 40, 2);
    CHECK_EQ(add.get(), 42);
    std::atomic<int> hits{0};
    auto v = pool.submit([&hits] { ++hits; });
    static_assert(std::is_same_v<decltype(v), std::future<void>>);
    v.get();
    CHECK_EQ(hits.load(), 1);
    auto boom = pool.submit([]() -> int { throw std::runtime_error("boom"); });
    CHECK_THROWS(boom.get());
    auto after = pool.submit([] { return std::string("still alive"); });
    CHECK_EQ(after.get(), std::string("still alive"));
  }
  {  // move-only callables and arguments
    ThreadPool pool(2);
    auto owned = std::make_unique<int>(5);
    auto f1 = pool.submit([p = std::move(owned)] { return *p; });
    auto f2 = pool.submit([](std::unique_ptr<int> p) { return *p + 1; }, std::make_unique<int>(41));
    CHECK_EQ(f1.get(), 5);
    CHECK_EQ(f2.get(), 42);
  }
  {  // destructor drains the queue
    std::atomic<int> counter{0};
    {
      ThreadPool pool(2);
      for (int i = 0; i < 2000; ++i) {
        pool.submit([&counter] {
          std::this_thread::sleep_for(std::chrono::microseconds(10));
          counter.fetch_add(1);
        });
      }
    }
    CHECK_EQ(counter.load(), 2000);
  }
  {  // wait_idle
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    for (int i = 0; i < 64; ++i) {
      pool.submit([&counter] {
        std::this_thread::sleep_for(1ms);
        counter.fetch_add(1);
      });
    }
    pool.wait_idle();
    CHECK_EQ(counter.load(), 64);
    pool.wait_idle();  // idempotent, must not block
  }
  {  // tasks may submit more tasks
    ThreadPool pool(2);
    std::atomic<int> counter{0};
    pool.submit([&] {
      counter.fetch_add(1);
      pool.submit([&] { counter.fetch_add(1); });
    });
    pool.wait_idle();
    CHECK_EQ(counter.load(), 2);
  }
  {  // tasks really run in parallel: 4 tasks rendezvous on 4 threads
    ThreadPool pool(4);
    std::atomic<int> arrived{0};
    std::vector<std::future<bool>> futures;
    for (int i = 0; i < 4; ++i) {
      futures.push_back(pool.submit([&arrived] {
        arrived.fetch_add(1);
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (arrived.load() < 4) {
          if (std::chrono::steady_clock::now() > deadline) return false;
          std::this_thread::yield();
        }
        return true;
      }));
    }
    for (auto& f : futures) CHECK(f.get());
  }
  return check::report();
}
