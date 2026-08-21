#include "services/client_settings_service.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace mocktail {
namespace services {
namespace {

class FakeHttpClient final : public HttpClient {
 public:
  HttpResponse response;
  int requests = 0;

  HttpResponse Get(const HttpRequest& request) override {
    ++requests;
    last_request = request;
    return response;
  }

  HttpRequest last_request;
};

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_client_settings_XXXXXX";
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

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  ASSERT_FALSE(error);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << content;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
}

TEST(ClientSettingsServiceTest, ExplicitJsonHasHighestPrecedence) {
  FakeHttpClient http;
  ClientSettingsService service(http);
  ClientSettingsOptions options;
  options.explicit_json = "{\"applicationSettings\":{\"A\":\"B\"}}";
  options.fetch = true;
  options.auto_update = true;

  const ClientSettingsResult result = service.Resolve(options);

  EXPECT_EQ(result.source, ClientSettingsSource::kExplicitJson);
  EXPECT_EQ(result.json, options.explicit_json);
  EXPECT_TRUE(result.valid_json);
  EXPECT_EQ(http.requests, 0);
}

TEST(ClientSettingsServiceTest, UsesSafeOfflineDefaults) {
  FakeHttpClient http;
  ClientSettingsService service(http);
  ClientSettingsOptions options;
  options.sober_mode = true;
  options.fetch = false;

  const ClientSettingsResult result = service.Resolve(options);

  EXPECT_EQ(result.source, ClientSettingsSource::kSafeDefaults);
  EXPECT_TRUE(result.valid_json);
  EXPECT_EQ(http.requests, 0);
}

TEST(ClientSettingsServiceTest, DownloadsAndAtomicallyCachesValidJson) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  FakeHttpClient http;
  http.response = {true, 200, "{\"applicationSettings\":{\"Flag\":true}}", {}};
  ClientSettingsService service(http);
  ClientSettingsOptions options;
  options.sober_mode = false;
  options.fetch = true;
  options.auto_update = true;
  options.cache_file = temporary.path() / "nested/settings.json";

  const ClientSettingsResult result = service.Resolve(options);

  EXPECT_EQ(result.source, ClientSettingsSource::kDownloaded);
  EXPECT_TRUE(result.cache_updated);
  EXPECT_EQ(ReadFile(options.cache_file), http.response.body);
  EXPECT_EQ(http.requests, 1);
}

TEST(ClientSettingsServiceTest, RetainsCacheOnInvalidDownload) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path cache = temporary.path() / "settings.json";
  const std::string cached_json = "{\"applicationSettings\":{\"Old\":1}}";
  WriteFile(cache, cached_json);

  FakeHttpClient http;
  http.response = {true, 200, "not-json", {}};
  ClientSettingsService service(http);
  ClientSettingsOptions options;
  options.sober_mode = false;
  options.fetch = true;
  options.auto_update = true;
  options.cache_file = cache;

  const ClientSettingsResult result = service.Resolve(options);

  EXPECT_EQ(result.source, ClientSettingsSource::kCache);
  EXPECT_EQ(result.json, cached_json);
  EXPECT_FALSE(result.error.empty());
  EXPECT_EQ(ReadFile(cache), cached_json);
}

TEST(ClientSettingsServiceTest, UsesCacheWhenNetworkIsUnavailable) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path cache = temporary.path() / "settings.json";
  const std::string cached_json = "{\"applicationSettings\":{}}";
  WriteFile(cache, cached_json);

  FakeHttpClient http;
  http.response = {false, 0, "", "offline"};
  ClientSettingsService service(http);
  ClientSettingsOptions options;
  options.sober_mode = false;
  options.fetch = true;
  options.auto_update = true;
  options.cache_file = cache;

  const ClientSettingsResult result = service.Resolve(options);

  EXPECT_EQ(result.source, ClientSettingsSource::kCache);
  EXPECT_EQ(result.json, cached_json);
  EXPECT_EQ(result.error, "offline");
}

}  // namespace
}  // namespace services
}  // namespace mocktail
