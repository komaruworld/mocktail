#include "update/update_coordinator.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compat/elf_build_id.h"
#include "update/apk_bundle.h"
#include "update/apk_provider.h"
#include "update/apkpure_provider.h"
#include "update/compatibility_catalog.h"
#include "update/host_abi_deriver.h"
#include "update/payload_integrity.h"
#include "update/payload_store.h"
#include "update/readiness_canary.h"
#include "update/unsafe_latest_runner.h"
#include "update/update_config.h"

namespace mocktail::update {
namespace {

class UpdateLock final {
 public:
  UpdateLock(const std::filesystem::path& root, std::string* error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(root, filesystem_error);
    if (filesystem_error) {
      *error = "cannot create updater data root";
      return;
    }
    descriptor_ = open((root / ".native-update.lock").c_str(),
                       O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor_ < 0 || flock(descriptor_, LOCK_EX) != 0) {
      if (descriptor_ >= 0) close(descriptor_);
      descriptor_ = -1;
      *error = "cannot lock native updater";
    }
  }

  ~UpdateLock() {
    if (descriptor_ >= 0) close(descriptor_);
  }

  explicit operator bool() const { return descriptor_ >= 0; }

 private:
  int descriptor_ = -1;
};

void Progress(int descriptor, std::string_view message) {
  if (descriptor < 0) return;
  const std::string packet = "P" + std::string(message);
  (void)send(descriptor, packet.data(), packet.size(), MSG_NOSIGNAL);
}

bool TraceFlagEnabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

bool UpdateTraceEnabled() {
  static const bool enabled = TraceFlagEnabled("MOCKTAIL_UPDATE_TRACE") ||
                              TraceFlagEnabled("MOCKTAIL_TRACE_ALL") ||
                              TraceFlagEnabled("MOCKTAIL_FULL_TRACE");
  return enabled;
}

// A first install downloads, verifies, canaries, and promotes several hundred
// MiB. Without a per-stage breakdown there is no way to tell which stage owns
// the wall-clock time, so the timer reports one line per stage on request.
class StageTimer final {
 public:
  explicit StageTimer(std::string name)
      : name_(std::move(name)), start_(std::chrono::steady_clock::now()) {}

  ~StageTimer() { Report(); }

  StageTimer(const StageTimer&) = delete;
  StageTimer& operator=(const StageTimer&) = delete;

  void Report() {
    if (name_.empty()) return;
    if (UpdateTraceEnabled()) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start_);
      std::fprintf(stderr, "  [update] %s %lld ms\n", name_.c_str(),
                   static_cast<long long>(elapsed.count()));
    }
    name_.clear();
  }

 private:
  std::string name_;
  std::chrono::steady_clock::time_point start_;
};

// The update lock is held, so no live run owns one of these.
void RemoveStaleWorkspaces(const std::filesystem::path& parent) {
  std::error_code filesystem_error;
  std::vector<std::filesystem::path> stale;
  std::filesystem::directory_iterator iterator(
      parent, std::filesystem::directory_options::none, filesystem_error);
  const std::filesystem::directory_iterator end;
  while (!filesystem_error && iterator != end) {
    if (iterator->path().filename().string().rfind(".update-", 0) == 0) {
      stale.push_back(iterator->path());
    }
    iterator.increment(filesystem_error);
  }
  for (const std::filesystem::path& entry : stale) {
    std::error_code ignored;
    std::filesystem::remove_all(entry, ignored);
  }
}

std::filesystem::path UniqueDirectory(const std::filesystem::path& parent,
                                      std::string* error) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(parent, filesystem_error);
  if (filesystem_error) {
    *error = "cannot create native updater cache";
    return {};
  }
  std::string pattern = (parent / ".update-XXXXXX").string();
  std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
  mutable_pattern.push_back('\0');
  char* created = mkdtemp(mutable_pattern.data());
  if (created == nullptr) {
    *error = "cannot create native updater workspace";
    return {};
  }
  return created;
}

bool SameVersion(const PayloadStoreResult& payload,
                 const ProviderVersion& version) {
  return payload && payload.version_name == version.version_name &&
         payload.version_code == version.version_code;
}

