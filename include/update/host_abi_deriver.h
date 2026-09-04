#ifndef MOCKTAIL_UPDATE_HOST_ABI_DERIVER_H_
#define MOCKTAIL_UPDATE_HOST_ABI_DERIVER_H_

#include <filesystem>
#include <string>
#include <vector>

namespace mocktail::update {

struct HostAbiDerivationOptions {
  std::filesystem::path reference_library;
  std::filesystem::path reference_profile;
  std::vector<std::filesystem::path> reference_compatibility_manifests;
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

// Identity an exact HostAbi sidecar was derived for. It only validates
// against the one library whose bytes it describes, so the reference payload
// has to be selected by this Build ID rather than by catalog order.
struct HostAbiSidecarIdentity {
  std::string elf_build_id;
  std::string payload_id;
  std::string error;

  explicit operator bool() const {
    return error.empty() && !elf_build_id.empty() && !payload_id.empty();
  }
};

HostAbiSidecarIdentity ReadHostAbiSidecarIdentity(
    const std::filesystem::path& sidecar);

// Derives an exact candidate HostAbi profile by matching normalized x86-64
// instruction signatures against one already approved reference payload. This
// is static analysis only; the result still requires two real Tier C canaries
// before normal runtime activation.
HostAbiDerivationResult DeriveHostAbiProfile(
    const HostAbiDerivationOptions& options);

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_HOST_ABI_DERIVER_H_
