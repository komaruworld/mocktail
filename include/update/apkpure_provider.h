#ifndef MOCKTAIL_UPDATE_APKPURE_PROVIDER_H_
#define MOCKTAIL_UPDATE_APKPURE_PROVIDER_H_

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "update/apk_provider.h"
#include "update/http_download.h"

namespace mocktail::update {

ProviderVersion ParseApkPureLatestMetadata(std::string_view metadata);

std::vector<std::string> ParseApkPureExactDownloadUrls(
    std::string_view metadata, std::string_view version,
    std::string* error = nullptr, std::uint64_t version_code = 0);

class ApkPureProvider final : public ApkProvider {
 public:
  using MetadataFetcher = std::function<HttpBytesResult(std::string_view)>;
  using Downloader = std::function<HttpDownloadResult(
      const HttpTransferRequest&, const std::filesystem::path&, int)>;

  explicit ApkPureProvider(MetadataFetcher metadata = {},
                           Downloader download = DownloadFile);
  std::string_view name() const override { return "apk-pure"; }

  ProviderVersion CheckLatest() const override;

  ProviderDownloadResult DownloadExact(
      const ProviderVersion& version,
      const std::filesystem::path& output_directory,
      int progress_fd = -1) const override;
  ProviderDownloadResult DownloadVerified(
      const ProviderVersion& version,
      const std::filesystem::path& output_directory, int progress_fd,
      const ArchiveValidator& validate) const override;

 private:
  MetadataFetcher metadata_;
  Downloader download_;
};

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_APKPURE_PROVIDER_H_
