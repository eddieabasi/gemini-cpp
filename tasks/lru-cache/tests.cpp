#include "check.hpp"
#include "solution.hpp"

#include <string>

int main() {
  {  // basic eviction order
    LruCache<int, std::string> c(2);
    c.put(1, "one");
    c.put(2, "two");
    CHECK_EQ(c.size(), std::size_t{2});
    CHECK_EQ(c.capacity(), std::size_t{2});
    CHECK(c.get(1) == std::optional<std::string>("one"));  // 1 becomes MRU
    c.put(3, "three");                                    // evicts 2
    CHECK(!c.contains(2));
    CHECK(c.contains(1));
    CHECK(c.contains(3));
    CHECK(!c.get(2).has_value());
  }
  {  // contains() must not refresh recency
    LruCache<int, int> c(2);
    c.put(1, 1);
    c.put(2, 2);
    const auto& cc = c;
    CHECK(cc.contains(1));
    c.put(3, 3);  // 1 is still LRU
    CHECK(!c.contains(1));
    CHECK(c.contains(2));
  }
  {  // updating refreshes recency and replaces the value
    LruCache<std::string, int> c(2);
    c.put("a", 1);
    c.put("b", 2);
    c.put("a", 10);
    c.put("c", 3);  // evicts b
    CHECK(!c.contains("b"));
    CHECK(c.get("a") == std::optional<int>(10));
    CHECK_EQ(c.size(), std::size_t{2});
  }
  {  // zero capacity stores nothing
    LruCache<int, int> z(0);
    z.put(1, 1);
    CHECK_EQ(z.size(), std::size_t{0});
    CHECK(!z.get(1).has_value());
  }
  {  // capacity one
    LruCache<int, int> one(1);
    one.put(1, 1);
    one.put(2, 2);
    CHECK(!one.contains(1));
    CHECK(one.get(2) == std::optional<int>(2));
  }
  {  // scale: must be O(1), a linear scan would time out
    LruCache<int, int> big(50'000);
    for (int i = 0; i < 400'000; ++i) {
      big.put(i, i);
      if (i % 3 == 0) (void)big.get(i / 2);
    }
    CHECK_EQ(big.size(), std::size_t{50'000});
    CHECK(big.contains(399'999));
    CHECK(!big.contains(0));
  }
  return check::report();
}
