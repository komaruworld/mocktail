#include "services/browser_tracker_service.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>

namespace mocktail {
namespace services {
namespace {

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

class FakeHttpClient final : public HttpClient {
 public:
  HttpResponse Get(const HttpRequest&) override { return {}; }
  HttpResponse Post(const HttpRequest& request,
                    const std::string& body) override {
    ++post_count;
    last_request = request;
    last_body = body;
    if (on_post) on_post();
    return response;
  }

  int post_count = 0;
  HttpRequest last_request;
  std::string last_body;
  HttpResponse response;
  std::function<void()> on_post;
};

class BrowserTrackerServiceTest : public testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("mocktail-browser-tracker-" + std::to_string(getpid()) + "-" +
             ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(root_);
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  std::filesystem::path Storage() const {
    return root_ / "appData/LocalStorage/appStorage.json";
  }
  std::filesystem::path Cookies() const { return root_ / "roblox.cookie"; }
  std::filesystem::path root_;
};

TEST_F(BrowserTrackerServiceTest, ReusesValidStoredIdentityWithoutNetwork) {
  std::filesystem::create_directories(Storage().parent_path());
  std::ofstream(Storage()) << R"({"BrowserTrackerId":"123456","keep":true})";
  std::ofstream(Cookies()) << ".ROBLOSECURITY=secret; RBXEventTrackerV2="
                              "CreateDate=1&rbxid=2&browserid=123456";
  FakeHttpClient http;

  const BrowserTrackerResult result =
      BrowserTrackerService(http).EnsureInitialized(Storage(), Cookies(), true);

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.status, BrowserTrackerStatus::kExisting);
  EXPECT_EQ(result.browser_tracker_id, "123456");
  EXPECT_EQ(http.post_count, 0);
}

TEST_F(BrowserTrackerServiceTest, InitializesAndAtomicallyPreservesStorage) {
  std::filesystem::create_directories(Storage().parent_path());
  std::ofstream(Storage()) << R"({"keep":"value"})";
  std::ofstream(Cookies()) << ".ROBLOSECURITY=secret";
  FakeHttpClient http;
  http.response = {true,
                   200,
                   R"({"browserTrackerId":987654321})",
                   "",
                   {"Set-Cookie: RBXEventTrackerV2="
                    "CreateDate=1&rbxid=2&browserid=987654321; Path=/"}};

  const BrowserTrackerResult result =
      BrowserTrackerService(http).EnsureInitialized(Storage(), Cookies(), true);

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.status, BrowserTrackerStatus::kCreated);
  EXPECT_EQ(result.browser_tracker_id, "987654321");
  EXPECT_EQ(http.post_count, 1);
  EXPECT_EQ(http.last_body, "{}");
  EXPECT_NE(http.last_request.url.find("suggestedBrowserTrackerId="),
            std::string::npos);
  std::ifstream input(Storage());
  const nlohmann::json saved = nlohmann::json::parse(input);
  EXPECT_EQ(saved.at("keep"), "value");
  EXPECT_EQ(saved.at("BrowserTrackerId"), "987654321");
  EXPECT_FALSE(std::filesystem::exists(Storage().string() + ".mocktail.tmp"));
  std::ifstream cookies(Cookies());
  const std::string cookie_contents((std::istreambuf_iterator<char>(cookies)),
                                    std::istreambuf_iterator<char>());
  EXPECT_NE(cookie_contents.find("browserid=987654321"), std::string::npos);
}

TEST_F(BrowserTrackerServiceTest, CookieBrowserIdReconcilesStoredMismatch) {
  std::filesystem::create_directories(Storage().parent_path());
  std::ofstream(Storage()) << R"({"BrowserTrackerId":"111","keep":true})";
  std::ofstream(Cookies())
      << "RBXEventTrackerV2=CreateDate=1&rbxid=2&browserid=222";
  FakeHttpClient http;

  const BrowserTrackerResult result =
      BrowserTrackerService(http).EnsureInitialized(Storage(), Cookies(), true);

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.browser_tracker_id, "222");
  EXPECT_EQ(result.status, BrowserTrackerStatus::kCreated);
  EXPECT_EQ(http.post_count, 0);
  std::ifstream input(Storage());
  const nlohmann::json saved = nlohmann::json::parse(input);
  EXPECT_EQ(saved.at("BrowserTrackerId"), "222");
  EXPECT_TRUE(saved.at("keep"));
}

TEST_F(BrowserTrackerServiceTest, RejectsMalformedResponseWithoutMutation) {
  FakeHttpClient http;
  std::ofstream(Cookies()) << ".ROBLOSECURITY=secret";
  http.response = {true,
                   200,
                   R"({"browserTrackerId":"12x"})",
                   "",
                   {"Set-Cookie: RBXEventTrackerV2="
                    "CreateDate=1&rbxid=2&browserid=12x; Path=/"}};

  const BrowserTrackerResult result =
      BrowserTrackerService(http).EnsureInitialized(Storage(), Cookies(), true);

  EXPECT_FALSE(result);
  EXPECT_EQ(result.error,
            "BrowserTracker response does not contain a valid numeric ID");
  EXPECT_FALSE(std::filesystem::exists(Storage()));
}

