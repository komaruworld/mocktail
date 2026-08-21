#include "runtime/platform_cache_migration.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "runtime/environment.h"
#include "runtime/runtime_paths.h"

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
    return found == values_.end() ? std::nullopt : std::optional(found->second);
  }

 private:
  std::unordered_map<std::string, std::string> values_;
};

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_platform_cache_XXXXXX";
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

bool WriteJson(const std::filesystem::path& path, const nlohmann::json& value) {
  std::error_code error;
  if (!RuntimePaths::EnsureDirectory(path.parent_path(), &error)) {
    return false;
  }
  std::ofstream output(path);
  output << value.dump();
  return output.good();
}

nlohmann::json ReadJson(const std::filesystem::path& path) {
  std::ifstream input(path);
  return nlohmann::json::parse(input, nullptr, false, true);
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

mode_t FileMode(const std::filesystem::path& path) {
  struct stat status{};
  return stat(path.c_str(), &status) == 0 ? status.st_mode & 0777 : 0;
}

struct FixturePaths {
  MapEnvironment environment;
  RuntimePaths paths;
  std::filesystem::path app_storage;
};

FixturePaths PathsFor(const TemporaryDirectory& temporary) {
  const std::filesystem::path runtime_root = temporary.path() / "android";
  MapEnvironment environment({
      {"HOME", temporary.path().string()},
      {"MOCKTAIL_RUNTIME_ROOT", runtime_root.string()},
  });
  RuntimePaths paths =
      RuntimePaths::FromEnvironment(environment, temporary.path());
  return {std::move(environment), std::move(paths),
          runtime_root / "data/files/appData/LocalStorage/appStorage.json"};
}

const std::string& DesktopProfile() {
  static const std::string profile =
      BuildPlatformProfileRevision("pc-windows-11", false, true, true);
  return profile;
}

const std::string& MobileProfile() {
  static const std::string profile =
      BuildPlatformProfileRevision("mobile-pixel-7", true, false, false);
  return profile;
}

TEST(PlatformCacheMigrationTest, FingerprintIncludesDeviceAndInputProfile) {
  EXPECT_EQ(DesktopProfile(), "device-v1-pc-windows-11-t0-m1-k1");
  EXPECT_EQ(MobileProfile(), "device-v1-mobile-pixel-7-t1-m0-k0");
  EXPECT_EQ(BuildPlatformProfileRevision("console-ps5", false, true, true),
            "device-v1-console-ps5-t0-m1-k1");
}

TEST(PlatformCacheMigrationTest,
     InvalidatesOnlyRefreshablePlatformCachesOnFirstTransition) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  FixturePaths fixture = PathsFor(temporary);
  const nlohmann::json original = {
      {"PlayerHydrationBlob", "signed-blob"},
      {"PlayerHydrationSignature", "signature"},
      {"AppConfiguration", "cached-policy"},
      {"AppInstallationId", "stable-installation"},
      {"_Patch__UniversalAppPatch", "ota-state"},
      {"UnrelatedPreference", "preserve-me"},
  };
  ASSERT_TRUE(WriteJson(fixture.app_storage, original));

  const PlatformCacheMigrationResult result = MigratePlatformProfileCaches(
      fixture.environment, fixture.paths, DesktopProfile());
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.transitioned);
  EXPECT_TRUE(result.app_storage_found);
  EXPECT_TRUE(result.app_storage_updated);

  const nlohmann::json migrated = ReadJson(fixture.app_storage);
  EXPECT_FALSE(migrated.contains("PlayerHydrationBlob"));
  EXPECT_FALSE(migrated.contains("PlayerHydrationSignature"));
  EXPECT_FALSE(migrated.contains("AppConfiguration"));
  EXPECT_EQ(migrated["AppInstallationId"], "stable-installation");
  EXPECT_EQ(migrated["_Patch__UniversalAppPatch"], "ota-state");
  EXPECT_EQ(migrated["UnrelatedPreference"], "preserve-me");
  EXPECT_EQ(FileMode(fixture.app_storage), 0600);
  EXPECT_EQ(FileMode(result.fingerprint_file), 0600);
  EXPECT_EQ(ReadJson(result.fingerprint_file)["profile_revision"],
            DesktopProfile());
}

TEST(PlatformCacheMigrationTest, SameRevisionDoesNotTouchRehydratedCache) {
  TemporaryDirectory temporary;
  FixturePaths fixture = PathsFor(temporary);
  PlatformCacheMigrationResult first = MigratePlatformProfileCaches(
      fixture.environment, fixture.paths, DesktopProfile());
  ASSERT_TRUE(first) << first.error;
  ASSERT_TRUE(WriteJson(fixture.app_storage,
                        {{"PlayerHydrationBlob", "new-valid-blob"},
                         {"PlayerHydrationSignature", "new-signature"},
                         {"AppConfiguration", "new-desktop-policy"}}));

  const PlatformCacheMigrationResult second = MigratePlatformProfileCaches(
      fixture.environment, fixture.paths, DesktopProfile());
  ASSERT_TRUE(second) << second.error;
  EXPECT_FALSE(second.transitioned);
  const nlohmann::json current = ReadJson(fixture.app_storage);
  EXPECT_EQ(current["PlayerHydrationBlob"], "new-valid-blob");
  EXPECT_EQ(current["PlayerHydrationSignature"], "new-signature");
  EXPECT_EQ(current["AppConfiguration"], "new-desktop-policy");
}

