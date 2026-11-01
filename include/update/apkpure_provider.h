#ifndef MOCKTAIL_UPDATE_APKPURE_PROVIDER_H_
#define MOCKTAIL_UPDATE_APKPURE_PROVIDER_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "update/apk_provider.h"

namespace mocktail::update {

ProviderVersion ParseApkPureLatestMetadata(std::string_view metadata);

std::vector<std::string> ParseApkPureExactDownloadUrls(
    std::string_view metadata, std::string_view version,
    std::string* error = nullptr, std::uint64_t version_code = 0);

class ApkPureProvider final : public ApkProvider {
 public:
  std::string_view name() const override { return "apk-pure"; }

  ProviderVersion CheckLatest() const override;

  ProviderDownloadResult DownloadExact(
      const ProviderVersion& version,
      const std::filesystem::path& output_directory,
      int progress_fd = -1) const override;
};

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_APKPURE_PROVIDER_H_
