#include "runtime/runtime_paths.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

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

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_runtime_paths_XXXXXX";
    char* created = mkdtemp(pattern);
    if (created != nullptr) {
      path_ = created;
    }
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class ScopedProcessEnvironment {
 public:
  explicit ScopedProcessEnvironment(std::string name) : name_(std::move(name)) {
    const char* value = std::getenv(name_.c_str());
    if (value != nullptr) {
      original_ = value;
    }
  }

  ~ScopedProcessEnvironment() {
    if (original_.has_value()) {
      (void)setenv(name_.c_str(), original_->c_str(), 1);
    } else {
      (void)unsetenv(name_.c_str());
    }
  }

 private:
  std::string name_;
  std::optional<std::string> original_;
};

class ScopedCurrentPath {
 public:
  ScopedCurrentPath() : original_(std::filesystem::current_path()) {}
  ~ScopedCurrentPath() {
    std::error_code error;
    std::filesystem::current_path(original_, error);
  }

 private:
  std::filesystem::path original_;
};

void WriteManagedPayloadFiles(const std::filesystem::path& payload) {
  ASSERT_TRUE(RuntimePaths::EnsureDirectory(payload / "assets/content"));
  ASSERT_TRUE(RuntimePaths::EnsureDirectory(payload / "sober_apk"));
  std::ofstream(payload / "libroblox.so") << "ELF fixture";
  std::ofstream(payload / "sober_apk/base.apk") << "base APK fixture";
  std::ofstream(payload / "sober_apk/split_config.x86_64.apk")
      << "x86_64 split APK fixture";
}

TEST(RuntimePathsTest, DerivesXdgDefaults) {
  const MapEnvironment environment({{"HOME", "/home/mocktail"}});
  const RuntimePaths paths =
      RuntimePaths::FromEnvironment(environment, "/workspace");

  EXPECT_EQ(paths.sober_data_root(),
            "/home/mocktail/.var/app/org.vinegarhq.Sober/data/sober");
  EXPECT_EQ(paths.sober_cache_root(),
            "/home/mocktail/.var/app/org.vinegarhq.Sober/cache/sober");
  EXPECT_EQ(paths.data_root(), "/home/mocktail/.local/share/mocktail");
  EXPECT_EQ(paths.cache_root(), "/home/mocktail/.cache/mocktail");
  EXPECT_EQ(paths.state_root(), "/home/mocktail/.local/state/mocktail");
  EXPECT_EQ(paths.config_root(), "/home/mocktail/.config/mocktail");
  EXPECT_EQ(paths.config_file(), "/home/mocktail/.config/mocktail/config.yaml");
  EXPECT_EQ(paths.payloads_root(),
            "/home/mocktail/.local/share/mocktail/payloads");
  EXPECT_EQ(paths.active_payload_manifest(),
            "/home/mocktail/.local/share/mocktail/current.json");
  EXPECT_EQ(paths.downloads_root(), "/home/mocktail/.cache/mocktail/downloads");
  EXPECT_EQ(paths.logs_root(), "/home/mocktail/.local/state/mocktail/logs");
  EXPECT_EQ(paths.android_runtime_root(),
            "/home/mocktail/.local/share/mocktail/android");
  EXPECT_EQ(paths.android_data_root(),
            "/home/mocktail/.local/share/mocktail/android/data");
  EXPECT_EQ(paths.android_cache_root(),
            "/home/mocktail/.cache/mocktail/android");
  EXPECT_EQ(paths.vulkan_shader_cache_file(),
            "/home/mocktail/.cache/mocktail/graphics/shadercachevk.bin");
  EXPECT_EQ(paths.auth_root(), "/home/mocktail/.local/share/mocktail/auth");
  EXPECT_EQ(paths.cookie_file(),
            "/home/mocktail/.local/share/mocktail/auth/roblox.cookie");
}

TEST(RuntimePathsTest, IgnoresRelativeXdgHomes) {
  const MapEnvironment environment({
      {"HOME", "/home/mocktail"},
      {"XDG_CONFIG_HOME", "relative-config"},
      {"XDG_DATA_HOME", "relative-data"},
      {"XDG_CACHE_HOME", "relative-cache"},
      {"XDG_STATE_HOME", "relative-state"},
  });
  const RuntimePaths paths =
      RuntimePaths::FromEnvironment(environment, "/workspace");

  EXPECT_EQ(paths.config_root(), "/home/mocktail/.config/mocktail");
  EXPECT_EQ(paths.data_root(), "/home/mocktail/.local/share/mocktail");
  EXPECT_EQ(paths.cache_root(), "/home/mocktail/.cache/mocktail");
  EXPECT_EQ(paths.state_root(), "/home/mocktail/.local/state/mocktail");
}

