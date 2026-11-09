#include <vulkan/vulkan.h>

#include <cstdint>

extern "C" {

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*) {
  return VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance, const char*) {
  return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char*, std::uint32_t*, VkExtensionProperties*) {
  return VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
    std::uint32_t*, VkLayerProperties*) {
  return VK_ERROR_INITIALIZATION_FAILED;
}

}  // extern "C"
