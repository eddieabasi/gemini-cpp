#include "check.hpp"
#include "solution.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

using namespace std::chrono_literals;
using ns = std::chrono::nanoseconds;

static std::optional<ns> P(std::string_view s) { return parse_duration(s); }
static bool is(std::string_view s, ns expected) {
  auto v = parse_duration(s);
  if (!v) {
    std::fprintf(stderr, "  parse_duration(\"%.*s\") returned nullopt, expected %lld ns\n",
                 static_cast<int>(s.size()), s.data(), static_cast<long long>(expected.count()));
    return false;
  }
  if (*v != expected) {
    std::fprintf(stderr, "  parse_duration(\"%.*s\") = %lld ns, expected %lld ns\n", static_cast<int>(s.size()),
                 s.data(), static_cast<long long>(v->count()), static_cast<long long>(expected.count()));
    return false;
  }
  return true;
}
static bool invalid(std::string_view s) {
  auto v = parse_duration(s);
  if (v) {
    std::fprintf(stderr, "  parse_duration(\"%.*s\") = %lld ns, expected nullopt\n", static_cast<int>(s.size()),
                 s.data(), static_cast<long long>(v->count()));
  }
  return !v;
}

int main() {
  // examples from the spec
  CHECK(is("1h30m", 5400s));
  CHECK(is("1.5s", 1500ms));
  CHECK(is("-2m3.5s", -123500ms));
  CHECK(is("300ms", 300ms));
  CHECK(is("2h45m0.5s", 2h + 45min + 500ms));

  // zero
  CHECK(is("0", 0ns));
  CHECK(is("+0", 0ns));
  CHECK(is("-0", 0ns));
  CHECK(is("0s", 0ns));
  CHECK(is("0.000s", 0ns));

  // units
  CHECK(is("10ns", 10ns));
  CHECK(is("1us", 1us));
  CHECK(is("1\xC2\xB5s", 1us));
  CHECK(is("1\xCE\xBCs", 1us));
  CHECK(is("7m", 7min));
  CHECK(is("1h1h", 2h));
  CHECK(is("1s1h", 1h + 1s));
  CHECK(is("+5s", 5s));

  // fractions
  CHECK(is(".5s", 500ms));
  CHECK(is("5.s", 5s));
  CHECK(is("1.0000000001s", 1s));
  CHECK(is("1.9999999999s", 1'999'999'999ns));
  CHECK(is("0.1ns", 0ns));
  CHECK(is("1.5us", 1500ns));
  CHECK(is("0.5h", 30min));
  CHECK(is("1.000000000000000000000001s", 1s));

  // range limits
  CHECK(is("9223372036854775807ns", ns{std::numeric_limits<std::int64_t>::max()}));
  CHECK(is("-9223372036854775808ns", ns{std::numeric_limits<std::int64_t>::min()}));
  CHECK(is("2562047h", 2562047h));
  CHECK(invalid("9223372036854775808ns"));
  CHECK(invalid("2562048h"));
  CHECK(invalid("99999999999999999999999s"));
  CHECK(invalid("9223372036854775807ns1ns"));

  // malformed
  for (const char* bad : {"", "-", "+", "1", "12", "00", "s", "1x", "1 s", " 1s", "1s ", ".s", "1..5s",
                          "1sm", "1h-30m", "--1s", "+-1s", "1.5.5s", "ms", "1S", "1H", "1e3s", "0x10s"}) {
    CHECK(invalid(bad));
  }
  return check::report();
}
