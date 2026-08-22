#include "update/payload_store.h"

#define JSON_NOEXCEPTION 1
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "update/candidate_approval.h"
#include "update/compatibility_catalog.h"
#include "update/payload_integrity.h"

namespace mocktail::update {
namespace {

class StoreLock final {
 public:
  StoreLock(const std::filesystem::path& root, std::string* error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(root / "payloads", filesystem_error);
    if (filesystem_error) {
      *error = "cannot create payload store";
      return;
    }
    descriptor_ = open((root / ".payload-store.lock").c_str(),
                       O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor_ < 0 || flock(descriptor_, LOCK_EX) != 0) {
      *error = "cannot lock payload store";
      if (descriptor_ >= 0) close(descriptor_);
      descriptor_ = -1;
    }
  }

  ~StoreLock() {
    if (descriptor_ >= 0) close(descriptor_);
  }

  explicit operator bool() const { return descriptor_ >= 0; }

 private:
  int descriptor_ = -1;
};

bool ValidPayloadId(std::string_view value) {
  const std::size_t dash = value.find('-');
  if (dash == std::string_view::npos || dash == 0 ||
      value.size() - dash - 1 != 40) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == dash) continue;
    const unsigned char character = value[index];
    if ((index < dash && !std::isdigit(character)) ||
        (index > dash && !std::isdigit(character) &&
         !(character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::string ReadRegular(const std::filesystem::path& path, std::size_t maximum,
                        std::string* error, bool missing_ok = false) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (missing_ok && errno == ENOENT) return {};
    *error = "cannot open payload-store file: " + path.string();
    return {};
  }
  struct stat metadata = {};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 ||
      static_cast<std::uintmax_t>(metadata.st_size) > maximum) {
    close(descriptor);
    *error = "payload-store file is not a bounded regular file";
    return {};
  }
  std::string contents;
  contents.reserve(static_cast<std::size_t>(metadata.st_size));
  std::array<char, 16U * 1024U> buffer{};
  while (true) {
    const ssize_t bytes = read(descriptor, buffer.data(), buffer.size());
    if (bytes == 0) break;
    if (bytes < 0) {
      if (errno == EINTR) continue;
      close(descriptor);
      *error = "cannot read payload-store file";
      return {};
    }
    contents.append(buffer.data(), static_cast<std::size_t>(bytes));
  }
  close(descriptor);
  return contents;
}

bool WriteAtomic(const std::filesystem::path& root,
                 const std::filesystem::path& destination,
                 std::string_view contents, std::string* error) {
  static std::uint64_t generation = 0;
  const std::filesystem::path temporary =
      root / (".manifest-" + std::to_string(getpid()) + "-" +
              std::to_string(++generation));
  const int descriptor =
      open(temporary.c_str(),
           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (descriptor < 0) {
    *error = "cannot create atomic payload manifest";
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t bytes =
        write(descriptor, contents.data() + offset, contents.size() - offset);
    if (bytes < 0) {
      if (errno == EINTR) continue;
      close(descriptor);
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      *error = "cannot write atomic payload manifest";
      return false;
    }
    offset += static_cast<std::size_t>(bytes);
  }
  const bool synced = fsync(descriptor) == 0;
  close(descriptor);
  if (!synced) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    *error = "cannot persist atomic payload manifest";
    return false;
  }
  std::error_code filesystem_error;
  std::filesystem::rename(temporary, destination, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary, filesystem_error);
    *error = "cannot publish atomic payload manifest";
    return false;
  }
  const int root_descriptor =
      open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (root_descriptor >= 0) {
    (void)fsync(root_descriptor);
    close(root_descriptor);
  }
  return true;
}

std::string TimestampSuffix() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::to_string(
             std::chrono::duration_cast<std::chrono::seconds>(now).count()) +
         "-" + std::to_string(getpid());
}