TEST_F(BrowserTrackerServiceTest,
       CookieRefreshDuringHttpIsPreservedWithoutRestoringStaleCredential) {
  std::filesystem::create_directories(root_);
  const std::string original = ".ROBLOSECURITY=rejected";
  const std::string refreshed = ".ROBLOSECURITY=refreshed";
  std::ofstream(Cookies()) << original;
  FakeHttpClient http;
  http.response = {true,
                   200,
                   R"({"browserTrackerId":987654321})",
                   "",
                   {"Set-Cookie: RBXEventTrackerV2="
                    "CreateDate=1&rbxid=2&browserid=987654321; Path=/"}};
  http.on_post = [this, &refreshed]() {
    std::ofstream(Cookies(), std::ios::trunc) << refreshed;
  };

  const BrowserTrackerResult result =
      BrowserTrackerService(http).EnsureInitialized(Storage(), Cookies(), true);

  EXPECT_FALSE(result);
  EXPECT_EQ(result.error, "cookie file changed during BrowserTracker update");
  EXPECT_EQ(http.post_count, 1);
  EXPECT_EQ(ReadFile(Cookies()), refreshed);
  EXPECT_EQ(ReadFile(Cookies()).find("rejected"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Storage()));
}

TEST_F(BrowserTrackerServiceTest, RejectsCorruptStoredIdentityFailClosed) {
  std::filesystem::create_directories(Storage().parent_path());
  std::ofstream(Storage()) << R"({"BrowserTrackerId":"0"})";
  std::ofstream(Cookies()) << ".ROBLOSECURITY=secret";
  FakeHttpClient http;

  const BrowserTrackerResult result =
      BrowserTrackerService(http).EnsureInitialized(Storage(), Cookies(), true);

  EXPECT_FALSE(result);
  EXPECT_EQ(result.error, "appStorage BrowserTrackerId is invalid");
  EXPECT_EQ(http.post_count, 0);
}

TEST_F(BrowserTrackerServiceTest, RejectsTrackerCookieWithInvalidBrowserId) {
  std::filesystem::create_directories(root_);
  std::ofstream(Cookies())
      << "RBXEventTrackerV2=CreateDate=1&rbxid=2&browserid=invalid";
  FakeHttpClient http;

  const BrowserTrackerResult result =
      BrowserTrackerService(http).EnsureInitialized(Storage(), Cookies(), true);

  EXPECT_FALSE(result);
  EXPECT_EQ(result.error, "RBXEventTrackerV2 cookie has an invalid browserid");
  EXPECT_EQ(http.post_count, 0);
}

TEST_F(BrowserTrackerServiceTest, DoesNotMutateExternalCookieWithoutTracker) {
  std::filesystem::create_directories(root_);
  std::ofstream(Cookies()) << ".ROBLOSECURITY=secret";
  FakeHttpClient http;

  const BrowserTrackerResult result =
      BrowserTrackerService(http).EnsureInitialized(Storage(), Cookies(),
                                                    false);

  EXPECT_FALSE(result);
  EXPECT_EQ(result.error,
            "external cookie file has no RBXEventTrackerV2 browser identity");
  EXPECT_EQ(http.post_count, 0);
  std::ifstream input(Cookies());
  std::string contents;
  std::getline(input, contents);
  EXPECT_EQ(contents, ".ROBLOSECURITY=secret");
}

TEST_F(BrowserTrackerServiceTest,
       LegacyStorageTemporaryHardlinkDoesNotMutateExternalCookie) {
  std::filesystem::create_directories(Storage().parent_path());
  std::ofstream(Storage()) << R"({"BrowserTrackerId":"111"})";
  const std::string cookies =
      ".ROBLOSECURITY=protected; RBXEventTrackerV2="
      "CreateDate=1&rbxid=2&browserid=222";
  std::ofstream(Cookies()) << cookies;
  ASSERT_EQ(chmod(Cookies().c_str(), S_IRUSR), 0);
  const std::filesystem::path legacy_temporary =
      Storage().string() + ".mocktail.tmp";
  std::error_code filesystem_error;
  std::filesystem::create_hard_link(Cookies(), legacy_temporary,
                                    filesystem_error);
  ASSERT_FALSE(filesystem_error);
  FakeHttpClient http;

  const BrowserTrackerResult result =
      BrowserTrackerService(http).EnsureInitialized(Storage(), Cookies(),
                                                    false);

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.browser_tracker_id, "222");
  EXPECT_EQ(result.status, BrowserTrackerStatus::kCreated);
  EXPECT_EQ(http.post_count, 0);
  EXPECT_EQ(ReadFile(Cookies()), cookies);
  EXPECT_EQ(ReadFile(legacy_temporary), cookies);
  struct stat metadata = {};
  ASSERT_EQ(lstat(Cookies().c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO), S_IRUSR);
}

