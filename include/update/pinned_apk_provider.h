#ifndef MOCKTAIL_UPDATE_PINNED_APK_PROVIDER_H_
#define MOCKTAIL_UPDATE_PINNED_APK_PROVIDER_H_

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "update/apk_provider.h"
#include "update/http_download.h"

namespace mocktail::update {

struct PinnedApkArchive {
  std::string filename;
  std::string url;
  std::string sha256;
  std::size_t size = 0;
};

struct PinnedApkSource {
  std::string build_id;
  std::vector<PinnedApkArchive> archives;
  std::string error;

  explicit operator bool() const { return error.empty() && !archives.empty(); }
};

PinnedApkSource LoadPinnedApkSource(const std::filesystem::path& manifest,
                                   const ProviderVersion& version,
                                   std::string_view abi);

class PinnedApkProvider final : public ApkProvider {
 public:
  using Downloader = std::function<HttpDownloadResult(
      const HttpTransferRequest&, const std::filesystem::path&, int)>;

  explicit PinnedApkProvider(std::filesystem::path manifest,
                             Downloader download = DownloadFile);

  std::string_view name() const override { return "pinned"; }
  bool HasVersionDiscovery() const override { return false; }
  ProviderVersion CheckLatest() const override;
  ProviderDownloadResult DownloadExact(
      const ProviderVersion& version, const std::filesystem::path& directory,
      int progress_fd) const override;

 private:
  std::filesystem::path manifest_;
  Downloader download_;
};

}  // namespace mocktail::update

#endif
