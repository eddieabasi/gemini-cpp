#include "gemini/eval/process.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace gemini::eval {

namespace {

// Serialises pipe creation + fork so that a pipe created by one thread is marked
// close-on-exec before another thread forks; otherwise the write end could leak into an
// unrelated child and we would never see EOF.
std::mutex g_spawn_mutex;

void set_cloexec(int fd) { ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC); }

using Clock = std::chrono::steady_clock;

int remaining_ms(Clock::time_point deadline) {
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
  return left.count() <= 0 ? 0 : static_cast<int>(std::min<long long>(left.count(), 1'000'000));
}

std::unexpected<Error> io_error(const char* what) {
  return make_error(ErrorCode::kIo, std::string(what) + ": " + std::strerror(errno));
}

}  // namespace

std::string ProcessResult::status_string() const {
  if (timed_out) return "timed out after " + std::to_string(duration.count()) + " ms";
  if (term_signal != 0) {
    const char* name = ::strsignal(term_signal);
    return "killed by signal " + std::to_string(term_signal) + (name ? std::string(" (") + name + ")" : "");
  }
  return "exit code " + std::to_string(exit_code);
}

Result<ProcessResult> run_process(const ProcessOptions& options) {
  if (options.argv.empty()) return make_error(ErrorCode::kInvalidArgument, "empty argv");

  // Everything the child needs is prepared before fork(): after fork only async-signal-safe
  // calls are allowed in a multi-threaded program.
  std::vector<char*> argv;
  argv.reserve(options.argv.size() + 1);
  for (const auto& arg : options.argv) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);
  const std::string cwd = options.cwd.string();

  int fds[2] = {-1, -1};
  pid_t pid = -1;
  const auto start = Clock::now();
  {
    std::lock_guard lock(g_spawn_mutex);
    if (::pipe(fds) != 0) return io_error("pipe");
    set_cloexec(fds[0]);
    set_cloexec(fds[1]);
    pid = ::fork();
    if (pid < 0) {
      ::close(fds[0]);
      ::close(fds[1]);
      return io_error("fork");
    }
    if (pid == 0) {
      ::setpgid(0, 0);
      if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(126);
      const int devnull = ::open("/dev/null", O_RDONLY);
      if (devnull >= 0) ::dup2(devnull, STDIN_FILENO);
      ::dup2(fds[1], STDOUT_FILENO);
      ::dup2(fds[1], STDERR_FILENO);
      rlimit no_core{0, 0};
      ::setrlimit(RLIMIT_CORE, &no_core);
#if defined(__linux__)
      if (options.memory_limit_bytes) {
        rlimit mem{static_cast<rlim_t>(*options.memory_limit_bytes),
                   static_cast<rlim_t>(*options.memory_limit_bytes)};
        ::setrlimit(RLIMIT_AS, &mem);
      }
#endif
      ::execvp(argv[0], argv.data());
      const char msg[] = "exec failed: ";
      (void)!::write(STDERR_FILENO, msg, sizeof msg - 1);
      (void)!::write(STDERR_FILENO, argv[0], std::strlen(argv[0]));
      (void)!::write(STDERR_FILENO, "\n", 1);
      ::_exit(127);
    }
  }
  ::setpgid(pid, pid);  // also done in the child; whichever runs first wins
  ::close(fds[1]);
  ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL) | O_NONBLOCK);

  ProcessResult result;
  auto deadline = start + options.timeout;
  const bool await_marker = !options.start_marker.empty();
  if (await_marker) deadline += options.startup_grace;
  auto kill_group = [&] {
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
    result.timed_out = true;
  };

  char buffer[16 * 1024];
  bool eof = false;
  while (!eof) {
    const int wait = remaining_ms(deadline);
    if (wait == 0) {
      kill_group();
      break;
    }
    pollfd pfd{fds[0], POLLIN, 0};
    const int ready = ::poll(&pfd, 1, wait);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) continue;  // loop re-checks the deadline
    for (;;) {
      const ssize_t n = ::read(fds[0], buffer, sizeof buffer);
      if (n > 0) {
        const auto room = options.max_output_bytes - std::min(options.max_output_bytes, result.output.size());
        const auto take = std::min(room, static_cast<std::size_t>(n));
        const std::size_t before = result.output.size();
        result.output.append(buffer, take);
        if (take < static_cast<std::size_t>(n)) result.output_truncated = true;
        if (await_marker && !result.startup) {
          const std::size_t from = before >= options.start_marker.size() ? before - options.start_marker.size() : 0;
          if (const auto at = result.output.find(options.start_marker, from); at != std::string::npos) {
            const auto now = Clock::now();
            result.startup = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
            result.output.erase(at, options.start_marker.size());
            deadline = now + options.timeout;
          }
        }
        continue;
      }
      if (n == 0) eof = true;
      else if (errno == EINTR) continue;
      break;  // EAGAIN or real error
    }
  }
  ::close(fds[0]);

  // The child may close its output early and keep running, so waiting is also bounded.
  int status = 0;
  for (;;) {
    const pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) break;
    if (r < 0 && errno != EINTR) return io_error("waitpid");
    if (!result.timed_out && Clock::now() >= deadline) kill_group();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (!result.timed_out) ::kill(-pid, SIGKILL);  // reap stray grandchildren, ignore errors

  result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.term_signal = WTERMSIG(status);
  }
  if (result.timed_out) result.term_signal = 0;
  return result;
}

}  // namespace gemini::eval