struct Candidate {
  PayloadStoreResult staged;
  ExpectedPayloadIdentity identity;
  bool exact_supported = false;
  std::filesystem::path profile;
  std::filesystem::path compatibility;
  std::string error;

  explicit operator bool() const { return error.empty() && staged; }
};

Candidate FindStagedCandidate(const UpdatePaths& paths,
                              const ExpectedPayloadIdentity& identity,
                              bool exact_supported) {
  Candidate result;
  const std::filesystem::path payloads = paths.data_root / "payloads";
  std::error_code filesystem_error;
  std::filesystem::directory_iterator iterator(
      payloads, std::filesystem::directory_options::none, filesystem_error);
  const std::filesystem::directory_iterator end;
  while (!filesystem_error && iterator != end) {
    const auto status = iterator->symlink_status(filesystem_error);
    if (filesystem_error) break;
    if (std::filesystem::is_directory(status) &&
        !std::filesystem::is_symlink(status)) {
      const PayloadIntegrityResult payload =
          InspectPreparedPayload(iterator->path());
      if (payload && payload.payload_id == iterator->path().filename() &&
          payload.metadata.version_name == identity.version_name &&
          payload.metadata.version_code == identity.version_code &&
          (!identity.exact_build_id.has_value() ||
           payload.metadata.build_id == *identity.exact_build_id)) {
        if (!result.staged.payload_id.empty()) {
          // Provider download remains the unambiguous source of truth if the
          // local store somehow contains two Build IDs for one version.
          return {};
        }
        result.identity = identity;
        result.exact_supported = exact_supported;
        result.staged.payload_id = payload.payload_id;
        result.staged.payload_directory = iterator->path();
        result.staged.version_name = payload.metadata.version_name;
        result.staged.version_code = payload.metadata.version_code;
        result.staged.build_id = payload.metadata.build_id;
      }
    }
    iterator.increment(filesystem_error);
  }
  if (filesystem_error) return {};
  return result;
}

std::filesystem::path RejectionMarker(
    const UpdatePaths& paths, const Candidate& candidate,
    CanaryGraphicsBackend graphics_backend) {
  if (!candidate.staged || paths.runtime_binary.empty()) return {};
  const compat::BuildIdResult runtime =
      compat::ReadElfBuildId(paths.runtime_binary.string());
  if (!runtime) return {};
  return paths.data_root / "rejections" /
         (candidate.staged.payload_id + "-" + runtime.build_id + "-" +
          std::string(CanaryGraphicsBackendName(graphics_backend)) + ".txt");
}

bool RejectedForRuntime(const UpdatePaths& paths, const Candidate& candidate,
                        CanaryGraphicsBackend graphics_backend) {
  const std::filesystem::path marker =
      RejectionMarker(paths, candidate, graphics_backend);
  if (marker.empty()) return false;
  std::error_code filesystem_error;
  const auto status = std::filesystem::symlink_status(marker, filesystem_error);
  return !filesystem_error && std::filesystem::is_regular_file(status) &&
         !std::filesystem::is_symlink(status);
}

void RecordRejection(const UpdatePaths& paths, const Candidate& candidate,
                     CanaryGraphicsBackend graphics_backend,
                     std::string_view reason) {
  const std::filesystem::path marker =
      RejectionMarker(paths, candidate, graphics_backend);
  if (marker.empty()) return;
  std::error_code filesystem_error;
  std::filesystem::create_directories(marker.parent_path(), filesystem_error);
  if (filesystem_error) return;
  const int descriptor =
      open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
           0600);
  if (descriptor < 0) return;
  std::string contents(reason.substr(0, 4096));
  contents.push_back('\n');
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t bytes =
        write(descriptor, contents.data() + offset, contents.size() - offset);
    if (bytes < 0 && errno == EINTR) continue;
    if (bytes <= 0) break;
    offset += static_cast<std::size_t>(bytes);
  }
  (void)fsync(descriptor);
  close(descriptor);
}

// Built once per run. Providers are consulted in order, so adding a second
// source is a one-line change here rather than a rewrite of the coordinator.
ProviderChain BuildProviderChain() {
  ProviderChain chain;
  chain.Add(std::make_unique<ApkPureProvider>());
  return chain;
}

