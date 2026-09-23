#include "runtime/temporary_instance.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "compat/guest_abi.h"
#include "runtime/single_instance_lock.h"

namespace mocktail {
namespace runtime {
namespace {

class MapEnvironment final : public Environment {
 public:
  explicit MapEnvironment(
      std::unordered_map<std::string, std::string> values = {})
      : values_(std::move(values)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    const auto found = values_.find(std::string(name));
    return found == values_.end() ? std::nullopt
                                  : std::optional<std::string>(found->second);
  }

 private:
  std::unordered_map<std::string, std::string> values_;
};

class TemporaryInstanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("mocktail-temp-instance-test-" + std::to_string(getpid()));
    std::filesystem::remove_all(root_);
    ASSERT_TRUE(std::filesystem::create_directories(root_));
    ClearEnvironment();
  }

  void TearDown() override {
    ClearEnvironment();
    std::filesystem::remove_all(root_);
  }

  static void ClearEnvironment() {
    for (const char* name :
         {"MOCKTAIL_DATA_ROOT", "MOCKTAIL_CACHE_ROOT", "MOCKTAIL_STATE_ROOT",
          "MOCKTAIL_AUTH_ROOT", "MOCKTAIL_TEMP_INSTANCE",
          "MOCKTAIL_TEMP_INSTANCE_ROOT", "MOCKTAIL_TEMP_INSTANCE_WINDOWS",
          "MOCKTAIL_SKIP_UPDATE_CHECK", "MOCKTAIL_ISOLATED_CANARY",
          "MOCKTAIL_COMPATIBILITY_MANIFEST", "MOCKTAIL_HOST_ABI_PROFILE_FILE",
          "MOCKTAIL_HOST_ABI_APPROVAL_RECEIPT",
          "ROBLOX_LIB_PATH", "MOCKTAIL_ASSET_PATH"}) {
      unsetenv(name);
    }
  }

  MapEnvironment PrimaryEnvironment() {
    return MapEnvironment({
        {"HOME", (root_ / "home").string()},
        {"MOCKTAIL_DATA_ROOT", (root_ / "data").string()},
        {"MOCKTAIL_CACHE_ROOT", (root_ / "cache").string()},
        {"MOCKTAIL_STATE_ROOT", (root_ / "state").string()},
    });
  }

