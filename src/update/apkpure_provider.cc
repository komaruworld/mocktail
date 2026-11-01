#include "update/apkpure_provider.h"

#include <sys/statvfs.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "compat/guest_abi.h"
#include "update/http_download.h"

namespace mocktail::update {
namespace {

constexpr std::size_t kMaximumMetadataBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumArchiveBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumExactCandidates = 4;
constexpr std::string_view kApiUrl =
    "https://api.pureapk.com/m/v3/cms/app_version?hl=en-US&package_name="
    "com.roblox.client";

const std::vector<std::string> kMetadataHosts = {"api.pureapk.com"};
const std::vector<std::string> kDownloadHosts = {"pureapk.com", "apkpure.com",
                                                 "winudf.com"};

bool VersionCharacter(unsigned char character) {
  return std::isalnum(character) || character == '.' || character == '+' ||
         character == '_' || character == '-';
}

struct VersionMarker {
  std::size_t begin = 0;
  std::size_t colon = 0;
  std::string value;
};

std::vector<VersionMarker> VersionMarkers(std::string_view metadata) {
  std::vector<VersionMarker> markers;
  for (std::size_t index = 0; index < metadata.size(); ++index) {
    if (!std::isdigit(static_cast<unsigned char>(metadata[index])) ||
        (index > 0 &&
         VersionCharacter(static_cast<unsigned char>(metadata[index - 1])))) {
      continue;
    }
    std::size_t end = index;
    bool dot = false;
    while (end < metadata.size() &&
           VersionCharacter(static_cast<unsigned char>(metadata[end]))) {
      dot = dot || metadata[end] == '.';
      ++end;
    }
    if (dot && end < metadata.size() && metadata[end] == ':') {
      markers.push_back(VersionMarker{
          index, end, std::string(metadata.substr(index, end - index))});
      index = end;
    }
  }
  return markers;
}

bool UrlCharacter(unsigned char character) {
  return std::isalnum(character) ||
         std::string_view("-@:%._+~#=?&/()")
                 .find(static_cast<char>(character)) != std::string_view::npos;
}

std::vector<std::pair<std::string, bool>> DownloadsInRecord(
    std::string_view record) {
  std::vector<std::pair<std::string, bool>> downloads;
  for (std::size_t index = 0; index < record.size(); ++index) {
    bool xapk = false;
    std::size_t url_begin = std::string_view::npos;
    if (record.substr(index, 5) == "XAPKJ" && index + 7 < record.size()) {
      xapk = true;
      url_begin = index + 7;
    } else if (record.substr(index, 4) == "APKJ" && index + 6 < record.size()) {
      url_begin = index + 6;
    }
    if (url_begin == std::string_view::npos ||
        record.substr(url_begin, 8) != "https://") {
      continue;
    }
    std::size_t url_end = url_begin;
    while (url_end < record.size() &&
           UrlCharacter(static_cast<unsigned char>(record[url_end]))) {
      ++url_end;
    }
    const std::string url(record.substr(url_begin, url_end - url_begin));
    if (IsTrustedHttpsUrl(url, kDownloadHosts)) {
      downloads.emplace_back(url, xapk);
    }
    index = url_end;
  }
  return downloads;
}

HttpBytesResult FetchMetadata(std::string_view architecture) {
  HttpTransferRequest request;
  request.url = std::string(kApiUrl);
  request.headers = {
      "x-cv: 3172501",
      "x-sv: 29",
      "x-gp: 1",
      "x-abis: " + std::string(architecture),
  };
  request.allowed_hosts = kMetadataHosts;
  request.maximum_bytes = kMaximumMetadataBytes;
  request.connect_timeout_ms = 5000;
  request.transfer_timeout_ms = 15000;
  request.maximum_attempts = 2;
  return DownloadBytes(request);
}

std::uint64_t VersionCode(std::string_view metadata,
                          const VersionMarker& marker) {
  const std::size_t begin = marker.begin > 128 ? marker.begin - 128 : 0;
  for (std::size_t position = marker.begin; position-- > begin;) {
    if (static_cast<unsigned char>(metadata[position]) != 0x2a ||
        position + 3 >= marker.begin)
      continue;
    const std::size_t length =
        static_cast<unsigned char>(metadata[position + 1]);
    const std::size_t code_begin = position + 2;
    const std::size_t name_tag = code_begin + length;
    if (length == 0 || length > 20 || name_tag + 2 != marker.begin ||
        static_cast<unsigned char>(metadata[name_tag]) != 0x32 ||
        static_cast<unsigned char>(metadata[name_tag + 1]) !=
            marker.value.size())
      continue;
    std::uint64_t code = 0;
    const auto parsed = std::from_chars(metadata.data() + code_begin,
                                        metadata.data() + name_tag, code);
    if (parsed.ec == std::errc() && parsed.ptr == metadata.data() + name_tag)
      return code;
  }
  return 0;
}

bool ZipMagic(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  char magic[4] = {};
  input.read(magic, sizeof(magic));
  return input.gcount() == 4 && magic[0] == 'P' && magic[1] == 'K' &&
         (magic[2] == 3 || magic[2] == 5 || magic[2] == 7) &&
         (magic[3] == 4 || magic[3] == 6 || magic[3] == 8);
}

}  // namespace

ProviderVersion ParseApkPureLatestMetadata(std::string_view metadata) {
  ProviderVersion result;
  const auto markers = VersionMarkers(metadata);
  for (std::size_t index = 0; index < markers.size(); ++index) {
    const auto& marker = markers[index];
    const std::uint64_t code = VersionCode(metadata, marker);
    const std::size_t end =
        index + 1 < markers.size() ? markers[index + 1].begin : metadata.size();
    if (code > result.version_code &&
        !DownloadsInRecord(
             metadata.substr(marker.colon + 1, end - marker.colon - 1))
             .empty()) {
      result.version_name = marker.value;
      result.version_code = code;
    }
  }
  if (!result)
    result.error = "APKPure metadata contains no downloadable version identity";
  return result;
}

std::vector<std::string> ParseApkPureExactDownloadUrls(
    std::string_view metadata, std::string_view version, std::string* error,
    std::uint64_t version_code) {
  if (error != nullptr) error->clear();
  std::vector<std::string> result;
  const auto markers = VersionMarkers(metadata);
  for (std::size_t index = 0; index < markers.size(); ++index) {
    if (markers[index].value != version ||
        (version_code != 0 &&
         VersionCode(metadata, markers[index]) != version_code))
      continue;
    const std::size_t record_end =
        index + 1 < markers.size() ? markers[index + 1].begin : metadata.size();
    const auto downloads = DownloadsInRecord(metadata.substr(
        markers[index].colon + 1, record_end - markers[index].colon - 1));
    if (!downloads.empty() &&
        std::find(result.begin(), result.end(), downloads.front().first) ==
            result.end()) {
      result.push_back(downloads.front().first);
    }
  }
  if (result.empty() && error != nullptr) {
    *error = "APKPure does not offer the requested exact version";
  } else if (result.size() > kMaximumExactCandidates) {
    result.clear();
    if (error != nullptr) *error = "APKPure returned too many candidates";
  }
  return result;
}

ProviderVersion ApkPureProvider::CheckLatest() const {
  const HttpBytesResult metadata =
      FetchMetadata(std::string(compat::kGuestAbi));
  if (!metadata) return ProviderVersion{{}, 0, metadata.error};
  return ParseApkPureLatestMetadata(metadata.bytes);
}

ProviderDownloadResult ApkPureProvider::DownloadExact(
    const ProviderVersion& version,
    const std::filesystem::path& output_directory, int progress_fd) const {
  ProviderDownloadResult result;
  result.source = std::string(name());
  if (!version || version.version_name.size() > 128 ||
      !std::all_of(version.version_name.begin(), version.version_name.end(),
                   [](unsigned char character) {
                     return VersionCharacter(character);
                   })) {
    result.error = "invalid APKPure version";
    return result;
  }
  std::error_code filesystem_error;
  std::filesystem::create_directories(output_directory, filesystem_error);
  if (filesystem_error ||
      std::filesystem::is_symlink(output_directory, filesystem_error)) {
    result.error = "cannot create APKPure download directory";
    return result;
  }
  HttpBytesResult metadata = FetchMetadata(std::string(compat::kGuestAbi));
  if (!metadata) {
    result.error = metadata.error;
    return result;
  }
  std::string parse_error;
  std::vector<std::string> urls = ParseApkPureExactDownloadUrls(
      metadata.bytes, version.version_name, &parse_error, version.version_code);
  if (urls.empty()) {
    metadata = FetchMetadata("arm64-v8a,armeabi-v7a,armeabi,x86,x86_64");
    if (!metadata) {
      result.error = metadata.error;
      return result;
    }
    urls = ParseApkPureExactDownloadUrls(metadata.bytes, version.version_name,
                                         &parse_error, version.version_code);
  }
  if (!parse_error.empty()) {
    result.error = parse_error;
    return result;
  }
  for (std::size_t index = 0; index < urls.size(); ++index) {
    const bool xapk = urls[index].find(".xapk") != std::string::npos;
    const std::filesystem::path destination =
        output_directory /
        ("candidate-" + std::to_string(index + 1) + (xapk ? ".xapk" : ".apk"));
    HttpTransferRequest request;
    request.url = urls[index];
    request.allowed_hosts = kDownloadHosts;
    request.maximum_bytes = kMaximumArchiveBytes;
    // Several hundred MiB over a slow link is not a failed transfer; the
    // quarter-hour cap failed those users outright. Stalls are caught by the
    // low-speed guard instead.
    request.transfer_timeout_ms = 0;
    if (ZipMagic(destination)) {
      result.archives.push_back(destination);
      continue;
    }
    const HttpDownloadResult downloaded =
        DownloadFile(request, destination, progress_fd);
    if (!downloaded || !ZipMagic(destination)) {
      std::filesystem::remove(destination, filesystem_error);
      result.error = downloaded ? "APKPure response is not a ZIP archive"
                                : downloaded.error;
      return result;
    }
    result.archives.push_back(destination);
  }
  return result;
}

}  // namespace mocktail::update
