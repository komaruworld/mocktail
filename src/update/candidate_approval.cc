#include "update/candidate_approval.h"

#define JSON_NOEXCEPTION 1
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "compat/elf_build_id.h"
#include "mocktail/sha256.h"
#include "update/payload_integrity.h"

namespace mocktail::update {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumApprovalBytes = 1024U * 1024U;

bool IsLowerHex(std::string_view value, std::size_t size) {
  return value.size() == size &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isdigit(character) ||
                  (character >= 'a' && character <= 'f');
         });
}

std::string ReadRegular(const std::filesystem::path& path,
                        bool require_immutable, std::string* error) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    *error = "cannot open approval artifact: " + path.string();
    return {};
  }
  struct stat metadata = {};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_uid != geteuid() || metadata.st_size < 0 ||
      static_cast<std::uintmax_t>(metadata.st_size) > kMaximumApprovalBytes ||
      (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
      (require_immutable && (metadata.st_mode & S_IWUSR) != 0)) {
    close(descriptor);
    *error = "approval artifact ownership or permissions are invalid";
    return {};
  }
  std::string contents(static_cast<std::size_t>(metadata.st_size), '\0');
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t bytes =
        read(descriptor, contents.data() + offset, contents.size() - offset);
    if (bytes < 0 && errno == EINTR) continue;
    if (bytes <= 0) {
      close(descriptor);
      *error = "cannot read approval artifact";
      return {};
    }
    offset += static_cast<std::size_t>(bytes);
  }
  close(descriptor);
  return contents;
}

std::optional<Json> ParseJson(std::string_view contents, std::string* error) {
  Json document = Json::parse(contents, nullptr, false, true);
  if (document.is_discarded() || !document.is_object()) {
    *error = "approval artifact is not a JSON object";
    return std::nullopt;
  }
  return document;
}

bool WriteExclusive(const std::filesystem::path& path,
                    std::string_view contents, mode_t mode,
                    std::string* error) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(path.parent_path(), filesystem_error);
  if (filesystem_error) {
    *error = "cannot create approval artifact directory";
    return false;
  }
  if (std::filesystem::exists(path, filesystem_error)) {
    std::string read_error;
    const std::string existing = ReadRegular(path, mode == 0444, &read_error);
    if (read_error.empty() && existing == contents) return true;
    *error = "immutable approval generation collision";
    return false;
  }
  const std::filesystem::path temporary =
      path.parent_path() /
      (".approval-" + std::to_string(getpid()) + "-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  const int descriptor =
      open(temporary.c_str(),
           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor < 0) {
    *error = "cannot create approval artifact";
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t bytes =
        write(descriptor, contents.data() + offset, contents.size() - offset);
    if (bytes < 0 && errno == EINTR) continue;
    if (bytes <= 0) {
      close(descriptor);
      std::filesystem::remove(temporary, filesystem_error);
      *error = "cannot write approval artifact";
      return false;
    }
    offset += static_cast<std::size_t>(bytes);
  }
  if (fchmod(descriptor, mode) != 0 || fsync(descriptor) != 0) {
    close(descriptor);
    std::filesystem::remove(temporary, filesystem_error);
    *error = "cannot make approval artifact immutable";
    return false;
  }
  close(descriptor);
  std::filesystem::rename(temporary, path, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary, filesystem_error);
    *error = "cannot publish approval artifact";
    return false;
  }
  return true;
}

bool CopyImmutable(const std::filesystem::path& source,
                   const std::filesystem::path& destination,
                   std::string* error) {
  const std::string contents = ReadRegular(source, false, error);
  return error->empty() && WriteExclusive(destination, contents, 0444, error);
}

std::string HashText(std::string_view contents) {
  return foundation::ComputeSha256Hex(contents);
}

std::string UtcTimestamp() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  if (gmtime_r(&now, &utc) == nullptr) return "1970-01-01T00:00:00Z";
  std::array<char, 32> value{};
  std::strftime(value.data(), value.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return value.data();
}

