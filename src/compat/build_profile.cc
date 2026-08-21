#include "compat/build_profile.h"

#include "compat/elf_build_id.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mocktail::compat {
namespace {

using Json = nlohmann::json;

ProfileLookupResult Failure(std::string error) {
  return ProfileLookupResult{std::nullopt, std::move(error)};
}

std::string NormalizeBuildId(std::string_view build_id) {
  std::string normalized(build_id);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return normalized;
}

std::optional<BuildStatus> ParseStatus(std::string_view status) {
  if (status == "supported") {
    return BuildStatus::kSupported;
  }
  if (status == "experimental") {
    return BuildStatus::kExperimental;
  }
  if (status == "legacy-researched") {
    return BuildStatus::kLegacyResearched;
  }
  if (status == "unverified") {
    return BuildStatus::kUnverified;
  }
  return std::nullopt;
}

const Json* RequiredField(const Json& object, const char* name) {
  const auto field = object.find(name);
  return field != object.end() ? &*field : nullptr;
}

bool ParseOptionalRva(const Json& object, const char* name,
                      std::optional<std::uintptr_t>* rva) {
  if (rva == nullptr) {
    return false;
  }
  rva->reset();
  const auto field = object.find(name);
  if (field == object.end() || field->is_null()) {
    return true;
  }
  if (!field->is_string()) {
    return false;
  }
  const std::string& encoded = field->get_ref<const std::string&>();
  if (encoded.size() <= 2 || encoded[0] != '0' ||
      (encoded[1] != 'x' && encoded[1] != 'X')) {
    return false;
  }
  std::uint64_t parsed = 0;
  const char* begin = encoded.data() + 2;
  const char* end = encoded.data() + encoded.size();
  const std::from_chars_result result =
      std::from_chars(begin, end, parsed, 16);
  if (result.ec != std::errc() || result.ptr != end || parsed == 0 ||
      parsed > std::numeric_limits<std::uintptr_t>::max()) {
    return false;
  }
  *rva = static_cast<std::uintptr_t>(parsed);
  return true;
}

bool ParseOptionalFmodOutputDeviceBridge(
    const Json& object,
    std::optional<FmodOutputDeviceBridgeProfile>* bridge_profile) {
  if (bridge_profile == nullptr) {
    return false;
  }
  bridge_profile->reset();
  const auto field = object.find("fmod_output_device_bridge");
  if (field == object.end() || field->is_null()) {
    return true;
  }
  if (!field->is_object()) {
    return false;
  }

  std::optional<std::uintptr_t> vtable_rva;
  std::optional<std::uintptr_t> string_constructor_rva;
  std::optional<std::uintptr_t> count_method_rva;
  std::optional<std::uintptr_t> info_method_rva;
  std::optional<std::uintptr_t> current_method_rva;
  std::optional<std::uintptr_t> select_method_rva;
  if (!ParseOptionalRva(*field, "vtable_rva", &vtable_rva) ||
      !ParseOptionalRva(*field, "string_constructor_rva",
                        &string_constructor_rva) ||
      !ParseOptionalRva(*field, "count_method_rva", &count_method_rva) ||
      !ParseOptionalRva(*field, "info_method_rva", &info_method_rva) ||
      !ParseOptionalRva(*field, "current_method_rva", &current_method_rva) ||
      !ParseOptionalRva(*field, "select_method_rva", &select_method_rva) ||
      !vtable_rva.has_value() || !string_constructor_rva.has_value() ||
      !count_method_rva.has_value() || !info_method_rva.has_value() ||
      !current_method_rva.has_value() || !select_method_rva.has_value()) {
    return false;
  }

  *bridge_profile = FmodOutputDeviceBridgeProfile{
      *vtable_rva,      *string_constructor_rva, *count_method_rva,
      *info_method_rva, *current_method_rva,     *select_method_rva,
  };
  return true;
}

}  // namespace

std::string_view BuildStatusName(BuildStatus status) noexcept {
  switch (status) {
    case BuildStatus::kSupported:
      return "supported";
    case BuildStatus::kExperimental:
      return "experimental";
    case BuildStatus::kLegacyResearched:
      return "legacy-researched";
    case BuildStatus::kUnverified:
      return "unverified";
  }
  return "unverified";
}

