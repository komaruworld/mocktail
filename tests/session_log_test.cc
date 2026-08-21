#include "runtime/session_log.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
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

}  // namespace
}  // namespace runtime
}  // namespace mocktail
