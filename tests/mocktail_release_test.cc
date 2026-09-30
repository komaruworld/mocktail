#include "update/mocktail_release.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mocktail::update {
namespace {

constexpr char kRepository[] = "komaruworld/mocktail";
constexpr char kReleaseUrl[] =
    "https://github.com/komaruworld/mocktail/releases/tag/1.0.5";

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_release_test_XXXXXX";
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

std::string ReleaseDocument(const std::string& tag,
                            const std::string& url = kReleaseUrl,
                            bool prerelease = false) {
  return "{\"tag_name\":\"" + tag + "\",\"html_url\":\"" + url +
         "\",\"draft\":false,\"prerelease\":" +
         (prerelease ? "true" : "false") + ",\"assets\":[]}";
}

std::string Catalog(const std::vector<std::uint64_t>& codes) {
  std::string profiles;
  for (const std::uint64_t code : codes) {
    if (!profiles.empty()) profiles += ",";
    profiles += "{\"version_name\":\"2.0." + std::to_string(code) +
                "\",\"version_code\":" + std::to_string(code) +
                ",\"elf_build_id\":\"" + std::string(40, 'a') +
                "\",\"status\":\"supported\",\"default_allowed\":true,"
                "\"allow_legacy_binary_patches\":false}";
  }
  return "{\"schema_version\":1,\"profiles\":[" + profiles + "]}";
}

struct FakeGitHub {
  std::map<std::string, std::string> bodies;
  std::vector<HttpTransferRequest> requests;

