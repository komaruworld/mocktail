#ifndef MOCKTAIL_COMPAT_BUILD_PROFILE_H_
#define MOCKTAIL_COMPAT_BUILD_PROFILE_H_

#include <array>

#include <cstddef>
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
  // Roblox 2.738 inserted device-list methods before the current/select slots.
  int vtable_layout_version = 1;

  // Optional exact-build input count/info/current/select methods.
  std::array<std::uintptr_t, 4> input_method_rvas{};
  bool has_input_devices() const { return input_method_rvas[0] != 0; }
  std::array<std::size_t, 4> input_vtable_indexes() const {
    return vtable_layout_version == 2
               ? std::array<std::size_t, 4>{9, 10, 12, 18}
               : std::array<std::size_t, 4>{8, 9, 10, 16};
  }
  std::array<std::size_t, 6> capture_vtable_indexes() const {
    return vtable_layout_version == 2
               ? std::array<std::size_t, 6>{4, 13, 14, 15, 16, 17}
               : std::array<std::size_t, 6>{4, 11, 12, 13, 14, 15};
  }

  bool valid_vtable_layout() const {
    return vtable_layout_version == 1 || vtable_layout_version == 2;
  }
  std::size_t current_vtable_index() const {
    return vtable_layout_version == 2 ? 8 : 7;
  }
  std::size_t select_vtable_index() const {
    return vtable_layout_version == 2 ? 19 : 17;
  }
};

// Exact-build metadata for the experimental native VR bridge. The bridge
// interposes DebugDeviceVR slots and a checked world-pointer trampoline.
// All RVAs are validated against machine-code contracts before first use.
struct VrDebugDeviceBridgeProfile {
  std::uintptr_t vtable_rva = 0;
  std::uintptr_t constructor_rva = 0;
  std::uintptr_t state_getter_rva = 0;
  std::uintptr_t eye_getter_rva = 0;
  std::uintptr_t eye_initializer_rva = 0;
  std::uintptr_t emulator_flag_storage_rva = 0;
  std::uintptr_t device_create_framebuffer_vtable_offset = 0;
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
  // Optional exact-build experimental VR capability; see the struct above.
  std::optional<VrDebugDeviceBridgeProfile> vr_debug_device_bridge;
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
