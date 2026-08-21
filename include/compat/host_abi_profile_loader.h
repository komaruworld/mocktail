#ifndef MOCKTAIL_COMPAT_HOST_ABI_PROFILE_LOADER_H_
#define MOCKTAIL_COMPAT_HOST_ABI_PROFILE_LOADER_H_

#include <string>

#include "compat/host_abi_profile.h"

namespace mocktail::compat {

struct FileSha256Result {
  std::string sha256;
  std::string error;

  explicit operator bool() const noexcept { return error.empty(); }
};

// Approval receipts bind the profile, manifest, and payload to this digest.
// Runtime identity uses its Build ID because installation may rewrite RPATH.
FileSha256Result ComputeFileSha256(const std::string& path);

struct ExternalHostAbiProfileRequest {
  std::string profile_file;
  std::string payload_path;
  std::string compatibility_manifest_path;
  std::string expected_build_id;
  std::string approval_receipt_path;
  bool candidate_canary = false;
  bool candidate_process_authorization = false;
  bool explicit_unverified_authorization = false;
};

struct ExternalHostAbiProfileResult {
  const HostAbiProfile* profile = nullptr;
  std::string payload_sha256;
  std::string profile_sha256;
  std::string error;

  explicit operator bool() const noexcept { return error.empty(); }
};

// Candidates require canary authorization; normal launches require an
// approval receipt. Built-in profiles always win.
ExternalHostAbiProfileResult LoadExternalHostAbiProfile(
    const ExternalHostAbiProfileRequest& request);

// An unset profile is a no-op. Candidates require both canary flags and the
// research CLI marker; approved profiles require a receipt and no canary flag.
ExternalHostAbiProfileResult InitializeHostAbiProfileFromEnvironment(
    const std::string& payload_path,
    const std::string& compatibility_manifest_path,
    const std::string& expected_build_id,
    bool explicit_unverified_authorization);

const HostAbiProfile* FindLoadedExternalHostAbiProfile(
    std::string_view build_id) noexcept;

}  // namespace mocktail::compat

#endif  // MOCKTAIL_COMPAT_HOST_ABI_PROFILE_LOADER_H_
