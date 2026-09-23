#include "check.hpp"
#include "solution.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

static bool cmp(std::string_view a, std::string_view b, int expected) {
  const auto got = compare_semver(a, b);
  if (got != std::optional<int>(expected)) {
    std::fprintf(stderr, "  compare_semver(\"%.*s\", \"%.*s\") = %s, expected %d\n", static_cast<int>(a.size()),
                 a.data(), static_cast<int>(b.size()), b.data(),
                 got ? std::to_string(*got).c_str() : "nullopt", expected);
    return false;
  }
  return true;
}

static bool invalid(std::string_view v) {
  const bool ok = !compare_semver(v, "1.0.0").has_value() && !compare_semver("1.0.0", v).has_value();
  if (!ok) std::fprintf(stderr, "  \"%.*s\" should be rejected as invalid\n", static_cast<int>(v.size()), v.data());
  return ok;
}

int main() {
  // Ordering example from the SemVer spec, checked pairwise in both directions.
  const std::vector<std::string_view> ordered = {
      "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
      "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "2.0.0", "2.1.0", "2.1.1", "10.0.0"};
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    CHECK(cmp(ordered[i], ordered[i], 0));
    for (std::size_t j = i + 1; j < ordered.size(); ++j) {
      CHECK(cmp(ordered[i], ordered[j], -1));
      CHECK(cmp(ordered[j], ordered[i], 1));
    }
  }

  // Build metadata is ignored.
  CHECK(cmp("1.0.0+build.1", "1.0.0+other", 0));
  CHECK(cmp("1.0.0-alpha+001", "1.0.0-alpha", 0));
  CHECK(cmp("1.0.0+20130313144700", "1.0.0-beta", 1));
  CHECK(cmp("1.0.0-beta+exp.sha.5114f85", "1.0.0-beta", 0));

  // Numeric vs lexical comparison.
  CHECK(cmp("1.0.0-2", "1.0.0-10", -1));
  CHECK(cmp("1.0.0-10", "1.0.0-a", -1));
  CHECK(cmp("1.0.0-Alpha", "1.0.0-alpha", -1));   // ASCII: uppercase sorts first
  CHECK(cmp("1.0.0-alpha", "1.0.0-alpha-1", -1));
  CHECK(cmp("1.0.0-a.b.c", "1.0.0-a.b", 1));
  CHECK(cmp("1.0.0-00a", "1.0.0-0", 1));          // "00a" is alphanumeric, so it is allowed
  CHECK(cmp("1.0.0-x-y-z.--", "1.0.0-x-y-z.-", 1));
  CHECK(cmp("1.2.3----RC-SNAPSHOT.12.9.1--.12+788", "1.2.3----R-S.12.9.1--.12+meta", 1));
  CHECK(cmp("9.0.0", "10.0.0", -1));
  CHECK(cmp("1.10.0", "1.9.0", 1));

  // Arbitrarily large numbers.
  CHECK(cmp("18446744073709551616.0.0", "18446744073709551615.0.0", 1));
  CHECK(cmp("99999999999999999999999.0.0", "9.0.0", 1));
  CHECK(cmp("1.0.0-99999999999999999999999", "1.0.0-9", 1));
  CHECK(cmp("1.0.0-99999999999999999999999", "1.0.0-99999999999999999999999", 0));

  // Valid edge cases.
  CHECK(cmp("0.0.0", "0.0.0", 0));
  CHECK(cmp("1.0.0-0", "1.0.0", -1));
  CHECK(cmp("1.0.0+001", "1.0.0", 0));
  CHECK(cmp("1.0.0-0A.is.legal", "1.0.0-0A.is.legal", 0));

  // Invalid versions.
  for (std::string_view bad : {"", "1", "1.0", "1.0.0.0", "01.0.0", "1.00.0", "1.0.00", "v1.0.0", " 1.0.0",
                               "1.0.0 ", "1.0.0-", "1.0.0+", "1.0.0-01", "1.0.0-alpha..1", "1.0.0-alpha.",
                               "1.0.0+a..b", "1.0.0-alpha_1", "1.0.0+build+x", "-1.0.0", "1.-1.0", "1.0.0-é",
                               "a.b.c", "1..0", "1.0.0-+", "+1.0.0", "1.0.0-alpha+", "1.2", "1.2.3.DEV"}) {
    CHECK(invalid(bad));
  }
  return check::report();
}
