#include "fake_transport.hpp"

#include "gemini/eval/runner.hpp"
#include "gemini/eval/task.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace gemini;
using namespace gemini::eval;
using gemini::testing::FakeTransport;
using gemini::testing::text_response;

namespace {

Task add_task() {
  Task t;
  t.id = "add";
  t.prompt = "Implement `int add(int a, int b)`.";
  t.timeout = std::chrono::milliseconds(5000);
  t.test_source = R"(#include "check.hpp"
#include "solution.hpp"
int main() { CHECK_EQ(add(2, 3), 5); CHECK_EQ(add(-1, 1), 0); return check::report(); })";
  return t;
}

std::string fenced(const std::string& code) { return "Sure!\n```cpp\n" + code + "\n```\n"; }

struct Env {
  FakeTransport* transport = nullptr;
  std::unique_ptr<Client> client;
  RunnerOptions options;
  Env() {
    auto fake = std::make_unique<FakeTransport>();
    transport = fake.get();
    ClientOptions o;
    o.api_key = "k";
    o.retry = RetryPolicy::none();
    client = std::make_unique<Client>(std::move(o), std::move(fake));
    options.work_root = std::filesystem::temp_directory_path() / "gemini-cpp-unit";
  }
};

}  // namespace

TEST(Runner, PassesCorrectSolution) {
  Env env;
  env.transport->push_json(200, text_response(fenced("#pragma once\ninline int add(int a, int b) { return a + b; }")));
  Runner runner(env.client.get(), env.options);
  const auto result = runner.run_task(add_task());
  ASSERT_EQ(result.attempts.size(), 1u);
  EXPECT_EQ(result.outcome, Outcome::kPass) << result.attempts[0].detail;
  EXPECT_TRUE(result.passed_first_try());
  EXPECT_EQ(result.usage.total_tokens, 15);

  // The model saw the harness system prompt and the task.
  const auto body = nlohmann::json::parse(env.transport->requests[0].body);
  EXPECT_NE(body["contents"][0]["parts"][0]["text"].get<std::string>().find("int add"), std::string::npos);
  EXPECT_NE(body["systemInstruction"]["parts"][0]["text"].get<std::string>().find("solution.hpp"), std::string::npos);
}

TEST(Runner, ClassifiesFailures) {
  Env env;
  env.transport->push_json(200, text_response(fenced("#pragma once\ninline int add(int a, int b) { return a - b; }")));
  env.transport->push_json(200, text_response(fenced("#pragma once\ninline int add(int a, int b) { return a + }")));
  env.transport->push_json(200, text_response("I'd rather not."));
  env.transport->push_json(200, text_response(fenced("#pragma once\ninline int add(int a, int b) { while (true) {} }")));
  env.transport->push_json(200, text_response(fenced("#pragma once\n#include <cstdlib>\ninline int add(int, int) { std::abort(); }")));
  env.options.jobs = 1;
  Runner runner(env.client.get(), env.options);
  Task t = add_task();
  t.timeout = std::chrono::milliseconds(1000);
  EXPECT_EQ(runner.run_task(t).outcome, Outcome::kTestFailure);
  EXPECT_EQ(runner.run_task(t).outcome, Outcome::kCompileError);
  EXPECT_EQ(runner.run_task(t).outcome, Outcome::kNoCode);
  EXPECT_EQ(runner.run_task(t).outcome, Outcome::kTimeout);
  EXPECT_EQ(runner.run_task(t).outcome, Outcome::kCrash);
}

TEST(Runner, RepairLoopFeedsBackCompilerOutput) {
  Env env;
  env.transport->push_json(200, text_response(fenced("#pragma once\ninline int add(int a, int b) { return a + c; }")));
  env.transport->push_json(200, text_response(fenced("#pragma once\ninline int add(int a, int b) { return a + b; }")));
  env.options.repair_rounds = 2;
  Runner runner(env.client.get(), env.options);
  const auto result = runner.run_task(add_task());
  ASSERT_EQ(result.attempts.size(), 2u) << result.attempts.back().detail;
  EXPECT_EQ(result.attempts[0].outcome, Outcome::kCompileError);
  EXPECT_EQ(result.outcome, Outcome::kPass);
  EXPECT_FALSE(result.passed_first_try());

  const auto second = nlohmann::json::parse(env.transport->requests[1].body);
  const auto repair_prompt = second["contents"][2]["parts"][0]["text"].get<std::string>();
  EXPECT_NE(repair_prompt.find("did not compile"), std::string::npos);
  EXPECT_NE(repair_prompt.find("'c'"), std::string::npos) << repair_prompt;  // compiler diagnostic included
}

TEST(Runner, ApiErrorsAreReported) {
  Env env;
  env.transport->push_json(403, R"({"error":{"message":"denied","status":"PERMISSION_DENIED"}})");
  Runner runner(env.client.get(), env.options);
  const auto result = runner.run_task(add_task());
  EXPECT_EQ(result.outcome, Outcome::kApiError);
  EXPECT_NE(result.attempts[0].detail.find("denied"), std::string::npos);
}

TEST(Runner, ParallelRunAndReports) {
  Env env;
  for (int i = 0; i < 4; ++i) {
    env.transport->push_json(200, text_response(fenced("#pragma once\ninline int add(int a, int b) { return a + b; }")));
  }
  env.options.jobs = 4;
  Runner runner(env.client.get(), env.options);
  std::vector<Task> tasks;
  for (int i = 0; i < 4; ++i) {
    tasks.push_back(add_task());
    tasks.back().id = "add-" + std::to_string(i);
  }
  int callbacks = 0;
  const auto report = runner.run(tasks, [&](const TaskResult&) { ++callbacks; });
  EXPECT_EQ(callbacks, 4);
  EXPECT_EQ(report.passed(), 4u);
  EXPECT_EQ(report.results[2].task_id, "add-2");  // results keep task order

  const auto j = to_json(report);
  EXPECT_EQ(j["summary"]["passed"], 4);
  EXPECT_EQ(j["summary"]["outcomes"]["pass"], 4);
  const auto md = to_markdown(report);
  EXPECT_NE(md.find("pass@1 | 4/4 (100.0%)"), std::string::npos) << md;
}

TEST(Tasks, BundledTasksLoad) {
  auto tasks = load_tasks(GEMINI_TASKS_DIR);
  ASSERT_TRUE(tasks.has_value()) << tasks.error().describe();
  ASSERT_GE(tasks->size(), 6u);
  for (const auto& t : *tasks) {
    EXPECT_FALSE(t.prompt.empty()) << t.id;
    EXPECT_NE(t.test_source.find("solution.hpp"), std::string::npos) << t.id;
    EXPECT_TRUE(t.reference_source.has_value()) << t.id;
  }
}

TEST(Tasks, RejectsBrokenTaskDirectories) {
  const auto dir = std::filesystem::temp_directory_path() / "gemini-cpp-bad-task";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  EXPECT_FALSE(load_task(dir).has_value());  // no task.json
  std::ofstream(dir / "task.json") << R"({"id": "x", "prompt": "p"})";
  EXPECT_FALSE(load_task(dir).has_value());  // no tests.cpp
  std::ofstream(dir / "tests.cpp") << "int main() {}";
  auto ok = load_task(dir);
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(ok->id, "x");
  EXPECT_FALSE(ok->reference_source.has_value());
  std::filesystem::remove_all(dir);
}
