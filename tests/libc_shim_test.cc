#include "libc_shim/libc_shim.h"

#include <atomic>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace libc_shim {
namespace {

std::atomic<int> g_guest_allocations{0};

void* CountingGuestAllocator(size_t size) {
  g_guest_allocations.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(size);
}

class LibcShimTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* ca_bundle = std::getenv("MOCKTAIL_CA_BUNDLE");
    had_ca_bundle_override_ = ca_bundle != nullptr;
    original_ca_bundle_override_ = ca_bundle == nullptr ? "" : ca_bundle;
    const char* asset_path = std::getenv("MOCKTAIL_ASSET_PATH");
    had_asset_path_override_ = asset_path != nullptr;
    original_asset_path_override_ = asset_path == nullptr ? "" : asset_path;
    ASSERT_EQ(unsetenv("MOCKTAIL_CA_BUNDLE"), 0);
    ASSERT_EQ(unsetenv("MOCKTAIL_ASSET_PATH"), 0);
    ClearPathMappings();
    ConfigureGuestAllocator(nullptr);
  }
  void TearDown() override {
    ConfigureGuestAllocator(nullptr);
    ClearPathMappings();
    if (had_ca_bundle_override_) {
      EXPECT_EQ(setenv("MOCKTAIL_CA_BUNDLE",
                       original_ca_bundle_override_.c_str(), 1),
                0);
    } else {
      EXPECT_EQ(unsetenv("MOCKTAIL_CA_BUNDLE"), 0);
    }
    if (had_asset_path_override_) {
      EXPECT_EQ(setenv("MOCKTAIL_ASSET_PATH",
                       original_asset_path_override_.c_str(), 1),
                0);
    } else {
      EXPECT_EQ(unsetenv("MOCKTAIL_ASSET_PATH"), 0);
    }
  }

  bool had_ca_bundle_override_ = false;
  std::string original_ca_bundle_override_;
  bool had_asset_path_override_ = false;
  std::string original_asset_path_override_;
};

TEST_F(LibcShimTest, TranslatesRegisteredPrefix) {
  RegisterPathMapping("/data/user/0/com.roblox.client",
                      "/home/user/.var/app/org.vinegarhq.Sober/data/sober");

  const std::string input = "/data/user/0/com.roblox.client/files/logs";
  const std::string expected =
      "/home/user/.var/app/org.vinegarhq.Sober/data/sober/files/logs";

  EXPECT_EQ(TranslatePath(input), expected);
}

TEST_F(LibcShimTest, UnregisteredPathIsReturnedUnchanged) {
  const std::string input = "/proc/self/maps";
  EXPECT_EQ(TranslatePath(input), input);
}

TEST_F(LibcShimTest, TranslatesRelativeRobloxAssetPrefixes) {
  setenv("MOCKTAIL_ASSET_PATH", "/payload/assets/content/", 1);
  Install();

  EXPECT_EQ(TranslatePath("content/guac/default.json"),
            "/payload/assets/content/guac/default.json");
  EXPECT_EQ(TranslatePath("rbx_bin/assets/content/fonts/font.json"),
            "/payload/assets/content/fonts/font.json");
  EXPECT_EQ(TranslatePath("rbx_bin/assets/shaders/mobile.pack"),
            "/payload/assets/shaders/mobile.pack");
}

TEST_F(LibcShimTest, EmptyPathIsReturnedUnchanged) {
  EXPECT_EQ(TranslatePath(""), "");
}

TEST_F(LibcShimTest, ExactPrefixMatchIsTranslated) {
  RegisterPathMapping("/data/user/0/com.roblox.client",
                      "/host/sober");

  EXPECT_EQ(TranslatePath("/data/user/0/com.roblox.client"), "/host/sober");
}

// Prefix matching stops at path-component boundaries.
TEST_F(LibcShimTest, ShortPathIsNotTranslated) {
  RegisterPathMapping("/data/user/0/com.roblox.client", "/host/sober");

  EXPECT_EQ(TranslatePath("/data/user"), "/data/user");
}

TEST_F(LibcShimTest, SimilarPrefixWithoutPathBoundaryIsNotTranslated) {
  RegisterPathMapping("/data/user/0/com.roblox.client", "/host/sober");

  EXPECT_EQ(TranslatePath("/data/user/0/com.roblox.client.beta/cache"),
            "/data/user/0/com.roblox.client.beta/cache");
}