Candidate DownloadCandidate(const ApkProvider* provider, PayloadStore* store,
                            const ExpectedPayloadIdentity& identity,
                            bool exact_supported, const UpdatePaths& paths,
                            const std::filesystem::path& workspace,
                            int progress_fd) {
  Candidate result;
  result.identity = identity;
  result.exact_supported = exact_supported;
  Progress(progress_fd, "Downloading Roblox...");
  StageTimer download_timer("download");
  const ProviderDownloadResult downloaded = provider->DownloadExact(
      identity.version_name, workspace / "provider", progress_fd);
  if (!downloaded) {
    result.error = downloaded.error;
    return result;
  }
  download_timer.Report();
  Progress(progress_fd, "Verifying Roblox...");
  StageTimer prepare_timer("prepare");
  const PreparedPayload prepared = PreparePayloadFromArchives(
      downloaded.archives, identity, paths.signing_trust_manifest,
      workspace / "prepare", downloaded.source + "-native");
  if (!prepared) {
    result.error = prepared.error;
    return result;
  }
  prepare_timer.Report();
  StageTimer stage_timer("stage");
  result.staged = store->Stage(prepared.directory);
  if (!result.staged) result.error = result.staged.error;
  return result;
}

struct ReferenceProfile {
  std::filesystem::path library;
  std::filesystem::path profile;
  std::vector<std::filesystem::path> compatibility_manifests;
  std::string error;

  explicit operator bool() const {
    return error.empty() && !library.empty() && !profile.empty();
  }
};

// `location` is one sidecar or a directory of them.
std::map<std::string, std::filesystem::path> CollectReferenceSidecars(
    const std::filesystem::path& location) {
  std::map<std::string, std::filesystem::path> sidecars;
  std::error_code filesystem_error;
  std::vector<std::filesystem::path> files;
  if (std::filesystem::is_directory(location, filesystem_error)) {
    std::filesystem::directory_iterator iterator(
        location, std::filesystem::directory_options::none, filesystem_error);
    const std::filesystem::directory_iterator end;
    while (!filesystem_error && iterator != end) {
      if (iterator->path().extension() == ".json") {
        files.push_back(iterator->path());
      }
      iterator.increment(filesystem_error);
    }
  } else if (std::filesystem::is_regular_file(location, filesystem_error)) {
    files.push_back(location);
  }
  for (const std::filesystem::path& file : files) {
    const HostAbiSidecarIdentity identity = ReadHostAbiSidecarIdentity(file);
    if (identity) sidecars.emplace(identity.elf_build_id, file);
  }
  return sidecars;
}

