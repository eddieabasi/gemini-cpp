#include "gemini/eval/process.hpp"

#include <gtest/gtest.h>

#include <csignal>
#include <filesystem>
#include <thread>
#include <vector>

using namespace gemini::eval;
using std::chrono::milliseconds;

TEST(Process, CapturesStdoutAndStderr) {
  auto r = run_process({.argv = {"sh", "-c", "echo out; echo err 1>&2; exit 3"}});
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->exit_code, 3);
  EXPECT_FALSE(r->ok());
  EXPECT_NE(r->output.find("out"), std::string::npos);
  EXPECT_NE(r->output.find("err"), std::string::npos);
}

TEST(Process, RunsInWorkingDirectory) {
  const auto dir = std::filesystem::temp_directory_path();
  auto r = run_process({.argv = {"pwd"}, .cwd = dir});
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->ok());
  EXPECT_EQ(std::filesystem::canonical(r->output.substr(0, r->output.find('\n'))),
            std::filesystem::canonical(dir));
}

TEST(Process, KillsProcessTreeOnTimeout) {
  const auto start = std::chrono::steady_clock::now();
  // The grandchild keeps the pipe open; it must be killed too or we would hang.
  auto r = run_process({.argv = {"sh", "-c", "sleep 30 & sleep 30"}, .timeout = milliseconds(300)});
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->timed_out);
  EXPECT_FALSE(r->ok());
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
}

TEST(Process, ReportsSignals) {
  auto r = run_process({.argv = {"sh", "-c", "kill -SEGV $$"}});
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->term_signal, SIGSEGV);
  EXPECT_NE(r->status_string().find("signal"), std::string::npos);
}

TEST(Process, CapsOutput) {
  auto r = run_process({.argv = {"sh", "-c", "yes | head -c 200000"}, .max_output_bytes = 1000});
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->ok());
  EXPECT_EQ(r->output.size(), 1000u);
  EXPECT_TRUE(r->output_truncated);
}

TEST(Process, MissingExecutable) {
  auto r = run_process({.argv = {"definitely-not-a-real-binary-xyz"}});
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->exit_code, 127);
  EXPECT_NE(r->output.find("exec failed"), std::string::npos);
}

TEST(Process, ConcurrentSpawnsDoNotLeakPipes) {
  std::vector<std::thread> threads;
  std::atomic<int> ok{0};
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 10; ++i) {
        auto r = run_process({.argv = {"sh", "-c", "echo hi"}, .timeout = milliseconds(5000)});
        if (r && r->ok() && r->output == "hi\n") ++ok;
      }
    });
  }
  // A long-running sibling must not delay EOF detection for the short ones.
  auto slow = run_process({.argv = {"sleep", "1"}, .timeout = milliseconds(5000)});
  for (auto& t : threads) t.join();
  EXPECT_TRUE(slow && slow->ok());
  EXPECT_EQ(ok.load(), 80);
}

TEST(Process, TimeoutClockStartsAtMarker) {
  // Slow "startup" before the marker is covered by the grace period...
  auto r = run_process({.argv = {"sh", "-c", "sleep 1; printf 'MARK'; echo done"},
                        .timeout = milliseconds(500),
                        .start_marker = "MARK",
                        .startup_grace = milliseconds(10000)});
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->ok()) << r->status_string();
  EXPECT_EQ(r->output, "done\n");  // marker stripped
  ASSERT_TRUE(r->startup.has_value());
  EXPECT_GE(*r->startup, milliseconds(900));

  // ...but once started, the normal budget applies.
  auto slow = run_process({.argv = {"sh", "-c", "printf 'MARK'; sleep 5"},
                           .timeout = milliseconds(300),
                           .start_marker = "MARK",
                           .startup_grace = milliseconds(10000)});
  ASSERT_TRUE(slow.has_value());
  EXPECT_TRUE(slow->timed_out);
  EXPECT_LT(slow->duration, milliseconds(3000));
}