TEST(RuntimePathsTest, ResolvesValidatedActivePayload) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path data = temporary.path() / "data";
  const std::string payload_id =
      "2546-d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21";
  const std::filesystem::path payload = data / "payloads" / payload_id;
  WriteManagedPayloadFiles(payload);
  std::ofstream(data / "current.json")
      << "{\"schema_version\":1,\"payload_id\":\"" << payload_id
      << "\",\"payload_path\":\"payloads/" << payload_id
      << "\",\"version_name\":\"2.725.1142\",\"version_code\":2546,"
         "\"elf_build_id\":\"d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21\"}";
  const MapEnvironment environment({
      {"HOME", temporary.path().string()},
      {"MOCKTAIL_DATA_ROOT", data.string()},
  });

  const RuntimePaths paths =
      RuntimePaths::FromEnvironment(environment, temporary.path());
  const ActivePayloadPaths active = paths.ResolveActivePayload();

  ASSERT_TRUE(active) << active.error;
  ASSERT_TRUE(active.active);
  EXPECT_EQ(active.root, std::filesystem::canonical(payload));
  EXPECT_EQ(active.roblox_library, active.root / "libroblox.so");
  EXPECT_EQ(active.base_apk, active.root / "sober_apk/base.apk");
  EXPECT_EQ(active.x86_64_split_apk,
            active.root / "sober_apk/split_config.x86_64.apk");
  EXPECT_EQ(active.assets_content, active.root / "assets/content");
  EXPECT_EQ(paths.DefaultAssetPath(), active.assets_content);
}

TEST(RuntimePathsTest, RejectsIncompleteActiveAndroidPackage) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path data = temporary.path() / "data";
  const std::string payload_id =
      "2546-d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21";
  const std::filesystem::path payload = data / "payloads" / payload_id;
  WriteManagedPayloadFiles(payload);
  ASSERT_TRUE(std::filesystem::remove(
      payload / "sober_apk/split_config.x86_64.apk"));
  std::ofstream(data / "current.json")
      << "{\"schema_version\":1,\"payload_id\":\"" << payload_id
      << "\",\"payload_path\":\"payloads/" << payload_id
      << "\",\"version_name\":\"2.725.1142\",\"version_code\":2546,"
         "\"elf_build_id\":\"d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21\"}";
  const MapEnvironment environment({
      {"HOME", temporary.path().string()},
      {"MOCKTAIL_DATA_ROOT", data.string()},
  });

  const ActivePayloadPaths active =
      RuntimePaths::FromEnvironment(environment, temporary.path())
          .ResolveActivePayload();

  EXPECT_FALSE(active);
  EXPECT_NE(active.error.find("split_config.x86_64.apk"), std::string::npos);
}

