#include "mocktail/graphics/vulkan_swapchain_policy.h"

#include <algorithm>
#include <cstring>

namespace mocktail::graphics {

bool EnableSurfaceMaintenanceExtensions(
    const std::vector<VkExtensionProperties>& supported,
    std::vector<std::string>* enabled) {
  if (enabled == nullptr) return false;
  const auto supports = [&](const char* name) {
    return std::any_of(supported.begin(), supported.end(),
                       [&](const VkExtensionProperties& extension) {
                         return std::strcmp(extension.extensionName, name) == 0;
                       });
  };
  const char* maintenance =
      supports(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME)
          ? VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME
          : VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
  if (!supports(maintenance) ||
      !supports(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) ||
      !supports(VK_KHR_SURFACE_EXTENSION_NAME)) {
    return false;
  }
  for (const char* name : {VK_KHR_SURFACE_EXTENSION_NAME,
                            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
                            maintenance}) {
    if (std::find(enabled->begin(), enabled->end(), name) == enabled->end()) {
      enabled->emplace_back(name);
    }
  }
  return true;
}

VkResult ApplySwapchainPresentPolicy(PresentModePolicy policy,
                                     VkPhysicalDevice physical_device,
                                     const VulkanSurfaceQueries& queries,
                                     VkSwapchainCreateInfoKHR* create_info) {
  if (create_info == nullptr || physical_device == VK_NULL_HANDLE) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkSwapchainCreateInfoKHR selected = *create_info;
  if (policy != PresentModePolicy::kHostDefault) {
    if (queries.present_modes == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    std::uint32_t count = 0;
    VkResult result = queries.present_modes(physical_device, selected.surface,
                                            &count, nullptr);
    if (result != VK_SUCCESS) return result;
    if (count == 0) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkPresentModeKHR> modes(count);
    result = queries.present_modes(physical_device, selected.surface, &count,
                                    modes.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) return result;
    modes.resize(std::min<std::size_t>(count, modes.size()));
    const auto filtered = FilterPresentModes(policy, modes);
    if (filtered.empty()) return VK_ERROR_INITIALIZATION_FAILED;
    selected.presentMode = filtered.front();
  }

  VkSurfaceCapabilitiesKHR capabilities{};
  VkResult result;
  if (queries.mode_capabilities != nullptr) {
    VkSurfacePresentModeKHR mode{};
    mode.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR;
    mode.presentMode = selected.presentMode;
    VkPhysicalDeviceSurfaceInfo2KHR info{};
    info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    info.pNext = &mode;
    info.surface = selected.surface;
    VkSurfaceCapabilities2KHR output{};
    output.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    result = queries.mode_capabilities(physical_device, &info, &output);
    capabilities = output.surfaceCapabilities;
  } else {
    if (queries.capabilities == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    result = queries.capabilities(physical_device, selected.surface,
                                   &capabilities);
  }
  if (result != VK_SUCCESS) return result;

  auto image_policy = policy;
  if (selected.presentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
    image_policy = PresentModePolicy::kMailbox;
  } else if (const auto explicit_mode = ExplicitPresentMode(policy);
             explicit_mode && selected.presentMode != *explicit_mode) {
    image_policy = PresentModePolicy::kFifo;
  }
  selected.minImageCount = PreferSwapchainMinImageCount(
      image_policy, selected.minImageCount, capabilities.minImageCount,
      capabilities.maxImageCount);
  *create_info = selected;
  return VK_SUCCESS;
}

}  // namespace mocktail::graphics
