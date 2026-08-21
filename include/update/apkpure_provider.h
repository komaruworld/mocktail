#ifndef MOCKTAIL_UPDATE_APKPURE_PROVIDER_H_
#define MOCKTAIL_UPDATE_APKPURE_PROVIDER_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail::update {

struct ProviderVersion {
  std::string version_name;
  std::uint64_t version_code = 0;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

struct ProviderDownloadResult {
  std::vector<std::filesystem::path> archives;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

ProviderVersion ParseApkPureLatestMetadata(std::string_view metadata);

std::vector<std::string> ParseApkPureExactDownloadUrls(
    std::string_view metadata, std::string_view version,
    std::string* error = nullptr);

class ApkPureProvider final {
 public:
  ProviderVersion CheckLatest() const;

  ProviderDownloadResult DownloadExact(
      std::string_view version, const std::filesystem::path& output_directory,
      int progress_fd = -1) const;
};

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_APKPURE_PROVIDER_H_