TEST(RuntimePathsTest, ResolvesCompleteApprovedProfileReferences) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path data = temporary.path() / "data";
  const std::string payload_id =
      "2718-48fc8ee1fb36fc39072fd8619154ce90eea4b316";
  const std::string approval_generation =
      "1111111111111111111111111111111111111111";
  const std::string approval_filename =
      payload_id + "-" + approval_generation + ".json";
  const std::filesystem::path payload = data / "payloads" / payload_id;
  WriteManagedPayloadFiles(payload);
  ASSERT_TRUE(RuntimePaths::EnsureDirectory(data / "compatibility_profiles"));
  ASSERT_TRUE(RuntimePaths::EnsureDirectory(data / "host_abi_profiles"));
  ASSERT_TRUE(RuntimePaths::EnsureDirectory(data / "approvals"));
  std::ofstream(data / "compatibility_profiles" / approval_filename) << "{}";
  std::ofstream(data / "host_abi_profiles" / approval_filename) << "{}";
  std::ofstream(data / "approvals" / approval_filename) << "{}";
  std::ofstream(data / "current.json")
      << "{\"schema_version\":1,\"payload_id\":\"" << payload_id
      << "\",\"payload_path\":\"payloads/" << payload_id
      << "\",\"version_name\":\"2.730.790\",\"version_code\":2718,"
         "\"elf_build_id\":\"48fc8ee1fb36fc39072fd8619154ce90eea4b316\","
         "\"compatibility_manifest_path\":\"compatibility_profiles/"
      << approval_filename
      << "\",\"host_abi_profile_path\":\"host_abi_profiles/"
      << approval_filename << "\",\"approval_path\":\"approvals/"
      << approval_filename << "\"}";
  const MapEnvironment environment({
      {"HOME", temporary.path().string()},
      {"MOCKTAIL_DATA_ROOT", data.string()},
  });

  const ActivePayloadPaths active =
      RuntimePaths::FromEnvironment(environment, temporary.path())
          .ResolveActivePayload();

  ASSERT_TRUE(active) << active.error;
  ASSERT_TRUE(active.active);
  EXPECT_EQ(active.compatibility_manifest,
            std::filesystem::canonical(data / "compatibility_profiles" /
                                       approval_filename));
  EXPECT_EQ(active.host_abi_profile,
            std::filesystem::canonical(data / "host_abi_profiles" /
                                       approval_filename));
  EXPECT_EQ(active.host_abi_approval_receipt,
            std::filesystem::canonical(data / "approvals" / approval_filename));
}

TEST(RuntimePathsTest, RejectsPartialOrEscapingApprovedProfileReferences) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path data = temporary.path() / "data";
  const std::string payload_id =
      "2718-48fc8ee1fb36fc39072fd8619154ce90eea4b316";
  const std::string approval_generation =
      "1111111111111111111111111111111111111111";
  const std::string approval_filename =
      payload_id + "-" + approval_generation + ".json";
  const std::filesystem::path payload = data / "payloads" / payload_id;
  WriteManagedPayloadFiles(payload);
  const MapEnvironment environment({
      {"HOME", temporary.path().string()},
      {"MOCKTAIL_DATA_ROOT", data.string()},
  });
  const RuntimePaths paths =
      RuntimePaths::FromEnvironment(environment, temporary.path());

  std::ofstream(data / "current.json")
      << "{\"schema_version\":1,\"payload_id\":\"" << payload_id
      << "\",\"payload_path\":\"payloads/" << payload_id
      << "\",\"version_name\":\"2.730.790\",\"version_code\":2718,"
         "\"elf_build_id\":\"48fc8ee1fb36fc39072fd8619154ce90eea4b316\","
         "\"approval_path\":\"approvals/"
      << approval_filename << "\"}";
  ActivePayloadPaths active = paths.ResolveActivePayload();
  EXPECT_FALSE(active);
  EXPECT_NE(active.error.find("complete string set"), std::string::npos);

  ASSERT_TRUE(RuntimePaths::EnsureDirectory(data / "compatibility_profiles"));
  ASSERT_TRUE(RuntimePaths::EnsureDirectory(data / "host_abi_profiles"));
  ASSERT_TRUE(RuntimePaths::EnsureDirectory(data / "approvals"));
  std::ofstream(data / "compatibility_profiles" / approval_filename) << "{}";
  std::ofstream(data / "host_abi_profiles" / approval_filename) << "{}";
  std::ofstream(data / "approvals" / approval_filename) << "{}";
  std::ofstream(data / "current.json", std::ios::trunc)
      << "{\"schema_version\":1,\"payload_id\":\"" << payload_id
      << "\",\"payload_path\":\"payloads/" << payload_id
      << "\",\"version_name\":\"2.730.790\",\"version_code\":2718,"
         "\"elf_build_id\":\"48fc8ee1fb36fc39072fd8619154ce90eea4b316\","
         "\"compatibility_manifest_path\":\"compatibility_profiles/../"
         "escape.json\","
         "\"host_abi_profile_path\":\"host_abi_profiles/"
      << approval_filename << "\",\"approval_path\":\"approvals/"
      << approval_filename << "\"}";
  active = paths.ResolveActivePayload();
  EXPECT_FALSE(active);
  EXPECT_NE(active.error.find("generated direct compatibility_profiles child"),
            std::string::npos);

  const std::string other_approval_filename =
      payload_id + "-2222222222222222222222222222222222222222.json";
  std::ofstream(data / "host_abi_profiles" / other_approval_filename) << "{}";
  std::ofstream(data / "current.json", std::ios::trunc)
      << "{\"schema_version\":1,\"payload_id\":\"" << payload_id
      << "\",\"payload_path\":\"payloads/" << payload_id
      << "\",\"version_name\":\"2.730.790\",\"version_code\":2718,"
         "\"elf_build_id\":\"48fc8ee1fb36fc39072fd8619154ce90eea4b316\","
         "\"compatibility_manifest_path\":\"compatibility_profiles/"
      << approval_filename
      << "\",\"host_abi_profile_path\":\"host_abi_profiles/"
      << other_approval_filename << "\",\"approval_path\":\"approvals/"
      << approval_filename << "\"}";
  active = paths.ResolveActivePayload();
  EXPECT_FALSE(active);
  EXPECT_NE(active.error.find("different generations"), std::string::npos);

  const std::filesystem::path outside_approvals =
      temporary.path() / "outside-approvals";
  ASSERT_TRUE(RuntimePaths::EnsureDirectory(outside_approvals));
  std::ofstream(outside_approvals / approval_filename) << "{}";
  std::error_code filesystem_error;
  ASSERT_TRUE(
      std::filesystem::remove_all(data / "approvals", filesystem_error) > 0);
  ASSERT_FALSE(filesystem_error);
  std::filesystem::create_directory_symlink(
      outside_approvals, data / "approvals", filesystem_error);
  ASSERT_FALSE(filesystem_error);
  std::ofstream(data / "current.json", std::ios::trunc)
      << "{\"schema_version\":1,\"payload_id\":\"" << payload_id
      << "\",\"payload_path\":\"payloads/" << payload_id
      << "\",\"version_name\":\"2.730.790\",\"version_code\":2718,"
         "\"elf_build_id\":\"48fc8ee1fb36fc39072fd8619154ce90eea4b316\","
         "\"compatibility_manifest_path\":\"compatibility_profiles/"
      << approval_filename
      << "\",\"host_abi_profile_path\":\"host_abi_profiles/"
      << approval_filename << "\",\"approval_path\":\"approvals/"
      << approval_filename << "\"}";
  active = paths.ResolveActivePayload();
  EXPECT_FALSE(active);
  EXPECT_NE(active.error.find("directory is missing, invalid, or a symlink"),
            std::string::npos);
}

