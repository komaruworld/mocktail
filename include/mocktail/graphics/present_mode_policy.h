#ifndef MOCKTAIL_GRAPHICS_PRESENT_MODE_POLICY_H_
#define MOCKTAIL_GRAPHICS_PRESENT_MODE_POLICY_H_

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

namespace mocktail {
namespace graphics {

enum class PresentModePolicy {
  kHostDefault,
  kVsync,
  kUnthrottled,
  kFifo,
  kMailbox,
  kImmediate,
};

PresentModePolicy ResolvePresentModePolicy(std::string_view vsync_value,
                                           std::string_view frame_rate_value,
                                           std::string_view present_mode = "auto");

std::optional<VkPresentModeKHR> ExplicitPresentMode(PresentModePolicy policy);
int ResolveSwapInterval(PresentModePolicy policy, int requested);

PresentModePolicy CachedPresentModePolicy();

std::vector<VkPresentModeKHR>
FilterPresentModes(PresentModePolicy policy,
                   const std::vector<VkPresentModeKHR> &host_modes);
const char *PresentModePolicyName(PresentModePolicy policy);
const char *PresentModeKhrName(VkPresentModeKHR mode);

// Extra images so acquire does not wait on the compositor-held buffer.
std::uint32_t PreferSwapchainMinImageCount(PresentModePolicy policy,
                                           std::uint32_t requested,
                                           std::uint32_t min_images,
                                           std::uint32_t max_images);

} // namespace graphics
} // namespace mocktail

#endif // MOCKTAIL_GRAPHICS_PRESENT_MODE_POLICY_H_
