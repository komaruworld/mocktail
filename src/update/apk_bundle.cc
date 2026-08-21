#include "update/apk_bundle.h"

#define JSON_NOEXCEPTION 1
#include <elf.h>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "compat/elf_build_id.h"
#include "update/android_manifest.h"
#include "update/apk_signature.h"
#include "update/payload_integrity.h"
#include "update/zip_archive.h"

namespace mocktail::update {
namespace {

constexpr std::size_t kMaximumApkBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumManifestBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumAssetBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumCandidates = 64;

constexpr std::array<std::string_view, 7> kRequiredExports = {
    "JNI_OnLoad",
    "Java_com_roblox_engine_jni_NativeGLInterface_nativeGameGlobalInit",
    "Java_com_roblox_engine_jni_NativeGLInterface_nativeUpdateAdapterInit",
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2InitWithParams",
    "Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeStartLuaAppDM",
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2StartAppWithParams",
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams",
};

struct ElfCloser {
  void operator()(Elf* value) const {
    if (value != nullptr) elf_end(value);
  }
};

std::string ReadRegular(const std::filesystem::path& path, std::size_t maximum,
                        std::string* error) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    *error = "cannot open regular file: " + path.string();
    return {};
  }
  struct stat metadata = {};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 ||
      static_cast<std::uintmax_t>(metadata.st_size) > maximum) {
    close(descriptor);
    *error = "file exceeds its size limit: " + path.string();
    return {};
  }
  std::string contents;
  contents.reserve(static_cast<std::size_t>(metadata.st_size));
  std::array<char, 64U * 1024U> buffer{};
  while (true) {
    const ssize_t bytes = read(descriptor, buffer.data(), buffer.size());
    if (bytes == 0) break;
    if (bytes < 0) {
      if (errno == EINTR) continue;
      close(descriptor);
      *error = "cannot read regular file: " + path.string();
      return {};
    }
    contents.append(buffer.data(), static_cast<std::size_t>(bytes));
  }
  close(descriptor);
  return contents;
}

bool WritePrivateFile(const std::filesystem::path& path,
                      std::string_view contents, std::string* error) {
  const int descriptor = open(
      path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor < 0) {
    *error = "cannot create payload metadata";
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t bytes =
        write(descriptor, contents.data() + offset, contents.size() - offset);
    if (bytes < 0) {
      if (errno == EINTR) continue;
      close(descriptor);
      *error = "cannot write payload metadata";
      return false;
    }
    offset += static_cast<std::size_t>(bytes);
  }
  const bool synced = fsync(descriptor) == 0;
  close(descriptor);
  if (!synced) *error = "cannot persist payload metadata";
  return synced;
}

std::string UtcTimestamp() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  if (gmtime_r(&now, &utc) == nullptr) return "1970-01-01T00:00:00Z";
  std::array<char, 32> value{};
  std::strftime(value.data(), value.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return value.data();
}

bool CopyRegular(const std::filesystem::path& source,
                 const std::filesystem::path& destination, std::string* error) {
  std::error_code filesystem_error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(source, filesystem_error);
  if (filesystem_error || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    *error = "APK candidate is not a regular file";
    return false;
  }
  const std::uintmax_t size =
      std::filesystem::file_size(source, filesystem_error);
  if (filesystem_error || size == 0 || size > kMaximumApkBytes) {
    *error = "APK candidate exceeds its size limit";
    return false;
  }
  // Provider downloads, candidate inspection, and prepared output all live in
  // one private workspace. Reuse the immutable bytes when the filesystem
  // supports hard links; this avoids several hundred MiB of transient copies.
  std::filesystem::create_hard_link(source, destination, filesystem_error);
  if (!filesystem_error) return true;
  filesystem_error.clear();
  std::filesystem::copy_file(source, destination,
                             std::filesystem::copy_options::none,
                             filesystem_error);
  if (filesystem_error) {
    *error = "cannot snapshot APK candidate";
    return false;
  }
  return true;
}

bool HasEntry(const ZipListResult& list, std::string_view name) {
  return std::any_of(list.entries.begin(), list.entries.end(),
                     [&](const ZipEntry& entry) { return entry.name == name; });
}

bool EndsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

struct ApkCandidate {
  std::filesystem::path path;
  AndroidManifestIdentity identity;
  bool has_x86_64_library = false;
  bool has_assets = false;
};