// Library and sidecar are one unit: choosing the library by catalog order
// let the two drift apart as soon as a newer profile was added, and the pair
// then failed every derivation.
ReferenceProfile ResolveReference(
    const UpdatePaths& paths, const PayloadStoreResult& installed,
    const std::vector<SupportedPayloadProfile>& profiles,
    const ApkProvider* provider, PayloadStore* store,
    const std::filesystem::path& workspace, int progress_fd) {
  ReferenceProfile result;
  // An approved probation payload carries the sidecar derived for its own
  // bytes, and is the closest reference to whatever Roblox ships next.
  if (installed && !installed.host_abi_profile.empty()) {
    std::error_code filesystem_error;
    const HostAbiSidecarIdentity identity =
        ReadHostAbiSidecarIdentity(installed.host_abi_profile);
    if (identity && identity.elf_build_id == installed.build_id &&
        std::filesystem::is_regular_file(
            installed.payload_directory / "libroblox.so", filesystem_error)) {
      result.library = installed.payload_directory / "libroblox.so";
      result.profile = installed.host_abi_profile;
      if (!installed.compatibility_manifest.empty() &&
          std::filesystem::is_regular_file(installed.compatibility_manifest,
                                           filesystem_error)) {
        result.compatibility_manifests.push_back(
            installed.compatibility_manifest);
      }
      if (std::filesystem::is_regular_file(paths.compatibility_manifest,
                                           filesystem_error) &&
          (result.compatibility_manifests.empty() ||
           result.compatibility_manifests.front() !=
               paths.compatibility_manifest)) {
        result.compatibility_manifests.push_back(paths.compatibility_manifest);
      }
      return result;
    }
  }
  std::filesystem::path sidecar;
  const std::optional<SupportedPayloadProfile> selected =
      ResolveReferenceProfile(paths.host_abi_reference_profile, profiles,
                              &sidecar);
  if (!selected.has_value()) {
    result.error =
        "no HostAbi reference sidecar matches a supported Roblox profile";
    return result;
  }
  const SupportedPayloadProfile& reference = *selected;
  const std::string reference_id =
      std::to_string(reference.version_code) + "-" + reference.elf_build_id;
  const std::filesystem::path reference_directory =
      paths.data_root / "payloads" / reference_id;
  const PayloadIntegrityResult reference_payload =
      InspectPreparedPayload(reference_directory);
  if (reference_payload && reference_payload.payload_id == reference_id) {
    result.library = reference_directory / "libroblox.so";
    result.profile = sidecar;
    result.compatibility_manifests.push_back(paths.compatibility_manifest);
    return result;
  }
  Candidate downloaded =
      DownloadCandidate(provider, store, ExactPayloadIdentity(reference), true,
                        paths, workspace / "reference", progress_fd);
  if (!downloaded) {
    result.error = "cannot obtain HostAbi derivation reference " +
                   reference.version_name + ": " + downloaded.error;
    return result;
  }
  result.library = downloaded.staged.payload_directory / "libroblox.so";
  result.profile = sidecar;
  result.compatibility_manifests.push_back(paths.compatibility_manifest);
  return result;
}

UpdateResult RunUnsafeLatest(
    const UpdatePaths& paths, const UpdateRequest& request,
    const std::vector<SupportedPayloadProfile>& profiles,
    const PayloadStoreResult& installed, PayloadStore* store) {
  UpdateResult result;
  if (request.startup_preflight || !request.check_latest) {
    result.error =
        "unsafe latest launch requires an explicit latest-version check";
    return result;
  }

  const ProviderChain provider = BuildProviderChain();
  Progress(request.progress_fd, "Checking latest Roblox...");
  const ProviderVersion latest = [&] {
    StageTimer timer("check-latest");
    return provider.CheckLatest();
  }();
  if (!latest) {
    result.error = "cannot check latest Roblox for unsafe launch: " +
                   latest.error;
    return result;
  }

  std::string workspace_error;
  const std::filesystem::path workspace = UniqueDirectory(
      paths.cache_root / "downloads/native-updater", &workspace_error);
  if (!workspace_error.empty()) {
    result.error = workspace_error;
    return result;
  }
  std::error_code filesystem_error;
  const auto cleanup = [&]() {
    std::filesystem::remove_all(workspace, filesystem_error);
  };

  ExpectedPayloadIdentity identity;
  identity.version_name = latest.version_name;
  identity.version_code = latest.version_code;
  const std::optional<SupportedPayloadProfile> exact =
      FindSupportedProfile(profiles, identity.version_name, identity.version_code);
  if (exact.has_value()) identity.exact_build_id = exact->elf_build_id;
  Candidate candidate = FindStagedCandidate(paths, identity, exact.has_value());
  if (!candidate) {
    candidate = DownloadCandidate(&provider, store, identity, exact.has_value(),
                                  paths, workspace / "candidate",
                                  request.progress_fd);
  }
  if (!candidate) {
    result.error = "cannot prepare latest Roblox for unsafe launch: " +
                   candidate.error;
    cleanup();
    return result;
  }

  if (!candidate.exact_supported) {
    const ReferenceProfile reference = ResolveReference(
        paths, installed, profiles, &provider, store, workspace,
        request.progress_fd);
    if (!reference) {
      result.error = "cannot prepare latest Roblox for unsafe launch: " +
                     reference.error;
      cleanup();
      return result;
    }
    HostAbiDerivationOptions derivation;
    derivation.reference_library = reference.library;
    derivation.reference_profile = reference.profile;
    derivation.reference_compatibility_manifests =
        reference.compatibility_manifests;
    derivation.candidate_payload_directory = candidate.staged.payload_directory;
    derivation.output_directory = workspace / "derived";
    Progress(request.progress_fd, "Deriving latest Roblox compatibility...");
    const HostAbiDerivationResult derived = [&] {
      StageTimer timer("derive-host-abi");
      return DeriveHostAbiProfile(derivation);
    }();
    if (!derived) {
      result.error = "cannot derive latest Roblox HostAbi profile: " +
                     derived.error;
      cleanup();
      return result;
    }
    candidate.profile = derived.profile;
    candidate.compatibility = derived.compatibility_manifest;
  }

  // Staging preserves the immutable APK for inspection, but this path must
  // never promote it or alter the normal current payload.
  UnsafeLatestRunOptions launch;
  launch.runtime_binary = paths.runtime_binary;
  launch.payload_directory = candidate.staged.payload_directory;
  launch.compatibility_manifest = candidate.exact_supported
                                      ? paths.compatibility_manifest
                                      : candidate.compatibility;
  launch.host_abi_profile = candidate.profile;
  Progress(request.progress_fd, "Launching unapproved latest Roblox...");
  const UnsafeLatestRunResult launched = RunUnsafeLatestCandidate(launch);
  result.payload_id = candidate.staged.payload_id;
  if (!launched) {
    result.error = "unsafe latest Roblox " + candidate.staged.version_name +
                   " did not complete: " + launched.error;
    cleanup();
    return result;
  }
  result.message = "unsafe latest Roblox " + candidate.staged.version_name +
                   " exited successfully; active payload was not changed";
  cleanup();
  return result;
}