TEST_F(LibcShimTest, MultipleRegistrationsCoexist) {
  RegisterPathMapping("/data/user/0/com.roblox.client", "/host/sober");
  RegisterPathMapping("/sdcard/Android/data/com.roblox.client",
                      "/host/sober/sdcard");

  EXPECT_EQ(TranslatePath("/data/user/0/com.roblox.client/cache"),
            "/host/sober/cache");
  EXPECT_EQ(TranslatePath("/sdcard/Android/data/com.roblox.client/obb"),
            "/host/sober/sdcard/obb");
}

TEST_F(LibcShimTest, LongestPrefixWins) {
  RegisterPathMapping("/data/user/0/com.roblox.client", "/host/sober");
  RegisterPathMapping("/data/user/0/com.roblox.client/cache",
                      "/host/sober-cache");

  EXPECT_EQ(TranslatePath("/data/user/0/com.roblox.client/cache/tombstone.dat"),
            "/host/sober-cache/tombstone.dat");
}

TEST_F(LibcShimTest, ValidCaBundleOverrideMapsAndroidAliases) {
  char temp_template[] = "/tmp/mocktail_ca_bundle_XXXXXX";
  const int fd = mkstemp(temp_template);
  ASSERT_GE(fd, 0);
  constexpr char kTestCertificate[] =
      "-----BEGIN CERTIFICATE-----\nmocktail-test\n"
      "-----END CERTIFICATE-----\n";
  ASSERT_EQ(write(fd, kTestCertificate, sizeof(kTestCertificate) - 1),
            static_cast<ssize_t>(sizeof(kTestCertificate) - 1));
  ASSERT_EQ(close(fd), 0);
  ASSERT_EQ(setenv("MOCKTAIL_CA_BUNDLE", temp_template, 1), 0);

  RegisterPathMapping("/data/user/0/com.roblox.client", "/host/sober");
  const HostCaBundleResolution resolution =
      ConfigureHostCaBundlePathMappings();

  ASSERT_TRUE(resolution.ok());
  EXPECT_TRUE(resolution.from_override);
  EXPECT_EQ(resolution.host_path, temp_template);
  EXPECT_EQ(TranslatePath(
                "/data/user/0/com.roblox.client/files/exe/cacert.pem"),
            temp_template);
  EXPECT_EQ(TranslatePath(
                "/data/data/com.roblox.client/files/exe/cacert.pem"),
            temp_template);
  EXPECT_EQ(TranslatePath("/data/user/0/com.roblox.client/files/other"),
            "/host/sober/files/other");
  FILE* mapped_file = mocktail_fopen(
      "/data/user/0/com.roblox.client/files/exe/cacert.pem", "r");
  ASSERT_NE(mapped_file, nullptr);
  EXPECT_EQ(fgetc(mapped_file), '-');
  EXPECT_EQ(fclose(mapped_file), 0);

  EXPECT_EQ(unlink(temp_template), 0);
}

TEST_F(LibcShimTest, SystemCaBundleIsResolvedAsAbsoluteRegularFile) {
  const HostCaBundleResolution resolution = ResolveHostCaBundle();

  ASSERT_TRUE(resolution.ok());
  EXPECT_FALSE(resolution.from_override);
  ASSERT_FALSE(resolution.host_path.empty());
  EXPECT_EQ(resolution.host_path.front(), '/');
  struct stat file_status {};
  ASSERT_EQ(stat(resolution.host_path.c_str(), &file_status), 0);
  EXPECT_TRUE(S_ISREG(file_status.st_mode));
  EXPECT_GT(file_status.st_size, 0);
  EXPECT_EQ(access(resolution.host_path.c_str(), R_OK), 0);
}

TEST_F(LibcShimTest, RelativeCaBundleOverrideFailsWithoutSystemFallback) {
  ASSERT_EQ(setenv("MOCKTAIL_CA_BUNDLE", "relative/cacert.pem", 1), 0);

  const HostCaBundleResolution resolution = ResolveHostCaBundle();

  EXPECT_EQ(resolution.status, HostCaBundleStatus::kInvalidOverride);
  EXPECT_TRUE(resolution.from_override);
  EXPECT_TRUE(resolution.host_path.empty());
}