bool CollectCandidates(const std::vector<std::filesystem::path>& archives,
                       const std::filesystem::path& directory,
                       std::vector<std::filesystem::path>* candidates,
                       std::string* error) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(directory, filesystem_error);
  if (filesystem_error) {
    *error = "cannot create APK candidate directory";
    return false;
  }
  std::size_t candidate_number = 0;
  for (const std::filesystem::path& archive : archives) {
    const ZipListResult list = ListZipEntries(archive);
    if (!list) {
      *error = list.error;
      return false;
    }
    if (HasEntry(list, "AndroidManifest.xml")) {
      if (++candidate_number > kMaximumCandidates) {
        *error = "provider produced too many APK candidates";
        return false;
      }
      const std::filesystem::path destination =
          directory /
          ("candidate-" + std::to_string(candidate_number) + ".apk");
      if (!CopyRegular(archive, destination, error)) return false;
      candidates->push_back(destination);
      continue;
    }
    for (const ZipEntry& entry : list.entries) {
      if (entry.directory || !EndsWith(entry.name, ".apk")) continue;
      if (++candidate_number > kMaximumCandidates ||
          entry.uncompressed_size > kMaximumApkBytes) {
        *error = "provider bundle has excessive APK candidates";
        return false;
      }
      const std::filesystem::path destination =
          directory /
          ("candidate-" + std::to_string(candidate_number) + ".apk");
      if (!ExtractZipEntry(archive, entry.name, destination, kMaximumApkBytes,
                           error)) {
        return false;
      }
      candidates->push_back(destination);
    }
  }
  if (candidates->empty()) {
    *error = "provider output contains no APK candidates";
    return false;
  }
  return true;
}

bool InspectCandidates(const std::vector<std::filesystem::path>& paths,
                       const ExpectedPayloadIdentity& expected,
                       std::vector<ApkCandidate>* candidates,
                       std::string* error) {
  for (const std::filesystem::path& path : paths) {
    const ZipListResult list = ListZipEntries(path);
    if (!list) continue;
    const ZipReadResult manifest =
        ReadZipEntry(path, "AndroidManifest.xml", kMaximumManifestBytes);
    if (!manifest) continue;
    AndroidManifestIdentity identity = ParseAndroidManifest(manifest.bytes);
    if (!identity || identity.package_name != "com.roblox.client" ||
        identity.version_code != expected.version_code ||
        (identity.version_name != expected.version_name &&
         !(identity.version_name.empty() && !identity.split_name.empty()))) {
      continue;
    }
    ApkCandidate candidate;
    candidate.path = path;
    candidate.identity = std::move(identity);
    candidate.has_x86_64_library = HasEntry(list, "lib/x86_64/libroblox.so");
    candidate.has_assets = std::any_of(
        list.entries.begin(), list.entries.end(), [](const ZipEntry& entry) {
          return entry.name.size() > 7 && entry.name.substr(0, 7) == "assets/";
        });
    candidates->push_back(std::move(candidate));
  }
  if (candidates->empty()) {
    *error = "no APK matches the requested Roblox version";
    return false;
  }
  return true;
}

