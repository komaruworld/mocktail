#include "update/payload_integrity.h"

#define JSON_NOEXCEPTION 1
#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "mocktail/sha256.h"

namespace mocktail::update {
namespace {

bool LowerHex(std::string_view value, std::size_t size) {
  return value.size() == size &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isdigit(character) ||
                  (character >= 'a' && character <= 'f');
         });
}

std::string ReadMetadata(const std::filesystem::path& path,
                         std::string* error) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    *error = "payload metadata is unavailable";
    return {};
  }
  struct stat metadata = {};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 || metadata.st_size > 1024 * 1024) {
    close(descriptor);
    *error = "payload metadata is not a bounded regular file";
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
      *error = "cannot read payload metadata";
      return {};
    }
    contents.append(buffer.data(), static_cast<std::size_t>(bytes));
  }
  close(descriptor);
  return contents;
}

// foundation::Sha256 is a portable implementation and runs at roughly 180 MB/s
// here; OpenSSL reaches the CPU's SHA extensions and measures 8x faster on the
// same bytes. A payload is several hundred MiB, so file hashing uses OpenSSL.
// The digests are identical, and foundation::Sha256 stays in use for the small
// inputs elsewhere in the updater.
class FileDigest final {
 public:
  FileDigest() : context_(EVP_MD_CTX_new()) {
    if (context_ != nullptr &&
        EVP_DigestInit_ex(context_, EVP_sha256(), nullptr) != 1) {
      EVP_MD_CTX_free(context_);
      context_ = nullptr;
    }
  }

  ~FileDigest() {
    if (context_ != nullptr) EVP_MD_CTX_free(context_);
  }

  FileDigest(const FileDigest&) = delete;
  FileDigest& operator=(const FileDigest&) = delete;

  bool valid() const { return context_ != nullptr; }

  bool Update(const unsigned char* bytes, std::size_t size) {
    return context_ != nullptr &&
           EVP_DigestUpdate(context_, bytes, size) == 1;
  }

  std::string FinalHex() {
    unsigned char digest[EVP_MAX_MD_SIZE] = {};
    unsigned int size = 0;
    if (context_ == nullptr ||
        EVP_DigestFinal_ex(context_, digest, &size) != 1 || size != 32U) {
      return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(size * 2U);
    for (unsigned int index = 0; index < size; ++index) {
      hex.push_back(kHex[digest[index] >> 4]);
      hex.push_back(kHex[digest[index] & 0x0FU]);
    }
    return hex;
  }

 private:
  EVP_MD_CTX* context_ = nullptr;
};

}  // namespace

std::string HashRegularFile(const std::filesystem::path& path,
                            std::string* output_error) {
  std::string local_error;
  std::string* error = output_error != nullptr ? output_error : &local_error;
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    *error = "cannot open regular file for hashing: " + path.string();
    return {};
  }
  struct stat metadata = {};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
    close(descriptor);
    *error = "hash input is not a regular file: " + path.string();
    return {};
  }
  FileDigest digest;
  if (!digest.valid()) {
    close(descriptor);
    *error = "cannot initialise the file digest";
    return {};
  }
  std::vector<unsigned char> buffer(256U * 1024U);
  while (true) {
    const ssize_t bytes = read(descriptor, buffer.data(), buffer.size());
    if (bytes == 0) break;
    if (bytes < 0) {
      if (errno == EINTR) continue;
      close(descriptor);
      *error = "cannot hash regular file: " + path.string();
      return {};
    }
    if (!digest.Update(buffer.data(), static_cast<std::size_t>(bytes))) {
      close(descriptor);
      *error = "cannot hash regular file: " + path.string();
      return {};
    }
  }
  close(descriptor);
  const std::string hex = digest.FinalHex();
  if (hex.empty()) *error = "cannot finalise the hash of " + path.string();
  return hex;
}

