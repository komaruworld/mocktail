#include "window/video_driver_policy.h"

namespace mocktail {
namespace window {

VideoDriverChoice ResolveVideoDriverChoice(
    const VideoDriverPolicyInput& input) {
  if (input.has_explicit_sdl_driver) {
    return VideoDriverChoice::kSdlDefault;
  }
  if (input.force_wayland) {
    return VideoDriverChoice::kWayland;
  }
  if (input.force_x11 && input.has_x11_display) {
    return VideoDriverChoice::kX11;
  }
  if (input.uses_direct_vulkan && input.has_nvidia_kernel_driver &&
      input.has_wayland_session && input.has_x11_display) {
    return VideoDriverChoice::kNvidiaDirectVulkanX11;
  }
  if (input.prefer_wayland && input.has_wayland_session) {
    return VideoDriverChoice::kWayland;
  }
  return VideoDriverChoice::kSdlDefault;
}

const char* VideoDriverChoiceName(VideoDriverChoice choice) {
  switch (choice) {
    case VideoDriverChoice::kWayland:
      return "wayland";
    case VideoDriverChoice::kX11:
    case VideoDriverChoice::kNvidiaDirectVulkanX11:
      return "x11";
    case VideoDriverChoice::kSdlDefault:
      return nullptr;
  }
  return nullptr;
}

}  // namespace window
}  // namespace mocktail
