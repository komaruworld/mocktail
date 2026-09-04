#ifndef MOCKTAIL_UPDATE_UPDATE_COORDINATOR_H_
#define MOCKTAIL_UPDATE_UPDATE_COORDINATOR_H_

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "update/apk_bundle.h"
#include "update/apk_provider.h"
#include "update/compatibility_catalog.h"
#include "update/payload_store.h"
#include "update/readiness_canary.h"

namespace mocktail::update {

struct UpdatePaths {
  std::filesystem::path config_file;
  std::filesystem::path data_root;
  std::filesystem::path cache_root;
  std::filesystem::path state_root;
  std::filesystem::path compatibility_manifest;
  std::filesystem::path signing_trust_manifest;
  // One exact HostAbi sidecar, or a directory of them. A directory lets a new
  // supported profile ship its own reference instead of inheriting whichever
  // build the single file happens to describe.
  std::filesystem::path host_abi_reference_profile;
  std::filesystem::path runtime_binary;
};

struct UpdateRequest {
  bool startup_preflight = false;
  bool check_latest = true;
  bool run_canary = true;
  // Explicit manual escape hatch. It launches the provider latest once with a
  // derived candidate profile and never promotes it into current.json.
  bool force_run_latest = false;
  CanaryGraphicsBackend canary_graphics_backend =
      CanaryGraphicsBackend::kDirectVulkan;
  int progress_fd = -1;
};

struct UpdateResult {
  bool changed = false;
  // The provider publishes a newer Roblox than the payload that stayed
  // active. Launching still works, but the Roblox servers reject an outdated
  // client, so this is a user-visible outcome rather than a quiet success.
  bool stale = false;
  std::string payload_id;
  std::string message;
  std::vector<std::string> warnings;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

// One payload worth trying, best first. The installed payload is deliberately
// not a plan entry: it is what remains when every plan fails, so a rejected
// candidate cannot pin the user to a version the catalog has moved past.
struct UpdateCandidatePlan {
  ExpectedPayloadIdentity identity;
  bool exact_supported = false;
  // The store already holds an approved profile for this exact version, so it
  // only needs its canaries re-run rather than another few hundred MiB.
  bool reuse_installed = false;
  std::string origin;
};

// Newest supported profile whose own bytes an available sidecar describes,
// with `sidecar` set to that file. `location` is one sidecar or a directory
// of them. A library picked any other way cannot pass the derivation's own
// reference check.
std::optional<SupportedPayloadProfile> ResolveReferenceProfile(
    const std::filesystem::path& location,
    const std::vector<SupportedPayloadProfile>& profiles,
    std::filesystem::path* sidecar);

// `latest` is null when the provider could not be consulted. `current` is the
// runnable payload (empty when there is none) and `installed` is whatever the
// activation manifest points at, runnable or not.
std::vector<UpdateCandidatePlan> PlanUpdateCandidates(
    const std::vector<SupportedPayloadProfile>& profiles,
    const SupportedPayloadProfile& preferred, const ProviderVersion* latest,
    const PayloadStoreResult& current, const PayloadStoreResult& installed);

UpdateResult RunUpdate(const UpdatePaths& paths, const UpdateRequest& request);

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_UPDATE_COORDINATOR_H_
