#include "runtime/single_instance_lock.h"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

constexpr char kNamespaceProbeDescriptor[] =
    "MOCKTAIL_TEST_NAMESPACE_PROBE_DESCRIPTOR";
constexpr char kNamespaceProbeMutationAppDir[] =
    "MOCKTAIL_TEST_NAMESPACE_PROBE_MUTATION_APPDIR";
constexpr char kNamespaceProbeMutationAppImage[] =
    "MOCKTAIL_TEST_NAMESPACE_PROBE_MUTATION_APPIMAGE";

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

struct NamespaceProbeOptions {
  std::optional<std::string> app_image;
  std::optional<std::string> app_dir;
  std::optional<std::string> mutation_app_image;
  std::optional<std::string> mutation_app_dir;
};

struct NamespaceProbeResult {
  int wait_status = -1;
  std::string output;
};

bool ConfigureChildEnvironment(
    const char* name, const std::optional<std::string>& value) {
  return value.has_value() ? setenv(name, value->c_str(), 1) == 0
                           : unsetenv(name) == 0;
}

NamespaceProbeResult RunNamespaceProbe(const NamespaceProbeOptions& options) {
  NamespaceProbeResult result;
  std::array<int, 2> descriptors{};
  if (pipe(descriptors.data()) != 0) return result;
  const std::string descriptor = std::to_string(descriptors[1]);
  const pid_t child = fork();
  if (child < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    return result;
  }
  if (child == 0) {
    close(descriptors[0]);
    if (setenv(kNamespaceProbeDescriptor, descriptor.c_str(), 1) != 0 ||
        !ConfigureChildEnvironment("APPIMAGE", options.app_image) ||
        !ConfigureChildEnvironment("APPDIR", options.app_dir) ||
        !ConfigureChildEnvironment(kNamespaceProbeMutationAppImage,
                                   options.mutation_app_image) ||
        !ConfigureChildEnvironment(kNamespaceProbeMutationAppDir,
                                   options.mutation_app_dir)) {
      _exit(125);
    }
    execl("/proc/self/exe", "single_instance_lock_test",
          "--gtest_filter=SingleInstanceLockTest.ExecutableNamespaceProbe",
          "--gtest_brief=1", nullptr);
    _exit(127);
  }

  close(descriptors[1]);
  std::array<char, 128> buffer{};
  while (true) {
    const ssize_t read_result =
        read(descriptors[0], buffer.data(), buffer.size());
    if (read_result > 0) {
      result.output.append(buffer.data(),
                           static_cast<std::size_t>(read_result));
      continue;
    }
    if (read_result < 0 && errno == EINTR) continue;
    break;
  }
  close(descriptors[0]);
  while (waitpid(child, &result.wait_status, 0) < 0 && errno == EINTR) {
  }
  return result;
}

