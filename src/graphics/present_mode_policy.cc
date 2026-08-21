#include "mocktail/graphics/present_mode_policy.h"

#include <algorithm>

namespace mocktail {
namespace graphics {

PresentModePolicy ResolvePresentModePolicy(std::string_view vsync_value,
                                           std::string_view frame_rate_value) {
  if (vsync_value == "on" || vsync_value == "1") {
    return PresentModePolicy::kVsync;
  }
  if (vsync_value == "off" || vsync_value == "0") {
    return PresentModePolicy::kUnthrottled;
  }
  if (frame_rate_value.empty() || frame_rate_value == "display") {
    return PresentModePolicy::kVsync;
  }
  if (frame_rate_value == "unlimited") {
    return PresentModePolicy::kUnthrottled;
  }
  return PresentModePolicy::kHostDefault;
}

std::vector<VkPresentModeKHR>
FilterPresentModes(PresentModePolicy policy,
                   const std::vector<VkPresentModeKHR> &host_modes) {
  if (policy == PresentModePolicy::kHostDefault) {
    return host_modes;
  }
  const VkPresentModeKHR preferred = policy == PresentModePolicy::kVsync
                                         ? VK_PRESENT_MODE_FIFO_KHR
                                         : VK_PRESENT_MODE_IMMEDIATE_KHR;
  if (std::find(host_modes.begin(), host_modes.end(), preferred) !=
      host_modes.end()) {
    return {preferred};
  }
  if (policy == PresentModePolicy::kUnthrottled &&
      std::find(host_modes.begin(), host_modes.end(),
                VK_PRESENT_MODE_MAILBOX_KHR) != host_modes.end()) {
    return {VK_PRESENT_MODE_MAILBOX_KHR};
  }
  return host_modes;
}

const char *PresentModePolicyName(PresentModePolicy policy) {
  switch (policy) {
  case PresentModePolicy::kHostDefault:
    return "host-default";
  case PresentModePolicy::kVsync:
    return "vsync";
  case PresentModePolicy::kUnthrottled:
    return "unthrottled";
  }
  return "unknown";
}

} // namespace graphics
} // namespace mocktail
