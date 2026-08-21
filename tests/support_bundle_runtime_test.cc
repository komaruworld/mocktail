#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/support_bundle.h"

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
    char pattern[] = "/tmp/mocktail_support_bundle_XXXXXX";
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

std::filesystem::path OnlyBundle(const std::filesystem::path& output) {
  std::vector<std::filesystem::path> bundles;
  for (const auto& entry : std::filesystem::directory_iterator(output)) {
    if (entry.is_directory()) bundles.push_back(entry.path());
  }
  EXPECT_EQ(bundles.size(), 1);
  return bundles.empty() ? std::filesystem::path{} : bundles.front();
}

TEST(SupportBundleRuntimeTest, WritesPrivateSanitizedNativeBundle) {
  TemporaryDirectory temporary;
  const std::filesystem::path home = temporary.root() / "home";
  const std::filesystem::path state = temporary.root() / "state";
  const std::filesystem::path output = temporary.root() / "support-output";
  const std::filesystem::path log = temporary.root() / "runtime.log";
  ASSERT_TRUE(std::filesystem::create_directories(state));
  std::ofstream(log) << "[vulkan] good diagnostic\n"
                     << "Authorization: secret-token\n"
                     << "[FATAL] safe failure marker\n"
                     << "https://example.invalid/private\n";
  const MapEnvironment environment({
      {"HOME", home.string()},
      {"MOCKTAIL_STATE_ROOT", state.string()},
      {"MOCKTAIL_SUPPORT_OUTPUT_ROOT", output.string()},
  });
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);

  const SupportBundleCollectionResult result = CollectFailureSupportBundle(
      environment, paths, "launch", "launch-failed", 17, log);

  ASSERT_TRUE(result) << result.error;
  ASSERT_TRUE(result.attempted);
  const std::filesystem::path bundle = OnlyBundle(output);
  ASSERT_FALSE(bundle.empty());
  struct stat metadata = {};
  ASSERT_EQ(stat(bundle.c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & 0777, 0700);
  EXPECT_NE(ReadFile(bundle / "runtime.txt").find("format=native-directory"),
            std::string::npos);
  const std::string recent = ReadFile(bundle / "recent.log");
  EXPECT_NE(recent.find("[vulkan] good diagnostic"), std::string::npos);
  EXPECT_NE(recent.find("[FATAL] safe failure marker"), std::string::npos);
  EXPECT_EQ(recent.find("secret-token"), std::string::npos);
  EXPECT_EQ(recent.find("example.invalid"), std::string::npos);
  for (const std::string_view filename :
       {"runtime.txt", "payload.txt", "recent.log"}) {
    ASSERT_EQ(stat((bundle / filename).c_str(), &metadata), 0);
    EXPECT_EQ(metadata.st_mode & 0777, 0600);
  }
}

TEST(SupportBundleRuntimeTest, DisableFlagSkipsNativeCollection) {
  TemporaryDirectory temporary;
  const MapEnvironment environment({
      {"HOME", temporary.root().string()},
      {"MOCKTAIL_DISABLE_SUPPORT_BUNDLE", "1"},
  });
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  const auto result = CollectFailureSupportBundle(environment, paths, "launch",
                                                  "launch-failed", 1);
  EXPECT_TRUE(result) << result.error;
  EXPECT_FALSE(result.attempted);
}

TEST(SupportBundleRuntimeTest, RejectsUnstructuredReason) {
  TemporaryDirectory temporary;
  const MapEnvironment environment({{"HOME", temporary.root().string()}});
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  const auto result = CollectFailureSupportBundle(
      environment, paths, "launch", "cookie=must-not-be-forwarded", 1);
  EXPECT_FALSE(result);
  EXPECT_FALSE(result.attempted);
}

}  // namespace
}  // namespace mocktail::runtime
