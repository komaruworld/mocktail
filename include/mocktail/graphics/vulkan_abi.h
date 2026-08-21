#ifndef MOCKTAIL_GRAPHICS_VULKAN_ABI_H_
#define MOCKTAIL_GRAPHICS_VULKAN_ABI_H_

#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#else

#include <cstddef>
#include <cstdint>

#define VULKAN_CORE_H_ 1
#define VKAPI_ATTR
#define VKAPI_CALL
#define VKAPI_PTR VKAPI_CALL
#define VK_MAX_EXTENSION_NAME_SIZE 256

using VkFlags = std::uint32_t;
using VkResult = std::int32_t;
using VkStructureType = std::int32_t;
using PFN_vkVoidFunction = void(VKAPI_PTR*)();

#define VK_DEFINE_HANDLE(object) typedef struct object##_T* object;
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) \
  typedef struct object##_T* object;
VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_HANDLE(VkDevice)
VK_DEFINE_HANDLE(VkQueue)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSurfaceKHR)
#undef VK_DEFINE_HANDLE
#undef VK_DEFINE_NON_DISPATCHABLE_HANDLE

struct VkAllocationCallbacks;
struct VkPresentInfoKHR;

struct VkInstanceCreateInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  const void* pApplicationInfo;
  std::uint32_t enabledLayerCount;
  const char* const* ppEnabledLayerNames;
  std::uint32_t enabledExtensionCount;
  const char* const* ppEnabledExtensionNames;
};

struct VkExtensionProperties {
  char extensionName[VK_MAX_EXTENSION_NAME_SIZE];
  std::uint32_t specVersion;
};

using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(VKAPI_PTR*)(
    VkInstance, const char*);
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction(VKAPI_PTR*)(VkDevice,
                                                               const char*);
using PFN_vkCreateInstance = VkResult(VKAPI_PTR*)(
    const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
using PFN_vkEnumerateInstanceExtensionProperties = VkResult(VKAPI_PTR*)(
    const char*, std::uint32_t*, VkExtensionProperties*);
using PFN_vkQueuePresentKHR = VkResult(VKAPI_PTR*)(VkQueue,
                                                   const VkPresentInfoKHR*);

inline constexpr VkResult VK_SUCCESS = 0;
inline constexpr VkResult VK_TIMEOUT = 2;
inline constexpr VkResult VK_INCOMPLETE = 5;
inline constexpr VkResult VK_SUBOPTIMAL_KHR = 1000001003;
inline constexpr VkResult VK_ERROR_INITIALIZATION_FAILED = -3;
inline constexpr VkResult VK_ERROR_EXTENSION_NOT_PRESENT = -7;
inline constexpr VkResult VK_ERROR_OUT_OF_DATE_KHR = -1000001004;

#endif

#endif  // MOCKTAIL_GRAPHICS_VULKAN_ABI_H_