ProfileLookupResult FindBuildProfile(const std::string& manifest_path,
                                     std::string_view build_id) {
  if (!IsValidBuildId(build_id)) {
    return Failure("invalid ELF Build ID: " + std::string(build_id));
  }

  std::ifstream input(manifest_path);
  if (!input) {
    return Failure("cannot open compatibility manifest: " + manifest_path);
  }

  const Json root = Json::parse(input, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return Failure("invalid compatibility manifest JSON: " + manifest_path);
  }

  const Json* schema_version = RequiredField(root, "schema_version");
  const Json* profiles = RequiredField(root, "profiles");
  if (schema_version == nullptr || !schema_version->is_number_integer() ||
      schema_version->get<int>() != 1 || profiles == nullptr ||
      !profiles->is_array()) {
    return Failure("unsupported compatibility manifest schema: " +
                   manifest_path);
  }

  const std::string normalized_requested = NormalizeBuildId(build_id);
  std::optional<BuildProfile> matched_profile;

  for (const Json& entry : *profiles) {
    if (!entry.is_object()) {
      return Failure("compatibility profile is not an object");
    }

    const Json* version_name = RequiredField(entry, "version_name");
    const Json* version_code = RequiredField(entry, "version_code");
    const Json* profile_build_id = RequiredField(entry, "elf_build_id");
    const Json* status = RequiredField(entry, "status");
    const Json* default_allowed = RequiredField(entry, "default_allowed");
    const Json* allow_legacy_binary_patches =
        RequiredField(entry, "allow_legacy_binary_patches");
    const Json* allow_host_abi_bridges =
        RequiredField(entry, "allow_host_abi_bridges");
    const Json* allow_host_constructor_replay =
        RequiredField(entry, "allow_host_constructor_replay");
    const Json* reason = RequiredField(entry, "reason");
    std::optional<std::uintptr_t> fullscreen_setter_rva;
    std::optional<FmodOutputDeviceBridgeProfile> fmod_output_device_bridge;
    if (version_name == nullptr || !version_name->is_string() ||
        version_code == nullptr || !version_code->is_number_integer() ||
        profile_build_id == nullptr || !profile_build_id->is_string() ||
        status == nullptr || !status->is_string() ||
        default_allowed == nullptr || !default_allowed->is_boolean() ||
        allow_legacy_binary_patches == nullptr ||
        !allow_legacy_binary_patches->is_boolean() ||
        allow_host_abi_bridges == nullptr ||
        !allow_host_abi_bridges->is_boolean() ||
        allow_host_constructor_replay == nullptr ||
        !allow_host_constructor_replay->is_boolean() ||
        reason == nullptr || !reason->is_string() ||
        !ParseOptionalRva(entry,
                          "user_game_settings_fullscreen_setter_rva",
                          &fullscreen_setter_rva) ||
        !ParseOptionalFmodOutputDeviceBridge(
            entry, &fmod_output_device_bridge)) {
      return Failure("compatibility profile has missing or invalid fields");
    }

    const std::string normalized_profile =
        NormalizeBuildId(profile_build_id->get_ref<const std::string&>());
    if (!IsValidBuildId(normalized_profile)) {
      return Failure("compatibility profile contains an invalid Build ID");
    }

    const auto parsed_status =
        ParseStatus(status->get_ref<const std::string&>());
    if (!parsed_status.has_value()) {
      return Failure("compatibility profile contains an unknown status");
    }

    if (normalized_profile != normalized_requested) {
      continue;
    }
    if (matched_profile.has_value()) {
      return Failure("duplicate compatibility profile for Build ID " +
                     normalized_requested);
    }

    matched_profile = BuildProfile{
        version_name->get_ref<const std::string&>(),
        version_code->get<int>(),
        normalized_profile,
        *parsed_status,
        default_allowed->get<bool>(),
        allow_legacy_binary_patches->get<bool>(),
        allow_host_abi_bridges->get<bool>(),
        allow_host_constructor_replay->get<bool>(),
        fullscreen_setter_rva,
        fmod_output_device_bridge,
        reason->get_ref<const std::string&>(),
    };
  }

  return ProfileLookupResult{std::move(matched_profile), {}};
}

}  // namespace mocktail::compat
