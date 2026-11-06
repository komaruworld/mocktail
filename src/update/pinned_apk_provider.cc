#include "update/pinned_apk_provider.h"

#define JSON_NOEXCEPTION 1
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <utility>

#include "compat/guest_abi.h"
#include "update/payload_integrity.h"
#include "update/state_file.h"

namespace mocktail::update {
namespace {

constexpr std::size_t kMaximumArchiveBytes = 1024ULL * 1024ULL * 1024ULL;
const std::vector<std::string> kDownloadHosts = {
    "github.com", "release-assets.githubusercontent.com",
    "objects.githubusercontent.com"};

bool HexDigest(const std::string& value, std::size_t size) {
  return value.size() == size &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

bool MatchesArchive(const std::filesystem::path& path,
                    const PinnedApkArchive& archive) {
  std::error_code error;
  return std::filesystem::is_regular_file(
             std::filesystem::symlink_status(path, error)) &&
         std::filesystem::file_size(path, error) == archive.size && !error &&
         HashRegularFile(path) == archive.sha256;
}

}  // namespace

PinnedApkSource LoadPinnedApkSource(const std::filesystem::path& manifest,
                                   const ProviderVersion& version,
                                   std::string_view abi) {
  PinnedApkSource result;
  if (!version) {
    result.error = "invalid pinned APK version";
    return result;
  }
  const auto document = ReadStateFile(manifest);
  if (!document.is_object() || !document.contains("schema_version") ||
      !document["schema_version"].is_number_integer() ||
      document["schema_version"] != 1 || !document.contains("sources") ||
      !document["sources"].is_array()) {
    result.error = "pinned APK source manifest is unavailable or invalid";
    return result;
  }
  for (const auto& source : document["sources"]) {
    if (!source.is_object() || !source.contains("abi") ||
        source["abi"] != abi || !source.contains("version_name") ||
        source["version_name"] != version.version_name ||
        !source.contains("version_code") ||
        source["version_code"] != version.version_code)
      continue;
    if (!result.archives.empty()) {
      result.error = "pinned APK source is duplicated";
      return result;
    }
    if (!source.contains("package") || source["package"] != "com.roblox.client" ||
        !source.contains("elf_build_id") ||
        !source["elf_build_id"].is_string() ||
        !HexDigest(source["elf_build_id"].get<std::string>(), 40) ||
        !source.contains("archives") || !source["archives"].is_array() ||
        source["archives"].empty() || source["archives"].size() > 4) {
      result.error = "pinned APK source has invalid identity or archives";
      return result;
    }
    result.build_id = source["elf_build_id"].get<std::string>();
    std::unordered_set<std::string> filenames;
    for (const auto& entry : source["archives"]) {
      if (!entry.is_object() || !entry.contains("filename") ||
          !entry["filename"].is_string() || !entry.contains("url") ||
          !entry["url"].is_string() || !entry.contains("sha256") ||
          !entry["sha256"].is_string() || !entry.contains("size") ||
          !entry["size"].is_number_unsigned()) {
        result.error = "pinned APK archive is incomplete";
        return result;
      }
      PinnedApkArchive archive{entry["filename"].get<std::string>(),
                               entry["url"].get<std::string>(),
                               entry["sha256"].get<std::string>(),
                               entry["size"].get<std::size_t>()};
      if (archive.filename.empty() || archive.filename.size() > 128 ||
          !std::all_of(archive.filename.begin(), archive.filename.end(),
                       [](unsigned char character) {
                         return std::isalnum(character) || character == '.' ||
                                character == '_' || character == '-';
                       }) ||
          std::filesystem::path(archive.filename).extension() != ".apk" ||
          !filenames.insert(archive.filename).second ||
          archive.size == 0 || archive.size > kMaximumArchiveBytes ||
          !HexDigest(archive.sha256, 64) ||
          !IsTrustedHttpsUrl(archive.url, kDownloadHosts)) {
        result.error = "pinned APK archive has an invalid name, URL, or digest";
        return result;
      }
      result.archives.push_back(std::move(archive));
    }
  }
  if (result.archives.empty())
    result.error = "no pinned APK source for Roblox " + version.version_name +
                   " (" + std::string(abi) + ")";
  return result;
}

PinnedApkProvider::PinnedApkProvider(std::filesystem::path manifest,
                                     Downloader download)
    : manifest_(std::move(manifest)), download_(std::move(download)) {}

ProviderVersion PinnedApkProvider::CheckLatest() const {
  return {{}, 0, "pinned APK sources do not provide version discovery"};
}

ProviderDownloadResult PinnedApkProvider::DownloadExact(
    const ProviderVersion& version, const std::filesystem::path& directory,
    int progress_fd) const {
  ProviderDownloadResult result;
  result.source = name();
  const auto source = LoadPinnedApkSource(manifest_, version, compat::kGuestAbi);
  if (!source) {
    result.error = source.error;
    return result;
  }
  for (const auto& archive : source.archives) {
    const auto destination = directory / archive.filename;
    if (!MatchesArchive(destination, archive)) {
      std::error_code error;
      std::filesystem::remove(destination, error);
      if (error) {
        result.error = "cannot replace cached APK: " + error.message();
        return result;
      }
      HttpTransferRequest request;
      request.url = archive.url;
      request.allowed_hosts = kDownloadHosts;
      request.maximum_bytes = archive.size;
      request.transfer_timeout_ms = 0;
      const auto downloaded = download_(request, destination, progress_fd);
      if (!downloaded) {
        result.error = downloaded.error;
        return result;
      }
      if (!MatchesArchive(destination, archive)) {
        std::filesystem::remove(destination, error);
        result.error = archive.filename +
                       " does not match the pinned SHA-256 or size";
        return result;
      }
    }
    result.archives.push_back(destination);
  }
  return result;
}

}  // namespace mocktail::update