bool WriteAll(int descriptor, std::string_view value) {
  while (!value.empty()) {
    const ssize_t write_result =
        write(descriptor, value.data(), value.size());
    if (write_result > 0) {
      value.remove_prefix(static_cast<std::size_t>(write_result));
      continue;
    }
    if (write_result < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

class SingleInstanceLockTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("mocktail-instance-test-" + std::to_string(getpid()));
    std::filesystem::remove_all(root_);
    ASSERT_TRUE(std::filesystem::create_directories(root_));
    lock_file_ = root_ / "instance.lock";
  }

  void TearDown() override { std::filesystem::remove_all(root_); }

  std::filesystem::path root_;
  std::filesystem::path lock_file_;
};

TEST_F(SingleInstanceLockTest, RejectsSecondConcurrentOwner) {
  SingleInstanceLock first = SingleInstanceLock::Acquire(lock_file_);
  ASSERT_TRUE(first.acquired()) << first.error();

  SingleInstanceLock second = SingleInstanceLock::Acquire(lock_file_);
  EXPECT_TRUE(second.already_running());
  EXPECT_FALSE(second.acquired());
}

TEST_F(SingleInstanceLockTest, ReleasesLockWithOwnerLifetime) {
  {
    SingleInstanceLock first = SingleInstanceLock::Acquire(lock_file_);
    ASSERT_TRUE(first.acquired()) << first.error();
  }

  SingleInstanceLock replacement = SingleInstanceLock::Acquire(lock_file_);
  EXPECT_TRUE(replacement.acquired()) << replacement.error();
}

TEST_F(SingleInstanceLockTest, UsesStableExecutableNamespace) {
  const std::string first = CurrentExecutableInstanceKey();
  const std::string second = CurrentExecutableInstanceKey();
  EXPECT_EQ(first, second);
  EXPECT_TRUE(std::regex_match(first, std::regex("^[0-9a-f]{16}$")));
}

TEST_F(SingleInstanceLockTest, ExecutableNamespaceProbe) {
  const char* descriptor_value = std::getenv(kNamespaceProbeDescriptor);
  if (descriptor_value == nullptr) {
    EXPECT_TRUE(std::regex_match(CurrentExecutableInstanceKey(),
                                 std::regex("^[0-9a-f]{16}$")));
    return;
  }
  char* descriptor_end = nullptr;
  const long descriptor =
      std::strtol(descriptor_value, &descriptor_end, 10);
  ASSERT_NE(descriptor_end, descriptor_value);
  ASSERT_EQ(*descriptor_end, '\0');
  ASSERT_GE(descriptor, 0);

  std::string output = CurrentExecutableInstanceKey();
  const char* mutation_app_image =
      std::getenv(kNamespaceProbeMutationAppImage);
  const char* mutation_app_dir = std::getenv(kNamespaceProbeMutationAppDir);
  if (mutation_app_image != nullptr && mutation_app_dir != nullptr) {
    ASSERT_EQ(setenv("APPIMAGE", mutation_app_image, 1), 0);
    ASSERT_EQ(setenv("APPDIR", mutation_app_dir, 1), 0);
    output += "\n" + CurrentExecutableInstanceKey();
  }
  ASSERT_TRUE(WriteAll(static_cast<int>(descriptor), output));
  ASSERT_EQ(close(static_cast<int>(descriptor)), 0);
}

TEST_F(SingleInstanceLockTest, SeparatesValidatedAppImageInstallationPaths) {
  std::error_code error;
  const std::filesystem::path executable =
      std::filesystem::canonical("/proc/self/exe", error);
  ASSERT_FALSE(error) << error.message();
  const std::string app_dir = executable.parent_path().string();

  const NamespaceProbeResult first = RunNamespaceProbe(
      {{"/tmp/mocktail-one.AppImage"}, {app_dir}, std::nullopt, std::nullopt});
  const NamespaceProbeResult second = RunNamespaceProbe(
      {{"/tmp/mocktail-two.AppImage"}, {app_dir}, std::nullopt, std::nullopt});
  ASSERT_TRUE(WIFEXITED(first.wait_status));
  ASSERT_EQ(WEXITSTATUS(first.wait_status), 0);
  ASSERT_TRUE(WIFEXITED(second.wait_status));
  ASSERT_EQ(WEXITSTATUS(second.wait_status), 0);
  EXPECT_NE(first.output, second.output);
}

TEST_F(SingleInstanceLockTest, IgnoresInheritedAppImageOutsideAppDir) {
  const std::filesystem::path foreign_app_dir = root_ / "foreign-appdir";
  ASSERT_TRUE(std::filesystem::create_directories(foreign_app_dir));
  const NamespaceProbeResult baseline = RunNamespaceProbe({});
  const NamespaceProbeResult inherited = RunNamespaceProbe(
      {{"/tmp/foreign-browser.AppImage"}, {foreign_app_dir.string()},
       std::nullopt, std::nullopt});
  const NamespaceProbeResult missing_app_dir = RunNamespaceProbe(
      {{"/tmp/foreign-browser.AppImage"}, std::nullopt, std::nullopt,
       std::nullopt});
  ASSERT_TRUE(WIFEXITED(baseline.wait_status));
  ASSERT_EQ(WEXITSTATUS(baseline.wait_status), 0);
  ASSERT_TRUE(WIFEXITED(inherited.wait_status));
  ASSERT_EQ(WEXITSTATUS(inherited.wait_status), 0);
  ASSERT_TRUE(WIFEXITED(missing_app_dir.wait_status));
  ASSERT_EQ(WEXITSTATUS(missing_app_dir.wait_status), 0);
  EXPECT_EQ(inherited.output, baseline.output);
  EXPECT_EQ(missing_app_dir.output, baseline.output);
}

TEST_F(SingleInstanceLockTest, CachesExecutableNamespaceForProcessLifetime) {
  std::error_code error;
  const std::filesystem::path executable =
      std::filesystem::canonical("/proc/self/exe", error);
  ASSERT_FALSE(error) << error.message();
  const NamespaceProbeResult result = RunNamespaceProbe(
      {std::nullopt, std::nullopt, {"/tmp/mutated-mocktail.AppImage"},
       {executable.parent_path().string()}});
  ASSERT_TRUE(WIFEXITED(result.wait_status));
  ASSERT_EQ(WEXITSTATUS(result.wait_status), 0);
  const std::size_t separator = result.output.find('\n');
  ASSERT_NE(separator, std::string::npos);
  EXPECT_EQ(result.output.substr(0, separator),
            result.output.substr(separator + 1));
}

TEST_F(SingleInstanceLockTest, RejectsSymlinkLockFile) {
  const std::filesystem::path target = root_ / "target";
  std::ofstream(target) << "not a lock";
  std::filesystem::create_symlink(target, lock_file_);
  ASSERT_TRUE(std::filesystem::is_symlink(lock_file_));

  SingleInstanceLock lock = SingleInstanceLock::Acquire(lock_file_);
  EXPECT_EQ(lock.status(), SingleInstanceLock::Status::kError);
  EXPECT_FALSE(lock.error().empty());
}

TEST_F(SingleInstanceLockTest, IsolatedCanaryUsesStateRootLock) {
  const std::filesystem::path data = root_ / "data";
  const std::filesystem::path cache = root_ / "cache";
  const std::filesystem::path state = root_ / "state";
  ASSERT_TRUE(std::filesystem::create_directories(data));
  ASSERT_TRUE(std::filesystem::create_directories(cache));
  ASSERT_TRUE(std::filesystem::create_directories(state));
  const MapEnvironment environment({
      {"HOME", (root_ / "home").string()},
      {"MOCKTAIL_DATA_ROOT", data.string()},
      {"MOCKTAIL_CACHE_ROOT", cache.string()},
      {"MOCKTAIL_STATE_ROOT", state.string()},
      {"ROBLOX_LIB_PATH", (root_ / "libroblox.so").string()},
      {"MOCKTAIL_ASSET_PATH", (root_ / "assets").string()},
      {"MOCKTAIL_SKIP_UPDATE_CHECK", "1"},
      {"MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS", "5000"},
      {"MOCKTAIL_ISOLATED_CANARY", "1"},
  });
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);

  SingleInstanceLock first =
      SingleInstanceLock::AcquireForLaunch(environment, paths);
  ASSERT_TRUE(first.acquired()) << first.error();
  EXPECT_EQ(first.path(), state / "instance.lock");
  SingleInstanceLock second =
      SingleInstanceLock::AcquireForLaunch(environment, paths);
  EXPECT_TRUE(second.already_running());
}

TEST_F(SingleInstanceLockTest, RejectsCanaryWithSharedRuntimeRoots) {
  const MapEnvironment environment({
      {"HOME", (root_ / "home").string()},
      {"MOCKTAIL_DATA_ROOT", root_.string()},
      {"MOCKTAIL_CACHE_ROOT", root_.string()},
      {"MOCKTAIL_STATE_ROOT", root_.string()},
      {"ROBLOX_LIB_PATH", (root_ / "libroblox.so").string()},
      {"MOCKTAIL_ASSET_PATH", (root_ / "assets").string()},
      {"MOCKTAIL_SKIP_UPDATE_CHECK", "1"},
      {"MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS", "5000"},
      {"MOCKTAIL_ISOLATED_CANARY", "1"},
  });
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  SingleInstanceLock lock =
      SingleInstanceLock::AcquireForLaunch(environment, paths);
  EXPECT_EQ(lock.status(), SingleInstanceLock::Status::kError);
  EXPECT_NE(lock.error().find("distinct absolute siblings"), std::string::npos);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