bool QuarantinePayloadCollision(const std::filesystem::path& root,
                                const std::filesystem::path& payload,
                                std::string_view payload_id,
                                std::string* error) {
  std::error_code filesystem_error;
  const std::filesystem::path quarantine =
      root / "quarantine" /
      (std::string(payload_id) + "-" + TimestampSuffix());
  std::filesystem::create_directories(quarantine.parent_path(),
                                      filesystem_error);
  if (filesystem_error) {
    *error = "cannot create corrupt payload quarantine: " +
             filesystem_error.message();
    return false;
  }

  const auto status =
      std::filesystem::symlink_status(payload, filesystem_error);
  if (filesystem_error) {
    *error = "cannot inspect corrupt payload collision: " +
             filesystem_error.message();
    return false;
  }
  const bool read_only_directory =
      std::filesystem::is_directory(status) &&
      !std::filesystem::is_symlink(status) &&
      (status.permissions() & std::filesystem::perms::owner_write) ==
          std::filesystem::perms::none;
  if (read_only_directory) {
    // Moving a directory across parents updates its `..` entry. Linux denies
    // that operation for the read-only roots intentionally produced by
    // CopyTree, so briefly restore owner-write only on the collision root.
    std::filesystem::permissions(payload,
                                 std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add,
                                 filesystem_error);
    if (filesystem_error) {
      *error = "cannot unlock corrupt payload collision: " +
               filesystem_error.message();
      return false;
    }
  }

  std::filesystem::rename(payload, quarantine, filesystem_error);
  if (filesystem_error) {
    if (read_only_directory) {
      std::error_code ignored;
      std::filesystem::permissions(payload, status.permissions(),
                                   std::filesystem::perm_options::replace,
                                   ignored);
    }
    *error = "cannot quarantine corrupt payload collision: " +
             filesystem_error.message();
    return false;
  }
  if (read_only_directory) {
    std::error_code ignored;
    std::filesystem::permissions(quarantine, status.permissions(),
                                 std::filesystem::perm_options::replace,
                                 ignored);
  }
  return true;
}

