#include "update/apk_provider.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace mocktail::update {
namespace {

class StubProvider final : public ApkProvider {
 public:
  StubProvider(std::string name, std::string failure, std::uint64_t code = 2628)
      : name_(std::move(name)), failure_(std::move(failure)), code_(code) {}

  std::string_view name() const override { return name_; }

  ProviderVersion CheckLatest() const override {
    ProviderVersion version;
    if (failure_.empty()) {
      version.version_name = "2.727.1199";
      version.version_code = code_;
    } else {
      version.error = failure_;
    }
    return version;
  }

  ProviderDownloadResult DownloadExact(const ProviderVersion&,
                                       const std::filesystem::path& directory,
                                       int) const override {
    ProviderDownloadResult result;
    result.source = name_;
    if (failure_.empty()) {
      result.archives.push_back(directory / "candidate.apk");
    } else {
      result.error = failure_;
    }
    return result;
  }

 private:
  std::string name_;
  std::string failure_;
  std::uint64_t code_;
};

TEST(ProviderChainTest, FallsBackToTheNextProvider) {
  ProviderChain chain;
  chain.Add(std::make_unique<StubProvider>("apk-pure",
                                           "api.pureapk.com request "
                                           "returned status 503"));
  chain.Add(std::make_unique<StubProvider>("pinned-mirror", ""));
  const ProviderDownloadResult downloaded =
      chain.DownloadExact({"2.725.1142", 2546, {}}, "/tmp/mocktail-chain", -1);
  ASSERT_TRUE(downloaded) << downloaded.error;
  EXPECT_EQ(downloaded.source, "pinned-mirror");
  ASSERT_EQ(downloaded.archives.size(), 1U);
  // Each provider downloads into its own directory so a failed attempt cannot
  // block the next one.
  EXPECT_EQ(downloaded.archives.front(),
            std::filesystem::path("/tmp/mocktail-chain/pinned-mirror/candidate.apk"));
}

TEST(ProviderChainTest, ReportsEveryProviderWhenAllFail) {
  ProviderChain chain;
  chain.Add(std::make_unique<StubProvider>("apk-pure", "returned status 503"));
  chain.Add(std::make_unique<StubProvider>("pinned-mirror", "no pinned source"));
  const ProviderDownloadResult downloaded =
      chain.DownloadExact({"2.734.917", 2908, {}}, "/tmp/mocktail-chain", -1);
  EXPECT_FALSE(downloaded);
  EXPECT_EQ(downloaded.error,
            "apk-pure: returned status 503; pinned-mirror: no pinned source");
}

TEST(ProviderChainTest, SelectsNewestVersionAcrossProviders) {
  ProviderChain chain;
  chain.Add(std::make_unique<StubProvider>("apk-pure", ""));
  chain.Add(std::make_unique<StubProvider>("newer-mirror", "", 3000));
  const ProviderVersion latest = chain.CheckLatest();
  ASSERT_TRUE(latest) << latest.error;
  EXPECT_EQ(latest.version_code, 3000);
}

TEST(ProviderChainTest, KeepsSuccessfulMetadataWhenAnotherProviderFails) {
  ProviderChain chain;
  chain.Add(std::make_unique<StubProvider>("working", "", 3000));
  chain.Add(std::make_unique<StubProvider>("broken", "offline"));
  const auto latest = chain.CheckLatest();
  ASSERT_TRUE(latest) << latest.error;
  EXPECT_EQ(latest.version_code, 3000);
}

TEST(ProviderChainTest, RejectsEmptyVersionIdentity) {
  EXPECT_FALSE(ProviderVersion{});
  ProviderChain chain;
  chain.Add(std::make_unique<StubProvider>("broken", "", 0));
  EXPECT_FALSE(chain.CheckLatest());
}

TEST(ProviderChainTest, ReportsAnEmptyChain) {
  const ProviderChain chain;
  EXPECT_TRUE(chain.empty());
  EXPECT_EQ(chain.CheckLatest().error, "no APK provider is configured");
}

}  // namespace
}  // namespace mocktail::update
