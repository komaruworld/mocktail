#ifndef MOCKTAIL_GRAPHICS_VULKAN_SWAPCHAIN_POLICY_H_
#define MOCKTAIL_GRAPHICS_VULKAN_SWAPCHAIN_POLICY_H_

#include <string>
#include <vector>

#include "mocktail/graphics/present_mode_policy.h"

namespace mocktail::graphics {

// Enables optional per-present-mode capabilities and their dependencies only
// when the host supports them. Existing extension names are preserved.
bool EnableSurfaceMaintenanceExtensions(
    const std::vector<VkExtensionProperties>& supported,
    std::vector<std::string>* enabled);

struct VulkanSurfaceQueries {
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR present_modes = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR capabilities = nullptr;
  // Set only for an instance with surface_maintenance1 enabled.
  PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR mode_capabilities = nullptr;
};

// Selects a supported mode, then queries its image limits. On failure the
// caller's create info is untouched. Other fields, including pNext, survive.
VkResult ApplySwapchainPresentPolicy(PresentModePolicy policy,
                                     VkPhysicalDevice physical_device,
                                     const VulkanSurfaceQueries& queries,
                                     VkSwapchainCreateInfoKHR* create_info);

}  // namespace mocktail::graphics

#endif
