#pragma once

#include "gemini/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gemini::eval {

struct ProcessOptions {
  std::vector<std::string> argv;  // argv[0] is looked up in PATH
  std::filesystem::path cwd;      // empty = inherit
  std::chrono::milliseconds timeout{10'000};
  std::size_t max_output_bytes = 1 << 20;  // output beyond this is discarded (not blocking)
  /// Address-space limit for the child (enforced on Linux; advisory elsewhere).
  std::optional<std::uint64_t> memory_limit_bytes;
  /// If set, `timeout` only starts counting once the child prints this marker (which is
  /// removed from the captured output). Until then the budget is `startup_grace + timeout`.
  /// This keeps OS-level launch latency (e.g. macOS verifying a freshly linked binary,
  /// which is serialised system-wide and can take seconds) out of the measured budget.
  std::string start_marker;
  std::chrono::milliseconds startup_grace{0};
};

struct ProcessResult {
  int exit_code = -1;   // valid when term_signal == 0
  int term_signal = 0;  // signal that killed the child, 0 if it exited normally
  bool timed_out = false;
  bool output_truncated = false;
  std::string output;  // interleaved stdout + stderr
  std::chrono::milliseconds duration{0};
  std::optional<std::chrono::milliseconds> startup;  // time until start_marker was seen

  [[nodiscard]] bool ok() const noexcept { return !timed_out && term_signal == 0 && exit_code == 0; }
  [[nodiscard]] std::string status_string() const;
};

/// Runs a child process with a wall-clock timeout. The child gets its own process group so
/// the whole tree (e.g. a test binary that forks) is killed on timeout. stdin is /dev/null.
/// Safe to call concurrently from multiple threads.
[[nodiscard]] Result<ProcessResult> run_process(const ProcessOptions& options);

}  // namespace gemini::eval