bool RunCandidateCanaries(const UpdatePaths& paths, const Candidate& candidate,
                          bool run_canary,
                          CanaryGraphicsBackend graphics_backend,
                          int progress_fd,
                          std::array<std::filesystem::path, 2>* logs,
                          std::string* error) {
  if (!run_canary) {
    if (!candidate.exact_supported) {
      *error = "latest candidate cannot be activated without Tier C probation";
      return false;
    }
    return true;
  }
  const std::size_t runs = candidate.exact_supported ? 1 : 2;
  for (std::size_t index = 0; index < runs; ++index) {
    Progress(progress_fd, candidate.exact_supported
                              ? "Testing Roblox..."
                              : "Testing latest Roblox (" +
                                    std::to_string(index + 1) + "/2)...");
    CanaryOptions canary;
    canary.runtime_binary = paths.runtime_binary;
    canary.payload_directory = candidate.staged.payload_directory;
    canary.compatibility_manifest = candidate.exact_supported
                                        ? paths.compatibility_manifest
                                        : candidate.compatibility;
    canary.host_abi_profile = candidate.profile;
    canary.cache_root = paths.cache_root;
    canary.state_root = paths.state_root;
    canary.graphics_backend = graphics_backend;
    const CanaryResult canary_result = [&] {
      StageTimer timer("canary[" + std::to_string(index + 1) + "/" +
                       std::to_string(runs) + "]");
      return RunReadinessCanary(canary);
    }();
    if (!canary_result) {
      *error = canary_result.error;
      if (!canary_result.log_path.empty()) {
        *error += "; log: " + canary_result.log_path.string();
      }
      return false;
    }
    (*logs)[index] = canary_result.log_path;
  }
  return true;
}

PayloadStoreResult PromoteCandidate(const UpdatePaths& paths,
                                    const UpdateRequest& request,
                                    const Candidate& candidate,
                                    PayloadStore* store, std::string* error) {
  std::array<std::filesystem::path, 2> logs;
  if (!RunCandidateCanaries(paths, candidate, request.run_canary,
                            request.canary_graphics_backend,
                            request.progress_fd, &logs, error)) {
    return {};
  }
  Progress(request.progress_fd, "Installing Roblox...");
  StageTimer promote_timer("promote");
  PayloadStoreResult promoted =
      candidate.exact_supported
          ? store->Promote(candidate.staged.payload_id)
          : store->PromoteProbation(candidate.staged.payload_id,
                                    candidate.profile, candidate.compatibility,
                                    logs);
  if (!promoted) *error = promoted.error;
  return promoted;
}

