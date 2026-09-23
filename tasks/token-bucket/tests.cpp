#include "check.hpp"
#include "solution.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using TimePoint = std::chrono::steady_clock::time_point;

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

int main() {
  TimePoint now{};  // fake clock
  auto clock = [&now] { return now; };

  {  // starts full, drains, refills continuously
    TokenBucket b(10.0, 5.0, clock);
    CHECK(near(b.available(), 5.0));
    for (int i = 0; i < 5; ++i) CHECK(b.try_acquire());
    CHECK(!b.try_acquire());
    now += 100ms;  // +1 token
    CHECK(b.try_acquire());
    CHECK(!b.try_acquire());

    const auto wait = b.time_until_available(1.0);
    CHECK(wait > 99ms && wait <= 100ms + 1us);
    now += 50ms;
    const auto wait2 = b.time_until_available(1.0);
    CHECK(wait2 > 49ms && wait2 <= 50ms + 1us);

    now += 10s;  // capped at burst
    CHECK(near(b.available(), 5.0));
    CHECK(b.time_until_available(1.0) == std::chrono::nanoseconds::zero());
    CHECK(b.time_until_available(6.0) == std::chrono::nanoseconds::max());
    CHECK(!b.try_acquire(6.0));
  }
  {  // fractional tokens; failed acquire consumes nothing
    TokenBucket b(1.0, 5.0, clock);
    CHECK(b.try_acquire(2.5));
    CHECK(near(b.available(), 2.5));
    CHECK(!b.try_acquire(3.0));
    CHECK(near(b.available(), 2.5));
  }
  {  // clock going backwards never removes tokens or produces NaN
    TokenBucket b(100.0, 10.0, clock);
    CHECK(b.try_acquire(4.0));
    const TimePoint saved = now;
    now -= 5s;
    const double avail = b.available();
    CHECK(near(avail, 6.0));
    CHECK(!std::isnan(avail));
    now = saved;
    CHECK(near(b.available(), 6.0));
  }
  {  // many tiny refills add up
    TokenBucket b(1000.0, 1000.0, clock);
    CHECK(b.try_acquire(1000.0));
    for (int i = 0; i < 1'000'000; ++i) {
      now += 1us;
      (void)b.available();
    }
    CHECK(near(b.available(), 1000.0, 0.01));
  }
  {  // zero rate
    TokenBucket b(0.0, 3.0, clock);
    CHECK(b.try_acquire(3.0));
    CHECK(b.time_until_available(1.0) == std::chrono::nanoseconds::max());
    now += 1h;
    CHECK(!b.try_acquire());
  }
  {  // thread safety: exactly `burst` acquisitions succeed with a frozen clock
    TokenBucket b(1.0, 1000.0, clock);
    std::atomic<int> granted{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
      threads.emplace_back([&] {
        for (int i = 0; i < 500; ++i) {
          if (b.try_acquire()) granted.fetch_add(1);
        }
      });
    }
    for (auto& t : threads) t.join();
    CHECK_EQ(granted.load(), 1000);
  }
  {  // default clock compiles and works
    TokenBucket b(1e9, 1.0);
    CHECK(b.try_acquire());
  }
  return check::report();
}
