#include "update/compatibility_catalog.h"

#define JSON_NOEXCEPTION 1
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <nlohmann/json.hpp>
#include <string>

namespace mocktail::update {
namespace {

constexpr std::size_t kMaximumManifestBytes = 4U * 1024U * 1024U;

bool LowerBuildId(std::string_view value) {
  if (value.size() != 40) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isdigit(character) || (character >= 'a' && character <= 'f');
  });
}

bool ReadManifest(const std::filesystem::path& path, std::string* contents,
                  std::string* error) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    *error = "compatibility manifest is unavailable: " + path.string();
    return false;
  }
  struct stat metadata = {};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 ||
      static_cast<std::uintmax_t>(metadata.st_size) > kMaximumManifestBytes) {
    close(descriptor);
    *error = "compatibility manifest is not a bounded regular file";
    return false;
  }
  contents->clear();
  std::array<char, 16U * 1024U> buffer{};
  while (true) {
    const ssize_t bytes = read(descriptor, buffer.data(), buffer.size());
    if (bytes == 0) break;
    if (bytes < 0) {
      if (errno == EINTR) continue;
      close(descriptor);
      *error = "cannot read compatibility manifest";
      return false;
    }
    contents->append(buffer.data(), static_cast<std::size_t>(bytes));
  }
  close(descriptor);
  return true;
}

}  // namespace

CompatibilityCatalogResult LoadCompatibilityCatalog(
    const std::filesystem::path& path) {
  CompatibilityCatalogResult result;
  std::string contents;
  if (!ReadManifest(path, &contents, &result.error)) {
    return result;
  }
  const nlohmann::json document =
      nlohmann::json::parse(contents, nullptr, false, true);
  if (document.is_discarded() || !document.is_object() ||
      document.value("schema_version", 0) != 1 ||
      !document.contains("profiles") || !document["profiles"].is_array()) {
    result.error = "compatibility manifest has an unsupported schema";
    return result;
  }
  for (const nlohmann::json& profile : document["profiles"]) {
    if (!profile.is_object() || profile.value("status", "") != "supported" ||
        !profile.value("default_allowed", false) ||
        profile.value("allow_legacy_binary_patches", true)) {
      continue;
    }
    if (!profile.contains("version_name") ||
        !profile["version_name"].is_string() ||
        !profile.contains("version_code") ||
        !profile["version_code"].is_number_unsigned() ||
        !profile.contains("elf_build_id") ||
        !profile["elf_build_id"].is_string()) {
      result.error = "supported compatibility profile is incomplete";
      return result;
    }
    SupportedPayloadProfile supported;
    supported.version_name = profile["version_name"].get<std::string>();
    supported.version_code = profile["version_code"].get<std::uint64_t>();
    supported.elf_build_id = profile["elf_build_id"].get<std::string>();
    std::transform(supported.elf_build_id.begin(), supported.elf_build_id.end(),
                   supported.elf_build_id.begin(), [](unsigned char value) {
                     return static_cast<char>(std::tolower(value));
                   });
    if (supported.version_name.empty() || supported.version_code == 0 ||
        !LowerBuildId(supported.elf_build_id)) {
      result.error = "supported compatibility profile has invalid identity";
      return result;
    }
    result.profiles.push_back(std::move(supported));
  }
  if (result.profiles.empty()) {
    result.error = "compatibility manifest has no default-supported payload";
  }
  return result;
}

std::optional<SupportedPayloadProfile> PreferredSupportedProfile(
    const std::vector<SupportedPayloadProfile>& profiles) {
  if (profiles.empty()) return std::nullopt;
  return *std::max_element(profiles.begin(), profiles.end(),
                           [](const auto& left, const auto& right) {
                             return left.version_code < right.version_code;
                           });
}

std::optional<SupportedPayloadProfile> FindSupportedProfile(
    const std::vector<SupportedPayloadProfile>& profiles,
    std::string_view version_name, std::uint64_t version_code) {
  const auto found =
      std::find_if(profiles.begin(), profiles.end(), [&](const auto& profile) {
        return profile.version_name == version_name &&
               (version_code == 0 || profile.version_code == version_code);
      });
  return found == profiles.end()
             ? std::nullopt
             : std::optional<SupportedPayloadProfile>(*found);
}

}  // namespace mocktail::update
