#include "gemini/eval/runner.hpp"

#include "gemini/chat.hpp"
#include "gemini/eval/code_extract.hpp"
#include "gemini/eval/process.hpp"
#include "gemini/logging.hpp"

#include <atomic>
#include <ctime>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

#include <unistd.h>

namespace gemini::eval {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

/// Printed by check.hpp before main() so the runner can start the task's timeout clock only
/// once the test binary is actually running.
constexpr std::string_view kStartMarker = "\x1egemini-cpp:started\n";

/// Tiny assertion library made available to every task's tests.cpp as "check.hpp".
constexpr std::string_view kCheckHeader = R"cpp(#pragma once
// Assertion helpers injected by the gemini-cpp eval harness.
#include <cstdio>
#include <exception>
#include <ostream>
#include <sstream>
#include <string>

namespace check {
inline int& failures() { static int count = 0; return count; }

namespace detail {
// Tells the harness the binary has started (see kStartMarker in runner.cpp).
inline const bool started = [] {
  std::fputs("\x1egemini-cpp:started\n", stderr);
  std::fflush(stderr);
  return true;
}();

template <typename T>
std::string show(const T& value) {
  if constexpr (requires(std::ostream& os) { os << value; }) {
    std::ostringstream os;
    os << value;
    return os.str();
  } else {
    return "<unprintable>";
  }
}
}  // namespace detail

inline int report() {
  if (failures() == 0) {
    std::puts("ALL TESTS PASSED");
    return 0;
  }
  std::fprintf(stderr, "%d check(s) failed\n", failures());
  return 1;
}
}  // namespace check

#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      ++::check::failures();                                                          \
    }                                                                                 \
  } while (0)

#define CHECK_EQ(a, b)                                                                \
  do {                                                                                \
    const auto& check_a_ = (a);                                                       \
    const auto& check_b_ = (b);                                                       \
    if (!(check_a_ == check_b_)) {                                                    \
      std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (got %s vs %s)\n",       \
                   __FILE__, __LINE__, #a, #b,                                        \
                   ::check::detail::show(check_a_).c_str(),                           \
                   ::check::detail::show(check_b_).c_str());                          \
      ++::check::failures();                                                          \
    }                                                                                 \
  } while (0)

#define CHECK_THROWS(expr)                                                            \
  do {                                                                                \
    bool check_threw_ = false;                                                        \
    try { (void)(expr); } catch (...) { check_threw_ = true; }                        \
    if (!check_threw_) {                                                              \
      std::fprintf(stderr, "%s:%d: CHECK_THROWS failed: %s\n", __FILE__, __LINE__, #expr); \
      ++::check::failures();                                                          \
    }                                                                                 \
  } while (0)
)cpp";

std::string truncate_log(const std::string& text, std::size_t max_bytes) {
  if (text.size() <= max_bytes) return text;
  // Keep the head (first errors are the most useful) and a bit of the tail.
  const std::size_t tail = max_bytes / 4;
  const std::size_t head = max_bytes - tail;
  return text.substr(0, head) + "\n... [" + std::to_string(text.size() - max_bytes) +
         " bytes truncated] ...\n" + text.substr(text.size() - tail);
}

bool write_file(const fs::path& path, std::string_view content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  return static_cast<bool>(out);
}

std::string iso_now() {
  const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string unique_suffix() {
  static std::atomic<unsigned> counter{0};
  thread_local std::mt19937 rng{std::random_device{}()};
  return std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)) + "-" +
         std::to_string(rng() % 100000);
}

bool mentions_sanitizer(const std::string& output) {
  return output.find("AddressSanitizer") != std::string::npos ||
         output.find("UndefinedBehaviorSanitizer") != std::string::npos ||
         output.find("runtime error:") != std::string::npos ||
         output.find("LeakSanitizer") != std::string::npos;
}

}  // namespace

std::string_view to_string(Outcome outcome) noexcept {
  switch (outcome) {
    case Outcome::kPass: return "pass";
    case Outcome::kCompileError: return "compile_error";
    case Outcome::kTestFailure: return "test_failure";
    case Outcome::kTimeout: return "timeout";
    case Outcome::kCrash: return "crash";
    case Outcome::kNoCode: return "no_code";
    case Outcome::kApiError: return "api_error";
    case Outcome::kHarnessError: return "harness_error";
  }
  return "unknown";
}