TEST(RuntimePathsTest, RejectsActivePayloadTraversalAndSymlink) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path data = temporary.path() / "data";
  ASSERT_TRUE(RuntimePaths::EnsureDirectory(data / "payloads"));
  std::ofstream(data / "current.json")
      << R"({"schema_version":1,"payload_id":"2546-d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21","payload_path":"payloads/../escape","version_name":"2.725.1142","version_code":2546,"elf_build_id":"d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21"})";
  const MapEnvironment environment({
      {"HOME", temporary.path().string()},
      {"MOCKTAIL_DATA_ROOT", data.string()},
  });
  const RuntimePaths paths =
      RuntimePaths::FromEnvironment(environment, temporary.path());

  ActivePayloadPaths active = paths.ResolveActivePayload();
  EXPECT_FALSE(active);
  EXPECT_NE(active.error.find("direct payloads child"), std::string::npos);

  const std::filesystem::path external = temporary.path() / "external";
  ASSERT_TRUE(RuntimePaths::EnsureDirectory(external / "assets/content"));
  std::ofstream(external / "libroblox.so") << "ELF fixture";
  std::error_code error;
  std::filesystem::create_directory_symlink(
      external, data / "payloads/2546-d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21",
      error);
  ASSERT_FALSE(error);
  std::ofstream(data / "current.json", std::ios::trunc)
      << R"({"schema_version":1,"payload_id":"2546-d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21","payload_path":"payloads/2546-d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21","version_name":"2.725.1142","version_code":2546,"elf_build_id":"d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21"})";
  active = paths.ResolveActivePayload();
  EXPECT_FALSE(active);
  EXPECT_NE(active.error.find("symlink"), std::string::npos);
}