TEST_F(BrowserTrackerServiceTest,
       LegacyCookieTemporaryHardlinkDoesNotMutateProtectedFile) {
  std::filesystem::create_directories(root_);
  std::ofstream(Cookies()) << ".ROBLOSECURITY=managed";
  const std::filesystem::path protected_file = root_ / "protected-sober.cookie";
  const std::string protected_contents = ".ROBLOSECURITY=protected";
  std::ofstream(protected_file) << protected_contents;
  ASSERT_EQ(chmod(protected_file.c_str(), S_IRUSR), 0);
  const std::filesystem::path legacy_temporary =
      Cookies().string() + ".mocktail.tmp";
  std::error_code filesystem_error;
  std::filesystem::create_hard_link(protected_file, legacy_temporary,
                                    filesystem_error);
  ASSERT_FALSE(filesystem_error);
  FakeHttpClient http;
  http.response = {true,
                   200,
                   R"({"browserTrackerId":987654321})",
                   "",
                   {"Set-Cookie: RBXEventTrackerV2="
                    "CreateDate=1&rbxid=2&browserid=987654321; Path=/"}};

  const BrowserTrackerResult result =
      BrowserTrackerService(http).EnsureInitialized(Storage(), Cookies(), true);

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.browser_tracker_id, "987654321");
  EXPECT_EQ(http.post_count, 1);
  EXPECT_EQ(ReadFile(protected_file), protected_contents);
  EXPECT_EQ(ReadFile(legacy_temporary), protected_contents);
  struct stat metadata = {};
  ASSERT_EQ(lstat(protected_file.c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO), S_IRUSR);
}

TEST_F(BrowserTrackerServiceTest,
       RejectsHardlinkedStorageDestinationWithoutMutation) {
  std::filesystem::create_directories(Storage().parent_path());
  const std::filesystem::path protected_storage = root_ / "protected-storage";
  const std::string storage_contents = R"({"BrowserTrackerId":"111"})";
  std::ofstream(protected_storage) << storage_contents;
  ASSERT_EQ(chmod(protected_storage.c_str(), S_IRUSR), 0);
  std::error_code filesystem_error;
  std::filesystem::create_hard_link(protected_storage, Storage(),
                                    filesystem_error);
  ASSERT_FALSE(filesystem_error);
  std::ofstream(Cookies())
      << "RBXEventTrackerV2=CreateDate=1&rbxid=2&browserid=222";
  FakeHttpClient http;

  const BrowserTrackerResult result =
      BrowserTrackerService(http).EnsureInitialized(Storage(), Cookies(), true);

  EXPECT_FALSE(result);
  EXPECT_EQ(result.error, "cannot atomically replace appStorage");
  EXPECT_EQ(http.post_count, 0);
  EXPECT_EQ(ReadFile(protected_storage), storage_contents);
  EXPECT_EQ(ReadFile(Storage()), storage_contents);
  struct stat metadata = {};
  ASSERT_EQ(lstat(protected_storage.c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO), S_IRUSR);
}

TEST_F(BrowserTrackerServiceTest,
       RejectsHardlinkedLockWithoutMutatingExternalCookie) {
  std::filesystem::create_directories(Storage().parent_path());
  std::ofstream(Storage()) << R"({"BrowserTrackerId":"222"})";
  const std::string cookies =
      ".ROBLOSECURITY=protected; RBXEventTrackerV2="
      "CreateDate=1&rbxid=2&browserid=222";
  std::ofstream(Cookies()) << cookies;
  ASSERT_EQ(chmod(Cookies().c_str(), S_IRUSR | S_IWUSR), 0);
  std::error_code filesystem_error;
  std::filesystem::create_hard_link(Cookies(), Storage().string() + ".lock",
                                    filesystem_error);
  ASSERT_FALSE(filesystem_error);
  FakeHttpClient http;

  const BrowserTrackerResult result =
      BrowserTrackerService(http).EnsureInitialized(Storage(), Cookies(),
                                                    false);

  EXPECT_FALSE(result);
  EXPECT_EQ(result.error, "cannot lock appStorage");
  EXPECT_EQ(http.post_count, 0);
  EXPECT_EQ(ReadFile(Cookies()), cookies);
  struct stat metadata = {};
  ASSERT_EQ(lstat(Cookies().c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO),
            S_IRUSR | S_IWUSR);
}

}  // namespace
}  // namespace services
}  // namespace mocktail
