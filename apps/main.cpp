// gemini-cpp: command line front-end for the gemini-cpp library.
//
//   gemini-cpp ask "Explain RAII in one paragraph"
//   gemini-cpp chat --tools
//   gemini-cpp eval --tasks tasks --jobs 4 --repair 1 --report-md report.md

#include "cli_args.hpp"

#include "gemini/gemini.hpp"
#include "gemini/eval/runner.hpp"
#include "gemini/eval/task.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>

#include <unistd.h>

#ifndef GEMINI_CPP_VERSION
#define GEMINI_CPP_VERSION "dev"
#endif

namespace {

namespace fs = std::filesystem;
using namespace gemini;

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

std::atomic<bool> g_interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free);

extern "C" void on_sigint(int) { g_interrupted.store(true); }

constexpr std::string_view kUsage = R"(gemini-cpp )" GEMINI_CPP_VERSION R"( - C++ client and coding-agent eval harness for Google Gemini

Usage:
  gemini-cpp ask [options] [PROMPT...]     one-shot prompt (reads stdin if PROMPT is empty or -)
  gemini-cpp chat [options]                interactive multi-turn chat
  gemini-cpp tokens [options] [PROMPT...]  count tokens for a prompt
  gemini-cpp models                        list models available to your API key
  gemini-cpp eval [options]                have Gemini solve C++ tasks; compile & test the results

Common options:
  --model NAME            model to use (default: $GEMINI_MODEL or gemini-2.5-flash)
  --system TEXT           system instruction
  --temperature X         sampling temperature
  --max-tokens N          maximum output tokens
  --thinking-budget N     thinking token budget (0 disables thinking where supported)
  --log-level LEVEL       debug | info | warn | error | off   (default: warn)
  --log-json              emit logs as JSON lines on stderr

ask:
  --no-stream             wait for the full response instead of streaming
  --json                  ask for a JSON response (responseMimeType=application/json)
  --usage                 print token usage to stderr

chat:
  --tools                 enable built-in tools (get_current_time, evaluate_expression)
  In-chat commands: /reset  /usage  /history  /exit

eval:
  --tasks DIR             task directory (default: ./tasks)
  --task ID               only run this task (repeatable)
  --jobs N                tasks to evaluate in parallel (default: 1)
  --repair N              feed failures back to the model up to N times (default: 0)
  --sanitize              build tests with AddressSanitizer + UBSan
  --reference             evaluate each task's reference.hpp instead of calling the model
  --compiler PATH         C++ compiler (default: $CXX or c++)
  --std FLAG              language standard flag (default: -std=c++20)
  --work-dir DIR          where per-task build directories are created
  --keep                  keep build directories for inspection
  --report-json FILE      write the full JSON report (includes sources and logs)
  --report-md FILE        write a Markdown summary
  --min-pass RATIO        exit 0 if the final pass rate is >= RATIO (default: 1.0)

