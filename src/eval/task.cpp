#include "gemini/eval/task.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace gemini::eval {

namespace fs = std::filesystem;

namespace {

std::optional<std::string> read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::unexpected<Error> task_error(const fs::path& dir, const std::string& what) {
  return make_error(ErrorCode::kConfig, dir.string() + ": " + what);
}

}  // namespace

Result<Task> load_task(const fs::path& dir) {
  const auto manifest = read_file(dir / "task.json");
  if (!manifest) return task_error(dir, "cannot read task.json");
  const auto j = nlohmann::json::parse(*manifest, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return task_error(dir, "task.json is not a JSON object");

  Task task;
  task.dir = dir;
  task.id = j.value("id", dir.filename().string());
  task.title = j.value("title", task.id);
  if (auto p = j.find("prompt"); p != j.end() && p->is_string()) {
    task.prompt = p->get<std::string>();
  } else if (p != j.end() && p->is_array()) {  // allow multi-line prompts as arrays of lines
    for (const auto& line : *p) {
      if (line.is_string()) task.prompt += line.get<std::string>() + "\n";
    }
  }
  if (task.prompt.empty()) return task_error(dir, "task.json has no prompt");
  if (auto tags = j.find("tags"); tags != j.end() && tags->is_array()) {
    for (const auto& t : *tags) {
      if (t.is_string()) task.tags.push_back(t.get<std::string>());
    }
  }
  if (auto t = j.find("timeout_ms"); t != j.end() && t->is_number_integer() && t->get<int>() > 0) {
    task.timeout = std::chrono::milliseconds(t->get<int>());
  }

  auto tests = read_file(dir / "tests.cpp");
  if (!tests) return task_error(dir, "missing tests.cpp");
  task.test_source = std::move(*tests);
  task.reference_source = read_file(dir / "reference.hpp");
  return task;
}

Result<std::vector<Task>> load_tasks(const fs::path& root) {
  std::error_code ec;
  if (!fs::is_directory(root, ec)) {
    return make_error(ErrorCode::kConfig, "task directory not found: " + root.string());
  }
  std::vector<Task> tasks;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    if (!entry.is_directory() || !fs::exists(entry.path() / "task.json")) continue;
    auto task = load_task(entry.path());
    if (!task) return std::unexpected(task.error());
    tasks.push_back(std::move(*task));
  }
  if (ec) return make_error(ErrorCode::kIo, "cannot list " + root.string() + ": " + ec.message());
  std::sort(tasks.begin(), tasks.end(), [](const Task& a, const Task& b) { return a.id < b.id; });
  return tasks;
}

}  // namespace gemini::eval
