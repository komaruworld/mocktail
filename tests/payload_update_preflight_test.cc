#include "runtime/payload_update_preflight.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef MOCKTAIL_TEST_UPDATE_HELPER
#error "MOCKTAIL_TEST_UPDATE_HELPER is required"
#endif

#ifndef MOCKTAIL_TEST_PROGRESS_HELPER
#error "MOCKTAIL_TEST_PROGRESS_HELPER is required"
#endif

namespace mocktail::runtime {
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
    char pattern[] = "/tmp/mocktail_update_preflight_XXXXXX";
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

class ScopedVariable final {
 public:
  ScopedVariable(const char* name, const std::string& value) : name_(name) {
    const char* previous = std::getenv(name);
    if (previous != nullptr) previous_ = previous;
    EXPECT_EQ(setenv(name, value.c_str(), 1), 0);
  }

  ~ScopedVariable() {
    if (previous_.has_value()) {
      (void)setenv(name_.c_str(), previous_->c_str(), 1);
    } else {
      (void)unsetenv(name_.c_str());
    }
  }

 private:
  std::string name_;
  std::optional<std::string> previous_;
};

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(input, line);) lines.push_back(line);
  return lines;
}

MapEnvironment NativeEnvironment(
    std::unordered_map<std::string, std::string> additions = {}) {
  additions["MOCKTAIL_UPDATE_HELPER"] = MOCKTAIL_TEST_UPDATE_HELPER;
  return MapEnvironment(std::move(additions));
}

TEST(PayloadUpdatePreflightTest, RunsNativeUpdater) {
  TemporaryDirectory temporary;
  const MapEnvironment environment = NativeEnvironment();
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  const auto result = RunPayloadUpdatePreflight(environment, paths);
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.attempted);
}

TEST(PayloadUpdatePreflightTest, RunsPackagedUpdaterFromProjectBin) {
  TemporaryDirectory temporary;
  const std::filesystem::path project_root = temporary.root() / "portable";
  const std::filesystem::path helper = project_root / "bin/mocktail_updater";
  ASSERT_TRUE(std::filesystem::create_directories(helper.parent_path()));
  ASSERT_TRUE(std::filesystem::copy_file(MOCKTAIL_TEST_UPDATE_HELPER, helper));
  std::filesystem::permissions(
      helper,
      std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
          std::filesystem::perms::others_exec,
      std::filesystem::perm_options::add);
  const MapEnvironment environment(
      {{"MOCKTAIL_PROJECT_ROOT", project_root.string()}});
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);

  const auto result = RunPayloadUpdatePreflight(environment, paths);

  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.attempted);
}

TEST(PayloadUpdatePreflightTest, StreamsUpdaterStagesToNativeDialog) {
  TemporaryDirectory temporary;
  const std::filesystem::path capture = temporary.root() / "progress";
  ScopedVariable capture_variable("MOCKTAIL_TEST_PROGRESS_CAPTURE",
                                  capture.string());
  const MapEnvironment environment = NativeEnvironment({
      {"MOCKTAIL_UPDATE_PROGRESS_HELPER", MOCKTAIL_TEST_PROGRESS_HELPER},
      {"DISPLAY", ":test"},
  });
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  const auto result = RunPayloadUpdatePreflight(environment, paths);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(ReadLines(capture),
            std::vector<std::string>({"PDownloading Roblox...", "S"}));
}

TEST(PayloadUpdatePreflightTest, PassesExactResolvedXdgPathsToUpdater) {
  TemporaryDirectory temporary;
  const std::filesystem::path home = temporary.root() / "home";
  const std::filesystem::path capture = temporary.root() / "paths";
  ScopedVariable capture_variable("MOCKTAIL_TEST_UPDATE_CAPTURE",
                                  capture.string());
  const MapEnvironment environment =
      NativeEnvironment({{"HOME", home.string()}});
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  const auto result = RunPayloadUpdatePreflight(environment, paths);
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(ReadLines(capture),
            std::vector<std::string>({
                (home / ".config/mocktail/config.yaml").string(),
                (home / ".local/share/mocktail").string(),
                (home / ".cache/mocktail").string(),
                (home / ".local/state/mocktail").string(),
            }));
}