Environment:
  GEMINI_API_KEY          API key (https://aistudio.google.com/apikey); GOOGLE_API_KEY also works
  GEMINI_MODEL            default model
  GEMINI_BASE_URL         API base URL (default: https://generativelanguage.googleapis.com/v1beta)
)";

const std::set<std::string> kCommonValues = {"model", "system", "temperature", "max-tokens",
                                             "thinking-budget", "log-level"};
const std::set<std::string> kCommonFlags = {"log-json", "help"};

cli::Args::Spec make_spec(std::set<std::string> values, std::set<std::string> flags) {
  values.insert(kCommonValues.begin(), kCommonValues.end());
  flags.insert(kCommonFlags.begin(), kCommonFlags.end());
  return {std::move(values), std::move(flags)};
}

int usage_error(const std::string& message) {
  std::cerr << "gemini-cpp: " << message << "\nRun 'gemini-cpp --help' for usage.\n";
  return kExitUsage;
}

int api_error(const Error& error) {
  std::cerr << "gemini-cpp: " << error.describe() << "\n";
  return kExitFailure;
}

/// Applies options shared by every command. Returns an error message on bad input.
std::optional<std::string> apply_common(const cli::Args& args, GenerationConfig& config) {
  if (auto level = args.get("log-level")) {
    log::Level parsed{};
    if (!log::parse_level(*level, parsed)) return "invalid --log-level '" + *level + "'";
    log::set_level(parsed);
  }
  log::set_json(args.flag("log-json"));
  if (auto err = args.get_double("temperature", config.temperature)) return err;
  if (auto err = args.get_int("max-tokens", config.max_output_tokens)) return err;
  if (auto err = args.get_int("thinking-budget", config.thinking_budget)) return err;
  return std::nullopt;
}

Result<std::unique_ptr<Client>> make_client(const cli::Args& args) {
  auto options = ClientOptions::from_env();
  if (!options) return std::unexpected(options.error());
  if (auto model = args.get("model")) options->model = *model;
  return std::make_unique<Client>(std::move(*options));
}

std::string prompt_from(const cli::Args& args) {
  const auto& words = args.positional();
  if (words.empty() || (words.size() == 1 && words[0] == "-")) {
    return std::string(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
  }
  std::string prompt;
  for (const auto& w : words) {
    if (!prompt.empty()) prompt += ' ';
    prompt += w;
  }
  return prompt;
}

void print_usage_line(const UsageMetadata& usage) {
  std::cerr << "[tokens] prompt=" << usage.prompt_tokens << " output=" << usage.candidates_tokens;
  if (usage.thoughts_tokens != 0) std::cerr << " thinking=" << usage.thoughts_tokens;
  std::cerr << " total=" << usage.total_tokens << "\n";
}

/// Streams visible (non-thought) text to stdout; cancels on Ctrl-C.
Client::StreamCallback stdout_printer() {
  return [](const GenerateResponse& chunk) {
    if (g_interrupted.load()) return false;
    if (!chunk.candidates.empty()) {
      std::cout << chunk.candidates.front().content.text() << std::flush;
    }
    return true;
  };
}

// ---------------------------------------------------------------------------
int cmd_ask(int argc, char** argv) {
  cli::Args args;
  if (auto err = args.parse(argc, argv, 2, make_spec({}, {"no-stream", "json", "usage"}))) {
    return usage_error(*err);
  }
  GenerateRequest request;
  if (auto err = apply_common(args, request.config)) return usage_error(*err);
  if (args.flag("json")) request.config.response_mime_type = "application/json";
  if (auto system = args.get("system")) request.system_instruction = Content::user(*system);

  const std::string prompt = prompt_from(args);
  if (prompt.find_first_not_of(" \t\r\n") == std::string::npos) return usage_error("empty prompt");
  request.contents.push_back(Content::user(prompt));

  auto client = make_client(args);
  if (!client) return api_error(client.error());

  Result<GenerateResponse> response;
  if (args.flag("no-stream")) {
    response = (*client)->generate(request);
    if (response) std::cout << response->text();
  } else {
    response = (*client)->stream(request, stdout_printer());
  }
  std::cout << "\n";
  if (!response) return api_error(response.error());
  if (g_interrupted.load()) std::cerr << "[interrupted]\n";
  if (response->finish_reason() == "MAX_TOKENS") std::cerr << "[truncated: hit max output tokens]\n";
  if (args.flag("usage")) print_usage_line(response->usage);
  return kExitOk;
}

// ---------------------------------------------------------------------------
int cmd_chat(int argc, char** argv) {
  cli::Args args;
  if (auto err = args.parse(argc, argv, 2, make_spec({}, {"tools"}))) return usage_error(*err);
  ChatOptions options;
  if (auto err = apply_common(args, options.config)) return usage_error(*err);
  if (auto system = args.get("system")) options.system_instruction = *system;

  auto client = make_client(args);
  if (!client) return api_error(client.error());

  ToolRegistry tools;
  if (args.flag("tools")) builtin_tools::register_defaults(tools);
  ChatSession chat(**client, options, args.flag("tools") ? &tools : nullptr);
  chat.set_tool_observer([](const FunctionCall& call, const FunctionResponse& result) {
    std::cerr << "  \033[2m-> " << call.name << "(" << call.args.dump() << ") = "
              << result.response.dump() << "\033[0m\n";
  });

  const bool tty = ::isatty(STDIN_FILENO) != 0;
  if (tty) {
    std::cerr << "gemini-cpp chat with " << (*client)->model()
              << (args.flag("tools") ? " (tools enabled)" : "")
              << ". Ctrl-C cancels a reply, Ctrl-D or /exit quits.\n";
  }

  std::string line;
  while (true) {
    if (tty) std::cerr << "\033[1myou>\033[0m " << std::flush;
    if (!std::getline(std::cin, line)) break;
    if (line.find_first_not_of(" \t") == std::string::npos) continue;
    if (line == "/exit" || line == "/quit") break;
    if (line == "/reset") {
      chat.reset();
      std::cerr << "[history cleared]\n";
      continue;
    }
    if (line == "/usage") {
      print_usage_line(chat.usage());
      continue;
    }
    if (line == "/history") {
      for (const auto& c : chat.history()) {
        std::cerr << to_string(c.role) << ": " << c.text() << (c.text().empty() ? "<tool data>" : "")
                  << "\n";
      }
      continue;
    }

    g_interrupted.store(false);
    if (tty) std::cerr << "\033[1mgemini>\033[0m " << std::flush;
    auto reply = chat.send(line, stdout_printer());
    std::cout << "\n" << std::flush;
    if (!reply) {
      if (reply.error().code == ErrorCode::kCancelled) {
        std::cerr << "[cancelled]\n";
      } else {
        std::cerr << "error: " << reply.error().describe() << "\n";
      }
    }
  }
  return kExitOk;
}

// ---------------------------------------------------------------------------
int cmd_tokens(int argc, char** argv) {
  cli::Args args;
  if (auto err = args.parse(argc, argv, 2, make_spec({}, {}))) return usage_error(*err);
  GenerateRequest request;
  if (auto err = apply_common(args, request.config)) return usage_error(*err);
  request.config = {};  // countTokens rejects generation config
  if (auto system = args.get("system")) request.system_instruction = Content::user(*system);
  request.contents.push_back(Content::user(prompt_from(args)));

  auto client = make_client(args);
  if (!client) return api_error(client.error());
  auto count = (*client)->count_tokens(request);
  if (!count) return api_error(count.error());
  std::cout << *count << "\n";
  return kExitOk;
}

// ---------------------------------------------------------------------------
int cmd_models(int argc, char** argv) {
  cli::Args args;
  if (auto err = args.parse(argc, argv, 2, make_spec({}, {"all"}))) return usage_error(*err);
  GenerationConfig unused;
  if (auto err = apply_common(args, unused)) return usage_error(*err);

  auto client = make_client(args);
  if (!client) return api_error(client.error());
  auto models = (*client)->list_models();
  if (!models) return api_error(models.error());
  for (const auto& m : *models) {
    const bool generates = std::find(m.supported_methods.begin(), m.supported_methods.end(),
                                     "generateContent") != m.supported_methods.end();
    if (!generates && !args.flag("all")) continue;
    std::string name = m.name.starts_with("models/") ? m.name.substr(7) : m.name;
    std::printf("%-40s in=%-8lld out=%-7lld %s\n", name.c_str(),
                static_cast<long long>(m.input_token_limit), static_cast<long long>(m.output_token_limit),
                m.display_name.c_str());
  }
  return kExitOk;
}

// ---------------------------------------------------------------------------
int cmd_eval(int argc, char** argv) {
  cli::Args args;
  const auto spec = make_spec({"tasks", "task", "jobs", "repair", "compiler", "std", "work-dir",
                               "report-json", "report-md", "min-pass"},
                              {"sanitize", "reference", "keep"});
  if (auto err = args.parse(argc, argv, 2, spec)) return usage_error(*err);

  eval::RunnerOptions options;
  if (auto err = apply_common(args, options.generation)) return usage_error(*err);
  std::optional<int> jobs, repair;
  std::optional<double> min_pass;
  if (auto err = args.get_int("jobs", jobs)) return usage_error(*err);
  if (auto err = args.get_int("repair", repair)) return usage_error(*err);
  if (auto err = args.get_double("min-pass", min_pass)) return usage_error(*err);
  options.jobs = jobs.value_or(1);
  options.repair_rounds = repair.value_or(0);
  options.sanitize = args.flag("sanitize");
  options.use_reference = args.flag("reference");
  options.keep_workdirs = args.flag("keep");
  const char* cxx = std::getenv("CXX");
  options.compiler = args.get_or("compiler", (cxx != nullptr && *cxx != '\0') ? cxx : "c++");
  options.std_flag = args.get_or("std", "-std=c++20");
  if (auto dir = args.get("work-dir")) options.work_root = *dir;

  auto tasks = eval::load_tasks(args.get_or("tasks", "tasks"));
  if (!tasks) return api_error(tasks.error());
  if (auto only = args.get_all("task"); !only.empty()) {
    std::erase_if(*tasks, [&](const eval::Task& t) {
      return std::find(only.begin(), only.end(), t.id) == only.end();
    });
  }
  if (tasks->empty()) return usage_error("no tasks selected");

  std::unique_ptr<Client> client;
  if (!options.use_reference) {
    auto made = make_client(args);
    if (!made) return api_error(made.error());
    client = std::move(*made);
  }

  std::cerr << "Evaluating " << tasks->size() << " task(s) with "
            << (options.use_reference ? std::string("reference solutions") : client->model())
            << " (jobs=" << options.jobs << ", repair=" << options.repair_rounds
            << (options.sanitize ? ", sanitizers" : "") << ")\n";

  eval::Runner runner(client.get(), options);
  std::size_t done = 0;
  const auto report = runner.run(*tasks, [&](const eval::TaskResult& r) {
    ++done;
    std::cerr << "[" << done << "/" << tasks->size() << "] " << r.task_id << ": "
              << (r.passed() ? "\033[32m" : "\033[31m") << eval::to_string(r.outcome) << "\033[0m"
              << " (" << r.attempts.size() << " attempt" << (r.attempts.size() == 1 ? "" : "s") << ")\n";
    if (!r.passed() && !r.attempts.empty()) {
      const auto& detail = r.attempts.back().detail;
      std::istringstream lines(detail);
      std::string l;
      for (int i = 0; i < 6 && std::getline(lines, l); ++i) std::cerr << "      " << l << "\n";
    }
  });

  const std::string markdown = eval::to_markdown(report);
  std::cout << markdown;
  if (auto path = args.get("report-md")) {
    std::ofstream(*path) << markdown;
  }
  if (auto path = args.get("report-json")) {
    std::ofstream(*path) << eval::to_json(report).dump(2) << "\n";
  }
  if (client) {
    const auto m = client->metrics();
    std::cerr << "[api] requests=" << m.requests << " attempts=" << m.attempts << " retries=" << m.retries
              << " failures=" << m.failures << " latency_ms=" << m.total_latency_ms << "\n";
  }

  const double rate = static_cast<double>(report.passed()) / static_cast<double>(report.results.size());
  return rate + 1e-9 >= min_pass.value_or(1.0) ? kExitOk : kExitFailure;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, on_sigint);

  if (argc < 2) {
    std::cerr << kUsage;
    return kExitUsage;
  }
  const std::string_view command = argv[1];
  if (command == "--help" || command == "-h" || command == "help") {
    std::cout << kUsage;
    return kExitOk;
  }
  if (command == "--version" || command == "version") {
    std::cout << "gemini-cpp " GEMINI_CPP_VERSION "\n";
    return kExitOk;
  }
  for (int i = 2; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--help") {
      std::cout << kUsage;
      return kExitOk;
    }
  }
  if (command == "ask") return cmd_ask(argc, argv);
  if (command == "chat") return cmd_chat(argc, argv);
  if (command == "tokens") return cmd_tokens(argc, argv);
  if (command == "models") return cmd_models(argc, argv);
  if (command == "eval") return cmd_eval(argc, argv);
  return usage_error("unknown command '" + std::string(command) + "'");
}