TEST(RuntimePathsTest, HonorsExplicitAndXdgOverrides) {
  const MapEnvironment explicit_environment({
      {"HOME", "/home/mocktail"},
      {"MOCKTAIL_CACHE_ROOT", "/cache"},
      {"MOCKTAIL_CONFIG_ROOT", "/config"},
      {"MOCKTAIL_DATA_ROOT", "/data"},
      {"MOCKTAIL_STATE_ROOT", "/state"},
      {"MOCKTAIL_AUTH_ROOT", "/auth"},
  });
  const RuntimePaths explicit_paths =
      RuntimePaths::FromEnvironment(explicit_environment, "/workspace");
  EXPECT_EQ(explicit_paths.cache_root(), "/cache");
  EXPECT_EQ(explicit_paths.config_root(), "/config");
  EXPECT_EQ(explicit_paths.data_root(), "/data");
  EXPECT_EQ(explicit_paths.state_root(), "/state");
  EXPECT_EQ(explicit_paths.auth_root(), "/auth");

  const MapEnvironment xdg_environment({
      {"HOME", "/home/mocktail"},
      {"XDG_CONFIG_HOME", "/xdg"},
      {"XDG_DATA_HOME", "/xdg-data"},
      {"XDG_CACHE_HOME", "/xdg-cache"},
      {"XDG_STATE_HOME", "/xdg-state"},
  });
  const RuntimePaths xdg_paths =
      RuntimePaths::FromEnvironment(xdg_environment, "/workspace");
  EXPECT_EQ(xdg_paths.config_root(), "/xdg/mocktail");
  EXPECT_EQ(xdg_paths.data_root(), "/xdg-data/mocktail");
  EXPECT_EQ(xdg_paths.cache_root(), "/xdg-cache/mocktail");
  EXPECT_EQ(xdg_paths.state_root(), "/xdg-state/mocktail");
}

TEST(RuntimePathsTest, SelectsExistingRealSoberAssetPath) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path home = temporary.path() / "home";
  const std::filesystem::path sober_assets =
      home / ".var/app/org.vinegarhq.Sober/data/sober/assets/content";
  ASSERT_TRUE(RuntimePaths::EnsureDirectory(sober_assets));

  const MapEnvironment environment({
      {"HOME", home.string()},
      {"MOCKTAIL_USE_REAL_SOBER_PATHS", "1"},
  });
  const RuntimePaths paths = RuntimePaths::FromEnvironment(
      environment, temporary.path() / "empty-workspace");

  EXPECT_EQ(paths.DefaultAssetPath(), sober_assets);
}

TEST(RuntimePathsTest, PrefersWorkspaceAssets) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  ASSERT_TRUE(RuntimePaths::EnsureDirectory(temporary.path() /
                                            "rbx_bin/assets/content"));
  const MapEnvironment environment;

  EXPECT_EQ(RuntimePaths::FromEnvironment(environment, temporary.path())
                .DefaultAssetPath(),
            "rbx_bin/assets/content");
}

TEST(RuntimePathsTest, ResolvesAssetsAdjacentToExplicitRobloxLibrary) {
  EXPECT_EQ(ResolveAdjacentRobloxAssetPath(
                "/data/mocktail/payloads/2814-build/libroblox.so",
                "/ignored"),
            "/data/mocktail/payloads/2814-build/assets/content");
  EXPECT_EQ(ResolveAdjacentRobloxAssetPath("rbx_bin/libroblox.so",
                                          "/workspace/mocktail"),
            "/workspace/mocktail/rbx_bin/assets/content");
  EXPECT_TRUE(
      ResolveAdjacentRobloxAssetPath({}, "/workspace/mocktail").empty());
  EXPECT_TRUE(
      ResolveAdjacentRobloxAssetPath("libroblox.so", {}).empty());
}

TEST(RuntimePathsTest, CreatesNestedDirectoryAndRejectsFile) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path nested = temporary.path() / "one/two/three";
  std::error_code error;
  EXPECT_TRUE(RuntimePaths::EnsureDirectory(nested, &error));
  EXPECT_FALSE(error);
  EXPECT_TRUE(std::filesystem::is_directory(nested));

  const std::filesystem::path file = temporary.path() / "regular-file";
  FILE* output = std::fopen(file.c_str(), "w");
  ASSERT_NE(output, nullptr);
  std::fclose(output);
  EXPECT_FALSE(RuntimePaths::EnsureDirectory(file, &error));
  EXPECT_TRUE(error);
}

