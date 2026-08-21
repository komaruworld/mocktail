#include <cstdint>
#include <cstdlib>
#include <cstring>

using VkInstance = void*;
using VkPhysicalDevice = void*;
using VkDevice = void*;
using VkAllocationCallbacks = void;
using VkResult = int32_t;
using VkSurfaceKHR = uint64_t;
using PFN_vkVoidFunction = void (*)();

namespace {

constexpr VkResult kVkSuccess = 0;
constexpr VkResult kVkErrorInitializationFailed = -3;
constexpr VkSurfaceKHR kMockSurface = 1;

int g_instance_handle;

bool TestVulkanStubEnabled() {
  const char* value = std::getenv("MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS");
  return value != nullptr && std::strcmp(value, "1") == 0;
}

}  // namespace

extern "C" {

VkResult vkCreateInstance(const void*,
                          const VkAllocationCallbacks*,
                          VkInstance* instance) {
  if (!TestVulkanStubEnabled()) {
    if (instance != nullptr) {
      *instance = nullptr;
    }
    return kVkErrorInitializationFailed;
  }
  if (instance != nullptr) {
    *instance = &g_instance_handle;
  }
  return kVkSuccess;
}

void vkDestroyInstance(VkInstance,
                       const VkAllocationCallbacks*) {}

VkResult vkCreateAndroidSurfaceKHR(VkInstance,
                                   const void*,
                                   const VkAllocationCallbacks*,
                                   VkSurfaceKHR* surface) {
  if (!TestVulkanStubEnabled()) {
    if (surface != nullptr) {
      *surface = 0;
    }
    return kVkErrorInitializationFailed;
  }
  if (surface != nullptr) {
    *surface = kMockSurface;
  }
  return kVkSuccess;
}

void vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR,
                         const VkAllocationCallbacks*) {}

VkResult vkEnumerateInstanceExtensionProperties(const char*,
                                                uint32_t* property_count,
                                                void*) {
  if (property_count != nullptr) {
    *property_count = 0;
  }
  return kVkSuccess;
}

VkResult vkEnumerateInstanceLayerProperties(uint32_t* property_count,
                                            void*) {
  if (property_count != nullptr) {
    *property_count = 0;
  }
  return kVkSuccess;
}

PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice,
                                       const char*) {
  return nullptr;
}

PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance,
                                         const char* name) {
  if (name == nullptr) {
    return nullptr;
  }
#define VK_PROC(fn) \
  if (std::strcmp(name, #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(fn)
  VK_PROC(vkCreateAndroidSurfaceKHR);
  VK_PROC(vkCreateInstance);
  VK_PROC(vkDestroyInstance);
  VK_PROC(vkDestroySurfaceKHR);
  VK_PROC(vkEnumerateInstanceExtensionProperties);
  VK_PROC(vkEnumerateInstanceLayerProperties);
  VK_PROC(vkGetDeviceProcAddr);
  VK_PROC(vkGetInstanceProcAddr);
#undef VK_PROC
  return nullptr;
}

}  // extern "C"