std::set<std::string> TrustedCertificates(const std::filesystem::path& path,
                                          std::string* error) {
  const std::string contents = ReadRegular(path, 1024U * 1024U, error);
  if (!error->empty()) return {};
  const nlohmann::json document =
      nlohmann::json::parse(contents, nullptr, false, true);
  if (document.is_discarded() || !document.is_object() ||
      document.value("schema_version", 0) != 1 ||
      document.value("package", "") != "com.roblox.client" ||
      !document.contains("trusted_sha256") ||
      !document["trusted_sha256"].is_array()) {
    *error = "Roblox signing trust manifest is invalid";
    return {};
  }
  std::set<std::string> trusted;
  for (const auto& value : document["trusted_sha256"]) {
    if (!value.is_string()) {
      *error = "Roblox signing trust manifest contains a non-string digest";
      return {};
    }
    std::string digest = value.get<std::string>();
    std::transform(digest.begin(), digest.end(), digest.begin(),
                   [](unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
    if (digest.size() != 64 ||
        !std::all_of(digest.begin(), digest.end(), [](unsigned char character) {
          return std::isdigit(character) ||
                 (character >= 'a' && character <= 'f');
        })) {
      *error = "Roblox signing trust manifest contains an invalid digest";
      return {};
    }
    trusted.insert(std::move(digest));
  }
  if (trusted.empty()) *error = "Roblox signing trust manifest is empty";
  return trusted;
}

std::vector<std::string> CommonTrustedCertificates(
    const std::filesystem::path& base, const std::filesystem::path& split,
    const std::set<std::string>& trusted, std::string* error) {
  const ApkSignatureResult base_signature = VerifyApkSignature(base);
  if (!base_signature) {
    *error = "base APK signature: " + base_signature.error;
    return {};
  }
  const ApkSignatureResult split_signature =
      base == split ? base_signature : VerifyApkSignature(split);
  if (!split_signature) {
    *error = "x86_64 APK signature: " + split_signature.error;
    return {};
  }
  std::vector<std::string> accepted;
  for (const std::string& certificate : base_signature.certificate_sha256) {
    if (trusted.find(certificate) != trusted.end() &&
        std::find(split_signature.certificate_sha256.begin(),
                  split_signature.certificate_sha256.end(),
                  certificate) != split_signature.certificate_sha256.end()) {
      accepted.push_back(certificate);
    }
  }
  if (accepted.empty()) {
    *error = "APK pair has no common trusted Roblox signing certificate";
  }
  return accepted;
}

bool ValidateElf(const std::filesystem::path& path, std::string* build_id,
                 std::string* error) {
  if (elf_version(EV_CURRENT) == EV_NONE) {
    *error = "libelf cannot initialise";
    return false;
  }
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    *error = "cannot open extracted libroblox.so";
    return false;
  }
  std::unique_ptr<Elf, ElfCloser> elf(
      elf_begin(descriptor, ELF_C_READ, nullptr));
  const auto close_elf = [&]() {
    elf.reset();
    close(descriptor);
  };
  if (!elf || elf_kind(elf.get()) != ELF_K_ELF) {
    close_elf();
    *error = "extracted libroblox.so is not ELF";
    return false;
  }
  GElf_Ehdr header{};
  if (gelf_getehdr(elf.get(), &header) == nullptr ||
      header.e_machine != EM_X86_64 || gelf_getclass(elf.get()) != ELFCLASS64) {
    close_elf();
    *error = "libroblox.so is not an x86-64 ELF";
    return false;
  }
  std::size_t section_count = 0;
  std::size_t string_index = 0;
  if (elf_getshdrnum(elf.get(), &section_count) != 0 ||
      elf_getshdrstrndx(elf.get(), &string_index) != 0) {
    close_elf();
    *error = "cannot inspect libroblox.so sections";
    return false;
  }
  bool android_note = false;
  std::set<std::string> exports;
  for (std::size_t index = 0; index < section_count; ++index) {
    Elf_Scn* section = elf_getscn(elf.get(), index);
    GElf_Shdr section_header{};
    if (section == nullptr ||
        gelf_getshdr(section, &section_header) == nullptr) {
      close_elf();
      *error = "cannot inspect libroblox.so section";
      return false;
    }
    const char* name =
        elf_strptr(elf.get(), string_index, section_header.sh_name);
    android_note =
        android_note ||
        (name != nullptr && std::string_view(name) == ".note.android.ident");
    if (section_header.sh_type != SHT_DYNSYM ||
        section_header.sh_entsize == 0) {
      continue;
    }
    Elf_Data* data = elf_getdata(section, nullptr);
    if (data == nullptr) continue;
    const std::size_t symbol_count =
        section_header.sh_size / section_header.sh_entsize;
    for (std::size_t symbol_index = 0; symbol_index < symbol_count;
         ++symbol_index) {
      GElf_Sym symbol{};
      if (gelf_getsym(data, symbol_index, &symbol) == nullptr ||
          symbol.st_name == 0 || symbol.st_shndx == SHN_UNDEF) {
        continue;
      }
      const char* symbol_name =
          elf_strptr(elf.get(), section_header.sh_link, symbol.st_name);
      if (symbol_name != nullptr) exports.insert(symbol_name);
    }
  }
  close_elf();
  if (!android_note) {
    *error = "libroblox.so has no Android identity note";
    return false;
  }
  for (const std::string_view required : kRequiredExports) {
    if (exports.find(std::string(required)) == exports.end()) {
      *error =
          "libroblox.so is missing required export: " + std::string(required);
      return false;
    }
  }
  const compat::BuildIdResult identity = compat::ReadElfBuildId(path.string());
  if (!identity || identity.build_id.size() != 40) {
    *error =
        identity ? "libroblox.so Build ID is not 20 bytes" : identity.error;
    return false;
  }
  *build_id = identity.build_id;
  return true;
}

}  // namespace

