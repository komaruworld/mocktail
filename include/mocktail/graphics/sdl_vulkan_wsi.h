#ifndef MOCKTAIL_GRAPHICS_SDL_VULKAN_WSI_H_
#define MOCKTAIL_GRAPHICS_SDL_VULKAN_WSI_H_

#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#endif
#include <SDL3/SDL_vulkan.h>

#include <string>
#include <vector>

#include "mocktail/graphics/graphics_backend.h"
#include "mocktail/platform/platform_runtime.h"
#include "mocktail/status.h"

namespace mocktail {
namespace graphics {

// Provides SDL's host WSI entry points. Instance, device, swapchain, renderer,
// and allocator ownership stays with the caller.
class SdlVulkanWsi final {
 public:
  SdlVulkanWsi() = default;
  ~SdlVulkanWsi();

  SdlVulkanWsi(const SdlVulkanWsi&) = delete;
  SdlVulkanWsi& operator=(const SdlVulkanWsi&) = delete;

  Status Initialize(const platform::NativeWindowDescriptor& window,
                    const std::string& vulkan_loader_path = std::string());
  void Shutdown();
  bool IsInitialized() const { return initialized_; }

  const std::vector<std::string>& required_instance_extensions() const {
    return required_instance_extensions_;
  }

  SDL_FunctionPointer GetInstanceProcAddress() const;
  BackendCapability DescribeCapability() const;
  Status CreateSurface(VkInstance instance,
                       const VkAllocationCallbacks* allocator,
                       VkSurfaceKHR* surface) const;
  void DestroySurface(VkInstance instance, VkSurfaceKHR surface,
                      const VkAllocationCallbacks* allocator) const;

 private:
  void* sdl_window_ = nullptr;
  std::vector<std::string> required_instance_extensions_;
  bool initialized_ = false;
};

}  // namespace graphics
}  // namespace mocktail

#endif  // MOCKTAIL_GRAPHICS_SDL_VULKAN_WSI_H_
