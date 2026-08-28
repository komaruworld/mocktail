#include "mocktail/graphics/present_mode_policy.h"

#include <algorithm>
#include <cstdlib>

namespace mocktail {
namespace graphics {
namespace {

std::string_view EnvironmentOr(const char *name, const char *fallback) {
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  return value;
}

} // namespace

PresentModePolicy ResolvePresentModePolicy(std::string_view vsync_value,
                                           std::string_view frame_rate_value) {
  if (vsync_value == "on" || vsync_value == "1") {
    return PresentModePolicy::kVsync;
  }
  if (vsync_value == "off" || vsync_value == "0") {
    return PresentModePolicy::kUnthrottled;
  }
  if (frame_rate_value == "unlimited") {
    return PresentModePolicy::kUnthrottled;
  }
  // display, 30, 60, 120, 144, 240: cap the scheduler, keep vsync present.
  return PresentModePolicy::kVsync;
}

PresentModePolicy CachedPresentModePolicy() {
  static const PresentModePolicy policy = [] {
    return ResolvePresentModePolicy(
        EnvironmentOr("MOCKTAIL_VSYNC", "auto"),
        EnvironmentOr("MOCKTAIL_FRAME_RATE_LIMIT", "display"));
  }();
  return policy;
}

std::vector<VkPresentModeKHR>
FilterPresentModes(PresentModePolicy policy,
                   const std::vector<VkPresentModeKHR> &host_modes) {
  if (policy == PresentModePolicy::kHostDefault || host_modes.empty()) {
    return host_modes;
  }
  // Late frames: FIFO waits for the next vblank. FIFO_LATEST_READY and
  // MAILBOX present the newest image instead. FIFO_RELAXED only tears if
  // the deadline was already missed.
  const VkPresentModeKHR vsync_modes[] = {
      VK_PRESENT_MODE_FIFO_LATEST_READY_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
      VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_FIFO_KHR};
  const VkPresentModeKHR unthrottled_modes[] = {
      VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
      VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_FIFO_KHR};
  const VkPresentModeKHR *preferred =
      policy == PresentModePolicy::kVsync ? vsync_modes : unthrottled_modes;
  const std::size_t preferred_count =
      policy == PresentModePolicy::kVsync
          ? sizeof(vsync_modes) / sizeof(vsync_modes[0])
          : sizeof(unthrottled_modes) / sizeof(unthrottled_modes[0]);
  for (std::size_t i = 0; i < preferred_count; ++i) {
    if (std::find(host_modes.begin(), host_modes.end(), preferred[i]) !=
        host_modes.end()) {
      return {preferred[i]};
    }
  }
  return host_modes;
}

const char *PresentModeKhrName(VkPresentModeKHR mode) {
  switch (mode) {
  case VK_PRESENT_MODE_IMMEDIATE_KHR:
    return "immediate";
  case VK_PRESENT_MODE_MAILBOX_KHR:
    return "mailbox";
  case VK_PRESENT_MODE_FIFO_KHR:
    return "fifo";
  case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
    return "fifo-relaxed";
  case VK_PRESENT_MODE_FIFO_LATEST_READY_KHR:
    return "fifo-latest-ready";
  default:
    return "other";
  }
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

std::uint32_t PreferSwapchainMinImageCount(PresentModePolicy policy,
                                           std::uint32_t requested,
                                           std::uint32_t min_images,
                                           std::uint32_t max_images) {
  std::uint32_t count = std::max(requested, min_images);
  if (policy == PresentModePolicy::kUnthrottled) {
    constexpr std::uint32_t kUnthrottledImages = 5;
    count = std::max(count, kUnthrottledImages);
    if (min_images != 0) {
      count = std::max(count, min_images + 2);
    }
  } else if (policy == PresentModePolicy::kVsync) {
    constexpr std::uint32_t kVsyncImages = 4;
    count = std::max(count, kVsyncImages);
    if (min_images != 0) {
      count = std::max(count, min_images + 2);
    }
  }
  if (max_images != 0) {
    count = std::min(count, max_images);
  }
  return count;
}

} // namespace graphics
} // namespace mocktail