// A payload tree is several hundred MiB. On a copy-on-write filesystem a
// reflink shares the extents instead of duplicating them, and copy_file_range
// keeps the bytes inside the kernel; both fall back to an ordinary copy.
// A hard link is deliberately not used: CopyTree makes the result read-only
// while the source stays writable, and a shared inode would break that.
bool CloneOrCopyFile(const std::filesystem::path& source,
                     const std::filesystem::path& destination) {
  const int input = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (input < 0) return false;
  struct stat metadata = {};
  if (fstat(input, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
    close(input);
    return false;
  }
  const int output =
      open(destination.c_str(),
           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (output < 0) {
    close(input);
    return false;
  }
  bool copied = false;
#if defined(__linux__) && defined(FICLONE)
  copied = ioctl(output, FICLONE, input) == 0;
#endif
#if defined(__linux__)
  if (!copied) {
    off_t remaining = metadata.st_size;
    copied = true;
    while (remaining > 0) {
      const ssize_t moved = copy_file_range(
          input, nullptr, output, nullptr, static_cast<size_t>(remaining), 0);
      if (moved < 0 && errno == EINTR) continue;
      if (moved <= 0) {
        copied = false;
        break;
      }
      remaining -= moved;
    }
    // copy_file_range advances both file offsets, so a partial move must be
    // undone completely before the portable fallback can start from zero.
    if (!copied && (ftruncate(output, 0) != 0 ||
                    lseek(input, 0, SEEK_SET) != 0 ||
                    lseek(output, 0, SEEK_SET) != 0)) {
      close(input);
      close(output);
      std::error_code ignored;
      std::filesystem::remove(destination, ignored);
      return false;
    }
  }
#endif
  if (!copied) {
    std::array<char, 256U * 1024U> buffer{};
    while (true) {
      const ssize_t bytes = read(input, buffer.data(), buffer.size());
      if (bytes == 0) break;
      if (bytes < 0) {
        if (errno == EINTR) continue;
        close(input);
        close(output);
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
        return false;
      }
      std::size_t offset = 0;
      while (offset < static_cast<std::size_t>(bytes)) {
        const ssize_t written =
            write(output, buffer.data() + offset,
                  static_cast<std::size_t>(bytes) - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
          close(input);
          close(output);
          std::error_code ignored;
          std::filesystem::remove(destination, ignored);
          return false;
        }
        offset += static_cast<std::size_t>(written);
      }
    }
  }
  close(input);
  const bool closed = close(output) == 0;
  if (!closed) {
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);
    return false;
  }
  return true;
}

bool CopyTree(const std::filesystem::path& source,
              const std::filesystem::path& destination, std::string* error) {
  std::error_code filesystem_error;
  if (!std::filesystem::create_directory(destination, filesystem_error)) {
    *error = "cannot create immutable payload staging directory";
    return false;
  }
  std::filesystem::recursive_directory_iterator iterator(
      source, std::filesystem::directory_options::none, filesystem_error);
  const std::filesystem::recursive_directory_iterator end;
  while (!filesystem_error && iterator != end) {
    const auto status = iterator->symlink_status(filesystem_error);
    if (filesystem_error) break;
    const std::filesystem::path relative =
        iterator->path().lexically_relative(source);
    const std::filesystem::path output = destination / relative;
    if (std::filesystem::is_symlink(status) ||
        (!std::filesystem::is_directory(status) &&
         !std::filesystem::is_regular_file(status))) {
      *error = "payload tree contains an unsafe entry";
      return false;
    }
    if (std::filesystem::is_directory(status)) {
      std::filesystem::create_directory(output, filesystem_error);
    } else if (!CloneOrCopyFile(iterator->path(), output)) {
      std::filesystem::copy_file(iterator->path(), output,
                                 std::filesystem::copy_options::none,
                                 filesystem_error);
    }
    if (filesystem_error) break;
    iterator.increment(filesystem_error);
  }
  if (filesystem_error) {
    *error = "cannot copy payload into immutable store";
    return false;
  }
  std::filesystem::recursive_directory_iterator permission_iterator(
      destination, std::filesystem::directory_options::none, filesystem_error);
  while (!filesystem_error && permission_iterator != end) {
    const auto status = permission_iterator->symlink_status(filesystem_error);
    if (filesystem_error) break;
    std::filesystem::permissions(permission_iterator->path(),
                                 std::filesystem::is_directory(status)
                                     ? std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_exec |
                                           std::filesystem::perms::group_read |
                                           std::filesystem::perms::group_exec |
                                           std::filesystem::perms::others_read |
                                           std::filesystem::perms::others_exec
                                     : std::filesystem::perms::owner_read |
                                           std::filesystem::perms::group_read |
                                           std::filesystem::perms::others_read,
                                 std::filesystem::perm_options::replace,
                                 filesystem_error);
    permission_iterator.increment(filesystem_error);
  }
  std::filesystem::permissions(
      destination,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec |
          std::filesystem::perms::group_read |
          std::filesystem::perms::group_exec |
          std::filesystem::perms::others_read |
          std::filesystem::perms::others_exec,
      std::filesystem::perm_options::replace, filesystem_error);
  if (filesystem_error) {
    *error = "cannot make staged payload immutable";
    return false;
  }
  return true;
}

struct ManifestIdentity {
  std::string payload_id;
  std::string contents;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

ManifestIdentity ReadManifest(const std::filesystem::path& path,
                              bool missing_ok = false) {
  ManifestIdentity result;
  result.contents = ReadRegular(path, 1024U * 1024U, &result.error, missing_ok);
  if (!result.error.empty() || result.contents.empty()) return result;
  const nlohmann::json document =
      nlohmann::json::parse(result.contents, nullptr, false, true);
  if (document.is_discarded() || !document.is_object() ||
      document.value("schema_version", 0) != 1 ||
      !document.contains("payload_id") || !document["payload_id"].is_string()) {
    result.error = "active payload manifest is invalid";
    return result;
  }
  result.payload_id = document["payload_id"].get<std::string>();
  if (!ValidPayloadId(result.payload_id) ||
      document.value("payload_path", "") != "payloads/" + result.payload_id) {
    result.error = "active payload manifest identity is invalid";
  }
  return result;
}

bool ExactSupported(const std::filesystem::path& compatibility,
                    const PayloadMetadata& metadata, std::string* error) {
  const CompatibilityCatalogResult catalog =
      LoadCompatibilityCatalog(compatibility);
  if (!catalog) {
    *error = catalog.error;
    return false;
  }
  const auto found =
      std::find_if(catalog.profiles.begin(), catalog.profiles.end(),
                   [&](const auto& profile) {
                     return profile.version_code == metadata.version_code &&
                            profile.version_name == metadata.version_name &&
                            profile.elf_build_id == metadata.build_id;
                   });
  if (found == catalog.profiles.end()) {
    *error = "payload is not an exact default-supported profile";
    return false;
  }
  return true;
}

nlohmann::json ActivationManifest(const PayloadIntegrityResult& payload) {
  return {
      {"schema_version", 1},
      {"payload_id", payload.payload_id},
      {"payload_path", "payloads/" + payload.payload_id},
      {"version_name", payload.metadata.version_name},
      {"version_code", payload.metadata.version_code},
      {"elf_build_id", payload.metadata.build_id},
      {"payload_sha256", payload.metadata.library_sha256},
      {"activated_at_epoch",
       std::chrono::duration_cast<std::chrono::seconds>(
           std::chrono::system_clock::now().time_since_epoch())
           .count()},
  };
}

nlohmann::json ProbationActivationManifest(
    const PayloadIntegrityResult& payload,
    const CandidateApprovalResult& approval) {
  nlohmann::json manifest = ActivationManifest(payload);
  const std::string stem = payload.payload_id + "-" + approval.generation;
  manifest["approval_path"] = "approvals/" + stem + ".json";
  manifest["host_abi_profile_path"] = "host_abi_profiles/" + stem + ".json";
  manifest["compatibility_manifest_path"] =
      "compatibility_profiles/" + stem + ".json";
  return manifest;
}

bool PopulateInspectedResult(
    const std::filesystem::path& root, const ManifestIdentity& manifest,
    PayloadStoreResult* result,
    PayloadIntegrityResult* verified_payload = nullptr) {
  result->payload_id = manifest.payload_id;
  result->payload_directory = root / "payloads" / result->payload_id;
  const PayloadIntegrityResult payload =
      InspectPreparedPayload(result->payload_directory);
  if (!payload || payload.payload_id != result->payload_id) {
    result->error =
        payload ? "current payload identity mismatch" : payload.error;
    return false;
  }
  result->version_name = payload.metadata.version_name;
  result->version_code = payload.metadata.version_code;
  result->build_id = payload.metadata.build_id;
  if (verified_payload != nullptr) *verified_payload = payload;
  const nlohmann::json document =
      nlohmann::json::parse(manifest.contents, nullptr, false, true);
  if (document.is_discarded() ||
      document.value("version_name", "") != payload.metadata.version_name ||
      document.value("version_code", 0ULL) != payload.metadata.version_code ||
      document.value("elf_build_id", "") != payload.metadata.build_id) {
    result->error = "active payload manifest is invalid";
    return false;
  }
  const auto cached_sha256 = document.find("payload_sha256");
  if (cached_sha256 != document.end() &&
      (!cached_sha256->is_string() ||
       cached_sha256->get_ref<const std::string&>() !=
           payload.metadata.library_sha256)) {
    result->error = "active payload manifest hash does not match payload";
    return false;
  }
  const std::string profile = document.value("host_abi_profile_path", "");
  const std::string compatibility =
      document.value("compatibility_manifest_path", "");
  const std::string receipt = document.value("approval_path", "");
  if (!profile.empty() || !compatibility.empty() || !receipt.empty()) {
    if (profile.empty() || compatibility.empty() || receipt.empty()) {
      result->error = "active payload approval reference set is incomplete";
      return false;
    }
    result->host_abi_profile = root / profile;
    result->compatibility_manifest = root / compatibility;
    result->approval_receipt = root / receipt;
  }
  return true;
}

bool PersistPayloadSha256(const std::filesystem::path& root,
                          const ManifestIdentity& manifest,
                          const PayloadIntegrityResult& payload,
                          std::string* error) {
  nlohmann::json document =
      nlohmann::json::parse(manifest.contents, nullptr, false, true);
  if (document.is_discarded() || !document.is_object()) {
    *error = "active payload manifest is invalid";
    return false;
  }
  const auto cached = document.find("payload_sha256");
  if (cached != document.end()) {
    if (!cached->is_string() ||
        cached->get_ref<const std::string&>() !=
            payload.metadata.library_sha256) {
      *error = "active payload manifest hash does not match payload";
      return false;
    }
    return true;
  }
  document["payload_sha256"] = payload.metadata.library_sha256;
  return WriteAtomic(root, root / "current.json", document.dump(2) + "\n",
                     error);
}

}  // namespace

PayloadStore::PayloadStore(std::filesystem::path root,
                           std::filesystem::path compatibility_manifest,
                           std::filesystem::path runtime_binary)
    : root_(std::move(root)),
      compatibility_manifest_(std::move(compatibility_manifest)),
      runtime_binary_(std::move(runtime_binary)) {}

PayloadStoreResult PayloadStore::Stage(
    const std::filesystem::path& prepared_payload) {
  PayloadStoreResult result;
  StoreLock lock(root_, &result.error);
  if (!lock) return result;
  // Preparation produced these bytes and recorded their hashes. Hashing the
  // workspace again proves nothing that the post-copy verify below does not
  // prove about the bytes that actually become the immutable store entry.
  const PayloadIntegrityResult verified =
      InspectPreparedPayload(prepared_payload);
  if (!verified) {
    result.error = verified.error;
    return result;
  }
  result.payload_id = verified.payload_id;
  result.payload_directory = root_ / "payloads" / result.payload_id;
  result.version_name = verified.metadata.version_name;
  result.version_code = verified.metadata.version_code;
  result.build_id = verified.metadata.build_id;
  std::error_code filesystem_error;
  if (std::filesystem::exists(result.payload_directory, filesystem_error)) {
    const PayloadIntegrityResult existing =
        VerifyPreparedPayload(result.payload_directory);
    if (existing && existing.payload_id == result.payload_id &&
        HashRegularFile(prepared_payload / "roblox_payload.json") ==
            HashRegularFile(result.payload_directory / "roblox_payload.json")) {
      return result;
    }
    if (!QuarantinePayloadCollision(root_, result.payload_directory,
                                    result.payload_id, &result.error))
      return result;
  }
  const std::filesystem::path staging =
      root_ / "payloads" /
      (".stage-" + result.payload_id + "-" + std::to_string(getpid()));
  std::filesystem::remove_all(staging, filesystem_error);
  if (!CopyTree(prepared_payload, staging, &result.error)) {
    std::filesystem::remove_all(staging, filesystem_error);
    return result;
  }
  const PayloadIntegrityResult staged = VerifyPreparedPayload(staging);
  if (!staged || staged.payload_id != result.payload_id) {
    std::filesystem::permissions(staging, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add,
                                 filesystem_error);
    std::filesystem::remove_all(staging, filesystem_error);
    result.error = staged ? "staged payload identity changed" : staged.error;
    return result;
  }
  std::filesystem::rename(staging, result.payload_directory, filesystem_error);
  if (filesystem_error) {
    result.error = "cannot publish immutable payload";
  }
  return result;
}

PayloadStoreResult PayloadStore::Promote(std::string_view payload_id) {
  PayloadStoreResult result;
  if (!ValidPayloadId(payload_id)) {
    result.error = "invalid payload ID";
    return result;
  }
  StoreLock lock(root_, &result.error);
  if (!lock) return result;
  result.payload_id = std::string(payload_id);
  result.payload_directory = root_ / "payloads" / result.payload_id;
  const PayloadIntegrityResult payload =
      VerifyPreparedPayload(result.payload_directory);
  if (!payload || payload.payload_id != result.payload_id) {
    result.error =
        payload ? "payload directory identity mismatch" : payload.error;
    return result;
  }
  if (!ExactSupported(compatibility_manifest_, payload.metadata,
                      &result.error)) {
    return result;
  }
  result.version_name = payload.metadata.version_name;
  result.version_code = payload.metadata.version_code;
  result.build_id = payload.metadata.build_id;
  const ManifestIdentity current = ReadManifest(root_ / "current.json", true);
  if (!current && !current.error.empty()) {
    result.error = current.error;
    return result;
  }
  if (!current.contents.empty() && current.payload_id != result.payload_id &&
      !WriteAtomic(root_, root_ / "previous_good.json", current.contents,
                   &result.error)) {
    return result;
  }
  const std::string contents = ActivationManifest(payload).dump(2) + "\n";
  if (!WriteAtomic(root_, root_ / "current.json", contents, &result.error)) {
    return result;
  }
  return result;
}

PayloadStoreResult PayloadStore::PromoteProbation(
    std::string_view payload_id, const std::filesystem::path& candidate_profile,
    const std::filesystem::path& candidate_compatibility,
    const std::array<std::filesystem::path, 2>& canary_logs) {
  PayloadStoreResult result;
  if (!ValidPayloadId(payload_id) || runtime_binary_.empty()) {
    result.error = "probation promotion requires a valid payload and runtime";
    return result;
  }
  StoreLock lock(root_, &result.error);
  if (!lock) return result;
  result.payload_id = std::string(payload_id);
  result.payload_directory = root_ / "payloads" / result.payload_id;
  const PayloadIntegrityResult payload =
      VerifyPreparedPayload(result.payload_directory);
  if (!payload || payload.payload_id != result.payload_id) {
    result.error =
        payload ? "payload directory identity mismatch" : payload.error;
    return result;
  }
  std::string support_error;
  if (ExactSupported(compatibility_manifest_, payload.metadata,
                     &support_error)) {
    result.error = "exact-supported payload must use regular promotion";
    return result;
  }
  CandidateApprovalOptions options;
  options.store_root = root_;
  options.payload_directory = result.payload_directory;
  options.profile = candidate_profile;
  options.compatibility_manifest = candidate_compatibility;
  options.runtime_binary = runtime_binary_;
  options.canary_logs = canary_logs;
  const CandidateApprovalResult approval =
      CreateCandidateApproval(options, payload);
  if (!approval) {
    result.error = approval.error;
    return result;
  }
  const std::string contents =
      ProbationActivationManifest(payload, approval).dump(2) + "\n";
  CandidateApprovalResult verified_approval;
  if (!ValidateCandidateApproval(root_, contents, payload, &verified_approval,
                                 &result.error)) {
    return result;
  }
  const ManifestIdentity current = ReadManifest(root_ / "current.json", true);
  if (!current && !current.error.empty()) {
    result.error = current.error;
    return result;
  }
  if (!current.contents.empty() && current.payload_id != result.payload_id &&
      !WriteAtomic(root_, root_ / "previous_good.json", current.contents,
                   &result.error)) {
    return result;
  }
  if (!WriteAtomic(root_, root_ / "current.json", contents, &result.error)) {
    return result;
  }
  result.version_name = payload.metadata.version_name;
  result.version_code = payload.metadata.version_code;
  result.build_id = payload.metadata.build_id;
  result.host_abi_profile = verified_approval.profile;
  result.compatibility_manifest = verified_approval.compatibility_manifest;
  result.approval_receipt = verified_approval.receipt;
  return result;
}

PayloadStoreResult PayloadStore::Import(
    const std::filesystem::path& prepared_payload) {
  PayloadStoreResult staged = Stage(prepared_payload);
  if (!staged) return staged;
  return Promote(staged.payload_id);
}

PayloadStoreResult PayloadStore::VerifyCurrent() {
  PayloadStoreResult result;
  StoreLock lock(root_, &result.error);
  if (!lock) return result;
  const ManifestIdentity manifest = ReadManifest(root_ / "current.json");
  if (!manifest) {
    result.error =
        manifest.error.empty() ? "no current payload" : manifest.error;
    return result;
  }
  PayloadIntegrityResult payload;
  if (!PopulateInspectedResult(root_, manifest, &result, &payload)) {
    return result;
  }
  const std::string library_sha256 =
      HashRegularFile(result.payload_directory / "libroblox.so", &result.error);
  if (!result.error.empty() ||
      library_sha256 != payload.metadata.library_sha256) {
    if (result.error.empty())
      result.error = "current payload library hash mismatch";
    return result;
  }
  std::string support_error;
  if (ExactSupported(compatibility_manifest_, payload.metadata,
                     &support_error)) {
    if (!PersistPayloadSha256(root_, manifest, payload, &result.error)) {
      return result;
    }
    return result;
  }
  CandidateApprovalResult approval;
  if (!ValidateCandidateApproval(root_, manifest.contents, payload, &approval,
                                 &result.error)) {
    return result;
  }
  if (!PersistPayloadSha256(root_, manifest, payload, &result.error)) {
    return result;
  }
  result.host_abi_profile = approval.profile;
  result.compatibility_manifest = approval.compatibility_manifest;
  result.approval_receipt = approval.receipt;
  return result;
}

PayloadStoreResult PayloadStore::InspectCurrent() const {
  PayloadStoreResult result;
  StoreLock lock(root_, &result.error);
  if (!lock) return result;
  const ManifestIdentity manifest = ReadManifest(root_ / "current.json");
  if (!manifest) {
    result.error =
        manifest.error.empty() ? "no current payload" : manifest.error;
    return result;
  }
  (void)PopulateInspectedResult(root_, manifest, &result);
  return result;
}

PayloadStoreResult PayloadStore::Rollback() {
  PayloadStoreResult result;
  StoreLock lock(root_, &result.error);
  if (!lock) return result;
  const ManifestIdentity previous = ReadManifest(root_ / "previous_good.json");
  if (!previous) {
    result.error =
        previous.error.empty() ? "no previous-good payload" : previous.error;
    return result;
  }
  result.payload_id = previous.payload_id;
  result.payload_directory = root_ / "payloads" / result.payload_id;
  const PayloadIntegrityResult payload =
      VerifyPreparedPayload(result.payload_directory);
  if (!payload) {
    result.error = payload.error;
    return result;
  }
  std::string support_error;
  if (!ExactSupported(compatibility_manifest_, payload.metadata,
                      &support_error)) {
    CandidateApprovalResult approval;
    if (!ValidateCandidateApproval(root_, previous.contents, payload,
                                   &approval, &result.error)) {
      return result;
    }
  }
  if (payload.payload_id != result.payload_id) {
    result.error = "previous-good payload identity mismatch";
    return result;
  }
  result.version_name = payload.metadata.version_name;
  result.version_code = payload.metadata.version_code;
  result.build_id = payload.metadata.build_id;
  if (!WriteAtomic(root_, root_ / "current.json", previous.contents,
                   &result.error)) {
    return result;
  }
  return result;
}

std::string PayloadStore::StatusJson(std::string* output_error) const {
  std::string local_error;
  std::string* error = output_error != nullptr ? output_error : &local_error;
  StoreLock lock(root_, error);
  if (!lock) return {};
  const ManifestIdentity current = ReadManifest(root_ / "current.json", true);
  const ManifestIdentity previous =
      ReadManifest(root_ / "previous_good.json", true);
  if (!current.error.empty() || !previous.error.empty()) {
    *error = !current.error.empty() ? current.error : previous.error;
    return {};
  }
  nlohmann::json document;
  document["current"] =
      current.contents.empty()
          ? nlohmann::json(nullptr)
          : nlohmann::json::parse(current.contents, nullptr, false, true);
  document["previous_good"] =
      previous.contents.empty()
          ? nlohmann::json(nullptr)
          : nlohmann::json::parse(previous.contents, nullptr, false, true);
  return document.dump(2) + "\n";
}

}  // namespace mocktail::update
