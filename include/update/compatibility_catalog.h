#ifndef MOCKTAIL_UPDATE_COMPATIBILITY_CATALOG_H_
#define MOCKTAIL_UPDATE_COMPATIBILITY_CATALOG_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail::update {

struct SupportedPayloadProfile {
  std::string version_name;
  std::uint64_t version_code = 0;
  std::string elf_build_id;
};

struct CompatibilityCatalogResult {
  std::vector<SupportedPayloadProfile> profiles;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

CompatibilityCatalogResult LoadCompatibilityCatalog(
    const std::filesystem::path& path);

std::optional<SupportedPayloadProfile> PreferredSupportedProfile(
    const std::vector<SupportedPayloadProfile>& profiles);

std::optional<SupportedPayloadProfile> FindSupportedProfile(
    const std::vector<SupportedPayloadProfile>& profiles,
    std::string_view version_name, std::uint64_t version_code = 0);

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_COMPATIBILITY_CATALOG_H_