std::string HashAssetTree(const std::filesystem::path& root,
                          std::size_t* file_count, std::string* output_error) {
  std::string local_error;
  std::string* error = output_error != nullptr ? output_error : &local_error;
  std::error_code filesystem_error;
  const auto root_status =
      std::filesystem::symlink_status(root, filesystem_error);
  if (filesystem_error || !std::filesystem::is_directory(root_status) ||
      std::filesystem::is_symlink(root_status)) {
    *error = "asset root is not a real directory";
    return {};
  }
  std::vector<std::filesystem::path> relative_files;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::none, filesystem_error);
  const std::filesystem::recursive_directory_iterator end;
  while (!filesystem_error && iterator != end) {
    const auto status = iterator->symlink_status(filesystem_error);
    if (filesystem_error) break;
    if (std::filesystem::is_symlink(status)) {
      *error = "asset tree contains a symbolic link";
      return {};
    }
    if (std::filesystem::is_regular_file(status)) {
      relative_files.push_back(iterator->path().lexically_relative(root));
    } else if (!std::filesystem::is_directory(status)) {
      *error = "asset tree contains a non-regular entry";
      return {};
    }
    iterator.increment(filesystem_error);
  }
  if (filesystem_error) {
    *error = "cannot enumerate asset tree";
    return {};
  }
  std::sort(relative_files.begin(), relative_files.end(),
            [](const auto& left, const auto& right) {
              return left.generic_string() < right.generic_string();
            });
  // Each file digest is independent; only the fold below depends on the sorted
  // order, so the per-file work spreads across cores without changing the
  // result. An asset tree holds a few thousand files.
  std::vector<std::string> digests(relative_files.size());
  std::atomic<std::size_t> next_index{0};
  std::mutex error_mutex;
  std::string first_error;
  const auto hash_range = [&]() {
    while (true) {
      const std::size_t index = next_index.fetch_add(1);
      if (index >= relative_files.size()) return;
      {
        const std::lock_guard<std::mutex> lock(error_mutex);
        if (!first_error.empty()) return;
      }
      std::string local_failure;
      std::string digest =
          HashRegularFile(root / relative_files[index], &local_failure);
      if (!local_failure.empty()) {
        const std::lock_guard<std::mutex> lock(error_mutex);
        if (first_error.empty()) first_error = std::move(local_failure);
        return;
      }
      digests[index] = std::move(digest);
    }
  };
  unsigned int workers = std::thread::hardware_concurrency();
  if (workers == 0U) workers = 1U;
  workers = std::min<unsigned int>(workers, 8U);
  workers = std::min<unsigned int>(
      workers, static_cast<unsigned int>(relative_files.size()));
  std::vector<std::thread> pool;
  pool.reserve(workers > 0U ? workers - 1U : 0U);
  for (unsigned int index = 1; index < workers; ++index) {
    pool.emplace_back(hash_range);
  }
  if (workers > 0U) hash_range();
  for (std::thread& worker : pool) worker.join();
  if (!first_error.empty()) {
    *error = first_error;
    return {};
  }
  foundation::Sha256 tree;
  for (std::size_t index = 0; index < relative_files.size(); ++index) {
    tree.Update(digests[index]);
    tree.Update("  ");
    tree.Update(relative_files[index].generic_string());
    const unsigned char terminator = 0;
    tree.Update(&terminator, 1);
  }
  if (file_count != nullptr) *file_count = relative_files.size();
  return tree.FinalHex();
}