std::string PlanDescription(const UpdateCandidatePlan& plan) {
  return "Roblox " + plan.identity.version_name + " (" +
         std::to_string(plan.identity.version_code) + ")";
}

// No manifest points at the derivation reference, so the collector below has
// to be told about it or the next Tier C update downloads it again.
std::vector<std::string> ReferencePayloadIds(
    const UpdatePaths& paths,
    const std::vector<SupportedPayloadProfile>& profiles) {
  const std::optional<SupportedPayloadProfile> reference =
      ResolveReferenceProfile(paths.host_abi_reference_profile, profiles,
                              nullptr);
  if (!reference.has_value()) return {};
  return {std::to_string(reference->version_code) + "-" +
          reference->elf_build_id};
}

// Called only once an install is known good; keeps the rollback target.
std::string CollectSupersededPayloads(PayloadStore* store,
                                      const std::vector<std::string>& keep) {
  if (TraceFlagEnabled("MOCKTAIL_KEEP_OLD_PAYLOADS")) return {};
  const PayloadGarbageResult collected = store->CollectGarbage(keep);
  if (!collected || collected.removed.empty()) return {};
  return "; freed " +
         std::to_string(collected.freed_bytes / (1024ULL * 1024ULL)) +
         " MiB from " + std::to_string(collected.removed.size()) +
         " superseded payload" + (collected.removed.size() == 1 ? "" : "s");
}

}  // namespace

std::optional<SupportedPayloadProfile> ResolveReferenceProfile(
    const std::filesystem::path& location,
    const std::vector<SupportedPayloadProfile>& profiles,
    std::filesystem::path* sidecar) {
  const std::map<std::string, std::filesystem::path> sidecars =
      CollectReferenceSidecars(location);
  // Newest one available: the shorter the distance to the candidate, the more
  // signatures survive derivation.
  std::optional<SupportedPayloadProfile> selected;
  for (const SupportedPayloadProfile& profile : profiles) {
    const auto found = sidecars.find(profile.elf_build_id);
    if (found == sidecars.end()) continue;
    if (!selected.has_value() ||
        selected->version_code < profile.version_code) {
      selected = profile;
      if (sidecar != nullptr) *sidecar = found->second;
    }
  }
  return selected;
}

std::vector<UpdateCandidatePlan> PlanUpdateCandidates(
    const std::vector<SupportedPayloadProfile>& profiles,
    const SupportedPayloadProfile& preferred, const ProviderVersion* latest,
    const PayloadStoreResult& current, const PayloadStoreResult& installed) {
  std::vector<UpdateCandidatePlan> plans;
  const bool runnable = static_cast<bool>(current);
  const std::uint64_t runnable_code = runnable ? current.version_code : 0;
  if (latest != nullptr && *latest &&
      (!runnable || latest->version_code > runnable_code)) {
    UpdateCandidatePlan plan;
    plan.identity.version_name = latest->version_name;
    plan.identity.version_code = latest->version_code;
    const std::optional<SupportedPayloadProfile> exact = FindSupportedProfile(
        profiles, latest->version_name, latest->version_code);
    if (exact.has_value()) plan.identity.exact_build_id = exact->elf_build_id;
    plan.exact_supported = exact.has_value();
    plan.reuse_installed = installed &&
                           installed.version_name == latest->version_name &&
                           installed.version_code == latest->version_code &&
                           !installed.host_abi_profile.empty() &&
                           !installed.compatibility_manifest.empty();
    plan.origin = "provider latest";
    plans.push_back(std::move(plan));
  }
  // Without this step a rejected latest candidate leaves the user on whatever
  // ancient payload they had, fixable only by deleting the store by hand.
  if (!runnable || preferred.version_code > runnable_code) {
    if (plans.empty() ||
        plans.front().identity.version_code != preferred.version_code) {
      UpdateCandidatePlan plan;
      plan.identity = ExactPayloadIdentity(preferred);
      plan.exact_supported = true;
      plan.origin = "supported catalog";
      plans.push_back(std::move(plan));
    }
  }
  return plans;
}

