#ifndef MOCKTAIL_UPDATE_PAYLOAD_INTEGRITY_H_
#define MOCKTAIL_UPDATE_PAYLOAD_INTEGRITY_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

struct evp_md_ctx_st;

namespace mocktail::update {

class FileDigest final {
 public:
  FileDigest();
  ~FileDigest();
  FileDigest(const FileDigest&) = delete;
  FileDigest& operator=(const FileDigest&) = delete;

  bool valid() const noexcept { return context_ != nullptr; }
  bool Update(const void* data, std::size_t size);
  bool Update(std::string_view bytes);
  std::string FinalHex(std::size_t max_chars = 0);

 private:
  struct evp_md_ctx_st* context_ = nullptr;
};

std::string HashText(std::string_view text, std::string* error = nullptr);
std::string HashText(std::string_view text, std::size_t max_hex_chars,
                     std::string* error = nullptr);

struct PayloadMetadata {
  std::string package_name;
  std::string version_name;
  std::uint64_t version_code = 0;
  std::string build_id;
  std::string library_sha256;
  std::string base_apk_sha256;
  std::string split_apk_sha256;
  std::size_t asset_file_count = 0;
  std::string asset_tree_sha256;
};

struct PayloadIntegrityResult {
  PayloadMetadata metadata;
  std::string payload_id;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

std::string HashRegularFile(const std::filesystem::path& path,
                            std::string* error = nullptr);

std::string HashAssetTree(const std::filesystem::path& root,
                          std::size_t* file_count,
                          std::string* error = nullptr);

PayloadIntegrityResult VerifyPreparedPayload(
    const std::filesystem::path& directory);

// Reads and validates the immutable payload layout and metadata without
// rehashing the archived APKs and complete asset tree. Callers that stage or
// promote new bytes must use VerifyPreparedPayload instead.
PayloadIntegrityResult InspectPreparedPayload(
    const std::filesystem::path& directory);

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_PAYLOAD_INTEGRITY_H_