PayloadIntegrityResult InspectPreparedPayload(
    const std::filesystem::path& directory) {
  PayloadIntegrityResult result;
  std::error_code filesystem_error;
  const auto root_status =
      std::filesystem::symlink_status(directory, filesystem_error);
  if (filesystem_error || !std::filesystem::is_directory(root_status) ||
      std::filesystem::is_symlink(root_status)) {
    result.error = "payload root is not a real directory";
    return result;
  }
  for (const std::filesystem::path& relative : {
           std::filesystem::path("libroblox.so"),
           std::filesystem::path("sober_apk/base.apk"),
           std::filesystem::path("sober_apk/split_config.x86_64.apk"),
           std::filesystem::path("roblox_payload.json"),
       }) {
    const auto status =
        std::filesystem::symlink_status(directory / relative, filesystem_error);
    if (filesystem_error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
      result.error = "payload is missing regular file: " + relative.string();
      return result;
    }
  }
  for (const std::filesystem::path& relative : {
           std::filesystem::path("assets"),
           std::filesystem::path("assets/content"),
       }) {
    const auto status =
        std::filesystem::symlink_status(directory / relative, filesystem_error);
    if (filesystem_error || !std::filesystem::is_directory(status) ||
        std::filesystem::is_symlink(status)) {
      result.error = "payload is missing real directory: " + relative.string();
      return result;
    }
  }
  const std::string contents =
      ReadMetadata(directory / "roblox_payload.json", &result.error);
  if (!result.error.empty()) return result;
  const nlohmann::json document =
      nlohmann::json::parse(contents, nullptr, false, true);
  if (document.is_discarded() || !document.is_object() ||
      document.value("schema_version", 0) != 1 ||
      !document.contains("version_code") ||
      !document["version_code"].is_number_unsigned() ||
      !document.contains("sha256") || !document["sha256"].is_object() ||
      !document.contains("assets") || !document["assets"].is_object()) {
    result.error = "payload metadata schema is invalid";
    return result;
  }
  result.metadata.package_name = document.value("package", "");
  result.metadata.version_name = document.value("version_name", "");
  result.metadata.version_code = document["version_code"].get<std::uint64_t>();
  result.metadata.build_id = document.value("elf_build_id", "");
  result.metadata.library_sha256 = document["sha256"].value("libroblox", "");
  result.metadata.base_apk_sha256 = document["sha256"].value("base_apk", "");
  result.metadata.split_apk_sha256 =
      document["sha256"].value("x86_64_split_apk", "");
  if (!document["assets"].contains("file_count") ||
      !document["assets"]["file_count"].is_number_unsigned()) {
    result.error = "payload metadata asset count is invalid";
    return result;
  }
  result.metadata.asset_file_count =
      document["assets"]["file_count"].get<std::size_t>();
  result.metadata.asset_tree_sha256 =
      document["assets"].value("sha256_tree", "");
  if (result.metadata.package_name != "com.roblox.client" ||
      result.metadata.version_name.empty() ||
      result.metadata.version_code == 0 ||
      !LowerHex(result.metadata.build_id, 40) ||
      !LowerHex(result.metadata.library_sha256, 64) ||
      !LowerHex(result.metadata.base_apk_sha256, 64) ||
      !LowerHex(result.metadata.split_apk_sha256, 64) ||
      !LowerHex(result.metadata.asset_tree_sha256, 64)) {
    result.error = "payload metadata identity or hashes are invalid";
    return result;
  }
  result.payload_id = std::to_string(result.metadata.version_code) + "-" +
                      result.metadata.build_id;
  return result;
}

PayloadIntegrityResult VerifyPreparedPayload(
    const std::filesystem::path& directory) {
  PayloadIntegrityResult result = InspectPreparedPayload(directory);
  if (!result) return result;
  const std::array<std::pair<std::filesystem::path, std::string>, 3> files = {{
      {"libroblox.so", result.metadata.library_sha256},
      {"sober_apk/base.apk", result.metadata.base_apk_sha256},
      {"sober_apk/split_config.x86_64.apk", result.metadata.split_apk_sha256},
  }};
  for (const auto& [relative, expected] : files) {
    const std::string actual =
        HashRegularFile(directory / relative, &result.error);
    if (!result.error.empty()) return result;
    if (actual != expected) {
      result.error = "payload hash mismatch: " + relative.string();
      return result;
    }
  }
  std::size_t count = 0;
  const std::string tree =
      HashAssetTree(directory / "assets", &count, &result.error);
  if (!result.error.empty()) return result;
  if (count != result.metadata.asset_file_count ||
      tree != result.metadata.asset_tree_sha256) {
    result.error = "payload asset tree does not match metadata";
    return result;
  }
  return result;
}

}  // namespace mocktail::update
