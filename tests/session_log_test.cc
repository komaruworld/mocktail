#include "runtime/session_log.h"
#include "runtime/process_diagnostics.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

class MapEnvironment final : public Environment {
 public:
  explicit MapEnvironment(std::unordered_map<std::string, std::string> values)
      : values_(std::move(values)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    const auto found = values_.find(std::string(name));
    return found == values_.end() ? std::nullopt
                                  : std::optional<std::string>(found->second);
  }

 private:
  std::unordered_map<std::string, std::string> values_;
};

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_session_log_XXXXXX";
    const char* created = mkdtemp(pattern);
    if (created != nullptr) root_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path root_;
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

TEST(SessionLogTest, CapturesBothStreamsAndUpdatesLatest) {
  TemporaryDirectory temporary;
  const std::filesystem::path home = temporary.root() / "home";
  const std::filesystem::path state = temporary.root() / "state";
  const MapEnvironment environment({
      {"HOME", home.string()},
      {"MOCKTAIL_STATE_ROOT", state.string()},
      {"WAYLAND_DISPLAY", "wayland-0"},
  });
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  std::filesystem::path session_path;
  std::filesystem::path latest_path;
  {
    SessionLog log = SessionLog::Start(environment, paths);
    ASSERT_TRUE(log) << log.error();
    session_path = log.path();
    latest_path = log.latest_path();
    std::cout << log.Header(environment, paths, "direct-vulkan")
              << "session-stdout-marker\n"
              << std::flush;
    std::cerr << "session-stderr-marker\n" << std::flush;
  }

  ASSERT_TRUE(std::filesystem::is_regular_file(session_path));
  ASSERT_TRUE(std::filesystem::exists(latest_path));
  EXPECT_EQ(std::filesystem::canonical(latest_path),
            std::filesystem::canonical(session_path));
  EXPECT_TRUE(std::regex_match(
      session_path.filename().string(),
      std::regex(
          R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}_[0-9]{2}-[0-9]{2}-[0-9]{2}\.log$)")));
  struct stat metadata = {};
  ASSERT_EQ(stat(session_path.c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & 0777, 0600);
  const std::string contents = ReadFile(session_path);
  EXPECT_EQ(contents.rfind("[mocktail] version=", 0), 0U);
  EXPECT_NE(contents.find("session-stdout-marker"), std::string::npos);
  EXPECT_NE(contents.find("session-stderr-marker"), std::string::npos);
  EXPECT_NE(contents.find(" commit="), std::string::npos);
  EXPECT_NE(contents.find(" target="), std::string::npos);
  EXPECT_NE(contents.find("[mocktail] cpu=\""), std::string::npos);
  EXPECT_NE(contents.find(" ram="), std::string::npos);
  EXPECT_NE(contents.find(" gpu=\""), std::string::npos);
  EXPECT_NE(contents.find(" gpu_driver=\""), std::string::npos);
  EXPECT_NE(contents.find("[mocktail] log="), std::string::npos);
}

TEST(SessionLogTest, PreservesSessionsStartedInTheSameSecond) {
  TemporaryDirectory temporary;
  const MapEnvironment environment({
      {"HOME", (temporary.root() / "home").string()},
      {"MOCKTAIL_STATE_ROOT", (temporary.root() / "state").string()},
  });
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  const auto timestamp = std::chrono::system_clock::from_time_t(1770000000);
  std::filesystem::path first;
  std::filesystem::path second;
  {
    SessionLog log = SessionLog::Start(environment, paths, timestamp);
    ASSERT_TRUE(log) << log.error();
    first = log.path();
  }
  {
    SessionLog log = SessionLog::Start(environment, paths, timestamp);
    ASSERT_TRUE(log) << log.error();
    second = log.path();
  }
  EXPECT_NE(first, second);
  EXPECT_TRUE(std::filesystem::is_regular_file(first));
  EXPECT_TRUE(std::filesystem::is_regular_file(second));
  EXPECT_EQ(second.stem().string().substr(second.stem().string().size() - 2),
            "-2");
  EXPECT_EQ(std::filesystem::canonical(paths.logs_root() / "latest.log"),
            std::filesystem::canonical(second));
}

TEST(SessionLogTest, SkipsIsolatedCanary) {
  TemporaryDirectory temporary;
  const MapEnvironment environment({
      {"HOME", temporary.root().string()},
      {"MOCKTAIL_ISOLATED_CANARY", "1"},
  });
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  SessionLog log = SessionLog::Start(environment, paths);
  EXPECT_FALSE(log);
  EXPECT_FALSE(log.attempted());
  EXPECT_FALSE(std::filesystem::exists(paths.logs_root()));
}

TEST(SessionLogTest, CpuLimitCrashReachesConsoleAndLogAndRemainsFatal) {
  TemporaryDirectory temporary;
  const MapEnvironment environment({
      {"HOME", temporary.root().string()},
      {"MOCKTAIL_STATE_ROOT", (temporary.root() / "state").string()},
  });
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  int console[2];
  ASSERT_EQ(pipe(console), 0);
  std::cout.flush();
  std::cerr.flush();
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    close(console[0]);
    if (dup2(console[1], STDOUT_FILENO) < 0 ||
        dup2(console[1], STDERR_FILENO) < 0) _exit(10);
    close(console[1]);
    alarm(10);
    rlimit core{};
    if (setrlimit(RLIMIT_CORE, &core) != 0) _exit(11);
    SessionLog log = SessionLog::Start(environment, paths);
    if (!log) _exit(12);
    if (signal(SIGXCPU, SIG_DFL) == SIG_ERR) _exit(13);
    InstallCpuLimitDiagnostics();
    LogProcessDiagnostics(ProcessDiagnosticStage::kNativeRuntime);
    // Set the limit after the logger forks, so only this test process expires.
    rlimit cpu{};
    cpu.rlim_cur = 1;
    cpu.rlim_max = 5;
    if (setrlimit(RLIMIT_CPU, &cpu) != 0) _exit(14);
    timespec used{};
    do {
      if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &used) != 0) _exit(15);
    } while (used.tv_sec < 3);
    _exit(16);
  }
  close(console[1]);
  std::string output;
  char buffer[4096];
  for (;;) {
    const ssize_t count = read(console[0], buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  close(console[0]);
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFSIGNALED(status)) << status << "\n" << output;
  EXPECT_EQ(WTERMSIG(status), SIGXCPU);
  const std::string logged = ReadFile(paths.logs_root() / "latest.log");
  // EOF on the console pipe means the tee has drained and closed both outputs.
  EXPECT_EQ(logged, output);
  EXPECT_NE(logged.find("[crash] SIGXCPU signal=24 si_code=128"), std::string::npos);
  EXPECT_NE(logged.find("stage=native-runtime"), std::string::npos);
  EXPECT_NE(logged.find("process_cpu_ns="), std::string::npos);
  EXPECT_NE(logged.find("thread_cpu_ns="), std::string::npos);
  EXPECT_NE(logged.find("Max cpu time"), std::string::npos);
  EXPECT_NE(logged.find("Max realtime timeout"), std::string::npos);
  EXPECT_NE(logged.find("/proc/thread-self/sched"), std::string::npos);
  EXPECT_NE(logged.find("/proc/self/maps"), std::string::npos);
  EXPECT_NE(logged.find("SIGXCPU diagnostics complete"), std::string::npos);
}

TEST(SessionLogTest, CpuDiagnosticsPreservesExistingSignalDisposition) {
  ASSERT_EXIT(
      {
        if (signal(SIGXCPU, SIG_IGN) == SIG_ERR) _exit(10);
        InstallCpuLimitDiagnostics();
        struct sigaction action{};
        if (sigaction(SIGXCPU, nullptr, &action) != 0) _exit(11);
        _exit(action.sa_handler == SIG_IGN ? 0 : 12);
      },
      ::testing::ExitedWithCode(0), "existing SIGXCPU handler retained");
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