std::optional<std::string> ExtractGeneration(std::string_view relative,
                                             std::string_view directory,
                                             std::string_view payload_id,
                                             std::string_view suffix) {
  const std::string prefix =
      std::string(directory) + "/" + std::string(payload_id) + "-";
  if (relative.size() != prefix.size() + 40 + suffix.size() ||
      relative.substr(0, prefix.size()) != prefix ||
      relative.substr(relative.size() - suffix.size()) != suffix) {
    return std::nullopt;
  }
  const std::string generation =
      std::string(relative.substr(prefix.size(), 40));
  return IsLowerHex(generation, 40) ? std::optional<std::string>(generation)
                                    : std::nullopt;
}

bool ValidateRelativeApprovalPath(const std::filesystem::path& root,
                                  const std::filesystem::path& path,
                                  std::string_view directory,
                                  std::string_view payload_id,
                                  std::string_view generation,
                                  std::filesystem::path* resolved,
                                  std::string* error) {
  if (path.is_absolute() || path.has_root_path() ||
      path != std::filesystem::path(directory) /
                  (std::string(payload_id) + "-" + std::string(generation) +
                   ".json")) {
    *error = "approval reference is outside its generated directory";
    return false;
  }
  std::error_code filesystem_error;
  const std::filesystem::path canonical_root =
      std::filesystem::canonical(root, filesystem_error);
  if (filesystem_error) {
    *error = "approval store root cannot be resolved safely";
    return false;
  }
  filesystem_error.clear();
  const std::filesystem::path canonical =
      std::filesystem::canonical(root / path, filesystem_error);
  if (filesystem_error ||
      canonical.parent_path() != canonical_root / directory ||
      canonical.filename() != path.filename()) {
    *error = "approval reference cannot be resolved safely";
    return false;
  }
  *resolved = canonical;
  return true;
}

bool ValidateProfileBinding(const Json& profile, const Json& compatibility,
                            const PayloadIntegrityResult& payload,
                            std::string* error) {
  if (profile.value("schema_version", 0) != 1 ||
      profile.value("payload_id", "") != payload.payload_id ||
      profile.value("payload_path", "") != "payloads/" + payload.payload_id ||
      profile.value("elf_build_id", "") != payload.metadata.build_id ||
      profile.value("payload_sha256", "") != payload.metadata.library_sha256 ||
      !profile.contains("profile") || !profile["profile"].is_object() ||
      profile["profile"].value("elf_build_id", "") !=
          payload.metadata.build_id ||
      !profile.contains("reference") || !profile["reference"].is_object() ||
      !compatibility.is_object() ||
      compatibility.value("schema_version", 0) != 1 ||
      !compatibility.contains("profiles") ||
      !compatibility["profiles"].is_array() ||
      compatibility["profiles"].size() != 1) {
    *error = "derived approval profile is not bound to exact payload bytes";
    return false;
  }
  const Json& entry = compatibility["profiles"][0];
  if (!entry.is_object() ||
      entry.value("version_name", "") != payload.metadata.version_name ||
      entry.value("version_code", 0ULL) != payload.metadata.version_code ||
      entry.value("elf_build_id", "") != payload.metadata.build_id ||
      entry.value("status", "") != "experimental" ||
      !entry.value("default_allowed", false) ||
      entry.value("allow_legacy_binary_patches", true) ||
      !entry.value("allow_host_abi_bridges", false) ||
      !entry.value("allow_host_constructor_replay", false)) {
    *error = "candidate compatibility manifest is not exact probation metadata";
    return false;
  }
  return true;
}

}  // namespace

std::string PayloadRuntimeFingerprint(
    const std::filesystem::path& payload_directory,
    const PayloadIntegrityResult& payload, std::string* error) {
  if (!payload || payload.payload_id.empty()) {
    *error = "cannot fingerprint an invalid payload";
    return {};
  }
  const std::string metadata =
      HashRegularFile(payload_directory / "roblox_payload.json", error);
  if (!error->empty()) return {};
  std::string evidence;
  evidence.append("payload_id=").append(payload.payload_id).append("\n");
  evidence.append("metadata=").append(metadata).append("\n");
  evidence.append("libroblox=")
      .append(payload.metadata.library_sha256)
      .append("\n");
  evidence.append("base_apk=")
      .append(payload.metadata.base_apk_sha256)
      .append("\n");
  evidence.append("x86_64_split=")
      .append(payload.metadata.split_apk_sha256)
      .append("\n");
  evidence.append("assets=")
      .append(payload.metadata.asset_tree_sha256)
      .append("\n");
  return HashText(evidence);
}