  HttpFetcher Fetcher() {
    return [this](const HttpTransferRequest& request) {
      requests.push_back(request);
      HttpBytesResult result;
      const auto found = bodies.find(request.url);
      if (found == bodies.end()) {
        result.error = "fixture has no response for " + request.url;
        return result;
      }
      result.status_code = 200;
      result.bytes = found->second;
      result.final_url = request.url;
      return result;
    };
  }
};

constexpr char kLatestUrl[] =
    "https://api.github.com/repos/komaruworld/mocktail/releases/latest";

std::string CatalogUrl(const std::string& tag) {
  return "https://raw.githubusercontent.com/komaruworld/mocktail/" + tag +
         "/config/roblox_compatibility.json";
}

TEST(ReleaseVersionTest, ParsesReleaseTags) {
  const auto plain = ParseReleaseVersion("1.0.4");
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(plain->major, 1U);
  EXPECT_EQ(plain->minor, 0U);
  EXPECT_EQ(plain->patch, 4U);
  const auto prefixed = ParseReleaseVersion("v2.10");
  ASSERT_TRUE(prefixed.has_value());
  EXPECT_EQ(prefixed->minor, 10U);
  EXPECT_EQ(prefixed->patch, 0U);
  for (const char* invalid :
       {"", "1", "1.0.4.5", "1.0.x", "1..0", "-1.0.0", "1.0.", "continuous",
        "1.0.4-nightly"}) {
    EXPECT_FALSE(ParseReleaseVersion(invalid).has_value()) << invalid;
  }
}

TEST(ReleaseVersionTest, ComparesNumerically) {
  EXPECT_LT(CompareReleaseVersions(*ParseReleaseVersion("1.0.9"),
                                   *ParseReleaseVersion("1.0.10")),
            0);
  EXPECT_GT(CompareReleaseVersions(*ParseReleaseVersion("2.0.0"),
                                   *ParseReleaseVersion("1.99.99")),
            0);
  EXPECT_EQ(CompareReleaseVersions(*ParseReleaseVersion("1.0"),
                                   *ParseReleaseVersion("v1.0.0")),
            0);
}

TEST(ReleaseVersionTest, ValidatesRepositorySlugs) {
  EXPECT_TRUE(IsValidRepositorySlug("komaruworld/mocktail"));
  EXPECT_TRUE(IsValidRepositorySlug("some-user/my.fork_2"));
  for (const char* invalid :
       {"", "mocktail", "/mocktail", "owner/", "a/b/c", "owner/..",
        "own.er/repo", "owner/re po", "owner/repo?x=1"}) {
    EXPECT_FALSE(IsValidRepositorySlug(invalid)) << invalid;
  }
}

TEST(MocktailReleaseTest, ParsesLatestReleaseDocument) {
  const MocktailReleaseResult parsed =
      ParseLatestReleaseDocument(ReleaseDocument("1.0.5"), kRepository);
  ASSERT_TRUE(parsed) << parsed.error;
  EXPECT_EQ(parsed.release.tag, "1.0.5");
  EXPECT_EQ(parsed.release.version, "1.0.5");
  EXPECT_EQ(parsed.release.url, kReleaseUrl);

  const MocktailReleaseResult prefixed =
      ParseLatestReleaseDocument(ReleaseDocument("v1.0.5"), kRepository);
  ASSERT_TRUE(prefixed) << prefixed.error;
  EXPECT_EQ(prefixed.release.tag, "v1.0.5");
  EXPECT_EQ(prefixed.release.version, "1.0.5");
}

TEST(MocktailReleaseTest, RejectsUntrustedReleaseDocuments) {
  EXPECT_FALSE(ParseLatestReleaseDocument("not json", kRepository));
  EXPECT_FALSE(ParseLatestReleaseDocument("[]", kRepository));
  EXPECT_FALSE(ParseLatestReleaseDocument(
      ReleaseDocument("1.0.5", kReleaseUrl, true), kRepository));
  EXPECT_FALSE(ParseLatestReleaseDocument(
      ReleaseDocument("../../other/repo/main"), kRepository));
  EXPECT_FALSE(
      ParseLatestReleaseDocument(ReleaseDocument("continuous"), kRepository));
  EXPECT_FALSE(ParseLatestReleaseDocument(
      ReleaseDocument("1.0.5",
                      "https://github.com/someone/else/releases/tag/1.0.5"),
      kRepository));
  EXPECT_FALSE(ParseLatestReleaseDocument(
      ReleaseDocument("1.0.5",
                      "http://github.com/komaruworld/mocktail/releases/tag/1"),
      kRepository));
}

TEST(MocktailReleaseTest, FetchesReleaseAndItsRobloxCatalog) {
  FakeGitHub github;
  github.bodies[kLatestUrl] = ReleaseDocument("1.0.5");
  github.bodies[CatalogUrl("1.0.5")] = Catalog({3092, 2998, 3092});

  const MocktailReleaseResult fetched =
      FetchLatestMocktailRelease(kRepository, github.Fetcher());

  ASSERT_TRUE(fetched) << fetched.error;
  EXPECT_EQ(fetched.release.version, "1.0.5");
  EXPECT_TRUE(fetched.release.catalog_known);
  EXPECT_EQ(fetched.release.supported_roblox_codes,
            (std::vector<std::uint64_t>{2998, 3092}));
  ASSERT_EQ(github.requests.size(), 2U);
  EXPECT_EQ(github.requests[0].allowed_hosts,
            std::vector<std::string>{"api.github.com"});
  EXPECT_EQ(github.requests[1].allowed_hosts,
            std::vector<std::string>{"raw.githubusercontent.com"});
  for (const HttpTransferRequest& request : github.requests) {
    EXPECT_EQ(request.maximum_attempts, 1);
    EXPECT_LE(request.transfer_timeout_ms, 10000);
  }
}

TEST(MocktailReleaseTest, ReleaseWithoutReadableCatalogIsStillReported) {
  FakeGitHub github;
  github.bodies[kLatestUrl] = ReleaseDocument("1.0.5");

  const MocktailReleaseResult fetched =
      FetchLatestMocktailRelease(kRepository, github.Fetcher());

  ASSERT_TRUE(fetched) << fetched.error;
  EXPECT_FALSE(fetched.release.catalog_known);
  EXPECT_TRUE(fetched.release.supported_roblox_codes.empty());
}

TEST(MocktailReleaseTest, CachesChecksAndRetriesFailuresSooner) {
  TemporaryDirectory temporary;
  FakeGitHub github;
  github.bodies[kLatestUrl] = ReleaseDocument("1.0.5");
  github.bodies[CatalogUrl("1.0.5")] = Catalog({3092});
  MocktailReleaseCheckOptions options;
  options.repository = kRepository;
  options.state_file = temporary.root() / "state/mocktail-release.json";
  options.now = 1000000;
  options.fetch = github.Fetcher();

  MocktailReleaseCheck first = CheckMocktailRelease(options);
  ASSERT_TRUE(first.error.empty()) << first.error;
  EXPECT_TRUE(first.refreshed);
  ASSERT_TRUE(first.latest.has_value());
  EXPECT_EQ(first.latest->version, "1.0.5");
  EXPECT_EQ(std::filesystem::status(options.state_file).permissions() &
                std::filesystem::perms::all,
            std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write);

  options.now += options.refresh_seconds - 1;
  const MocktailReleaseCheck cached = CheckMocktailRelease(options);
  EXPECT_FALSE(cached.refreshed);
  ASSERT_TRUE(cached.latest.has_value());
  EXPECT_TRUE(cached.latest->catalog_known);
  EXPECT_EQ(github.requests.size(), 2U);

  github.bodies.clear();
  options.now += 2;
  const MocktailReleaseCheck failed = CheckMocktailRelease(options);
  EXPECT_TRUE(failed.refreshed);
  EXPECT_FALSE(failed.error.empty());
  ASSERT_TRUE(failed.latest.has_value());
  EXPECT_EQ(failed.latest->version, "1.0.5");

  options.now += options.retry_seconds - 1;
  EXPECT_FALSE(CheckMocktailRelease(options).refreshed);
  options.now += 2;
  EXPECT_TRUE(CheckMocktailRelease(options).refreshed);
}

TEST(MocktailReleaseTest, RemembersTheLastNoticeAndForgetsOtherRepositories) {
  TemporaryDirectory temporary;
  FakeGitHub github;
  github.bodies[kLatestUrl] = ReleaseDocument("1.0.5");
  MocktailReleaseCheckOptions options;
  options.repository = kRepository;
  options.state_file = temporary.root() / "mocktail-release.json";
  options.now = 5000;
  options.fetch = github.Fetcher();
  ASSERT_TRUE(CheckMocktailRelease(options).latest.has_value());

  std::string error;
  ASSERT_TRUE(RecordNotifiedKey(options.state_file, "release:1.0.5", &error))
      << error;
  const MocktailReleaseCheck remembered = CheckMocktailRelease(options);
  EXPECT_FALSE(remembered.refreshed);
  EXPECT_EQ(remembered.notified_key, "release:1.0.5");

  options.repository = "someone/fork";
  const MocktailReleaseCheck other = CheckMocktailRelease(options);
  EXPECT_TRUE(other.refreshed);
  EXPECT_TRUE(other.notified_key.empty());
}

MocktailRelease Release(const std::string& version,
                        std::vector<std::uint64_t> codes = {}) {
  MocktailRelease release;
  release.tag = version;
  release.version = version;
  release.url = kReleaseUrl;
  release.catalog_known = !codes.empty();
  release.supported_roblox_codes = std::move(codes);
  return release;
}

RobloxUpdateState RejectedRoblox() {
  RobloxUpdateState roblox;
  roblox.active_version_name = "2.736.1408";
  roblox.active_version_code = 2998;
  roblox.latest_version_name = "2.738.100";
  roblox.latest_version_code = 3092;
  roblox.latest_rejected = true;
  return roblox;
}

TEST(UpdateNoticeTest, NothingToSayWhenCurrent) {
  EXPECT_TRUE(ComposeUpdateNotice("1.0.4", Release("1.0.4"), {})
                  .empty());
  EXPECT_TRUE(
      ComposeUpdateNotice("1.0.4", std::nullopt, {}).empty());
  EXPECT_TRUE(ComposeUpdateNotice("1.0.5", Release("1.0.4"), {})
                  .empty());
  RobloxUpdateState downloading = RejectedRoblox();
  downloading.latest_rejected = false;
  EXPECT_TRUE(ComposeUpdateNotice("1.0.4", Release("1.0.4"), downloading)
                  .empty());
}

TEST(UpdateNoticeTest, PointsAtTheNewRelease) {
  const UpdateNotice notice = ComposeUpdateNotice(
      "1.0.4", Release("1.0.5"), {});
  EXPECT_EQ(notice.key, "release:1.0.5");
  EXPECT_EQ(notice.heading, "Mocktail update available");
  EXPECT_NE(notice.body.find("Mocktail 1.0.5 is available"),
            std::string::npos);
  EXPECT_NE(notice.body.find("Mocktail 1.0.4 is installed"),
            std::string::npos);
  EXPECT_EQ(notice.command, kReleaseUrl);
}

TEST(UpdateNoticeTest, ExplainsThatTheNewReleaseRunsTheRejectedRoblox) {
  const UpdateNotice notice = ComposeUpdateNotice(
      "1.0.4", Release("1.0.5", {2998, 3092}), RejectedRoblox());
  EXPECT_EQ(notice.key, "release:1.0.5:roblox:3092");
  EXPECT_EQ(notice.heading, "Update Mocktail for new Roblox");
  EXPECT_NE(notice.body.find("Roblox 2.738.100 is out, but it did not pass "
                             "Mocktail 1.0.4's compatibility check on this "
                             "computer, so Mocktail keeps Roblox 2.736.1408"),
            std::string::npos);
  EXPECT_NE(notice.body.find("Mocktail 1.0.5 supports Roblox 2.738.100"),
            std::string::npos);
  EXPECT_EQ(notice.command, kReleaseUrl);
}

TEST(UpdateNoticeTest, DoesNotPromiseSupportTheReleaseDoesNotList) {
  const UpdateNotice notice = ComposeUpdateNotice(
      "1.0.4", Release("1.0.5", {2998}), RejectedRoblox());
  EXPECT_EQ(notice.heading, "Mocktail update available");
  EXPECT_EQ(notice.body.find("supports Roblox 2.738.100"), std::string::npos);
  EXPECT_NE(notice.body.find("did not pass"), std::string::npos);
}

TEST(UpdateNoticeTest, WarnsOnceWhenNoReleaseRunsTheNewRoblox) {
  const UpdateNotice notice = ComposeUpdateNotice(
      "1.0.4", Release("1.0.4", {2998}), RejectedRoblox());
  EXPECT_EQ(notice.key, "roblox:3092:mocktail:1.0.4");
  EXPECT_NE(ComposeUpdateNotice("1.0.5", Release("1.0.5"), RejectedRoblox())
                .key,
            notice.key);
  EXPECT_EQ(notice.heading, "Roblox update pending");
  EXPECT_NE(notice.body.find("None is published yet (latest: 1.0.4)"),
            std::string::npos);
  EXPECT_TRUE(notice.command.empty());

  const UpdateNotice packaged =
      ComposeUpdateNotice("1.0.4", std::nullopt, RejectedRoblox());
  EXPECT_EQ(packaged.heading, "Roblox update pending");
  EXPECT_NE(packaged.body.find("same way you installed it"), std::string::npos);
  EXPECT_TRUE(packaged.command.empty());

  RobloxUpdateState caught_up = RejectedRoblox();
  caught_up.active_version_code = 3092;
  EXPECT_TRUE(ComposeUpdateNotice("1.0.4", Release("1.0.4"), caught_up)
                  .empty());
}

}  // namespace
}  // namespace mocktail::update
