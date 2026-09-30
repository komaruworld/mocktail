#ifndef MOCKTAIL_UPDATE_MOCKTAIL_RELEASE_H_
#define MOCKTAIL_UPDATE_MOCKTAIL_RELEASE_H_

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "update/http_download.h"

namespace mocktail::update {

struct ReleaseVersion {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  std::uint32_t patch = 0;
};

std::optional<ReleaseVersion> ParseReleaseVersion(std::string_view text);
int CompareReleaseVersions(const ReleaseVersion& left,
                           const ReleaseVersion& right);

bool IsValidRepositorySlug(std::string_view repository);

struct MocktailRelease {
  std::string tag;
  std::string version;
  std::string url;
  std::vector<std::uint64_t> supported_roblox_codes;
  bool catalog_known = false;
};

struct MocktailReleaseResult {
  MocktailRelease release;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

using HttpFetcher = std::function<HttpBytesResult(const HttpTransferRequest&)>;

MocktailReleaseResult ParseLatestReleaseDocument(std::string_view document,
                                                 std::string_view repository);

MocktailReleaseResult FetchLatestMocktailRelease(std::string_view repository,
                                                 const HttpFetcher& fetch);

struct MocktailReleaseCheckOptions {
  std::string repository;
  std::filesystem::path state_file;
  std::time_t now = 0;
  std::time_t refresh_seconds = 12 * 60 * 60;
  std::time_t retry_seconds = 60 * 60;
  bool force_refresh = false;
  HttpFetcher fetch;
};

struct MocktailReleaseCheck {
  std::optional<MocktailRelease> latest;
  bool refreshed = false;
  std::string notified_key;
  std::string error;
};

MocktailReleaseCheck CheckMocktailRelease(
    const MocktailReleaseCheckOptions& options);

std::string ReadNotifiedKey(const std::filesystem::path& state_file);
bool RecordNotifiedKey(const std::filesystem::path& state_file,
                       std::string_view key, std::string* error);

struct RobloxUpdateState {
  std::string active_version_name;
  std::uint64_t active_version_code = 0;
  std::string latest_version_name;
  std::uint64_t latest_version_code = 0;
  bool latest_rejected = false;
};

struct UpdateNotice {
  std::string key;
  std::string heading;
  std::string body;
  std::string command;

  bool empty() const { return key.empty(); }
};

// `latest` is empty when this build does not check for releases.
UpdateNotice ComposeUpdateNotice(std::string_view installed_version,
                                 const std::optional<MocktailRelease>& latest,
                                 const RobloxUpdateState& roblox);

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_MOCKTAIL_RELEASE_H_
