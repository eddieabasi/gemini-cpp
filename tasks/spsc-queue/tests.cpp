#include "check.hpp"
#include "solution.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <thread>

struct NoDefault {
  explicit NoDefault(int v) : value(v) {}
  int value;
};

int main() {
  {  // exact capacity, FIFO order, non power of two
    SpscQueue<int, 5> q;
    static_assert(SpscQueue<int, 5>::capacity() == 5);
    for (int i = 0; i < 5; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));
    CHECK_EQ(q.size_approx(), std::size_t{5});
    for (int i = 0; i < 5; ++i) CHECK(q.try_pop() == std::optional<int>(i));
    CHECK(!q.try_pop().has_value());
    CHECK_EQ(q.size_approx(), std::size_t{0});
  }
  {  // wrap-around many times
    SpscQueue<int, 3> q;
    int next_in = 0, next_out = 0;
    for (int round = 0; round < 10'000; ++round) {
      while (q.try_push(next_in)) ++next_in;
      for (int k = 0; k < 2; ++k) {
        auto v = q.try_pop();
        CHECK(v.has_value());
        if (v) CHECK_EQ(*v, next_out++);
      }
    }
  }
  {  // move-only and non-default-constructible element types
    SpscQueue<std::unique_ptr<std::string>, 2> q;
    CHECK(q.try_push(std::make_unique<std::string>("hello")));
    auto v = q.try_pop();
    CHECK(v.has_value() && *v && **v == "hello");
    SpscQueue<NoDefault, 2> nd;
    CHECK(nd.try_push(NoDefault(7)));
    CHECK(nd.try_pop()->value == 7);
  }
  {  // remaining elements are destroyed with the queue
    auto tracker = std::make_shared<int>(0);
    {
      SpscQueue<std::shared_ptr<int>, 4> q;
      q.try_push(tracker);
      q.try_push(tracker);
      q.try_push(tracker);
      (void)q.try_pop();
      CHECK_EQ(tracker.use_count(), 3L);
    }
    CHECK_EQ(tracker.use_count(), 1L);
  }
  {  // concurrent producer / consumer
    constexpr std::uint64_t kCount = 2'000'000;
    SpscQueue<std::uint64_t, 1000> q;
    std::uint64_t sum = 0;
    bool ordered = true;
    std::thread consumer([&] {
      std::uint64_t expected = 0;
      while (expected < kCount) {
        if (auto v = q.try_pop()) {
          if (*v != expected) ordered = false;
          sum += *v;
          ++expected;
        }
      }
    });
    std::thread producer([&] {
      for (std::uint64_t i = 0; i < kCount;) {
        if (q.try_push(i)) ++i;
      }
    });
    producer.join();
    consumer.join();
    CHECK(ordered);
    CHECK_EQ(sum, kCount * (kCount - 1) / 2);
    CHECK_EQ(q.size_approx(), std::size_t{0});
  }
  return check::report();
}
