#include "update/pinned_apk_provider.h"

#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

#include "compat/guest_abi.h"
#include "update/payload_integrity.h"

namespace mocktail::update {
namespace {

class PinnedApkProviderTest : public testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/mocktail_pinned_XXXXXX";
    const char* created = mkdtemp(pattern);
    ASSERT_NE(created, nullptr);
    root = created;
    original = root / "original.apk";
    std::ofstream(original) << "APK fixture bytes";
    manifest = root / "sources.json";
    entry = {{"package", "com.roblox.client"},
             {"version_name", version.version_name},
             {"version_code", version.version_code},
             {"abi", compat::kGuestAbi},
             {"elf_build_id", std::string(40, 'a')},
             {"archives", nlohmann::json::array({
                 {{"filename", "base.apk"},
                  {"url", "https://github.com/example/releases/base.apk"},
                  {"sha256", HashRegularFile(original)},
                  {"size", std::filesystem::file_size(original)}}})}};
    Save();
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  void Save() {
    std::ofstream(manifest)
        << nlohmann::json{{"schema_version", 1}, {"sources", {entry}}};
  }

  PinnedApkProvider Provider() {
    return PinnedApkProvider(manifest, [this](const HttpTransferRequest& request,
                                            const auto& destination, int) {
      ++downloads;
      EXPECT_EQ(request.maximum_bytes, std::filesystem::file_size(original));
      EXPECT_EQ(request.transfer_timeout_ms, 0);
      std::filesystem::create_directories(destination.parent_path());
      std::filesystem::copy_file(original, destination,
          std::filesystem::copy_options::overwrite_existing);
      return HttpDownloadResult{};
    });
  }

  std::filesystem::path root, original, manifest;
  ProviderVersion version{"2.742.1059", 3222, {}};
  nlohmann::json entry;
  int downloads = 0;
};

TEST_F(PinnedApkProviderTest, ReusesOnlyVerifiedCachedArchives) {
  const auto provider = Provider();
  const auto destination = root / "cache";
  ASSERT_TRUE(provider.DownloadExact(version, destination, -1));
  ASSERT_EQ(downloads, 1);
  ASSERT_TRUE(provider.DownloadExact(version, destination, -1));
  EXPECT_EQ(downloads, 1);
  std::ofstream(destination / "base.apk") << "bad fixture bytes";
  ASSERT_TRUE(provider.DownloadExact(version, destination, -1));
  EXPECT_EQ(downloads, 2);
  EXPECT_EQ(HashRegularFile(destination / "base.apk"), HashRegularFile(original));
}

TEST_F(PinnedApkProviderTest, RejectsContentThatDisagreesWithTheManifest) {
  const auto provider = PinnedApkProvider(manifest,
      [](const HttpTransferRequest&, const auto& destination, int) {
        std::filesystem::create_directories(destination.parent_path());
        std::ofstream(destination) << "incorrect archive";
        return HttpDownloadResult{};
      });
  const auto result = provider.DownloadExact(version, root / "cache", -1);
  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("pinned SHA-256 or size"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(root / "cache/base.apk"));
}

TEST_F(PinnedApkProviderTest, PreservesPartialDownloadsAfterNetworkFailure) {
  const auto provider = PinnedApkProvider(manifest,
      [](const HttpTransferRequest&, const auto& destination, int) {
        std::filesystem::create_directories(destination.parent_path());
        std::ofstream(destination.string() + ".part") << "partial";
        HttpDownloadResult result;
        result.error = "network interrupted";
        return result;
      });
  EXPECT_FALSE(provider.DownloadExact(version, root / "cache", -1));
  EXPECT_TRUE(std::filesystem::exists(root / "cache/base.apk.part"));
}

TEST_F(PinnedApkProviderTest, RequiresExactVersionAndArchitecture) {
  auto wrong = version;
  ++wrong.version_code;
  EXPECT_FALSE(Provider().DownloadExact(wrong, root / "cache", -1));
  EXPECT_FALSE(LoadPinnedApkSource(manifest, version,
      compat::kGuestAbi == "x86_64" ? "arm64-v8a" : "x86_64"));
  EXPECT_EQ(downloads, 0);
  EXPECT_FALSE(Provider().HasVersionDiscovery());
}

TEST_F(PinnedApkProviderTest, RejectsMalformedOrUnsafeArchivesBeforeDownloading) {
  const auto valid = entry;
  const std::vector<std::pair<std::string, nlohmann::json>> invalid = {
      {"filename", "../base.apk"}, {"filename", "/base.apk"},
      {"url", "http://github.com/base.apk"},
      {"url", "https://untrusted.invalid/base.apk"},
      {"sha256", std::string(64, 'z')}, {"sha256", false},
      {"size", -1}, {"size", 0}, {"size", "100"},
      {"size", 1073741825ULL}};
  for (const auto& [key, value] : invalid) {
    SCOPED_TRACE(key + ": " + value.dump());
    entry = valid;
    entry["archives"][0][key] = value;
    Save();
    EXPECT_FALSE(Provider().DownloadExact(version, root / "cache", -1));
  }
  EXPECT_EQ(downloads, 0);
}

TEST_F(PinnedApkProviderTest, RejectsDuplicateIdentities) {
  std::ofstream(manifest)
      << nlohmann::json{{"schema_version", 1}, {"sources", {entry, entry}}};
  EXPECT_FALSE(Provider().DownloadExact(version, root / "cache", -1));
  EXPECT_EQ(downloads, 0);
}

TEST_F(PinnedApkProviderTest, ReplacesCacheSymlinksWithoutTouchingTheirTarget) {
  std::filesystem::create_directories(root / "cache");
  std::filesystem::create_symlink(original, root / "cache/base.apk");
  ASSERT_TRUE(Provider().DownloadExact(version, root / "cache", -1));
  EXPECT_EQ(downloads, 1);
  EXPECT_FALSE(std::filesystem::is_symlink(root / "cache/base.apk"));
  EXPECT_EQ(std::filesystem::file_size(original), 17U);
}

}  // namespace
}  // namespace mocktail::update