PreparedPayload PreparePayloadFromArchives(
    const std::vector<std::filesystem::path>& archives,
    const ExpectedPayloadIdentity& expected,
    const std::filesystem::path& signing_trust_manifest,
    const std::filesystem::path& workspace, std::string_view source) {
  PreparedPayload result;
  if (archives.empty()) {
    result.error = "provider returned no archives";
    return result;
  }
  std::error_code filesystem_error;
  std::filesystem::create_directories(workspace, filesystem_error);
  if (filesystem_error ||
      !std::filesystem::is_empty(workspace, filesystem_error)) {
    result.error = "payload preparation workspace must be empty";
    return result;
  }
  std::vector<std::filesystem::path> candidate_paths;
  if (!CollectCandidates(archives, workspace / "candidates", &candidate_paths,
                         &result.error)) {
    return result;
  }
  std::vector<ApkCandidate> candidates;
  if (!InspectCandidates(candidate_paths, expected, &candidates,
                         &result.error)) {
    return result;
  }
  const ApkCandidate* base = nullptr;
  const ApkCandidate* split = nullptr;
  for (const ApkCandidate& candidate : candidates) {
    if (candidate.identity.split_name.empty() && candidate.has_assets &&
        base == nullptr) {
      base = &candidate;
    }
  }
  for (const ApkCandidate& candidate : candidates) {
    if (candidate.has_x86_64_library &&
        (candidate.identity.split_name == "config.x86_64" ||
         candidate.identity.split_name.empty()) &&
        split == nullptr) {
      split = &candidate;
    }
  }
  if (base == nullptr || split == nullptr) {
    result.error = "no matching Roblox base and x86_64 APK pair was found";
    return result;
  }
  const std::set<std::string> trusted =
      TrustedCertificates(signing_trust_manifest, &result.error);
  if (!result.error.empty()) return result;
  const std::vector<std::string> accepted = CommonTrustedCertificates(
      base->path, split->path, trusted, &result.error);
  if (!result.error.empty()) return result;

  const std::filesystem::path prepared = workspace / "prepared";
  std::filesystem::create_directories(prepared / "sober_apk", filesystem_error);
  if (filesystem_error ||
      !CopyRegular(base->path, prepared / "sober_apk/base.apk",
                   &result.error) ||
      !CopyRegular(split->path, prepared / "sober_apk/split_config.x86_64.apk",
                   &result.error)) {
    if (result.error.empty()) result.error = "cannot create prepared payload";
    return result;
  }
  if (!ExtractZipEntry(split->path, "lib/x86_64/libroblox.so",
                       prepared / "libroblox.so", kMaximumApkBytes,
                       &result.error)) {
    return result;
  }
  std::size_t asset_count = 0;
  if (!ExtractZipPrefix(base->path, "assets/", prepared / "assets",
                        kMaximumAssetBytes, &asset_count, &result.error) ||
      asset_count == 0) {
    if (result.error.empty()) result.error = "base APK contains no assets";
    return result;
  }
  std::string build_id;
  if (!ValidateElf(prepared / "libroblox.so", &build_id, &result.error)) {
    return result;
  }
  if (expected.exact_build_id.has_value() &&
      build_id != *expected.exact_build_id) {
    result.error = "downloaded Roblox Build ID is not exact-supported";
    return result;
  }
  std::string hash_error;
  std::size_t asset_count_verified = 0;
  const std::string asset_tree_hash =
      HashAssetTree(prepared / "assets", &asset_count_verified, &hash_error);
  if (!hash_error.empty()) {
    result.error = hash_error;
    return result;
  }
  const std::string library_hash =
      HashRegularFile(prepared / "libroblox.so", &hash_error);
  const std::string base_hash =
      HashRegularFile(prepared / "sober_apk/base.apk", &hash_error);
  const std::string split_hash = HashRegularFile(
      prepared / "sober_apk/split_config.x86_64.apk", &hash_error);
  if (!hash_error.empty()) {
    result.error = hash_error;
    return result;
  }
  nlohmann::json metadata = {
      {"schema_version", 1},
      {"package", "com.roblox.client"},
      {"version_name", expected.version_name},
      {"version_code", expected.version_code},
      {"abi", "x86_64"},
      {"elf_build_id", build_id},
      {"sha256",
       {{"libroblox", library_hash},
        {"base_apk", base_hash},
        {"x86_64_split_apk", split_hash}}},
      {"signing_certificates_sha256", accepted},
      {"assets",
       {{"file_count", asset_count_verified},
        {"sha256_tree", asset_tree_hash}}},
      {"source", std::string(source)},
      {"imported_at", UtcTimestamp()},
      {"compatibility_status", expected.exact_build_id.has_value()
                                   ? "exact-supported"
                                   : "verified-candidate"},
      {"apk_layout", base->path == split->path ? "monolithic" : "split"},
  };
  if (!WritePrivateFile(prepared / "roblox_payload.json",
                        metadata.dump(2) + "\n", &result.error)) {
    return result;
  }
  const PayloadIntegrityResult verified = VerifyPreparedPayload(prepared);
  if (!verified || verified.payload_id !=
                       std::to_string(expected.version_code) + "-" + build_id) {
    result.error =
        verified ? "prepared payload identity changed" : verified.error;
    return result;
  }
  result.directory = prepared;
  result.payload_id = std::to_string(expected.version_code) + "-" + build_id;
  result.version_name = expected.version_name;
  result.version_code = expected.version_code;
  result.build_id = build_id;
  return result;
}

ExpectedPayloadIdentity ExactPayloadIdentity(
    const SupportedPayloadProfile& profile) {
  return {profile.version_name, profile.version_code, profile.elf_build_id};
}

}  // namespace mocktail::update