CandidateApprovalResult CreateCandidateApproval(
    const CandidateApprovalOptions& options,
    const PayloadIntegrityResult& verified_payload) {
  CandidateApprovalResult result;
  if (!verified_payload || verified_payload.payload_id.empty() ||
      options.payload_directory.filename() != verified_payload.payload_id) {
    result.error = "candidate approval requires a preverified exact payload";
    return result;
  }
  if (options.canary_logs[0].empty() || options.canary_logs[1].empty() ||
      options.canary_logs[0] == options.canary_logs[1]) {
    result.error = "candidate approval requires two distinct canary logs";
    return result;
  }
  const PayloadIntegrityResult& payload = verified_payload;
  std::string profile_bytes =
      ReadRegular(options.profile, false, &result.error);
  std::string compatibility_bytes =
      ReadRegular(options.compatibility_manifest, false, &result.error);
  if (!result.error.empty()) return result;
  const auto profile_json = ParseJson(profile_bytes, &result.error);
  const auto compatibility_json = ParseJson(compatibility_bytes, &result.error);
  if (!profile_json.has_value() || !compatibility_json.has_value() ||
      !ValidateProfileBinding(*profile_json, *compatibility_json, payload,
                              &result.error)) {
    return result;
  }
  const std::string profile_sha256 = HashText(profile_bytes);
  const std::string compatibility_sha256 = HashText(compatibility_bytes);
  const std::string runtime_sha256 =
      HashRegularFile(options.runtime_binary, &result.error);
  const compat::BuildIdResult runtime_build_id =
      compat::ReadElfBuildId(options.runtime_binary.string());
  const std::string fingerprint = PayloadRuntimeFingerprint(
      options.payload_directory, payload, &result.error);
  if (!result.error.empty() || !runtime_build_id ||
      !IsLowerHex(runtime_build_id.build_id, 40)) {
    if (result.error.empty()) result.error = runtime_build_id.error;
    return result;
  }
  std::array<std::string, 2> attestations;
  std::array<std::string, 2> attestation_hashes;
  const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  for (std::size_t index = 0; index < options.canary_logs.size(); ++index) {
    const std::string log_sha256 =
        HashRegularFile(options.canary_logs[index], &result.error);
    if (!result.error.empty()) return result;
    const std::size_t run = index + 1;
    const std::string run_id = payload.payload_id + "-" + std::to_string(run) +
                               "-" + std::to_string(epoch) + "-" +
                               std::to_string(getpid());
    const Json attestation = {
        {"schema_version", 1},
        {"status", "passed"},
        {"canary_tier", "C"},
        {"run", run},
        {"run_id", run_id},
        {"payload_id", payload.payload_id},
        {"payload_fingerprint", fingerprint},
        {"profile_sha256", profile_sha256},
        {"compatibility_manifest_sha256", compatibility_sha256},
        {"runtime_sha256", runtime_sha256},
        {"runtime_build_id", runtime_build_id.build_id},
        {"readiness_log_sha256", log_sha256},
    };
    attestations[index] = attestation.dump(2) + "\n";
    attestation_hashes[index] = HashText(attestations[index]);
  }
  std::string generation_evidence;
  generation_evidence.append("runtime_build_id=")
      .append(runtime_build_id.build_id)
      .append("\n");
  generation_evidence.append("payload=").append(fingerprint).append("\n");
  generation_evidence.append("profile=").append(profile_sha256).append("\n");
  generation_evidence.append("compatibility=")
      .append(compatibility_sha256)
      .append("\n");
  generation_evidence.append("canary_1=")
      .append(attestation_hashes[0])
      .append("\n");
  generation_evidence.append("canary_2=")
      .append(attestation_hashes[1])
      .append("\n");
  result.generation = HashText(generation_evidence).substr(0, 40);
  const std::string stem = payload.payload_id + "-" + result.generation;
  result.profile = options.store_root / "host_abi_profiles" / (stem + ".json");
  result.compatibility_manifest =
      options.store_root / "compatibility_profiles" / (stem + ".json");
  result.receipt = options.store_root / "approvals" / (stem + ".json");
  const std::array<std::filesystem::path, 2> attestation_paths = {
      options.store_root / "approvals" / (stem + ".canary-1.json"),
      options.store_root / "approvals" / (stem + ".canary-2.json"),
  };
  if (!CopyImmutable(options.profile, result.profile, &result.error) ||
      !CopyImmutable(options.compatibility_manifest,
                     result.compatibility_manifest, &result.error) ||
      !WriteExclusive(attestation_paths[0], attestations[0], 0444,
                      &result.error) ||
      !WriteExclusive(attestation_paths[1], attestations[1], 0444,
                      &result.error)) {
    return result;
  }
  std::error_code filesystem_error;
  const std::filesystem::path canonical_library = std::filesystem::canonical(
      options.payload_directory / "libroblox.so", filesystem_error);
  if (filesystem_error) {
    result.error = "cannot canonicalize approved payload library";
    return result;
  }
  const Json receipt = {
      {"schema_version", 1},
      {"status", "approved"},
      {"payload_id", payload.payload_id},
      {"generation", result.generation},
      {"payload_path", canonical_library.string()},
      {"elf_build_id", payload.metadata.build_id},
      {"payload_sha256", payload.metadata.library_sha256},
      {"payload_fingerprint", fingerprint},
      {"profile_sha256", profile_sha256},
      {"compatibility_manifest_sha256", compatibility_sha256},
      {"canary_runtime_sha256", runtime_sha256},
      {"runtime_sha256", runtime_sha256},
      {"runtime_build_id", runtime_build_id.build_id},
      {"canary_tier", "C"},
      {"successful_runs", 2},
      {"canary_attestation_sha256",
       Json::array({attestation_hashes[0], attestation_hashes[1]})},
      {"approved_at", UtcTimestamp()},
  };
  if (!WriteExclusive(result.receipt, receipt.dump(2) + "\n", 0444,
                      &result.error)) {
    return result;
  }
  return result;
}