TEST_F(LibcShimTest, MissingCaBundleOverrideFailsWithoutSystemFallback) {
  ASSERT_EQ(setenv("MOCKTAIL_CA_BUNDLE",
                   "/tmp/mocktail-ca-bundle-does-not-exist", 1),
            0);

  const HostCaBundleResolution resolution = ResolveHostCaBundle();

  EXPECT_EQ(resolution.status, HostCaBundleStatus::kInvalidOverride);
  EXPECT_TRUE(resolution.host_path.empty());
}

TEST_F(LibcShimTest, NonRegularCaBundleOverrideFailsClosed) {
  char temp_template[] = "/tmp/mocktail_ca_directory_XXXXXX";
  char* directory = mkdtemp(temp_template);
  ASSERT_NE(directory, nullptr);
  ASSERT_EQ(setenv("MOCKTAIL_CA_BUNDLE", directory, 1), 0);

  const HostCaBundleResolution resolution = ResolveHostCaBundle();

  EXPECT_EQ(resolution.status, HostCaBundleStatus::kInvalidOverride);
  EXPECT_TRUE(resolution.host_path.empty());
  EXPECT_EQ(rmdir(directory), 0);
}

TEST_F(LibcShimTest, UnreadableCaBundleOverrideFailsClosed) {
  char temp_template[] = "/tmp/mocktail_ca_unreadable_XXXXXX";
  const int fd = mkstemp(temp_template);
  ASSERT_GE(fd, 0);
  constexpr char kContent[] = "not-readable";
  ASSERT_EQ(write(fd, kContent, sizeof(kContent) - 1),
            static_cast<ssize_t>(sizeof(kContent) - 1));
  ASSERT_EQ(close(fd), 0);
  ASSERT_EQ(chmod(temp_template, 0000), 0);
  ASSERT_EQ(setenv("MOCKTAIL_CA_BUNDLE", temp_template, 1), 0);

  const HostCaBundleResolution resolution = ResolveHostCaBundle();

  EXPECT_EQ(resolution.status, HostCaBundleStatus::kInvalidOverride);
  EXPECT_TRUE(resolution.host_path.empty());
  EXPECT_EQ(unlink(temp_template), 0);
}

TEST_F(LibcShimTest, FilesystemCapacityCallsTranslateAndroidPaths) {
  char temp_template[] = "/tmp/mocktail_libc_shim_test_XXXXXX";
  char* root = mkdtemp(temp_template);
  ASSERT_NE(root, nullptr);

  const std::string root_path = root;
  const std::string storage_path = root_path + "/rbx-storage";
  ASSERT_EQ(mkdir(storage_path.c_str(), 0755), 0);

  RegisterPathMapping("/data/user/0/com.roblox.client", root_path);

  struct statvfs vfs {};
  EXPECT_EQ(mocktail_statvfs("/data/user/0/com.roblox.client/rbx-storage",
                             &vfs),
            0);
  EXPECT_GT(vfs.f_bsize, 0U);

  struct statfs fs {};
  EXPECT_EQ(mocktail_statfs("/data/user/0/com.roblox.client/rbx-storage", &fs),
            0);
  EXPECT_GT(fs.f_bsize, 0);

  EXPECT_EQ(rmdir(storage_path.c_str()), 0);
  EXPECT_EQ(rmdir(root_path.c_str()), 0);
}

TEST_F(LibcShimTest, RealpathUsesConfiguredGuestAllocatorForOwnedResult) {
  g_guest_allocations.store(0, std::memory_order_relaxed);
  ConfigureGuestAllocator(CountingGuestAllocator);

  char* result = mocktail_realpath("/tmp", nullptr);

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(std::string(result), "/tmp");
  EXPECT_EQ(g_guest_allocations.load(std::memory_order_relaxed), 1);
  std::free(result);
}

TEST_F(LibcShimTest, ClearRemovesAllMappings) {
  RegisterPathMapping("/data/user/0/com.roblox.client", "/host/sober");
  ClearPathMappings();

  const std::string input = "/data/user/0/com.roblox.client/files";
  EXPECT_EQ(TranslatePath(input), input);
}

}  // namespace
}  // namespace libc_shim
