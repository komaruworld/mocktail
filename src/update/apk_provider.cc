#include "update/apk_provider.h"

#include <string>
#include <utility>

namespace mocktail::update {
namespace {

void AppendFailure(std::string* aggregate, std::string_view provider,
                   std::string_view failure) {
  if (!aggregate->empty()) aggregate->append("; ");
  aggregate->append(provider);
  aggregate->append(": ");
  aggregate->append(failure);
}

}  // namespace

void ProviderChain::Add(std::unique_ptr<ApkProvider> provider) {
  if (provider != nullptr) providers_.push_back(std::move(provider));
}

ProviderVersion ProviderChain::CheckLatest() const {
  ProviderVersion result;
  if (providers_.empty()) {
    result.error = "no APK provider is configured";
    return result;
  }
  std::string failures;
  for (const std::unique_ptr<ApkProvider>& provider : providers_) {
    const ProviderVersion version = provider->CheckLatest();
    if (version) {
      if (!result || version.version_code > result.version_code)
        result = version;
    } else {
      AppendFailure(
          &failures, provider->name(),
          version.error.empty() ? "invalid version identity" : version.error);
    }
  }
  if (!result) result.error = std::move(failures);
  return result;
}

ProviderDownloadResult ProviderChain::DownloadExact(
    const ProviderVersion& version,
    const std::filesystem::path& output_directory, int progress_fd) const {
  ProviderDownloadResult result;
  if (providers_.empty()) {
    result.error = "no APK provider is configured";
    return result;
  }
  std::string failures;
  for (const std::unique_ptr<ApkProvider>& provider : providers_) {
    // Each provider gets its own directory: providers refuse to write into a
    // directory another one already populated, and a failed attempt must not
    // poison the next.
    ProviderDownloadResult downloaded = provider->DownloadExact(
        version, output_directory / provider->name(), progress_fd);
    if (downloaded) return downloaded;
    AppendFailure(&failures, provider->name(), downloaded.error);
  }
  result.error = std::move(failures);
  return result;
}

}  // namespace mocktail::update
