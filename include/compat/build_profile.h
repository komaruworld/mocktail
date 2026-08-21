#ifndef MOCKTAIL_COMPAT_BUILD_PROFILE_H_
#define MOCKTAIL_COMPAT_BUILD_PROFILE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mocktail::compat {

enum class BuildStatus {
  kSupported,
  kExperimental,
  kLegacyResearched,
  kUnverified,
};

struct FmodOutputDeviceBridgeProfile {
  std::uintptr_t vtable_rva = 0;
  std::uintptr_t string_constructor_rva = 0;
  std::uintptr_t count_method_rva = 0;
  std::uintptr_t info_method_rva = 0;
  std::uintptr_t current_method_rva = 0;
  std::uintptr_t select_method_rva = 0;
};

struct BuildProfile {
  std::string version_name;
  int version_code = 0;
  std::string elf_build_id;
  BuildStatus status = BuildStatus::kUnverified;
  bool default_allowed = false;
  bool allow_legacy_binary_patches = false;
  bool allow_host_abi_bridges = false;
  bool allow_host_constructor_replay = false;
  // Optional, exact-build native capability. This is an invoked entrypoint,
  // never a writable binary patch; the adapter validates its code contract
  // before the first call.
  std::optional<std::uintptr_t> user_game_settings_fullscreen_setter_rva;
  // Optional exact-build host ABI bridge. It temporarily interposes only the
  // output-device virtual slots and restores them during controlled teardown;
  // executable guest code is never modified.
  std::optional<FmodOutputDeviceBridgeProfile> fmod_output_device_bridge;
  std::string reason;
};

struct ProfileLookupResult {
  std::optional<BuildProfile> profile;
  std::string error;

  explicit operator bool() const noexcept { return error.empty(); }
};

ProfileLookupResult FindBuildProfile(const std::string& manifest_path,
                                     std::string_view build_id);

std::string_view BuildStatusName(BuildStatus status) noexcept;

}  // namespace mocktail::compat

#endif  // MOCKTAIL_COMPAT_BUILD_PROFILE_H_
