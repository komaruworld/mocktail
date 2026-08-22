#ifndef MOCKTAIL_UPDATE_PAYLOAD_STORE_H_
#define MOCKTAIL_UPDATE_PAYLOAD_STORE_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace mocktail::update {

struct PayloadStoreResult {
  std::string payload_id;
  std::filesystem::path payload_directory;
  std::filesystem::path host_abi_profile;
  std::filesystem::path compatibility_manifest;
  std::filesystem::path approval_receipt;
  std::string version_name;
  std::uint64_t version_code = 0;
  std::string build_id;
  std::string error;

  explicit operator bool() const {
    return error.empty() && !payload_id.empty();
  }
};

class PayloadStore final {
 public:
  PayloadStore(std::filesystem::path root,
               std::filesystem::path compatibility_manifest,
               std::filesystem::path runtime_binary = {});

  PayloadStoreResult Stage(const std::filesystem::path& prepared_payload);
  PayloadStoreResult Promote(std::string_view payload_id);
  PayloadStoreResult PromoteProbation(
      std::string_view payload_id,
      const std::filesystem::path& candidate_profile,
      const std::filesystem::path& candidate_compatibility,
      const std::array<std::filesystem::path, 2>& canary_logs);
  PayloadStoreResult Import(const std::filesystem::path& prepared_payload);
  PayloadStoreResult InspectCurrent() const;
  PayloadStoreResult VerifyCurrent();
  PayloadStoreResult Rollback();
  std::string StatusJson(std::string* error = nullptr) const;

 private:
  std::filesystem::path root_;
  std::filesystem::path compatibility_manifest_;
  std::filesystem::path runtime_binary_;
};

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_PAYLOAD_STORE_H_
