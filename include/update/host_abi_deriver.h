#ifndef MOCKTAIL_UPDATE_HOST_ABI_DERIVER_H_
#define MOCKTAIL_UPDATE_HOST_ABI_DERIVER_H_

#include <filesystem>
#include <string>

namespace mocktail::update {

struct HostAbiDerivationOptions {
  std::filesystem::path reference_library;
  std::filesystem::path reference_profile;
  std::filesystem::path candidate_payload_directory;
  std::filesystem::path output_directory;
};

struct HostAbiDerivationResult {
  std::filesystem::path profile;
  std::filesystem::path compatibility_manifest;
  std::string error;

  explicit operator bool() const {
    return error.empty() && !profile.empty() && !compatibility_manifest.empty();
  }
};

// Derives an exact candidate HostAbi profile by matching normalized x86-64
// instruction signatures against one already approved reference payload. This
// is static analysis only; the result still requires two real Tier C canaries
// before normal runtime activation.
HostAbiDerivationResult DeriveHostAbiProfile(
    const HostAbiDerivationOptions& options);

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_HOST_ABI_DERIVER_H_