bool ValidateCandidateApproval(const std::filesystem::path& store_root,
                               std::string_view activation_json,
                               const PayloadIntegrityResult& payload,
                               CandidateApprovalResult* approval,
                               std::string* error) {
  const auto activation = ParseJson(activation_json, error);
  if (!activation.has_value()) return false;
  const std::string approval_ref = activation->value("approval_path", "");
  const std::string profile_ref =
      activation->value("host_abi_profile_path", "");
  const std::string compatibility_ref =
      activation->value("compatibility_manifest_path", "");
  const auto generation =
      ExtractGeneration(approval_ref, "approvals", payload.payload_id, ".json");
  if (!generation.has_value() ||
      profile_ref != "host_abi_profiles/" + payload.payload_id + "-" +
                         *generation + ".json" ||
      compatibility_ref != "compatibility_profiles/" + payload.payload_id +
                               "-" + *generation + ".json") {
    *error = "approved payload manifest has invalid approval references";
    return false;
  }
  approval->generation = *generation;
  if (!ValidateRelativeApprovalPath(store_root, approval_ref, "approvals",
                                    payload.payload_id, *generation,
                                    &approval->receipt, error) ||
      !ValidateRelativeApprovalPath(store_root, profile_ref,
                                    "host_abi_profiles", payload.payload_id,
                                    *generation, &approval->profile, error) ||
      !ValidateRelativeApprovalPath(store_root, compatibility_ref,
                                    "compatibility_profiles",
                                    payload.payload_id, *generation,
                                    &approval->compatibility_manifest, error)) {
    return false;
  }
  const std::string receipt_bytes = ReadRegular(approval->receipt, true, error);
  const std::string profile_bytes = ReadRegular(approval->profile, true, error);
  const std::string compatibility_bytes =
      ReadRegular(approval->compatibility_manifest, true, error);
  if (!error->empty()) return false;
  const auto receipt = ParseJson(receipt_bytes, error);
  const auto profile = ParseJson(profile_bytes, error);
  const auto compatibility = ParseJson(compatibility_bytes, error);
  if (!receipt.has_value() || !profile.has_value() ||
      !compatibility.has_value() ||
      !ValidateProfileBinding(*profile, *compatibility, payload, error)) {
    return false;
  }
  const std::string fingerprint = PayloadRuntimeFingerprint(
      store_root / "payloads" / payload.payload_id, payload, error);
  const std::string profile_sha256 = HashText(profile_bytes);
  const std::string compatibility_sha256 = HashText(compatibility_bytes);
  if (!error->empty()) return false;
  const std::string canary_runtime_sha256 =
      receipt->value("canary_runtime_sha256", "");
  const std::string runtime_sha256 = receipt->value("runtime_sha256", "");
  const std::string runtime_build_id =
      receipt->value("runtime_build_id", "");
  std::error_code filesystem_error;
  const std::filesystem::path canonical_library = std::filesystem::canonical(
      store_root / "payloads" / payload.payload_id / "libroblox.so",
      filesystem_error);
  if (filesystem_error) {
    *error = "cannot canonicalize approved payload library";
    return false;
  }
  if (receipt->value("schema_version", 0) != 1 ||
      receipt->value("status", "") != "approved" ||
      receipt->value("payload_id", "") != payload.payload_id ||
      receipt->value("generation", "") != *generation ||
      receipt->value("payload_path", "") != canonical_library.string() ||
      receipt->value("elf_build_id", "") != payload.metadata.build_id ||
      receipt->value("payload_sha256", "") != payload.metadata.library_sha256 ||
      receipt->value("payload_fingerprint", "") != fingerprint ||
      receipt->value("profile_sha256", "") != profile_sha256 ||
      receipt->value("compatibility_manifest_sha256", "") !=
          compatibility_sha256 ||
      !IsLowerHex(canary_runtime_sha256, 64) ||
      runtime_sha256 != canary_runtime_sha256 ||
      !IsLowerHex(runtime_build_id, 40) ||
      receipt->value("canary_tier", "") != "C" ||
      receipt->value("successful_runs", 0) != 2 ||
      !receipt->contains("canary_attestation_sha256") ||
      !(*receipt)["canary_attestation_sha256"].is_array() ||
      (*receipt)["canary_attestation_sha256"].size() != 2) {
    *error = "approval receipt does not authorize exact payload evidence";
    return false;
  }
  std::array<std::string, 2> attestation_hashes;
  std::array<std::string, 2> run_ids;
  for (std::size_t index = 0; index < 2; ++index) {
    const std::filesystem::path attestation_path =
        store_root / "approvals" /
        (payload.payload_id + "-" + *generation + ".canary-" +
         std::to_string(index + 1) + ".json");
    const std::string bytes = ReadRegular(attestation_path, true, error);
    const auto attestation = ParseJson(bytes, error);
    if (!error->empty() || !attestation.has_value()) return false;
    attestation_hashes[index] = HashText(bytes);
    run_ids[index] = attestation->value("run_id", "");
    if (attestation->value("schema_version", 0) != 1 ||
        attestation->value("status", "") != "passed" ||
        attestation->value("canary_tier", "") != "C" ||
        attestation->value("run", 0U) != index + 1 ||
        attestation->value("payload_id", "") != payload.payload_id ||
        attestation->value("payload_fingerprint", "") != fingerprint ||
        attestation->value("profile_sha256", "") != profile_sha256 ||
        attestation->value("compatibility_manifest_sha256", "") !=
            compatibility_sha256 ||
        attestation->value("runtime_sha256", "") !=
            canary_runtime_sha256 ||
        attestation->value("runtime_build_id", "") != runtime_build_id ||
        !IsLowerHex(attestation->value("readiness_log_sha256", ""), 64) ||
        !(*receipt)["canary_attestation_sha256"][index].is_string() ||
        (*receipt)["canary_attestation_sha256"][index]
                .get_ref<const std::string&>() != attestation_hashes[index]) {
      *error = "canary attestation does not match approval evidence";
      return false;
    }
  }
  if (run_ids[0].empty() || run_ids[0] == run_ids[1]) {
    *error = "approval canaries do not have distinct run identities";
    return false;
  }
  std::string generation_evidence;
  generation_evidence.append("runtime_build_id=")
      .append(runtime_build_id)
      .append("\n");
  generation_evidence.append("payload=").append(fingerprint).append("\n");
  generation_evidence.append("profile=").append(profile_sha256).append("\n");
  generation_evidence.append("compatibility=")
      .append(compatibility_sha256)
      .append("\n");
  generation_evidence.append("canary_1=")
      .append(attestation_hashes[0])
      .append("\n");
  generation_evidence.append("canary_2=")
      .append(attestation_hashes[1])
      .append("\n");
  if (HashText(generation_evidence).substr(0, 40) != *generation) {
    *error = "approval evidence generation does not match";
    return false;
  }
  return true;
}

}  // namespace mocktail::update