Runner::Runner(Client* client, RunnerOptions options) : client_(client), options_(std::move(options)) {
  if (options_.work_root.empty()) options_.work_root = fs::temp_directory_path() / "gemini-cpp-eval";
  if (options_.jobs < 1) options_.jobs = 1;
  if (options_.repair_rounds < 0) options_.repair_rounds = 0;
}

std::string Runner::build_prompt(const Task& task) {
  return task.prompt +
         "\n\nDeliverable: the complete contents of `solution.hpp` in a single ```cpp block. "
         "It will be compiled with `-std=c++20 -Wall -pthread` and exercised by hidden tests "
         "that `#include \"solution.hpp\"`.";
}

std::string Runner::build_repair_prompt(const Attempt& failed) {
  std::string what;
  switch (failed.outcome) {
    case Outcome::kCompileError: what = "did not compile"; break;
    case Outcome::kTestFailure: what = "compiled but failed the hidden tests"; break;
    case Outcome::kTimeout: what = "timed out (possible deadlock, infinite loop or poor complexity)"; break;
    case Outcome::kCrash: what = "crashed or triggered a sanitizer error"; break;
    case Outcome::kNoCode: what = "did not contain a ```cpp code block"; break;
    default: what = "failed"; break;
  }
  return "Your solution " + what + ". Output:\n```text\n" + failed.detail +
         "\n```\nFix the problem and reply with the complete corrected solution.hpp in a "
         "single ```cpp block.";
}

