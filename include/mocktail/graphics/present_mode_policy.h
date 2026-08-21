#ifndef MOCKTAIL_GRAPHICS_PRESENT_MODE_POLICY_H_
#define MOCKTAIL_GRAPHICS_PRESENT_MODE_POLICY_H_

#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

namespace mocktail {
namespace graphics {

enum class PresentModePolicy {
  kHostDefault,
  kVsync,
  kUnthrottled,
};

PresentModePolicy ResolvePresentModePolicy(std::string_view vsync_value,
                                           std::string_view frame_rate_value);
std::vector<VkPresentModeKHR>
FilterPresentModes(PresentModePolicy policy,
                   const std::vector<VkPresentModeKHR> &host_modes);
const char *PresentModePolicyName(PresentModePolicy policy);

} // namespace graphics
} // namespace mocktail

#endif // MOCKTAIL_GRAPHICS_PRESENT_MODE_POLICY_H_