TEST(RuntimePathsTest, PreparesManagedPayloadRelativeAssetRoot) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  ScopedCurrentPath restore_current_path;
  const std::filesystem::path data = temporary.path() / "data";
  const std::filesystem::path payload = data / "payloads/fixture";
  WriteManagedPayloadFiles(payload);
  const MapEnvironment environment({
      {"HOME", temporary.path().string()},
      {"MOCKTAIL_DATA_ROOT", data.string()},
  });
  const RuntimePaths paths =
      RuntimePaths::FromEnvironment(environment, temporary.path());
  ActivePayloadPaths active;
  active.active = true;
  active.root = payload;
  active.roblox_library = payload / "libroblox.so";
  active.base_apk = payload / "sober_apk/base.apk";
  active.x86_64_split_apk =
      payload / "sober_apk/split_config.x86_64.apk";
  active.assets_content = payload / "assets/content";

  std::string error;
  ASSERT_TRUE(PrepareManagedPayloadWorkingDirectory(paths, active, &error))
      << error;
  EXPECT_EQ(std::filesystem::current_path(), data);
  EXPECT_TRUE(std::filesystem::is_symlink(data / "rbx_bin/assets"));
  EXPECT_EQ(std::filesystem::canonical(data / "rbx_bin/assets"),
            std::filesystem::canonical(payload / "assets"));
  EXPECT_TRUE(std::filesystem::is_symlink(data / "rbx_bin/sober_apk"));
  EXPECT_EQ(std::filesystem::canonical(data / "rbx_bin/sober_apk"),
            std::filesystem::canonical(payload / "sober_apk"));
  EXPECT_TRUE(std::filesystem::is_symlink(data / "rbx_bin/libroblox.so"));
  EXPECT_EQ(std::filesystem::canonical(data / "rbx_bin/libroblox.so"),
            std::filesystem::canonical(payload / "libroblox.so"));
}

TEST(RuntimePathsTest, ReplacesAllStaleManagedPayloadLinksTogether) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  ScopedCurrentPath restore_current_path;
  const std::filesystem::path data = temporary.path() / "data";
  const std::filesystem::path old_payload = data / "payloads/old";
  const std::filesystem::path new_payload = data / "payloads/new";
  WriteManagedPayloadFiles(old_payload);
  WriteManagedPayloadFiles(new_payload);
  const MapEnvironment environment({
      {"HOME", temporary.path().string()},
      {"MOCKTAIL_DATA_ROOT", data.string()},
  });
  const RuntimePaths paths =
      RuntimePaths::FromEnvironment(environment, temporary.path());
  ActivePayloadPaths active;
  active.active = true;
  active.root = old_payload;

  std::string error;
  ASSERT_TRUE(PrepareManagedPayloadWorkingDirectory(paths, active, &error))
      << error;
  active.root = new_payload;
  ASSERT_TRUE(PrepareManagedPayloadWorkingDirectory(paths, active, &error))
      << error;

  EXPECT_EQ(std::filesystem::canonical(data / "rbx_bin/assets"),
            std::filesystem::canonical(new_payload / "assets"));
  EXPECT_EQ(std::filesystem::canonical(data / "rbx_bin/sober_apk"),
            std::filesystem::canonical(new_payload / "sober_apk"));
  EXPECT_EQ(std::filesystem::canonical(data / "rbx_bin/libroblox.so"),
            std::filesystem::canonical(new_payload / "libroblox.so"));
}

TEST(RuntimePathsTest, PreservesUnexpectedManagedPayloadFiles) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  ScopedCurrentPath restore_current_path;
  const std::filesystem::path data = temporary.path() / "data";
  const std::filesystem::path payload = data / "payloads/fixture";
  WriteManagedPayloadFiles(payload);
  ASSERT_TRUE(RuntimePaths::EnsureDirectory(data / "rbx_bin/sober_apk"));
  std::ofstream(data / "rbx_bin/sober_apk/user-file") << "preserve me";
  const MapEnvironment environment({
      {"HOME", temporary.path().string()},
      {"MOCKTAIL_DATA_ROOT", data.string()},
  });
  const RuntimePaths paths =
      RuntimePaths::FromEnvironment(environment, temporary.path());
  ActivePayloadPaths active;
  active.active = true;
  active.root = payload;

  std::string error;
  EXPECT_FALSE(PrepareManagedPayloadWorkingDirectory(paths, active, &error));
  EXPECT_NE(error.find("refusing to replace non-symlink"), std::string::npos);
  EXPECT_TRUE(std::filesystem::is_regular_file(
      data / "rbx_bin/sober_apk/user-file"));
  EXPECT_FALSE(std::filesystem::exists(data / "rbx_bin/assets"));
  EXPECT_FALSE(std::filesystem::exists(data / "rbx_bin/libroblox.so"));
}