  std::filesystem::path root_;
};

TEST_F(TemporaryInstanceTest, CreatesPrivateRootsAndExportsEnvironment) {
  const std::string library = (root_ / "libroblox.so").string();
  const std::string assets = (root_ / "assets").string();
  ASSERT_EQ(setenv("ROBLOX_LIB_PATH", library.c_str(), 1), 0);
  ASSERT_EQ(setenv("MOCKTAIL_ASSET_PATH", assets.c_str(), 1), 0);
  const MapEnvironment environment({
      {"HOME", (root_ / "home").string()},
      {"MOCKTAIL_STATE_ROOT", (root_ / "state").string()},
      {"ROBLOX_LIB_PATH", library},
      {"MOCKTAIL_ASSET_PATH", assets},
  });
  const RuntimePaths primary = RuntimePaths::FromEnvironment(environment);

  const TemporaryInstanceResult prepared =
      PrepareTemporaryInstance(environment, primary);

  ASSERT_TRUE(prepared) << prepared.error;
  const std::filesystem::path root = prepared.root;
  EXPECT_EQ(root.parent_path(), primary.state_root() / "instances");
  EXPECT_EQ(root.filename().string().rfind("temp-", 0), 0u);
  for (const char* child : {"data", "cache", "state"}) {
    const std::filesystem::path directory = root / child;
    EXPECT_TRUE(std::filesystem::is_directory(directory)) << directory;
    struct stat metadata = {};
    ASSERT_EQ(stat(directory.c_str(), &metadata), 0);
    EXPECT_EQ(metadata.st_mode & 0777, 0700);
  }
  EXPECT_STREQ(std::getenv("MOCKTAIL_DATA_ROOT"),
               (root / "data").c_str());
  EXPECT_STREQ(std::getenv("MOCKTAIL_CACHE_ROOT"),
               (root / "cache").c_str());
  EXPECT_STREQ(std::getenv("MOCKTAIL_STATE_ROOT"),
               (root / "state").c_str());
  EXPECT_STREQ(std::getenv("MOCKTAIL_AUTH_ROOT"),
               (root / "data/auth").c_str());
  EXPECT_STREQ(std::getenv("MOCKTAIL_TEMP_INSTANCE"), "1");
  EXPECT_STREQ(std::getenv("MOCKTAIL_TEMP_INSTANCE_ROOT"), root.c_str());
  EXPECT_STREQ(std::getenv("MOCKTAIL_SKIP_UPDATE_CHECK"), "1");
  EXPECT_STREQ(std::getenv("ROBLOX_LIB_PATH"), library.c_str());
  EXPECT_STREQ(std::getenv("MOCKTAIL_ASSET_PATH"), assets.c_str());
}

TEST_F(TemporaryInstanceTest, BindsActiveManagedPayloadWhenUnspecified) {
  const std::string build_id =
      "0123456789abcdef0123456789abcdef01234567";
  const std::string generation =
      "fedcba9876543210fedcba9876543210fedcba98";
  const std::string payload_id = "1337-" + build_id;
  const std::filesystem::path payload =
      root_ / "data" / "payloads" / payload_id;
  ASSERT_TRUE(
      std::filesystem::create_directories(payload / "sober_apk"));
  ASSERT_TRUE(
      std::filesystem::create_directories(payload / "assets" / "content"));
  std::ofstream(payload / "libroblox.so") << "library";
  std::ofstream(payload / "sober_apk" / "base.apk") << "base";
  std::ofstream(payload / "sober_apk" /
              std::string(mocktail::compat::kGuestSplitApkFile))
      << "split";
  const std::string approval_name = payload_id + "-" + generation + ".json";
  for (const char* directory :
       {"approvals", "compatibility_profiles", "host_abi_profiles"}) {
    const std::filesystem::path parent = root_ / "data" / directory;
    ASSERT_TRUE(std::filesystem::create_directories(parent));
    std::ofstream(parent / approval_name) << "{}";
  }
  std::ofstream(root_ / "data" / "current.json")
      << "{\"schema_version\":1,\"payload_id\":\"" << payload_id
      << "\",\"payload_path\":\"payloads/" << payload_id
      << "\",\"version_name\":\"1.0\",\"version_code\":1337"
      << ",\"elf_build_id\":\"" << build_id << "\""
      << ",\"approval_path\":\"approvals/" << approval_name << "\""
      << ",\"compatibility_manifest_path\":\"compatibility_profiles/"
      << approval_name << "\""
      << ",\"host_abi_profile_path\":\"host_abi_profiles/" << approval_name
      << "\"}";

  const MapEnvironment environment = PrimaryEnvironment();
  const RuntimePaths primary = RuntimePaths::FromEnvironment(environment);
  const TemporaryInstanceResult prepared =
      PrepareTemporaryInstance(environment, primary);

  ASSERT_TRUE(prepared) << prepared.error;
  std::error_code canonical_error;
  const std::filesystem::path canonical_payload =
      std::filesystem::canonical(payload, canonical_error);
  ASSERT_FALSE(canonical_error) << canonical_error.message();
  EXPECT_STREQ(std::getenv("ROBLOX_LIB_PATH"),
               (canonical_payload / "libroblox.so").c_str());
  EXPECT_STREQ(std::getenv("MOCKTAIL_ASSET_PATH"),
               (canonical_payload / "assets" / "content").c_str());
  const std::filesystem::path canonical_data =
      std::filesystem::canonical(root_ / "data", canonical_error);
  ASSERT_FALSE(canonical_error) << canonical_error.message();
  EXPECT_STREQ(std::getenv("MOCKTAIL_COMPATIBILITY_MANIFEST"),
               (canonical_data / "compatibility_profiles" / approval_name)
                   .c_str());
  EXPECT_STREQ(std::getenv("MOCKTAIL_HOST_ABI_PROFILE_FILE"),
               (canonical_data / "host_abi_profiles" / approval_name).c_str());
  EXPECT_STREQ(std::getenv("MOCKTAIL_HOST_ABI_APPROVAL_RECEIPT"),
               (canonical_data / "approvals" / approval_name).c_str());
}

TEST_F(TemporaryInstanceTest, ReusesRootExportedBeforeReexec) {
  const std::filesystem::path existing = root_ / "instances" / "temp-reused";
  const MapEnvironment environment({
      {"HOME", (root_ / "home").string()},
      {"MOCKTAIL_TEMP_INSTANCE_ROOT", existing.string()},
  });
  const RuntimePaths primary = RuntimePaths::FromEnvironment(environment);

  const TemporaryInstanceResult prepared =
      PrepareTemporaryInstance(environment, primary);

  ASSERT_TRUE(prepared) << prepared.error;
  EXPECT_EQ(prepared.root, existing);
  EXPECT_STREQ(std::getenv("MOCKTAIL_TEMP_INSTANCE"), "1");
}

TEST_F(TemporaryInstanceTest, RefusesInsideIsolatedCanary) {
  const MapEnvironment environment({
      {"HOME", (root_ / "home").string()},
      {"MOCKTAIL_ISOLATED_CANARY", "1"},
  });
  const RuntimePaths primary = RuntimePaths::FromEnvironment(environment);

  const TemporaryInstanceResult prepared =
      PrepareTemporaryInstance(environment, primary);

  EXPECT_FALSE(prepared);
  EXPECT_NE(prepared.error.find("canary"), std::string::npos);
}

TEST_F(TemporaryInstanceTest, RequiresActivatedPayloadWithoutExplicitPaths) {
  const MapEnvironment environment = PrimaryEnvironment();
  const RuntimePaths primary = RuntimePaths::FromEnvironment(environment);

  const TemporaryInstanceResult prepared =
      PrepareTemporaryInstance(environment, primary);

  EXPECT_FALSE(prepared);
  EXPECT_NE(prepared.error.find("activated managed payload"),
            std::string::npos);
}

TEST_F(TemporaryInstanceTest, RemovesRootWhenGuardLeavesScope) {
  const MapEnvironment environment({
      {"HOME", (root_ / "home").string()},
      {"MOCKTAIL_STATE_ROOT", (root_ / "state").string()},
      {"ROBLOX_LIB_PATH", (root_ / "libroblox.so").string()},
      {"MOCKTAIL_ASSET_PATH", (root_ / "assets").string()},
  });
  const RuntimePaths primary = RuntimePaths::FromEnvironment(environment);
  const TemporaryInstanceResult prepared =
      PrepareTemporaryInstance(environment, primary);
  ASSERT_TRUE(prepared) << prepared.error;

  const std::filesystem::path root = prepared.root;
  std::ofstream(root / "data" / "cookie") << "session";
  {
    TemporaryInstance guard(root);
    EXPECT_TRUE(std::filesystem::exists(root));
  }
  EXPECT_FALSE(std::filesystem::exists(root));
}

TEST_F(TemporaryInstanceTest, GuardRefusesUnmanagedPath) {
  const std::filesystem::path foreign = root_ / "keep-me";
  ASSERT_TRUE(std::filesystem::create_directories(foreign));
  {
    TemporaryInstance guard(foreign);
  }
  EXPECT_TRUE(std::filesystem::exists(foreign));
}

TEST_F(TemporaryInstanceTest, SweepsStaleRootsButKeepsLiveAndForeignEntries) {
  const std::filesystem::path instances = root_ / "state" / "instances";
  const std::filesystem::path stale = instances / "temp-stale";
  const std::filesystem::path live = instances / "temp-live";
  const std::filesystem::path foreign = instances / "downloads";
  const std::filesystem::path not_directory = instances / "temp-file";
  ASSERT_TRUE(std::filesystem::create_directories(stale / "state"));
  ASSERT_TRUE(std::filesystem::create_directories(live / "state"));
  ASSERT_TRUE(std::filesystem::create_directories(foreign));
  std::ofstream(not_directory) << "not a directory";
  const auto old = std::filesystem::file_time_type::clock::now() -
                   std::chrono::minutes(5);
  for (const std::filesystem::path& entry : {stale, live, foreign}) {
    std::filesystem::last_write_time(entry, old);
  }
  SingleInstanceLock live_lock =
      SingleInstanceLock::Acquire(live / "state" / "instance.lock");
  ASSERT_TRUE(live_lock.acquired()) << live_lock.error();

  const MapEnvironment environment({
      {"HOME", (root_ / "home").string()},
      {"MOCKTAIL_STATE_ROOT", (root_ / "state").string()},
      {"ROBLOX_LIB_PATH", (root_ / "libroblox.so").string()},
      {"MOCKTAIL_ASSET_PATH", (root_ / "assets").string()},
  });
  const RuntimePaths primary = RuntimePaths::FromEnvironment(environment);
  const TemporaryInstanceResult prepared =
      PrepareTemporaryInstance(environment, primary);

  ASSERT_TRUE(prepared) << prepared.error;
  EXPECT_FALSE(std::filesystem::exists(stale));
  EXPECT_TRUE(std::filesystem::exists(live));
  EXPECT_TRUE(std::filesystem::exists(foreign));
  EXPECT_TRUE(std::filesystem::exists(not_directory));
}

TEST_F(TemporaryInstanceTest, TemporaryWindowsFlagReadsConfigFile) {
  const std::filesystem::path config = root_ / "config.yaml";
  const MapEnvironment environment({{"HOME", (root_ / "home").string()}});

  EXPECT_FALSE(TemporaryInstanceWindowsEnabled(environment, config));

  std::ofstream(config) << "version: 1\nmulti_instance:\n"
                           "  temporary_windows: true\n";
  EXPECT_TRUE(TemporaryInstanceWindowsEnabled(environment, config));

  const MapEnvironment disabled(
      {{"MOCKTAIL_TEMP_INSTANCE_WINDOWS", "0"}});
  EXPECT_FALSE(TemporaryInstanceWindowsEnabled(disabled, config));

  std::ofstream(config) << "version: 1\n";
  const MapEnvironment enabled(
      {{"MOCKTAIL_TEMP_INSTANCE_WINDOWS", "1"}});
  EXPECT_TRUE(TemporaryInstanceWindowsEnabled(enabled, config));
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