TEST(PlatformCacheMigrationTest, NewRevisionInvalidatesCacheAgain) {
  TemporaryDirectory temporary;
  FixturePaths fixture = PathsFor(temporary);
  PlatformCacheMigrationResult first = MigratePlatformProfileCaches(
      fixture.environment, fixture.paths, MobileProfile());
  ASSERT_TRUE(first) << first.error;
  ASSERT_TRUE(WriteJson(fixture.app_storage,
                        {{"PlayerHydrationBlob", "tablet-blob"},
                         {"PlayerHydrationSignature", "tablet-signature"},
                         {"AppConfiguration", "tablet-policy"},
                         {"Keep", true}}));

  const PlatformCacheMigrationResult second = MigratePlatformProfileCaches(
      fixture.environment, fixture.paths, DesktopProfile());
  ASSERT_TRUE(second) << second.error;
  EXPECT_TRUE(second.transitioned);
  EXPECT_EQ(second.previous_revision, MobileProfile());
  const nlohmann::json migrated = ReadJson(fixture.app_storage);
  EXPECT_EQ(migrated.size(), 1U);
  EXPECT_EQ(migrated["Keep"], true);
}

TEST(PlatformCacheMigrationTest, MissingAppStorageStillCommitsFingerprint) {
  TemporaryDirectory temporary;
  FixturePaths fixture = PathsFor(temporary);
  const PlatformCacheMigrationResult result = MigratePlatformProfileCaches(
      fixture.environment, fixture.paths, DesktopProfile());
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.transitioned);
  EXPECT_FALSE(result.app_storage_found);
  EXPECT_FALSE(result.app_storage_updated);
  EXPECT_FALSE(std::filesystem::exists(fixture.app_storage));
  EXPECT_EQ(ReadJson(result.fingerprint_file)["profile_revision"],
            DesktopProfile());
}

TEST(PlatformCacheMigrationTest, RejectsSymlinkedAppStorage) {
  TemporaryDirectory temporary;
  FixturePaths fixture = PathsFor(temporary);
  const std::filesystem::path outside = temporary.path() / "outside.json";
  ASSERT_TRUE(WriteJson(outside, {{"AppConfiguration", "do-not-touch"}}));
  ASSERT_TRUE(RuntimePaths::EnsureDirectory(fixture.app_storage.parent_path()));
  ASSERT_EQ(symlink(outside.c_str(), fixture.app_storage.c_str()), 0);

  const PlatformCacheMigrationResult result = MigratePlatformProfileCaches(
      fixture.environment, fixture.paths, DesktopProfile());
  EXPECT_FALSE(result);
  EXPECT_EQ(ReadJson(outside)["AppConfiguration"], "do-not-touch");
  EXPECT_FALSE(std::filesystem::exists(result.fingerprint_file));
}

TEST(PlatformCacheMigrationTest,
     RejectsHardlinkedMigrationLockWithoutMutatingProtectedCookie) {
  TemporaryDirectory temporary;
  FixturePaths fixture = PathsFor(temporary);
  const std::filesystem::path data_root = temporary.path() / "android/data";
  ASSERT_TRUE(RuntimePaths::EnsureDirectory(data_root));
  const std::filesystem::path protected_cookie =
      temporary.path() / "sober-cookie";
  const std::string cookie_contents = ".ROBLOSECURITY=protected";
  std::ofstream(protected_cookie) << cookie_contents;
  ASSERT_EQ(chmod(protected_cookie.c_str(), S_IRWXU), 0);
  const std::filesystem::path lock_path =
      data_root / ".mocktail-platform-profile.lock";
  std::error_code filesystem_error;
  std::filesystem::create_hard_link(protected_cookie, lock_path,
                                    filesystem_error);
  ASSERT_FALSE(filesystem_error);

  const PlatformCacheMigrationResult result = MigratePlatformProfileCaches(
      fixture.environment, fixture.paths, DesktopProfile());

  EXPECT_FALSE(result);
  EXPECT_EQ(result.error,
            "another process owns the platform cache migration lock");
  EXPECT_EQ(ReadFile(protected_cookie), cookie_contents);
  EXPECT_EQ(ReadFile(lock_path), cookie_contents);
  EXPECT_EQ(FileMode(protected_cookie), 0700);
  EXPECT_FALSE(std::filesystem::exists(result.fingerprint_file));
}

TEST(PlatformCacheMigrationTest, MapsAndroidVirtualFilesDirectoryToXdgRoot) {
  TemporaryDirectory temporary;
  const std::filesystem::path runtime_root = temporary.path() / "android";
  const MapEnvironment environment({
      {"HOME", temporary.path().string()},
      {"MOCKTAIL_RUNTIME_ROOT", runtime_root.string()},
      {"MOCKTAIL_ANDROID_FILES_DIR", "/data/user/0/com.roblox.client/files"},
  });
  const RuntimePaths paths =
      RuntimePaths::FromEnvironment(environment, temporary.path());
  const PlatformCacheMigrationResult result =
      MigratePlatformProfileCaches(environment, paths, DesktopProfile());
  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.app_storage_file,
            runtime_root / "data/files/appData/LocalStorage/appStorage.json");
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