TEST(PayloadUpdatePreflightTest, ExplicitLibrarySkipsUpdater) {
  ScopedVariable exit_variable("MOCKTAIL_TEST_UPDATE_EXIT", "7");
  const MapEnvironment environment =
      NativeEnvironment({{"ROBLOX_LIB_PATH", "/tmp/fixture.so"}});
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  const auto result = RunPayloadUpdatePreflight(environment, paths);
  ASSERT_TRUE(result) << result.error;
  EXPECT_FALSE(result.attempted);
}

TEST(PayloadUpdatePreflightTest, RunnableManagedPayloadChecksForLatest) {
  TemporaryDirectory temporary;
  const std::filesystem::path data_root = temporary.root() / "data";
  constexpr std::string_view kBuildId =
      "1686400865ae0e408cd7bd67de7a439625c6fd13";
  const std::string payload_id = "2628-" + std::string(kBuildId);
  const std::filesystem::path payload_root =
      data_root / "payloads" / payload_id;
  ASSERT_TRUE(
      std::filesystem::create_directories(payload_root / "assets/content"));
  std::ofstream(payload_root / "libroblox.so") << "fixture";
  std::ofstream(data_root / "current.json")
      << "{\"schema_version\":1,\"payload_id\":\"" << payload_id
      << "\",\"payload_path\":\"payloads/" << payload_id
      << "\",\"version_name\":\"2.727.1199\",\"version_code\":2628,"
         "\"elf_build_id\":\""
      << kBuildId << "\"}\n";
  const MapEnvironment environment =
      NativeEnvironment({{"MOCKTAIL_DATA_ROOT", data_root.string()}});
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  const auto result = RunPayloadUpdatePreflight(environment, paths);
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.attempted);
}

TEST(PayloadUpdatePreflightTest, LegacyProbationPayloadCrossesNativeUpdater) {
  TemporaryDirectory temporary;
  const std::filesystem::path data_root = temporary.root() / "data";
  constexpr std::string_view kBuildId =
      "48fc8ee1fb36fc39072fd8619154ce90eea4b316";
  constexpr std::string_view kGeneration =
      "0123456789abcdef0123456789abcdef01234567";
  const std::string payload_id = "2718-" + std::string(kBuildId);
  const std::filesystem::path payload_root =
      data_root / "payloads" / payload_id;
  ASSERT_TRUE(
      std::filesystem::create_directories(payload_root / "assets/content"));
  std::ofstream(payload_root / "libroblox.so") << "fixture";
  const std::string artifact =
      payload_id + "-" + std::string(kGeneration) + ".json";
  for (const std::string_view directory :
       {"approvals", "host_abi_profiles", "compatibility_profiles"}) {
    ASSERT_TRUE(std::filesystem::create_directories(data_root / directory));
    std::ofstream(data_root / directory / artifact) << "{}\n";
  }
  std::ofstream(data_root / "current.json")
      << "{\"schema_version\":1,\"payload_id\":\"" << payload_id
      << "\",\"payload_path\":\"payloads/" << payload_id
      << "\",\"version_name\":\"2.730.790\",\"version_code\":2718,"
         "\"elf_build_id\":\""
      << kBuildId << "\",\"approval_path\":\"approvals/" << artifact
      << "\",\"host_abi_profile_path\":\"host_abi_profiles/" << artifact
      << "\",\"compatibility_manifest_path\":\"compatibility_profiles/"
      << artifact << "\"}\n";
  const MapEnvironment environment =
      NativeEnvironment({{"MOCKTAIL_DATA_ROOT", data_root.string()}});
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  const auto result = RunPayloadUpdatePreflight(environment, paths);
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.attempted);
}

TEST(PayloadUpdatePreflightTest, RejectsRelativeUpdaterOverride) {
  const MapEnvironment environment(
      {{"MOCKTAIL_UPDATE_HELPER", "build/mocktail_updater"}});
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  const auto result = RunPayloadUpdatePreflight(environment, paths);
  ASSERT_TRUE(result) << result.error;
  EXPECT_FALSE(result.attempted);
}

TEST(PayloadUpdatePreflightTest, PropagatesUpdaterFailure) {
  ScopedVariable exit_variable("MOCKTAIL_TEST_UPDATE_EXIT", "7");
  const MapEnvironment environment = NativeEnvironment();
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  const auto result = RunPayloadUpdatePreflight(environment, paths);
  EXPECT_FALSE(result);
  EXPECT_TRUE(result.attempted);
  EXPECT_EQ(result.error, "Roblox update preflight failed");
}

}  // namespace
}  // namespace mocktail::runtime
