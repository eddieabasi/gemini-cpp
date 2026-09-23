#include "gemini/tools.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <stdexcept>

using namespace gemini;
using builtin_tools::evaluate_expression;

TEST(Expression, Arithmetic) {
  EXPECT_DOUBLE_EQ(*evaluate_expression("1 + 2 * 3"), 7.0);
  EXPECT_DOUBLE_EQ(*evaluate_expression("(1 + 2) * 3"), 9.0);
  EXPECT_DOUBLE_EQ(*evaluate_expression("2 ^ 3 ^ 2"), 512.0);  // right associative
  EXPECT_DOUBLE_EQ(*evaluate_expression("-2 ^ 2"), -4.0);
  EXPECT_DOUBLE_EQ(*evaluate_expression("10 % 4"), 2.0);
  EXPECT_DOUBLE_EQ(*evaluate_expression("--3"), 3.0);
  EXPECT_DOUBLE_EQ(*evaluate_expression("1.5e3 / 3"), 500.0);
  EXPECT_DOUBLE_EQ(*evaluate_expression("sqrt(16) + abs(-2) + floor(2.7) + ceil(0.1)"), 9.0);
  EXPECT_NEAR(*evaluate_expression("sin(pi / 2)"), 1.0, 1e-12);
  EXPECT_NEAR(*evaluate_expression("log(e)"), 1.0, 1e-12);
  EXPECT_DOUBLE_EQ(*evaluate_expression("log10(1000)"), 3.0);
}

TEST(Expression, Errors) {
  for (const char* bad : {"", "1 +", "(1", "1)", "1 / 0", "5 % 0", "foo", "foo(1)", "sqrt(-1)", "log(0)",
                          "1 2", "2 ** 3", "1..2", "system(\"ls\")"}) {
    auto r = evaluate_expression(bad);
    EXPECT_FALSE(r.has_value()) << bad;
    if (!r) {
      EXPECT_EQ(r.error().code, ErrorCode::kInvalidArgument);
    }
  }
  EXPECT_FALSE(evaluate_expression(std::string(1000, '(') + "1" + std::string(1000, ')')).has_value());
  EXPECT_FALSE(evaluate_expression("10 ^ 400").has_value());  // overflow to inf
}

TEST(ToolRegistry, InvokesAndReportsErrorsToModel) {
  ToolRegistry reg;
  reg.add({"echo", "", {}}, [](const nlohmann::json& a) -> Result<nlohmann::json> { return a["v"]; });
  reg.add({"fail", "", {}}, [](const nlohmann::json&) -> Result<nlohmann::json> {
    return make_error(ErrorCode::kInvalidArgument, "nope");
  });
  reg.add({"throw", "", {}}, [](const nlohmann::json&) -> Result<nlohmann::json> {
    throw std::runtime_error("kaboom");
  });

  EXPECT_EQ(reg.invoke({"echo", {{"v", 3}}, "id1"}).response, (nlohmann::json{{"result", 3}}));
  EXPECT_EQ(reg.invoke({"echo", {{"v", 3}}, "id1"}).id, "id1");
  EXPECT_EQ(reg.invoke({"fail", {}, ""}).response["error"], "nope");
  EXPECT_EQ(reg.invoke({"throw", {}, ""}).response["error"], "tool threw: kaboom");
  EXPECT_EQ(reg.invoke({"missing", {}, ""}).response["error"], "unknown function: missing");
  EXPECT_EQ(reg.declarations().size(), 3u);
}

TEST(ToolRegistry, BuiltinsValidateArguments) {
  ToolRegistry reg;
  builtin_tools::register_defaults(reg);
  EXPECT_TRUE(reg.invoke({"evaluate_expression", {{"expression", 3}}, ""}).response.contains("error"));
  EXPECT_TRUE(reg.invoke({"get_current_time", {}, ""}).response.contains("utc"));
}
