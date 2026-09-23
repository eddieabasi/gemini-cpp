#include "gemini/eval/runner.hpp"

#include <array>
#include <cstdio>

namespace gemini::eval {

using nlohmann::json;

namespace {

constexpr std::array kAllOutcomes = {Outcome::kPass,     Outcome::kCompileError, Outcome::kTestFailure,
                                     Outcome::kTimeout,  Outcome::kCrash,        Outcome::kNoCode,
                                     Outcome::kApiError, Outcome::kHarnessError};

std::string percent(std::size_t part, std::size_t whole) {
  if (whole == 0) return "n/a";
  char buf[16];
  std::snprintf(buf, sizeof buf, "%.1f%%", 100.0 * static_cast<double>(part) / static_cast<double>(whole));
  return buf;
}

/// Most informative line of a failure log (first compiler error / failed check), sanitised
/// for a Markdown table cell.
std::string summary_line(const std::string& text, std::size_t max = 100) {
  std::string line;
  std::string first;
  std::size_t pos = 0;
  while (pos < text.size()) {
    auto end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string candidate = text.substr(pos, end - pos);
    if (first.empty() && !candidate.empty()) first = candidate;
    if (candidate.find("error:") != std::string::npos || candidate.find("CHECK") != std::string::npos ||
        candidate.find("Sanitizer") != std::string::npos) {
      line = std::move(candidate);
      break;
    }
    pos = end + 1;
  }
  if (line.empty()) line = first;
  if (line.size() > max) line = line.substr(0, max) + "…";
  for (auto& c : line) {
    if (c == '|') c = '/';
    if (c == '`') c = '\'';
  }
  return line;
}

}  // namespace

std::size_t Report::count(Outcome outcome) const {
  std::size_t n = 0;
  for (const auto& r : results) n += (r.outcome == outcome) ? 1 : 0;
  return n;
}

std::size_t Report::passed() const { return count(Outcome::kPass); }

std::size_t Report::passed_first_try() const {
  std::size_t n = 0;
  for (const auto& r : results) n += r.passed_first_try() ? 1 : 0;
  return n;
}

UsageMetadata Report::total_usage() const {
  UsageMetadata total;
  for (const auto& r : results) total += r.usage;
  return total;
}

json to_json(const Report& report) {
  json j;
  j["model"] = report.model;
  j["started_at"] = report.started_at;
  j["wall_time_ms"] = report.wall_time.count();
  j["options"] = {{"compiler", report.options.compiler},
                  {"std", report.options.std_flag},
                  {"sanitize", report.options.sanitize},
                  {"repair_rounds", report.options.repair_rounds},
                  {"jobs", report.options.jobs}};
  json outcomes = json::object();
  for (auto o : kAllOutcomes) outcomes[std::string(to_string(o))] = report.count(o);
  const auto usage = report.total_usage();
  j["summary"] = {{"tasks", report.results.size()},
                  {"passed", report.passed()},
                  {"passed_first_try", report.passed_first_try()},
                  {"outcomes", std::move(outcomes)},
                  {"prompt_tokens", usage.prompt_tokens},
                  {"output_tokens", usage.candidates_tokens},
                  {"thinking_tokens", usage.thoughts_tokens}};
  j["results"] = json::array();
  for (const auto& r : report.results) {
    json attempts = json::array();
    for (const auto& a : r.attempts) {
      attempts.push_back({{"round", a.round},
                          {"outcome", to_string(a.outcome)},
                          {"compile_ms", a.compile_time.count()},
                          {"run_ms", a.run_time.count()},
                          {"detail", a.detail},
                          {"source", a.source}});
    }
    j["results"].push_back({{"task", r.task_id},
                            {"outcome", to_string(r.outcome)},
                            {"passed_first_try", r.passed_first_try()},
                            {"model_latency_ms", r.model_latency.count()},
                            {"prompt_tokens", r.usage.prompt_tokens},
                            {"output_tokens", r.usage.candidates_tokens},
                            {"attempts", std::move(attempts)}});
  }
  return j;
}

std::string to_markdown(const Report& report) {
  const std::size_t n = report.results.size();
  std::string md;
  md += "# gemini-cpp eval report\n\n";
  md += "- **Model:** `" + report.model + "`\n";
  md += "- **Started:** " + report.started_at + "\n";
  md += "- **Wall time:** " + std::to_string(report.wall_time.count()) + " ms\n";
  md += "- **Compiler:** `" + report.options.compiler + " " + report.options.std_flag + "`" +
        (report.options.sanitize ? " with ASan+UBSan" : "") + "\n";
  md += "- **Repair rounds:** " + std::to_string(report.options.repair_rounds) + "\n\n";

  md += "| Metric | Value |\n|---|---|\n";
  md += "| pass@1 | " + std::to_string(report.passed_first_try()) + "/" + std::to_string(n) + " (" +
        percent(report.passed_first_try(), n) + ") |\n";
  md += "| pass after repair | " + std::to_string(report.passed()) + "/" + std::to_string(n) + " (" +
        percent(report.passed(), n) + ") |\n";
  const auto usage = report.total_usage();
  md += "| prompt tokens | " + std::to_string(usage.prompt_tokens) + " |\n";
  md += "| output tokens | " + std::to_string(usage.candidates_tokens) + " |\n\n";

  md += "| Task | Outcome | Attempts | First failure |\n|---|---|---|---|\n";
  for (const auto& r : report.results) {
    std::string failure;
    for (const auto& a : r.attempts) {
      if (a.outcome != Outcome::kPass) {
        failure = std::string(to_string(a.outcome)) + ": " + summary_line(a.detail);
        break;
      }
    }
    md += "| `" + r.task_id + "` | " + (r.passed() ? "✅ " : "❌ ") + std::string(to_string(r.outcome)) +
          " | " + std::to_string(r.attempts.size()) + " | " + failure + " |\n";
  }
  return md;
}

}  // namespace gemini::eval
