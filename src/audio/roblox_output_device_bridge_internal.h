#ifndef MOCKTAIL_AUDIO_ROBLOX_OUTPUT_DEVICE_BRIDGE_INTERNAL_H_
#define MOCKTAIL_AUDIO_ROBLOX_OUTPUT_DEVICE_BRIDGE_INTERNAL_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "compat/build_profile.h"

namespace mocktail::audio::internal {

struct FmodInputCaptureProfile {
  std::string_view build_id;
  std::uintptr_t vtable_rva;
  std::uint32_t vtable_layout_version;
  // Format, latency, start, stop, poll, recording.
  std::array<std::size_t, 6> slots;
  std::array<std::uintptr_t, 6> method_rvas;
};

const FmodInputCaptureProfile* FindFmodInputCaptureProfile(
    const compat::BuildProfile& profile);

bool HasExpectedFmodInputCaptureVtable(const std::uintptr_t* vtable,
                                       std::uintptr_t image_base,
                                       const FmodInputCaptureProfile& profile);

bool HasExpectedFmodStringConstructorContract(const std::uint8_t* code,
                                              std::size_t size);

bool HasExpectedFmodOutputDeviceVtable(
    const std::uintptr_t* vtable, std::uintptr_t image_base,
    const compat::FmodOutputDeviceBridgeProfile& profile);

std::string MakeOutputDeviceGuid(std::uint32_t playback_device_id,
                                 std::string_view name);

}  // namespace mocktail::audio::internal

#endif  // MOCKTAIL_AUDIO_ROBLOX_OUTPUT_DEVICE_BRIDGE_INTERNAL_H_