UpdateResult RunUpdate(const UpdatePaths& paths, const UpdateRequest& request) {
  UpdateResult result;
  StageTimer total_timer("total");
  UpdateLock update_lock(paths.data_root, &result.error);
  if (!update_lock) return result;

  const UpdateConfigResult configured = LoadUpdateConfig(paths.config_file);
  result.warnings = configured.warnings;
  if (!configured) {
    result.error = configured.error;
    return result;
  }
  if (configured.config.source != "apk-pure" &&
      configured.config.source != "auto") {
    result.error = "native updater supports only the direct APK providers";
    return result;
  }
  const CompatibilityCatalogResult catalog =
      LoadCompatibilityCatalog(paths.compatibility_manifest);
  if (!catalog) {
    result.error = catalog.error;
    return result;
  }
  const auto preferred = PreferredSupportedProfile(catalog.profiles);
  if (!preferred.has_value()) {
    result.error = "no preferred supported Roblox profile is available";
    return result;
  }

  PayloadStore store(paths.data_root, paths.compatibility_manifest,
                     paths.runtime_binary);
  const PayloadStoreResult current = store.VerifyCurrent();
  PayloadStoreResult installed = current;
  if (!current && !current.payload_id.empty() &&
      !current.host_abi_profile.empty() &&
      !current.compatibility_manifest.empty()) {
    // Integrity was already checked. A receipt from an older runtime is still
    // a valid derivation reference, but it must pass probation again before it
    // can become runnable with this runtime.
    installed.error.clear();
  } else if (!current) {
    installed = store.InspectCurrent();
  }
  if (request.force_run_latest) {
    UpdateResult unsafe =
        RunUnsafeLatest(paths, request, catalog.profiles, installed, &store);
    unsafe.warnings = std::move(result.warnings);
    return unsafe;
  }
  if (!configured.config.automatic && request.startup_preflight) {
    if (current) {
      result.payload_id = current.payload_id;
      result.message = "automatic Roblox updates are disabled";
      return result;
    }
    result.error =
        "no runnable payload is installed and automatic updates are disabled";
    return result;
  }
  const ProviderChain provider = BuildProviderChain();
  std::optional<ProviderVersion> latest_version;
  if (request.check_latest) {
    Progress(request.progress_fd, "Checking Roblox...");
    const ProviderVersion latest = [&] {
      StageTimer timer("check-latest");
      return provider.CheckLatest();
    }();
    if (latest) {
      latest_version = latest;
      if (current && SameVersion(current, latest)) {
        result.payload_id = current.payload_id;
        result.message = "latest Roblox " + latest.version_name + " (" +
                         std::to_string(latest.version_code) +
                         ") is already canary-approved";
        result.message += CollectSupersededPayloads(
            &store, ReferencePayloadIds(paths, catalog.profiles));
        return result;
      }
      if (current && current.version_code > latest.version_code) {
        result.payload_id = current.payload_id;
        result.message = "installed Roblox is newer than provider metadata";
        return result;
      }
    } else {
      if (current) {
        result.payload_id = current.payload_id;
        result.message =
            "update metadata is temporarily unavailable: " + latest.error;
        return result;
      }
    }
  }
  const std::vector<UpdateCandidatePlan> plans = PlanUpdateCandidates(
      catalog.profiles, *preferred,
      latest_version.has_value() ? &*latest_version : nullptr, current,
      installed);
  if (plans.empty()) {
    if (!current) {
      result.error = "no Roblox payload is available to install";
      return result;
    }
    result.payload_id = current.payload_id;
    result.message = "installed Roblox " + current.version_name + " (" +
                     std::to_string(current.version_code) + ") is current";
    return result;
  }

  RemoveStaleWorkspaces(paths.cache_root / "downloads/native-updater");
  const std::filesystem::path workspace = UniqueDirectory(
      paths.cache_root / "downloads/native-updater", &result.error);
  if (!result.error.empty()) return result;
  std::error_code filesystem_error;
  const auto cleanup = [&]() {
    std::filesystem::remove_all(workspace, filesystem_error);
  };

  std::string blocked;
  std::size_t attempt = 0;
  // Includes a rejected payload: it stays staged so the next launch
  // recognises it instead of downloading it again.
  std::vector<std::string> examined =
      ReferencePayloadIds(paths, catalog.profiles);
  for (const UpdateCandidatePlan& plan : plans) {
    const std::string description = PlanDescription(plan);
    ++attempt;
    Candidate candidate;
    if (plan.reuse_installed) {
      candidate.staged = installed;
      candidate.identity = plan.identity;
      candidate.exact_supported = plan.exact_supported;
      candidate.profile = installed.host_abi_profile;
      candidate.compatibility = installed.compatibility_manifest;
    } else {
      candidate =
          FindStagedCandidate(paths, plan.identity, plan.exact_supported);
      if (!candidate) {
        candidate = DownloadCandidate(
            &provider, &store, plan.identity, plan.exact_supported, paths,
            workspace / ("candidate-" + std::to_string(attempt)),
            request.progress_fd);
      }
    }
    if (!candidate) {
      blocked = description + " could not be prepared: " + candidate.error;
      continue;
    }
    examined.push_back(candidate.staged.payload_id);
    if (request.startup_preflight && current &&
        RejectedForRuntime(paths, candidate,
                           request.canary_graphics_backend)) {
      blocked = description + " already failed probation with this runtime";
      continue;
    }

    std::string candidate_error;
    bool candidate_rejected = false;
    if (!candidate.exact_supported && candidate.profile.empty()) {
      const ReferenceProfile reference =
          ResolveReference(paths, installed, catalog.profiles, &provider,
                           &store, workspace, request.progress_fd);
      if (!reference) {
        candidate_error = reference.error;
      } else {
        HostAbiDerivationOptions derivation;
        derivation.reference_library = reference.library;
        derivation.reference_profile = reference.profile;
        derivation.reference_compatibility_manifests =
            reference.compatibility_manifests;
        derivation.candidate_payload_directory =
            candidate.staged.payload_directory;
        derivation.output_directory =
            workspace / ("derived-" + std::to_string(attempt));
        Progress(request.progress_fd,
                 "Checking latest Roblox compatibility...");
        const HostAbiDerivationResult derived = [&] {
          StageTimer timer("derive-host-abi");
          return DeriveHostAbiProfile(derivation);
        }();
        if (!derived) {
          candidate_error = derived.error;
          candidate_rejected = true;
        } else {
          candidate.profile = derived.profile;
          candidate.compatibility = derived.compatibility_manifest;
        }
      }
    }
    PayloadStoreResult promoted;
    if (candidate_error.empty()) {
      promoted =
          PromoteCandidate(paths, request, candidate, &store, &candidate_error);
      candidate_rejected = !candidate_error.empty();
    }
    if (candidate_error.empty() && promoted) {
      result.changed = !current || current.payload_id != promoted.payload_id;
      result.payload_id = promoted.payload_id;
      result.message =
          candidate.exact_supported
              ? "installed exact-supported Roblox " + promoted.version_name +
                    " (" + std::to_string(promoted.version_code) + ")"
              : "Roblox " + promoted.version_name + " (" +
                    std::to_string(promoted.version_code) + ") passed two " +
                    std::string(CanaryGraphicsBackendName(
                        request.canary_graphics_backend)) +
                    " graphics canaries and is current";
      if (!blocked.empty()) result.message += "; " + blocked;
      result.message += CollectSupersededPayloads(&store, examined);
      cleanup();
      return result;
    }
    if (candidate_rejected) {
      RecordRejection(paths, candidate, request.canary_graphics_backend,
                      candidate_error);
    }
    blocked = description + " was rejected: " + candidate_error;
  }

  cleanup();
  if (!current) {
    result.error = "no Roblox payload could be installed; " + blocked;
    return result;
  }
  result.payload_id = current.payload_id;
  result.stale = latest_version.has_value() &&
                 current.version_code < latest_version->version_code;
  result.message = "kept Roblox " + current.version_name + " (" +
                   std::to_string(current.version_code) + "); " + blocked;
  if (result.stale) {
    result.message += "; Roblox " + latest_version->version_name +
                      " is published, so this session can be rejected as "
                      "outdated";
  }
  return result;
}

}  // namespace mocktail::update