TEST(RuntimePathsTest, ExportsXdgBackedAndroidPathsAndCreatesParents) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const ScopedProcessEnvironment runtime_root("MOCKTAIL_RUNTIME_ROOT");
  const ScopedProcessEnvironment cache_root(
      "MOCKTAIL_ANDROID_CACHE_HOST_ROOT");
  const ScopedProcessEnvironment shader_cache(
      "MOCKTAIL_VULKAN_SHADER_CACHE_HOST_PATH");
  ASSERT_EQ(unsetenv("MOCKTAIL_RUNTIME_ROOT"), 0);
  ASSERT_EQ(unsetenv("MOCKTAIL_ANDROID_CACHE_HOST_ROOT"), 0);
  ASSERT_EQ(unsetenv("MOCKTAIL_VULKAN_SHADER_CACHE_HOST_PATH"), 0);

  const MapEnvironment environment({
      {"HOME", temporary.path().string()},
      {"XDG_DATA_HOME", (temporary.path() / "data").string()},
      {"XDG_CACHE_HOME", (temporary.path() / "cache").string()},
  });
  const RuntimePaths paths =
      RuntimePaths::FromEnvironment(environment, temporary.path());
  std::string error;
  ASSERT_TRUE(ExportRuntimePathEnvironment(paths, &error)) << error;

  EXPECT_STREQ(std::getenv("MOCKTAIL_RUNTIME_ROOT"),
               paths.android_runtime_root().c_str());
  EXPECT_STREQ(std::getenv("MOCKTAIL_ANDROID_CACHE_HOST_ROOT"),
               paths.android_cache_root().c_str());
  EXPECT_STREQ(std::getenv("MOCKTAIL_VULKAN_SHADER_CACHE_HOST_PATH"),
               paths.vulkan_shader_cache_file().c_str());
  EXPECT_TRUE(std::filesystem::is_directory(paths.android_runtime_root()));
  EXPECT_TRUE(std::filesystem::is_directory(paths.android_cache_root()));
  EXPECT_TRUE(std::filesystem::is_directory(
      paths.vulkan_shader_cache_file().parent_path()));
}

TEST(RuntimePathsTest, PreservesExplicitAndroidPathOverrides) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const ScopedProcessEnvironment runtime_root("MOCKTAIL_RUNTIME_ROOT");
  const ScopedProcessEnvironment cache_root(
      "MOCKTAIL_ANDROID_CACHE_HOST_ROOT");
  const ScopedProcessEnvironment shader_cache(
      "MOCKTAIL_VULKAN_SHADER_CACHE_HOST_PATH");
  ASSERT_EQ(setenv("MOCKTAIL_RUNTIME_ROOT", "/explicit/runtime", 1), 0);
  ASSERT_EQ(setenv("MOCKTAIL_ANDROID_CACHE_HOST_ROOT", "/explicit/cache", 1),
            0);
  ASSERT_EQ(setenv("MOCKTAIL_VULKAN_SHADER_CACHE_HOST_PATH",
                   "/explicit/shader-cache", 1),
            0);

  const MapEnvironment environment({{"HOME", temporary.path().string()}});
  const RuntimePaths paths =
      RuntimePaths::FromEnvironment(environment, temporary.path());
  std::string error;
  ASSERT_TRUE(ExportRuntimePathEnvironment(paths, &error)) << error;

  EXPECT_STREQ(std::getenv("MOCKTAIL_RUNTIME_ROOT"), "/explicit/runtime");
  EXPECT_STREQ(std::getenv("MOCKTAIL_ANDROID_CACHE_HOST_ROOT"),
               "/explicit/cache");
  EXPECT_STREQ(std::getenv("MOCKTAIL_VULKAN_SHADER_CACHE_HOST_PATH"),
               "/explicit/shader-cache");
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
