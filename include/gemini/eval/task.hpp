#pragma once

#include "gemini/error.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gemini::eval {

/// A coding task: a natural-language spec plus a hidden test program.
///
/// On disk a task is a directory:
///   task.json      {"id", "title", "prompt", "tags": [...], "timeout_ms": 10000}
///   tests.cpp      test program; includes "solution.hpp" and "check.hpp", exits 0 on success
///   reference.hpp  (optional) known-good solution used to validate the tests themselves
struct Task {
  std::string id;
  std::string title;
  std::string prompt;
  std::vector<std::string> tags;
  std::chrono::milliseconds timeout{10'000};
  std::filesystem::path dir;
  std::string test_source;
  std::optional<std::string> reference_source;
};

[[nodiscard]] Result<Task> load_task(const std::filesystem::path& dir);

/// Loads every sub-directory of `root` that contains a task.json, sorted by id.
[[nodiscard]] Result<std::vector<Task>> load_tasks(const std::filesystem::path& root);

}  // namespace gemini::eval
