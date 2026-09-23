#pragma once

#include "gemini/client.hpp"
#include "gemini/eval/task.hpp"
#include "gemini/types.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace gemini::eval {

enum class Outcome {
  kPass,
  kCompileError,
  kTestFailure,
  kTimeout,
  kCrash,          // signal or sanitizer report
  kNoCode,         // reply contained no extractable C++
  kApiError,       // model call failed
  kHarnessError,   // local problem (filesystem, compiler missing, ...)
};

[[nodiscard]] std::string_view to_string(Outcome outcome) noexcept;

struct Attempt {
  int round = 0;  // 0 = first try, 1.. = repair rounds
  Outcome outcome = Outcome::kHarnessError;
  std::string detail;  // truncated compiler / test output or error message
  std::string source;  // the solution that was evaluated
  std::chrono::milliseconds compile_time{0};
  std::chrono::milliseconds run_time{0};
};

struct TaskResult {
  std::string task_id;
  Outcome outcome = Outcome::kHarnessError;  // outcome of the last attempt
  std::vector<Attempt> attempts;
  UsageMetadata usage;
  std::chrono::milliseconds model_latency{0};

  [[nodiscard]] bool passed() const noexcept { return outcome == Outcome::kPass; }
  [[nodiscard]] bool passed_first_try() const noexcept {
    return !attempts.empty() && attempts.front().outcome == Outcome::kPass;
  }
};

struct RunnerOptions {
  std::string compiler = "c++";
  std::string std_flag = "-std=c++20";
  std::vector<std::string> extra_flags;
  bool sanitize = false;  // build tests with ASan + UBSan
  int repair_rounds = 0;  // feed failures back to the model this many times
  int jobs = 1;           // tasks evaluated concurrently
  std::filesystem::path work_root;  // default: <tmp>/gemini-cpp-eval
  bool keep_workdirs = false;
  bool use_reference = false;  // evaluate reference.hpp instead of calling the model
  std::chrono::milliseconds compile_timeout{120'000};
  /// Extra wall time allowed before a test binary reaches main(); see ProcessOptions.
  std::chrono::milliseconds startup_grace{60'000};
  std::size_t max_log_bytes = 6'000;
  GenerationConfig generation;
};

struct Report {
  std::string model;
  std::string started_at;  // ISO-8601 UTC
  std::chrono::milliseconds wall_time{0};
  RunnerOptions options;
  std::vector<TaskResult> results;

  [[nodiscard]] std::size_t count(Outcome outcome) const;
  [[nodiscard]] std::size_t passed() const;
  [[nodiscard]] std::size_t passed_first_try() const;
  [[nodiscard]] UsageMetadata total_usage() const;
};

class Runner {
 public:
  using ResultCallback = std::function<void(const TaskResult&)>;

  /// `client` may be null when options.use_reference is set.
  Runner(Client* client, RunnerOptions options);

  [[nodiscard]] Report run(const std::vector<Task>& tasks, const ResultCallback& on_result = {});
  [[nodiscard]] TaskResult run_task(const Task& task);

  /// Compiles `source` as solution.hpp against the task's tests and runs them.
  [[nodiscard]] Attempt evaluate(const Task& task, const std::string& source,
                                 const std::filesystem::path& workdir, int round) const;

  [[nodiscard]] static std::string build_prompt(const Task& task);
  [[nodiscard]] static std::string build_repair_prompt(const Attempt& failed);
  static constexpr std::string_view kSystemPrompt =
      "You are an expert C++ engineer. You write correct, portable, standards-conforming C++20 "
      "that compiles cleanly with GCC and Clang. When asked to implement something, reply with "
      "exactly one ```cpp fenced code block containing the complete contents of solution.hpp "
      "(with #pragma once and every #include it needs). Do not write a main() function and do "
      "not include test code.";

 private:
  Client* client_;
  RunnerOptions options_;
};

[[nodiscard]] nlohmann::json to_json(const Report& report);
[[nodiscard]] std::string to_markdown(const Report& report);

}  // namespace gemini::eval
