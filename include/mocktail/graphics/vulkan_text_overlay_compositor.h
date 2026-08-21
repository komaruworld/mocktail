#ifndef MOCKTAIL_GRAPHICS_VULKAN_TEXT_OVERLAY_COMPOSITOR_H_
#define MOCKTAIL_GRAPHICS_VULKAN_TEXT_OVERLAY_COMPOSITOR_H_

#include <vulkan/vulkan.h>

#include <cstdint>

namespace mocktail {
namespace graphics {

// Registered Vulkan objects remain caller-owned and must be unregistered
// before host destruction. Unsupported registrations fall back cleanly.
class VulkanTextOverlayCompositor final {
 public:
  VulkanTextOverlayCompositor();
  ~VulkanTextOverlayCompositor();

  VulkanTextOverlayCompositor(const VulkanTextOverlayCompositor&) = delete;
  VulkanTextOverlayCompositor& operator=(const VulkanTextOverlayCompositor&) =
      delete;

  // enabled_features must match vkCreateDevice. Proc callbacks resolve host
  // functions directly, bypassing the Android adapter.
  bool RegisterDevice(VkInstance instance, VkPhysicalDevice physical_device,
                      VkDevice device, std::uint32_t api_version,
                      const char* const* enabled_extensions,
                      std::uint32_t enabled_extension_count,
                      std::uint32_t graphics_queue_family,
                      std::uint32_t graphics_queue_count,
                      const VkPhysicalDeviceFeatures2* enabled_features,
                      PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                      PFN_vkGetDeviceProcAddr get_device_proc_addr);
  void DestroyDevice(VkDevice device);

  bool RegisterQueue(VkDevice device, VkQueue queue, std::uint32_t queue_family,
                     std::uint32_t queue_index);

  bool RegisterSwapchain(VkDevice device, VkSwapchainKHR swapchain,
                         const VkSwapchainCreateInfoKHR& create_info,
                         const VkImage* images, std::uint32_t image_count);
  void DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain);

  // Adapter operations serialize only queues imported by libplacebo.
  VkResult QueueSubmit(VkQueue queue, std::uint32_t submit_count,
                       const VkSubmitInfo* submits, VkFence fence,
                       PFN_vkQueueSubmit fallback);
  VkResult QueueSubmit2(VkQueue queue, std::uint32_t submit_count,
                        const VkSubmitInfo2* submits, VkFence fence,
                        PFN_vkQueueSubmit2 fallback);
  VkResult QueueBindSparse(VkQueue queue, std::uint32_t bind_info_count,
                           const VkBindSparseInfo* bind_info, VkFence fence,
                           PFN_vkQueueBindSparse fallback);
  VkResult QueueWaitIdle(VkQueue queue, PFN_vkQueueWaitIdle fallback);
  VkResult DeviceWaitIdle(VkDevice device, PFN_vkDeviceWaitIdle fallback);

  // After wait collapse, fallback receives only compositor-owned semaphores.
  VkResult QueuePresent(VkQueue queue, const VkPresentInfoKHR* present_info,
                        PFN_vkQueuePresentKHR fallback);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace graphics
}  // namespace mocktail

#endif  // MOCKTAIL_GRAPHICS_VULKAN_TEXT_OVERLAY_COMPOSITOR_H_