Attempt Runner::evaluate(const Task& task, const std::string& source, const fs::path& workdir,
                         int round) const {
  Attempt attempt;
  attempt.round = round;
  attempt.source = source;

  if (!write_file(workdir / "solution.hpp", source)) {
    attempt.outcome = Outcome::kHarnessError;
    attempt.detail = "cannot write solution.hpp in " + workdir.string();
    return attempt;
  }

  ProcessOptions compile;
  compile.cwd = workdir;
  compile.timeout = options_.compile_timeout;
  compile.argv = {options_.compiler, options_.std_flag, "-O1", "-g", "-Wall", "-pthread", "-I."};
  if (options_.sanitize) {
    compile.argv.insert(compile.argv.end(), {"-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                             "-fno-sanitize-recover=undefined"});
  }
  compile.argv.insert(compile.argv.end(), options_.extra_flags.begin(), options_.extra_flags.end());
  // A fresh binary name per attempt: overwriting an executable in place can make macOS
  // reuse a stale code-signing verdict and stall or kill the new binary.
  const std::string binary = "test_bin_r" + std::to_string(round);
  compile.argv.insert(compile.argv.end(), {"tests.cpp", "-o", binary});

  auto built = run_process(compile);
  if (!built) {
    attempt.outcome = Outcome::kHarnessError;
    attempt.detail = built.error().describe();
    return attempt;
  }
  attempt.compile_time = built->duration;
  if (built->exit_code == 127 && built->output.starts_with("exec failed")) {
    attempt.outcome = Outcome::kHarnessError;
    attempt.detail = "compiler not found: " + options_.compiler;
    return attempt;
  }
  if (!built->ok()) {
    attempt.outcome = Outcome::kCompileError;
    attempt.detail = truncate_log((built->timed_out ? "compiler " + built->status_string() + "\n" : "") +
                                      built->output,
                                  options_.max_log_bytes);
    return attempt;
  }

  ProcessOptions run;
  run.cwd = workdir;
  run.timeout = task.timeout;
  run.argv = {(workdir / binary).string()};
  run.start_marker = std::string(kStartMarker);
  run.startup_grace = options_.startup_grace;
  if (!options_.sanitize) run.memory_limit_bytes = 2ULL << 30;  // ASan needs a huge address space

  auto ran = run_process(run);
  if (!ran) {
    attempt.outcome = Outcome::kHarnessError;
    attempt.detail = ran.error().describe();
    return attempt;
  }
  attempt.run_time = ran->duration - ran->startup.value_or(std::chrono::milliseconds(0));
  if (ran->ok()) {
    attempt.outcome = Outcome::kPass;
  } else if (ran->timed_out) {
    attempt.outcome = Outcome::kTimeout;
  } else if (ran->term_signal != 0 || mentions_sanitizer(ran->output)) {
    attempt.outcome = Outcome::kCrash;
  } else {
    attempt.outcome = Outcome::kTestFailure;
  }
  if (attempt.outcome != Outcome::kPass) {
    attempt.detail = truncate_log(ran->status_string() + "\n" + ran->output, options_.max_log_bytes);
  }
  return attempt;
}

TaskResult Runner::run_task(const Task& task) {
  TaskResult result;
  result.task_id = task.id;

  const fs::path workdir = options_.work_root / (task.id + "-" + unique_suffix());
  std::error_code ec;
  fs::create_directories(workdir, ec);
  if (ec || !write_file(workdir / "tests.cpp", task.test_source) ||
      !write_file(workdir / "check.hpp", kCheckHeader)) {
    result.attempts.push_back(Attempt{0, Outcome::kHarnessError,
                                      "cannot prepare " + workdir.string() + ": " + ec.message(),
                                      {}, {}, {}});
    result.outcome = Outcome::kHarnessError;
    return result;
  }

  if (options_.use_reference) {
    if (!task.reference_source) {
      result.attempts.push_back(Attempt{0, Outcome::kHarnessError, "task has no reference.hpp", {}, {}, {}});
    } else {
      result.attempts.push_back(evaluate(task, *task.reference_source, workdir, 0));
    }
  } else if (client_ == nullptr) {
    result.attempts.push_back(Attempt{0, Outcome::kHarnessError, "no model client configured", {}, {}, {}});
  } else {
    ChatOptions chat_options;
    chat_options.system_instruction = std::string(kSystemPrompt);
    chat_options.config = options_.generation;
    ChatSession chat(*client_, chat_options);

    std::string message = build_prompt(task);
    for (int round = 0; round <= options_.repair_rounds; ++round) {
      const auto start = Clock::now();
      auto reply = chat.send(message);
      result.model_latency += std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
      if (!reply) {
        result.attempts.push_back(Attempt{round, Outcome::kApiError, reply.error().describe(), {}, {}, {}});
        break;
      }
      const std::string text = reply->text();
      Attempt attempt;
      if (auto source = extract_cpp_source(text)) {
        attempt = evaluate(task, *source, workdir, round);
      } else {
        attempt.round = round;
        attempt.outcome = Outcome::kNoCode;
        attempt.detail = truncate_log("finish_reason=" + reply->finish_reason() + "\n" + text,
                                      options_.max_log_bytes);
      }
      log::info("eval.attempt", {{"task", task.id},
                                 {"round", std::to_string(round)},
                                 {"outcome", std::string(to_string(attempt.outcome))}});
      const bool done = attempt.outcome == Outcome::kPass || attempt.outcome == Outcome::kHarnessError;
      result.attempts.push_back(std::move(attempt));
      if (done) break;
      message = build_repair_prompt(result.attempts.back());
    }
    result.usage = chat.usage();
  }

  result.outcome = result.attempts.empty() ? Outcome::kHarnessError : result.attempts.back().outcome;
  if (!options_.keep_workdirs) fs::remove_all(workdir, ec);
  return result;
}

Report Runner::run(const std::vector<Task>& tasks, const ResultCallback& on_result) {
  Report report;
  report.model = options_.use_reference ? "reference" : (client_ ? client_->model() : "none");
  report.started_at = iso_now();
  report.options = options_;
  report.results.resize(tasks.size());

  const auto start = Clock::now();
  std::atomic<std::size_t> next{0};
  std::mutex callback_mutex;
  auto worker = [&] {
    for (std::size_t i = next.fetch_add(1); i < tasks.size(); i = next.fetch_add(1)) {
      report.results[i] = run_task(tasks[i]);
      if (on_result) {
        std::lock_guard lock(callback_mutex);
        on_result(report.results[i]);
      }
    }
  };

  const auto jobs = std::min<std::size_t>(static_cast<std::size_t>(options_.jobs), tasks.size());
  if (jobs <= 1) {
    worker();
  } else {
    std::vector<std::jthread> pool;
    pool.reserve(jobs);
    for (std::size_t i = 0; i < jobs; ++i) pool.emplace_back(worker);
  }  // jthreads join here
  report.wall_time = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
  return report;
}

}  // namespace gemini::eval
