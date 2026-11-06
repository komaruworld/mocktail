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

ProviderDownloadResult ApkProvider::DownloadVerified(
    const ProviderVersion& version, const std::filesystem::path& directory,
    int progress_fd, const ArchiveValidator& validate) const {
  auto result = DownloadExact(version, directory, progress_fd);
  if (result && validate) result.error = validate(result);
  if (!result && result.error.empty())
    result.error = "provider returned no APK archives";
  return result;
}

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
    if (!provider->HasVersionDiscovery()) continue;
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
  if (!result) result.error = failures.empty()
                                  ? "no version discovery provider is configured"
                                  : std::move(failures);
  return result;
}

ProviderDownloadResult ProviderChain::DownloadExact(
    const ProviderVersion& version,
    const std::filesystem::path& output_directory, int progress_fd) const {
  return DownloadVerified(version, output_directory, progress_fd, {});
}

ProviderDownloadResult ProviderChain::DownloadVerified(
    const ProviderVersion& version,
    const std::filesystem::path& output_directory, int progress_fd,
    const ArchiveValidator& validate) const {
  ProviderDownloadResult result;
  if (providers_.empty()) {
    result.error = "no APK provider is configured";
    return result;
  }
  std::string failures;
  for (const std::unique_ptr<ApkProvider>& provider : providers_) {
    ProviderDownloadResult downloaded = provider->DownloadVerified(
        version, output_directory / provider->name(), progress_fd, validate);
    if (downloaded) return downloaded;
    AppendFailure(&failures, provider->name(), downloaded.error);
  }
  result.error = std::move(failures);
  return result;
}

}  // namespace mocktail::update
