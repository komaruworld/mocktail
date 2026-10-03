#include "window/video_driver_policy.h"

#include <algorithm>
#include <system_error>
#include <unistd.h>

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

bool HasNvidiaKernelDriver(const std::filesystem::path& proc_version,
                          const std::filesystem::path& pci_driver) {
  if (access(proc_version.c_str(), R_OK) == 0) {
    return true;
  }

  std::error_code error;
  return std::filesystem::is_directory(pci_driver, error);
}

bool HasAvailableVideoDriverCandidate(
    std::string_view requested,
    const std::vector<std::string_view>& available_drivers) {
  std::size_t begin = 0;
  while (begin <= requested.size()) {
    const std::size_t end = requested.find(',', begin);
    const std::string_view candidate = requested.substr(
        begin, end == requested.npos ? requested.npos : end - begin);
    if (!candidate.empty() &&
        std::find(available_drivers.begin(), available_drivers.end(),
                  candidate) != available_drivers.end()) {
      return true;
    }
    if (end == requested.npos) {
      break;
    }
    begin = end + 1;
  }
  return false;
}

}  // namespace window
}  // namespace mocktail
