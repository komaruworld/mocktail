// Android Vulkan loader ABI -> host Vulkan loader + SDL3 WSI.

#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "mocktail/graphics/android_vulkan_wsi_adapter.h"
#include "mocktail/graphics/present_mode_policy.h"
#include "mocktail/graphics/vulkan_text_overlay_compositor.h"
#include "mocktail/platform/platform_runtime.h"

extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo* create_info,
                 const VkAllocationCallbacks* allocator, VkInstance* instance);
VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateAndroidSurfaceKHR(
    VkInstance instance, const void* create_info,
    const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface);
VKAPI_ATTR void VKAPI_CALL
vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                    const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char* layer_name, std::uint32_t* property_count,
    VkExtensionProperties* properties);
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
    std::uint32_t* property_count, VkLayerProperties* properties);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                             const char* name);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkDevice* device);
VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator);
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device,
                                            std::uint32_t queue_family_index,
                                            std::uint32_t queue_index,
                                            VkQueue* queue);
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(
    VkDevice device, const VkDeviceQueueInfo2* queue_info, VkQueue* queue);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR* create_info,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain);
VKAPI_ATTR void VKAPI_CALL
vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                      const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, std::uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, std::uint32_t* image_index);
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(
    VkDevice device, const VkAcquireNextImageInfoKHR* acquire_info,
    std::uint32_t* image_index);
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(
    VkDevice device, std::uint32_t fence_count, const VkFence* fences,
    VkBool32 wait_all, std::uint64_t timeout);
VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(
    VkDevice device, std::uint32_t fence_count, const VkFence* fences);
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(
    VkDevice device, VkCommandPool command_pool,
    VkCommandPoolResetFlags flags);
VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(
    VkDevice device, VkQueryPool query_pool, std::uint32_t first_query,
    std::uint32_t query_count, std::size_t data_size, void* data,
    VkDeviceSize stride, VkQueryResultFlags flags);
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo* allocate_info,
    VkCommandBuffer* command_buffers);
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(
    VkDevice device, VkCommandPool command_pool,
    std::uint32_t command_buffer_count,
    const VkCommandBuffer* command_buffers);
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(
    VkDevice device, VkCommandPool command_pool,
    const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(
    VkCommandBuffer command_buffer,
    const VkCommandBufferBeginInfo* begin_info);
VKAPI_ATTR VkResult VKAPI_CALL
vkEndCommandBuffer(VkCommandBuffer command_buffer);
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(
    VkCommandBuffer command_buffer, VkCommandBufferResetFlags flags);
VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(
    VkDevice device, const VkSemaphoreWaitInfo* wait_info,
    std::uint64_t timeout);
VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphoresKHR(
    VkDevice device, const VkSemaphoreWaitInfo* wait_info,
    std::uint64_t timeout);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
    VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo* submits,
    VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(
    VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo2* submits,
    VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR(
    VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo2* submits,
    VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueBindSparse(
    VkQueue queue, std::uint32_t bind_info_count,
    const VkBindSparseInfo* bind_info, VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue);
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device);
VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* present_info);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* capabilities);
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    std::uint32_t* present_mode_count, VkPresentModeKHR* present_modes);
}

namespace {

using BackendWindowFn = void* (*)();
using UsesDirectVulkanFn = bool (*)();
using NotePresentFn = void (*)();
using NoteHostPresentBeginFn = void (*)();
using NoteHostPresentEndFn = void (*)(std::int32_t);
using NoteVulkanCallBeginFn = std::uint64_t (*)(const char*);
using NoteVulkanCallEndFn = void (*)(std::uint64_t, std::int32_t);
using NoteSurfaceOutOfDateFn = void (*)();
using WindowDimensionFn = int (*)();

struct AdapterState {
  struct DeviceDispatch {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkQueuePresentKHR queue_present = nullptr;
    PFN_vkQueueSubmit queue_submit = nullptr;
    PFN_vkQueueSubmit2 queue_submit2 = nullptr;
    PFN_vkQueueSubmit2KHR queue_submit2_khr = nullptr;
    PFN_vkQueueBindSparse queue_bind_sparse = nullptr;
    PFN_vkQueueWaitIdle queue_wait_idle = nullptr;
    PFN_vkResetFences reset_fences = nullptr;
    PFN_vkResetCommandPool reset_command_pool = nullptr;
    PFN_vkGetQueryPoolResults get_query_pool_results = nullptr;
    PFN_vkAllocateCommandBuffers allocate_command_buffers = nullptr;
    PFN_vkFreeCommandBuffers free_command_buffers = nullptr;
    PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
    PFN_vkBeginCommandBuffer begin_command_buffer = nullptr;
    PFN_vkEndCommandBuffer end_command_buffer = nullptr;
    PFN_vkResetCommandBuffer reset_command_buffer = nullptr;
  };

  struct QueueBinding {
    VkQueue queue = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
  };

  struct CommandBufferBinding {
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
  };

  std::mutex mutex;
  mocktail::graphics::SdlVulkanWsi host_wsi;
  mocktail::graphics::AndroidVulkanWsiAdapter android_wsi;
  PFN_vkGetInstanceProcAddr host_get_instance_proc_addr = nullptr;
  PFN_vkGetDeviceProcAddr host_get_device_proc_addr = nullptr;
  std::vector<DeviceDispatch> device_dispatches;
  std::vector<QueueBinding> queue_bindings;
  std::vector<CommandBufferBinding> command_buffer_bindings;
  PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR host_surface_capabilities =
      nullptr;
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR host_surface_present_modes =
      nullptr;
  VkInstance latest_instance = VK_NULL_HANDLE;
  size_t active_instance_count = 0;
  mocktail::graphics::VulkanTextOverlayCompositor text_overlay;
  std::atomic<NotePresentFn> note_present{nullptr};
  std::atomic<NoteHostPresentBeginFn> note_host_present_begin{nullptr};
  std::atomic<NoteHostPresentEndFn> note_host_present_end{nullptr};
  std::atomic<NoteVulkanCallBeginFn> note_vulkan_call_begin{nullptr};
  std::atomic<NoteVulkanCallEndFn> note_vulkan_call_end{nullptr};
  std::atomic<NoteSurfaceOutOfDateFn> note_surface_out_of_date{nullptr};
  bool extent_translation_logged = false;
  bool present_policy_logged = false;
  bool initialized = false;
};

AdapterState& State() {
  static AdapterState state;
  return state;
}

template <typename Function>
Function ResolveProcessFunction(const char* name) {
  return reinterpret_cast<Function>(dlsym(RTLD_DEFAULT, name));
}

bool EnsureInitialized() {
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.initialized) {
    return true;
  }

  const BackendWindowFn backend_window =
      ResolveProcessFunction<BackendWindowFn>("mocktail_window_backend_window");
  const UsesDirectVulkanFn uses_direct_vulkan =
      ResolveProcessFunction<UsesDirectVulkanFn>(
          "mocktail_window_uses_direct_vulkan");
  state.note_present.store(ResolveProcessFunction<NotePresentFn>(
                               "mocktail_window_note_vulkan_present"),
                           std::memory_order_release);
  state.note_host_present_begin.store(
      ResolveProcessFunction<NoteHostPresentBeginFn>(
          "mocktail_window_note_vulkan_host_present_begin"),
      std::memory_order_release);
  state.note_host_present_end.store(
      ResolveProcessFunction<NoteHostPresentEndFn>(
          "mocktail_window_note_vulkan_host_present_end"),
      std::memory_order_release);
  state.note_vulkan_call_begin.store(
      ResolveProcessFunction<NoteVulkanCallBeginFn>(
          "mocktail_window_note_vulkan_call_begin"),
      std::memory_order_release);
  state.note_vulkan_call_end.store(
      ResolveProcessFunction<NoteVulkanCallEndFn>(
          "mocktail_window_note_vulkan_call_end"),
      std::memory_order_release);
  state.note_surface_out_of_date.store(
      ResolveProcessFunction<NoteSurfaceOutOfDateFn>(
          "mocktail_window_note_vulkan_surface_out_of_date"),
      std::memory_order_release);
  if (backend_window == nullptr || uses_direct_vulkan == nullptr ||
      !uses_direct_vulkan() || backend_window() == nullptr) {
    std::fprintf(stderr,
                 "  [vulkan] direct SDL Vulkan window is unavailable\n");
    return false;
  }

  mocktail::platform::NativeWindowDescriptor descriptor;
  descriptor.surface_api = mocktail::platform::WindowSurfaceApi::kDirectVulkan;
  descriptor.backend_window = backend_window();
  mocktail::Status status = state.host_wsi.Initialize(descriptor);
  if (!status.ok()) {
    std::fprintf(stderr, "  [vulkan] SDL WSI initialization failed: %s\n",
                 status.message().c_str());
    return false;
  }
  status = state.android_wsi.Initialize(&state.host_wsi);
  if (!status.ok()) {
    std::fprintf(stderr, "  [vulkan] Android WSI initialization failed: %s\n",
                 status.message().c_str());
    state.host_wsi.Shutdown();
    return false;
  }
  state.host_get_instance_proc_addr =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(
          state.host_wsi.GetInstanceProcAddress());
  if (state.host_get_instance_proc_addr == nullptr) {
    state.android_wsi.Shutdown();
    state.host_wsi.Shutdown();
    return false;
  }
  state.initialized = true;
  std::fprintf(stderr, "  [vulkan] Android WSI -> SDL3 host adapter ready\n");
  return true;
}

void NoteSuboptimalTranslation() {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true, std::memory_order_relaxed)) {
    std::fprintf(stderr,
                 "  [vulkan] normalized host VK_SUBOPTIMAL_KHR for Android "
                 "client compatibility\n");
  }
}

VkResult NormalizeSwapchainResult(VkResult result) {
  if (result == VK_SUBOPTIMAL_KHR) {
    NoteSuboptimalTranslation();
  }
  return mocktail::graphics::NormalizeAndroidSwapchainResult(result);
}

PFN_vkVoidFunction HostInstanceProc(VkInstance instance, const char* name) {
  if (!EnsureInitialized() || name == nullptr) {
    return nullptr;
  }
  return State().host_get_instance_proc_addr(instance, name);
}

PFN_vkVoidFunction HostDeviceProc(VkDevice device, const char* name) {
  if (!EnsureInitialized() || name == nullptr) {
    return nullptr;
  }
  PFN_vkGetDeviceProcAddr host_get_device_proc_addr = nullptr;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    host_get_device_proc_addr = state.host_get_device_proc_addr;
  }
  return host_get_device_proc_addr != nullptr
             ? host_get_device_proc_addr(device, name)
             : nullptr;
}

AdapterState::DeviceDispatch HostDispatchForQueue(VkQueue queue) {
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto binding =
      std::find_if(state.queue_bindings.begin(), state.queue_bindings.end(),
                   [queue](const AdapterState::QueueBinding& candidate) {
                     return candidate.queue == queue;
                   });
  if (binding == state.queue_bindings.end()) {
    return {};
  }
  const auto dispatch = std::find_if(
      state.device_dispatches.begin(), state.device_dispatches.end(),
      [binding](const AdapterState::DeviceDispatch& candidate) {
        return candidate.device == binding->device;
      });
  return dispatch != state.device_dispatches.end() ? *dispatch
                                                    : AdapterState::DeviceDispatch{};
}

AdapterState::DeviceDispatch HostDispatchForDevice(VkDevice device) {
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto dispatch = std::find_if(
      state.device_dispatches.begin(), state.device_dispatches.end(),
      [device](const AdapterState::DeviceDispatch& candidate) {
        return candidate.device == device;
      });
  return dispatch != state.device_dispatches.end()
             ? *dispatch
             : AdapterState::DeviceDispatch{};
}

AdapterState::DeviceDispatch HostDispatchForCommandBuffer(
    VkCommandBuffer command_buffer) {
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto binding = std::find_if(
      state.command_buffer_bindings.begin(),
      state.command_buffer_bindings.end(),
      [command_buffer](const AdapterState::CommandBufferBinding& candidate) {
        return candidate.command_buffer == command_buffer;
      });
  if (binding == state.command_buffer_bindings.end()) {
    return {};
  }
  const auto dispatch = std::find_if(
      state.device_dispatches.begin(), state.device_dispatches.end(),
      [binding](const AdapterState::DeviceDispatch& candidate) {
        return candidate.device == binding->device;
      });
  return dispatch != state.device_dispatches.end()
             ? *dispatch
             : AdapterState::DeviceDispatch{};
}

void RegisterHostDeviceDispatch(VkDevice device,
                                PFN_vkGetDeviceProcAddr get_device_proc_addr) {
  if (device == VK_NULL_HANDLE || get_device_proc_addr == nullptr) {
    return;
  }
  AdapterState::DeviceDispatch dispatch;
  dispatch.device = device;
  dispatch.queue_present = reinterpret_cast<PFN_vkQueuePresentKHR>(
      get_device_proc_addr(device, "vkQueuePresentKHR"));
  dispatch.queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(
      get_device_proc_addr(device, "vkQueueSubmit"));
  dispatch.queue_submit2 = reinterpret_cast<PFN_vkQueueSubmit2>(
      get_device_proc_addr(device, "vkQueueSubmit2"));
  dispatch.queue_submit2_khr = reinterpret_cast<PFN_vkQueueSubmit2KHR>(
      get_device_proc_addr(device, "vkQueueSubmit2KHR"));
  dispatch.queue_bind_sparse = reinterpret_cast<PFN_vkQueueBindSparse>(
      get_device_proc_addr(device, "vkQueueBindSparse"));
  dispatch.queue_wait_idle = reinterpret_cast<PFN_vkQueueWaitIdle>(
      get_device_proc_addr(device, "vkQueueWaitIdle"));
  dispatch.reset_fences = reinterpret_cast<PFN_vkResetFences>(
      get_device_proc_addr(device, "vkResetFences"));
  dispatch.reset_command_pool = reinterpret_cast<PFN_vkResetCommandPool>(
      get_device_proc_addr(device, "vkResetCommandPool"));
  dispatch.get_query_pool_results =
      reinterpret_cast<PFN_vkGetQueryPoolResults>(
          get_device_proc_addr(device, "vkGetQueryPoolResults"));
  dispatch.allocate_command_buffers =
      reinterpret_cast<PFN_vkAllocateCommandBuffers>(
          get_device_proc_addr(device, "vkAllocateCommandBuffers"));
  dispatch.free_command_buffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(
      get_device_proc_addr(device, "vkFreeCommandBuffers"));
  dispatch.destroy_command_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(
      get_device_proc_addr(device, "vkDestroyCommandPool"));
  dispatch.begin_command_buffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(
      get_device_proc_addr(device, "vkBeginCommandBuffer"));
  dispatch.end_command_buffer = reinterpret_cast<PFN_vkEndCommandBuffer>(
      get_device_proc_addr(device, "vkEndCommandBuffer"));
  dispatch.reset_command_buffer = reinterpret_cast<PFN_vkResetCommandBuffer>(
      get_device_proc_addr(device, "vkResetCommandBuffer"));

  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto existing = std::find_if(
      state.device_dispatches.begin(), state.device_dispatches.end(),
      [device](const AdapterState::DeviceDispatch& candidate) {
        return candidate.device == device;
      });
  if (existing != state.device_dispatches.end()) {
    *existing = dispatch;
  } else {
    state.device_dispatches.push_back(dispatch);
  }
}

void RegisterHostQueueBinding(VkDevice device, VkQueue queue) {
  if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
    return;
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto existing =
      std::find_if(state.queue_bindings.begin(), state.queue_bindings.end(),
                   [queue](const AdapterState::QueueBinding& candidate) {
                     return candidate.queue == queue;
                   });
  if (existing != state.queue_bindings.end()) {
    existing->device = device;
  } else {
    state.queue_bindings.push_back({queue, device});
  }
}

void RegisterHostCommandBuffers(VkDevice device, VkCommandPool command_pool,
                                std::uint32_t command_buffer_count,
                                const VkCommandBuffer* command_buffers) {
  if (device == VK_NULL_HANDLE || command_buffers == nullptr) {
    return;
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  for (std::uint32_t index = 0; index < command_buffer_count; ++index) {
    const VkCommandBuffer command_buffer = command_buffers[index];
    if (command_buffer == VK_NULL_HANDLE) {
      continue;
    }
    const auto existing = std::find_if(
        state.command_buffer_bindings.begin(),
        state.command_buffer_bindings.end(),
        [command_buffer](const AdapterState::CommandBufferBinding& candidate) {
          return candidate.command_buffer == command_buffer;
        });
    if (existing != state.command_buffer_bindings.end()) {
      existing->device = device;
      existing->command_pool = command_pool;
    } else {
      state.command_buffer_bindings.push_back(
          {command_buffer, device, command_pool});
    }
  }
}

void RemoveHostCommandBuffers(VkDevice device, VkCommandPool command_pool,
                              std::uint32_t command_buffer_count,
                              const VkCommandBuffer* command_buffers) {
  if (command_buffers == nullptr || command_buffer_count == 0) {
    return;
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.command_buffer_bindings.erase(
      std::remove_if(
          state.command_buffer_bindings.begin(),
          state.command_buffer_bindings.end(),
          [device, command_pool, command_buffer_count, command_buffers](
              const AdapterState::CommandBufferBinding& binding) {
            return binding.device == device &&
                   binding.command_pool == command_pool &&
                   std::find(command_buffers,
                             command_buffers + command_buffer_count,
                             binding.command_buffer) !=
                       command_buffers + command_buffer_count;
          }),
      state.command_buffer_bindings.end());
}

void RemoveHostCommandPoolBindings(VkDevice device,
                                   VkCommandPool command_pool) {
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.command_buffer_bindings.erase(
      std::remove_if(
          state.command_buffer_bindings.begin(),
          state.command_buffer_bindings.end(),
          [device, command_pool](
              const AdapterState::CommandBufferBinding& binding) {
            return binding.device == device &&
                   binding.command_pool == command_pool;
          }),
      state.command_buffer_bindings.end());
}

void RemoveHostDeviceDispatch(VkDevice device) {
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.queue_bindings.erase(
      std::remove_if(state.queue_bindings.begin(), state.queue_bindings.end(),
                     [device](const AdapterState::QueueBinding& binding) {
                       return binding.device == device;
                     }),
      state.queue_bindings.end());
  state.command_buffer_bindings.erase(
      std::remove_if(
          state.command_buffer_bindings.begin(),
          state.command_buffer_bindings.end(),
          [device](const AdapterState::CommandBufferBinding& binding) {
            return binding.device == device;
          }),
      state.command_buffer_bindings.end());
  state.device_dispatches.erase(
      std::remove_if(
          state.device_dispatches.begin(), state.device_dispatches.end(),
          [device](const AdapterState::DeviceDispatch& dispatch) {
            return dispatch.device == device;
          }),
      state.device_dispatches.end());
}

bool IsHostWsiExtension(const char* name) {
  if (name == nullptr) {
    return false;
  }
  const auto& extensions = State().host_wsi.required_instance_extensions();
  return std::find(extensions.begin(), extensions.end(), name) !=
         extensions.end();
}

std::vector<VkExtensionProperties> AndroidVisibleExtensions(
    const char* layer_name, VkResult* result) {
  std::vector<VkExtensionProperties> output;
  const auto host_enumerate =
      reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
          HostInstanceProc(VK_NULL_HANDLE,
                           "vkEnumerateInstanceExtensionProperties"));
  if (host_enumerate == nullptr) {
    *result = VK_ERROR_INITIALIZATION_FAILED;
    return output;
  }
  std::uint32_t count = 0;
  *result = host_enumerate(layer_name, &count, nullptr);
  if (*result != VK_SUCCESS || count == 0) {
    return output;
  }
  output.resize(count);
  *result = host_enumerate(layer_name, &count, output.data());
  if (*result != VK_SUCCESS && *result != VK_INCOMPLETE) {
    output.clear();
    return output;
  }
  output.resize(count);
  if (layer_name != nullptr) {
    return output;
  }

  output.erase(
      std::remove_if(output.begin(), output.end(),
                     [](const auto& property) {
                       return IsHostWsiExtension(property.extensionName) &&
                              std::strcmp(property.extensionName,
                                          VK_KHR_SURFACE_EXTENSION_NAME) != 0;
                     }),
      output.end());
  const bool has_android =
      std::any_of(output.begin(), output.end(), [](const auto& property) {
        return std::strcmp(property.extensionName,
                           mocktail::graphics::kAndroidSurfaceExtension) == 0;
      });
  if (!has_android) {
    VkExtensionProperties android{};
    std::strncpy(android.extensionName,
                 mocktail::graphics::kAndroidSurfaceExtension,
                 sizeof(android.extensionName) - 1);
    android.specVersion = 6;
    output.push_back(android);
  }
  *result = VK_SUCCESS;
  return output;
}

PFN_vkVoidFunction AdapterProc(const char* name) {
  if (name == nullptr) {
    return nullptr;
  }
#define MOCKTAIL_VK_PROC(function)                         \
  if (std::strcmp(name, #function) == 0) {                 \
    return reinterpret_cast<PFN_vkVoidFunction>(function); \
  }
  MOCKTAIL_VK_PROC(vkCreateInstance)
  MOCKTAIL_VK_PROC(vkDestroyInstance)
  MOCKTAIL_VK_PROC(vkCreateAndroidSurfaceKHR)
  MOCKTAIL_VK_PROC(vkDestroySurfaceKHR)
  MOCKTAIL_VK_PROC(vkEnumerateInstanceExtensionProperties)
  MOCKTAIL_VK_PROC(vkEnumerateInstanceLayerProperties)
  MOCKTAIL_VK_PROC(vkGetInstanceProcAddr)
  MOCKTAIL_VK_PROC(vkGetDeviceProcAddr)
  MOCKTAIL_VK_PROC(vkCreateDevice)
  MOCKTAIL_VK_PROC(vkDestroyDevice)
  MOCKTAIL_VK_PROC(vkGetDeviceQueue)
  MOCKTAIL_VK_PROC(vkGetDeviceQueue2)
  MOCKTAIL_VK_PROC(vkCreateSwapchainKHR)
  MOCKTAIL_VK_PROC(vkDestroySwapchainKHR)
  MOCKTAIL_VK_PROC(vkAcquireNextImageKHR)
  MOCKTAIL_VK_PROC(vkAcquireNextImage2KHR)
  MOCKTAIL_VK_PROC(vkWaitForFences)
  MOCKTAIL_VK_PROC(vkResetFences)
  MOCKTAIL_VK_PROC(vkResetCommandPool)
  MOCKTAIL_VK_PROC(vkGetQueryPoolResults)
  MOCKTAIL_VK_PROC(vkAllocateCommandBuffers)
  MOCKTAIL_VK_PROC(vkFreeCommandBuffers)
  MOCKTAIL_VK_PROC(vkDestroyCommandPool)
  MOCKTAIL_VK_PROC(vkBeginCommandBuffer)
  MOCKTAIL_VK_PROC(vkEndCommandBuffer)
  MOCKTAIL_VK_PROC(vkResetCommandBuffer)
  MOCKTAIL_VK_PROC(vkWaitSemaphores)
  MOCKTAIL_VK_PROC(vkWaitSemaphoresKHR)
  MOCKTAIL_VK_PROC(vkQueueSubmit)
  MOCKTAIL_VK_PROC(vkQueueSubmit2)
  MOCKTAIL_VK_PROC(vkQueueSubmit2KHR)
  MOCKTAIL_VK_PROC(vkQueueBindSparse)
  MOCKTAIL_VK_PROC(vkQueueWaitIdle)
  MOCKTAIL_VK_PROC(vkDeviceWaitIdle)
  MOCKTAIL_VK_PROC(vkQueuePresentKHR)
#undef MOCKTAIL_VK_PROC
  return nullptr;
}

bool IsDeviceAdapterProc(const char* name) {
  return name != nullptr && (std::strcmp(name, "vkDestroyDevice") == 0 ||
                             std::strcmp(name, "vkGetDeviceQueue") == 0 ||
                             std::strcmp(name, "vkGetDeviceQueue2") == 0 ||
                             std::strcmp(name, "vkCreateSwapchainKHR") == 0 ||
                             std::strcmp(name, "vkDestroySwapchainKHR") == 0 ||
                             std::strcmp(name, "vkAcquireNextImageKHR") == 0 ||
                             std::strcmp(name, "vkAcquireNextImage2KHR") == 0 ||
                             std::strcmp(name, "vkWaitForFences") == 0 ||
                             std::strcmp(name, "vkResetFences") == 0 ||
                             std::strcmp(name, "vkResetCommandPool") == 0 ||
                             std::strcmp(name, "vkGetQueryPoolResults") == 0 ||
                             std::strcmp(name, "vkAllocateCommandBuffers") ==
                                 0 ||
                             std::strcmp(name, "vkFreeCommandBuffers") == 0 ||
                             std::strcmp(name, "vkDestroyCommandPool") == 0 ||
                             std::strcmp(name, "vkBeginCommandBuffer") == 0 ||
                             std::strcmp(name, "vkEndCommandBuffer") == 0 ||
                             std::strcmp(name, "vkResetCommandBuffer") == 0 ||
                             std::strcmp(name, "vkWaitSemaphores") == 0 ||
                             std::strcmp(name, "vkWaitSemaphoresKHR") == 0 ||
                             std::strcmp(name, "vkQueueSubmit") == 0 ||
                             std::strcmp(name, "vkQueueSubmit2") == 0 ||
                             std::strcmp(name, "vkQueueSubmit2KHR") == 0 ||
                             std::strcmp(name, "vkQueueBindSparse") == 0 ||
                             std::strcmp(name, "vkQueueWaitIdle") == 0 ||
                             std::strcmp(name, "vkDeviceWaitIdle") == 0 ||
                             std::strcmp(name, "vkQueuePresentKHR") == 0);
}

bool IsGlobalAdapterProc(const char* name) {
  return name != nullptr &&
         (std::strcmp(name, "vkCreateInstance") == 0 ||
          std::strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0 ||
          std::strcmp(name, "vkEnumerateInstanceLayerProperties") == 0 ||
          std::strcmp(name, "vkGetInstanceProcAddr") == 0);
}

const VkBaseInStructure* FindFeature(const void* chain, VkStructureType type) {
  const auto* current = static_cast<const VkBaseInStructure*>(chain);
  while (current != nullptr) {
    if (current->sType == type) {
      return current;
    }
    current = current->pNext;
  }
  return nullptr;
}

struct EnabledDeviceFeatures {
  VkPhysicalDeviceFeatures2 root{};
  VkPhysicalDeviceVulkan11Features vulkan11{};
  VkPhysicalDeviceVulkan12Features vulkan12{};
  bool placebo_required = false;

  EnabledDeviceFeatures() {
    root.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    vulkan11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    root.pNext = &vulkan11;
    vulkan11.pNext = &vulkan12;
  }
};

EnabledDeviceFeatures InspectEnabledFeatures(
    const VkDeviceCreateInfo& create_info) {
  EnabledDeviceFeatures enabled;
  if (create_info.pEnabledFeatures != nullptr) {
    enabled.root.features = *create_info.pEnabledFeatures;
  }
  if (const auto* features2 =
          reinterpret_cast<const VkPhysicalDeviceFeatures2*>(FindFeature(
              create_info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2));
      features2 != nullptr) {
    enabled.root.features = features2->features;
  }

  bool timeline = false;
  bool host_query_reset = false;
  if (const auto* vulkan12 =
          reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(FindFeature(
              create_info.pNext,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES));
      vulkan12 != nullptr) {
    timeline = vulkan12->timelineSemaphore == VK_TRUE;
    host_query_reset = vulkan12->hostQueryReset == VK_TRUE;
  }
  if (const auto* timeline_features = reinterpret_cast<
          const VkPhysicalDeviceTimelineSemaphoreFeatures*>(FindFeature(
          create_info.pNext,
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES));
      timeline_features != nullptr) {
    timeline = timeline_features->timelineSemaphore == VK_TRUE;
  }
  if (const auto* host_query_features =
          reinterpret_cast<const VkPhysicalDeviceHostQueryResetFeatures*>(
              FindFeature(
                  create_info.pNext,
                  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES));
      host_query_features != nullptr) {
    host_query_reset = host_query_features->hostQueryReset == VK_TRUE;
  }
  enabled.vulkan12.timelineSemaphore = timeline;
  enabled.vulkan12.hostQueryReset = host_query_reset;
  enabled.placebo_required = timeline && host_query_reset;
  return enabled;
}

bool HasRequiredFeatureStructs(const VkDeviceCreateInfo& create_info) {
  return FindFeature(create_info.pNext,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) !=
             nullptr ||
         FindFeature(
             create_info.pNext,
             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) !=
             nullptr ||
         FindFeature(
             create_info.pNext,
             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES) !=
             nullptr;
}

bool ShouldLogNegativeVulkanResult(VkResult result, std::uint64_t* occurrence) {
  static std::atomic<std::uint64_t> device_lost_count{0};
  static std::atomic<std::uint64_t> other_error_count{0};
  std::atomic<std::uint64_t>& counter =
      result == VK_ERROR_DEVICE_LOST ? device_lost_count : other_error_count;
  const std::uint64_t count =
      counter.fetch_add(1, std::memory_order_relaxed) + 1;
  if (occurrence != nullptr) {
    *occurrence = count;
  }
  return count <= 4 || (count & (count - 1)) == 0;
}

class VulkanCallObservation final {
 public:
  explicit VulkanCallObservation(const char* call_name)
      : call_name_(call_name) {
    AdapterState& state = State();
    begin_ = state.note_vulkan_call_begin.load(std::memory_order_acquire);
    end_ = state.note_vulkan_call_end.load(std::memory_order_acquire);
    if (begin_ != nullptr && end_ != nullptr) {
      sequence_ = begin_(call_name);
    }
  }

  ~VulkanCallObservation() {
    if (sequence_ != 0 && end_ != nullptr) {
      end_(sequence_, static_cast<std::int32_t>(result_));
    }
  }

  VulkanCallObservation(const VulkanCallObservation&) = delete;
  VulkanCallObservation& operator=(const VulkanCallObservation&) = delete;

  void SetResult(VkResult result) {
    result_ = result;
    std::uint64_t occurrence = 0;
    if (result < VK_SUCCESS && result != VK_ERROR_OUT_OF_DATE_KHR &&
        ShouldLogNegativeVulkanResult(result, &occurrence)) {
      std::fprintf(stderr,
                   "  [vulkan] unexpected negative result: call=%s "
                   "result=%d device_lost=%u occurrence=%llu\n",
                   call_name_ != nullptr ? call_name_ : "unknown",
                   static_cast<int>(result),
                   result == VK_ERROR_DEVICE_LOST ? 1U : 0U,
                   static_cast<unsigned long long>(occurrence));
    }
  }

 private:
  NoteVulkanCallBeginFn begin_ = nullptr;
  NoteVulkanCallEndFn end_ = nullptr;
  const char* call_name_ = nullptr;
  std::uint64_t sequence_ = 0;
  VkResult result_ = VK_ERROR_UNKNOWN;
};

VkResult WaitForSemaphoresObserved(const char* call_name,
                                   PFN_vkWaitSemaphores host_wait,
                                   VkDevice device,
                                   const VkSemaphoreWaitInfo* wait_info,
                                   std::uint64_t timeout,
                                   VulkanCallObservation* observation) {
  if (host_wait == nullptr) {
    if (observation != nullptr) {
      observation->SetResult(VK_ERROR_INITIALIZATION_FAILED);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const std::uint64_t host_timeout =
      mocktail::graphics::BoundHostSynchronizationWaitTimeout(timeout);
  std::uint64_t timeout_slices = 0;
  VkResult result = VK_SUCCESS;
  do {
    result = host_wait(device, wait_info, host_timeout);
    if (!mocktail::graphics::ShouldContinueHostSynchronizationWait(timeout,
                                                                   result)) {
      break;
    }
    ++timeout_slices;
    if (timeout_slices == 1 || timeout_slices == 5 ||
        timeout_slices % 30 == 0) {
      std::fprintf(
          stderr,
          "  [vulkan] %s still blocked: elapsed=%llus semaphores=%u; "
          "preserving infinite guest wait\n",
          call_name, static_cast<unsigned long long>(timeout_slices),
          wait_info != nullptr ? wait_info->semaphoreCount : 0U);
    }
  } while (true);
  if (observation != nullptr) {
    observation->SetResult(result);
  }
  return result;
}

VkResult VKAPI_CALL
ObservedHostQueuePresent(VkQueue queue, const VkPresentInfoKHR* present_info) {
  AdapterState& state = State();
  const PFN_vkQueuePresentKHR host_present =
      HostDispatchForQueue(queue).queue_present;
  if (host_present == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const NoteHostPresentBeginFn note_begin =
      state.note_host_present_begin.load(std::memory_order_acquire);
  if (note_begin != nullptr) {
    note_begin();
  }
  const VkResult result = host_present(queue, present_info);
  const NoteHostPresentEndFn note_end =
      state.note_host_present_end.load(std::memory_order_acquire);
  if (note_end != nullptr) {
    note_end(static_cast<std::int32_t>(result));
  }
  return result;
}

}  // namespace

extern "C" {

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo* create_info,
                 const VkAllocationCallbacks* allocator, VkInstance* instance) {
  if (!EnsureInitialized() || create_info == nullptr || instance == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  std::vector<std::string> requested;
  requested.reserve(create_info->enabledExtensionCount);
  for (std::uint32_t index = 0; index < create_info->enabledExtensionCount;
       ++index) {
    const char* extension = create_info->ppEnabledExtensionNames[index];
    if (extension == nullptr) {
      return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    requested.emplace_back(extension);
  }
  std::vector<std::string> translated;
  const mocktail::Status status =
      State().android_wsi.TranslateInstanceExtensions(requested, &translated);
  if (!status.ok()) {
    std::fprintf(stderr, "  [vulkan] instance extension rewrite failed: %s\n",
                 status.message().c_str());
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  }
  std::vector<const char*> translated_names;
  translated_names.reserve(translated.size());
  for (const std::string& extension : translated) {
    translated_names.push_back(extension.c_str());
  }
  VkInstanceCreateInfo host_info = *create_info;
  host_info.enabledExtensionCount =
      static_cast<std::uint32_t>(translated_names.size());
  host_info.ppEnabledExtensionNames = translated_names.data();
  VkApplicationInfo host_application_info{};
  if (create_info->pApplicationInfo != nullptr) {
    host_application_info = *create_info->pApplicationInfo;
  } else {
    host_application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  }
  const std::uint32_t requested_api = host_application_info.apiVersion;
  const auto host_enumerate_version =
      reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
          HostInstanceProc(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
  std::uint32_t host_api = VK_API_VERSION_1_0;
  if (host_enumerate_version != nullptr) {
    (void)host_enumerate_version(&host_api);
  }
  if (host_api >= VK_API_VERSION_1_2 && requested_api < VK_API_VERSION_1_2) {
    host_application_info.apiVersion = VK_API_VERSION_1_2;
    host_info.pApplicationInfo = &host_application_info;
  }
  const auto host_create = reinterpret_cast<PFN_vkCreateInstance>(
      HostInstanceProc(VK_NULL_HANDLE, "vkCreateInstance"));
  if (host_create == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_create(&host_info, allocator, instance);
  if (result == VK_SUCCESS) {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.active_instance_count;
    state.latest_instance = *instance;
    state.host_get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        state.host_get_instance_proc_addr(*instance, "vkGetDeviceProcAddr"));
    std::fprintf(stderr, "  [vulkan] host VkInstance created\n");
    if (host_application_info.apiVersion != requested_api) {
      std::fprintf(stderr,
                   "  [vulkan] instance API raised to 1.2 for imported "
                   "libplacebo device interop\n");
    }
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
  const auto host_destroy = reinterpret_cast<PFN_vkDestroyInstance>(
      HostInstanceProc(instance, "vkDestroyInstance"));
  if (host_destroy != nullptr) {
    host_destroy(instance, allocator);
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.active_instance_count != 0) {
    --state.active_instance_count;
  }
  if (state.active_instance_count == 0 && state.initialized) {
    state.android_wsi.Shutdown();
    state.host_wsi.Shutdown();
    state.host_get_instance_proc_addr = nullptr;
    state.host_get_device_proc_addr = nullptr;
    state.device_dispatches.clear();
    state.queue_bindings.clear();
    state.command_buffer_bindings.clear();
    state.host_surface_capabilities = nullptr;
    state.host_surface_present_modes = nullptr;
    state.latest_instance = VK_NULL_HANDLE;
    state.note_present.store(nullptr, std::memory_order_release);
    state.note_host_present_begin.store(nullptr, std::memory_order_release);
    state.note_host_present_end.store(nullptr, std::memory_order_release);
    state.note_vulkan_call_begin.store(nullptr, std::memory_order_release);
    state.note_vulkan_call_end.store(nullptr, std::memory_order_release);
    state.note_surface_out_of_date.store(nullptr, std::memory_order_release);
    state.extent_translation_logged = false;
    state.present_policy_logged = false;
    state.initialized = false;
    std::fprintf(stderr, "  [vulkan] SDL WSI adapter shut down\n");
  }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateAndroidSurfaceKHR(
    VkInstance instance, const void* create_info,
    const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
  if (!EnsureInitialized()) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const auto result = State().android_wsi.CreateAndroidSurface(
      instance, create_info, allocator, surface);
  if (result == VK_SUCCESS) {
    std::fprintf(stderr, "  [vulkan] Android surface mapped to SDL WSI\n");
  }
  return static_cast<VkResult>(result);
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                    const VkAllocationCallbacks* allocator) {
  if (EnsureInitialized()) {
    State().android_wsi.DestroySurface(instance, surface, allocator);
  }
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char* layer_name, std::uint32_t* property_count,
    VkExtensionProperties* properties) {
  if (property_count == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkResult result = VK_SUCCESS;
  const std::vector<VkExtensionProperties> visible =
      AndroidVisibleExtensions(layer_name, &result);
  if (result != VK_SUCCESS) {
    return result;
  }
  if (properties == nullptr) {
    *property_count = static_cast<std::uint32_t>(visible.size());
    return VK_SUCCESS;
  }
  const std::uint32_t capacity = *property_count;
  const std::uint32_t copied =
      std::min(capacity, static_cast<std::uint32_t>(visible.size()));
  std::copy_n(visible.begin(), copied, properties);
  *property_count = copied;
  return copied < visible.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
    std::uint32_t* property_count, VkLayerProperties* properties) {
  const auto host_enumerate =
      reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(HostInstanceProc(
          VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties"));
  return host_enumerate != nullptr ? host_enumerate(property_count, properties)
                                   : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* name) {
  if (instance == VK_NULL_HANDLE) {
    const PFN_vkVoidFunction adapter = AdapterProc(name);
    return adapter != nullptr && IsGlobalAdapterProc(name)
               ? adapter
               : HostInstanceProc(VK_NULL_HANDLE, name);
  }
  if (name != nullptr &&
      std::strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0) {
    const PFN_vkVoidFunction host = HostInstanceProc(instance, name);
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.host_surface_capabilities =
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(host);
    return host != nullptr ? reinterpret_cast<PFN_vkVoidFunction>(
                                 vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
                           : nullptr;
  }
  if (name != nullptr &&
      std::strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0) {
    const PFN_vkVoidFunction host = HostInstanceProc(instance, name);
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.host_surface_present_modes =
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(host);
    return host != nullptr ? reinterpret_cast<PFN_vkVoidFunction>(
                                 vkGetPhysicalDeviceSurfacePresentModesKHR)
                           : nullptr;
  }
  if (const PFN_vkVoidFunction adapter = AdapterProc(name);
      adapter != nullptr) {
    if (IsDeviceAdapterProc(name) &&
        HostInstanceProc(instance, name) == nullptr) {
      return nullptr;
    }
    return adapter;
  }
  return HostInstanceProc(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,
                                                             const char* name) {
  if (name == nullptr || !EnsureInitialized()) {
    return nullptr;
  }
  AdapterState& state = State();
  PFN_vkGetDeviceProcAddr host_get_device_proc_addr = nullptr;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    host_get_device_proc_addr = state.host_get_device_proc_addr;
  }
  const PFN_vkVoidFunction host = host_get_device_proc_addr != nullptr
                                      ? host_get_device_proc_addr(device, name)
                                      : nullptr;
  if (!IsDeviceAdapterProc(name)) {
    return host;
  }
  return host != nullptr ? AdapterProc(name) : nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkDevice* device) {
  if (create_info == nullptr || device == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkInstance instance = VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr host_get_instance_proc_addr = nullptr;
  PFN_vkGetDeviceProcAddr host_get_device_proc_addr = nullptr;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    instance = state.latest_instance;
    host_get_instance_proc_addr = state.host_get_instance_proc_addr;
    host_get_device_proc_addr = state.host_get_device_proc_addr;
  }
  const auto host_create = reinterpret_cast<PFN_vkCreateDevice>(
      HostInstanceProc(instance, "vkCreateDevice"));
  if (host_create == nullptr || host_get_instance_proc_addr == nullptr ||
      host_get_device_proc_addr == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkPhysicalDeviceProperties properties{};
  const auto host_get_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
          HostInstanceProc(instance, "vkGetPhysicalDeviceProperties"));
  if (host_get_properties != nullptr) {
    host_get_properties(physical_device, &properties);
  }

  VkDeviceCreateInfo host_info = *create_info;
  EnabledDeviceFeatures enabled = InspectEnabledFeatures(*create_info);
  enabled.root.pNext = &enabled.vulkan11;
  enabled.vulkan11.pNext = &enabled.vulkan12;
  enabled.vulkan12.pNext = nullptr;
  VkPhysicalDeviceVulkan12Features injected{};
  injected.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  bool injected_required_features = false;
  if (properties.apiVersion >= VK_API_VERSION_1_2 &&
      !enabled.placebo_required && !HasRequiredFeatureStructs(*create_info)) {
    VkPhysicalDeviceFeatures2 supported{};
    supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    VkPhysicalDeviceVulkan12Features supported_vulkan12{};
    supported_vulkan12.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    supported.pNext = &supported_vulkan12;
    const auto host_get_features2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            HostInstanceProc(instance, "vkGetPhysicalDeviceFeatures2"));
    if (host_get_features2 != nullptr) {
      host_get_features2(physical_device, &supported);
      if (supported_vulkan12.timelineSemaphore == VK_TRUE &&
          supported_vulkan12.hostQueryReset == VK_TRUE) {
        injected.timelineSemaphore = VK_TRUE;
        injected.hostQueryReset = VK_TRUE;
        injected.pNext = const_cast<void*>(host_info.pNext);
        host_info.pNext = &injected;
        enabled.vulkan12.timelineSemaphore = VK_TRUE;
        enabled.vulkan12.hostQueryReset = VK_TRUE;
        enabled.placebo_required = true;
        injected_required_features = true;
      }
    }
  }

  const VkResult result =
      host_create(physical_device, &host_info, allocator, device);
  if (result != VK_SUCCESS) {
    return result;
  }
  RegisterHostDeviceDispatch(*device, host_get_device_proc_addr);

  std::uint32_t queue_family_count = 0;
  const auto host_get_queue_families =
      reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
          HostInstanceProc(instance,
                           "vkGetPhysicalDeviceQueueFamilyProperties"));
  if (host_get_queue_families != nullptr) {
    host_get_queue_families(physical_device, &queue_family_count, nullptr);
  }
  std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
  if (host_get_queue_families != nullptr && queue_family_count != 0) {
    host_get_queue_families(physical_device, &queue_family_count,
                            queue_families.data());
    queue_families.resize(queue_family_count);
  }
  std::uint32_t graphics_family = UINT32_MAX;
  std::uint32_t graphics_count = 0;
  for (std::uint32_t index = 0; index < create_info->queueCreateInfoCount;
       ++index) {
    const VkDeviceQueueCreateInfo& queue_info =
        create_info->pQueueCreateInfos[index];
    if (queue_info.queueFamilyIndex >= queue_families.size() ||
        queue_info.queueCount == 0 ||
        (queue_families[queue_info.queueFamilyIndex].queueFlags &
         VK_QUEUE_GRAPHICS_BIT) == 0) {
      continue;
    }
    graphics_family = queue_info.queueFamilyIndex;
    graphics_count = 1;
    break;
  }

  const bool registered =
      enabled.placebo_required && graphics_family != UINT32_MAX &&
      State().text_overlay.RegisterDevice(
          instance, physical_device, *device, VK_API_VERSION_1_2,
          create_info->ppEnabledExtensionNames,
          create_info->enabledExtensionCount, graphics_family, graphics_count,
          &enabled.root, host_get_instance_proc_addr,
          host_get_device_proc_addr);
  if (!registered) {
    std::fprintf(stderr,
                 "  [vulkan] same-surface text compositor unavailable for "
                 "this VkDevice\n");
  } else if (injected_required_features) {
    std::fprintf(stderr,
                 "  [vulkan] enabled Vulkan 1.2 timeline/host-query features "
                 "for libplacebo interop\n");
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
  const auto host_destroy = reinterpret_cast<PFN_vkDestroyDevice>(
      HostDeviceProc(device, "vkDestroyDevice"));
  State().text_overlay.DestroyDevice(device);
  if (host_destroy != nullptr) {
    host_destroy(device, allocator);
  }
  RemoveHostDeviceDispatch(device);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device,
                                            std::uint32_t queue_family_index,
                                            std::uint32_t queue_index,
                                            VkQueue* queue) {
  const auto host_get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(
      HostDeviceProc(device, "vkGetDeviceQueue"));
  if (host_get_queue == nullptr || queue == nullptr) {
    return;
  }
  host_get_queue(device, queue_family_index, queue_index, queue);
  if (*queue != VK_NULL_HANDLE) {
    RegisterHostQueueBinding(device, *queue);
    (void)State().text_overlay.RegisterQueue(device, *queue, queue_family_index,
                                             queue_index);
  }
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(
    VkDevice device, const VkDeviceQueueInfo2* queue_info, VkQueue* queue) {
  const auto host_get_queue = reinterpret_cast<PFN_vkGetDeviceQueue2>(
      HostDeviceProc(device, "vkGetDeviceQueue2"));
  if (host_get_queue == nullptr || queue_info == nullptr || queue == nullptr) {
    return;
  }
  host_get_queue(device, queue_info, queue);
  if (*queue != VK_NULL_HANDLE) {
    RegisterHostQueueBinding(device, *queue);
    (void)State().text_overlay.RegisterQueue(
        device, *queue, queue_info->queueFamilyIndex, queue_info->queueIndex);
  }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR* create_info,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
  const auto host_create = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
      HostDeviceProc(device, "vkCreateSwapchainKHR"));
  if (host_create == nullptr || create_info == nullptr ||
      swapchain == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result =
      host_create(device, create_info, allocator, swapchain);
  if (result != VK_SUCCESS) {
    return result;
  }
  const auto host_get_images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
      HostDeviceProc(device, "vkGetSwapchainImagesKHR"));
  std::uint32_t image_count = 0;
  if (host_get_images == nullptr ||
      host_get_images(device, *swapchain, &image_count, nullptr) !=
          VK_SUCCESS ||
      image_count == 0) {
    return result;
  }
  std::vector<VkImage> images(image_count);
  const VkResult images_result =
      host_get_images(device, *swapchain, &image_count, images.data());
  if (images_result == VK_SUCCESS || images_result == VK_INCOMPLETE) {
    images.resize(image_count);
    (void)State().text_overlay.RegisterSwapchain(
        device, *swapchain, *create_info, images.data(), image_count);
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                      const VkAllocationCallbacks* allocator) {
  const auto host_destroy = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
      HostDeviceProc(device, "vkDestroySwapchainKHR"));
  State().text_overlay.DestroySwapchain(device, swapchain);
  if (host_destroy != nullptr) {
    host_destroy(device, swapchain, allocator);
  }
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, std::uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, std::uint32_t* image_index) {
  VulkanCallObservation observation("vkAcquireNextImageKHR");
  const auto host_acquire = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
      HostDeviceProc(device, "vkAcquireNextImageKHR"));
  if (host_acquire == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const std::uint64_t host_timeout =
      mocktail::graphics::BoundHostImageAcquireTimeout(timeout);
  const VkResult host_result = host_acquire(device, swapchain, host_timeout,
                                            semaphore, fence, image_index);
  const bool watchdog_timeout =
      mocktail::graphics::IsHostImageAcquireWatchdogTimeout(timeout,
                                                            host_result);
  const VkResult result =
      mocktail::graphics::NormalizeHostImageAcquireResult(timeout, host_result);
  if (watchdog_timeout) {
    static std::atomic<std::uint64_t> watchdog_timeout_count{0};
    const std::uint64_t count =
        watchdog_timeout_count.fetch_add(1, std::memory_order_relaxed) + 1;
    std::fprintf(stderr,
                 "  [vulkan] host image acquire timed out after 1000ms; "
                 "requesting surface recovery count=%llu\n",
                 static_cast<unsigned long long>(count));
  }
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    const NoteSurfaceOutOfDateFn note_surface_out_of_date =
        State().note_surface_out_of_date.load(std::memory_order_acquire);
    if (note_surface_out_of_date != nullptr) {
      note_surface_out_of_date();
    }
  }
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImage2KHR(
    VkDevice device, const VkAcquireNextImageInfoKHR* acquire_info,
    std::uint32_t* image_index) {
  VulkanCallObservation observation("vkAcquireNextImage2KHR");
  const auto host_acquire = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(
      HostDeviceProc(device, "vkAcquireNextImage2KHR"));
  if (host_acquire == nullptr || acquire_info == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkAcquireNextImageInfoKHR host_info = *acquire_info;
  host_info.timeout =
      mocktail::graphics::BoundHostImageAcquireTimeout(acquire_info->timeout);
  const VkResult host_result = host_acquire(device, &host_info, image_index);
  const bool watchdog_timeout =
      mocktail::graphics::IsHostImageAcquireWatchdogTimeout(
          acquire_info->timeout, host_result);
  const VkResult result =
      mocktail::graphics::NormalizeHostImageAcquireResult(
          acquire_info->timeout, host_result);
  if (watchdog_timeout) {
    static std::atomic<std::uint64_t> watchdog_timeout_count{0};
    const std::uint64_t count =
        watchdog_timeout_count.fetch_add(1, std::memory_order_relaxed) + 1;
    std::fprintf(stderr,
                 "  [vulkan] host image acquire2 timed out after 1000ms; "
                 "requesting surface recovery count=%llu\n",
                 static_cast<unsigned long long>(count));
  }
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    const NoteSurfaceOutOfDateFn note_surface_out_of_date =
        State().note_surface_out_of_date.load(std::memory_order_acquire);
    if (note_surface_out_of_date != nullptr) {
      note_surface_out_of_date();
    }
  }
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(
    VkDevice device, std::uint32_t fence_count, const VkFence* fences,
    VkBool32 wait_all, std::uint64_t timeout) {
  VulkanCallObservation observation("vkWaitForFences");
  const auto host_wait = reinterpret_cast<PFN_vkWaitForFences>(
      HostDeviceProc(device, "vkWaitForFences"));
  if (host_wait == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const std::uint64_t host_timeout =
      mocktail::graphics::BoundHostSynchronizationWaitTimeout(timeout);
  std::uint64_t timeout_slices = 0;
  VkResult result = VK_SUCCESS;
  do {
    result = host_wait(device, fence_count, fences, wait_all, host_timeout);
    if (!mocktail::graphics::ShouldContinueHostSynchronizationWait(timeout,
                                                                   result)) {
      break;
    }
    ++timeout_slices;
    if (timeout_slices == 1 || timeout_slices == 5 ||
        timeout_slices % 30 == 0) {
      std::fprintf(
          stderr,
          "  [vulkan] vkWaitForFences still blocked: elapsed=%llus "
          "fences=%u wait_all=%u; preserving infinite guest wait\n",
          static_cast<unsigned long long>(timeout_slices), fence_count,
          wait_all == VK_TRUE ? 1U : 0U);
    }
  } while (true);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(
    VkDevice device, std::uint32_t fence_count, const VkFence* fences) {
  VulkanCallObservation observation("vkResetFences");
  const PFN_vkResetFences host_reset =
      HostDispatchForDevice(device).reset_fences;
  if (host_reset == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_reset(device, fence_count, fences);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(
    VkDevice device, VkCommandPool command_pool,
    VkCommandPoolResetFlags flags) {
  VulkanCallObservation observation("vkResetCommandPool");
  const PFN_vkResetCommandPool host_reset =
      HostDispatchForDevice(device).reset_command_pool;
  if (host_reset == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_reset(device, command_pool, flags);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(
    VkDevice device, VkQueryPool query_pool, std::uint32_t first_query,
    std::uint32_t query_count, std::size_t data_size, void* data,
    VkDeviceSize stride, VkQueryResultFlags flags) {
  VulkanCallObservation observation("vkGetQueryPoolResults");
  const PFN_vkGetQueryPoolResults host_get_results =
      HostDispatchForDevice(device).get_query_pool_results;
  if (host_get_results == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_get_results(device, query_pool, first_query,
                                           query_count, data_size, data,
                                           stride, flags);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo* allocate_info,
    VkCommandBuffer* command_buffers) {
  VulkanCallObservation observation("vkAllocateCommandBuffers");
  const PFN_vkAllocateCommandBuffers host_allocate =
      HostDispatchForDevice(device).allocate_command_buffers;
  if (host_allocate == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result =
      host_allocate(device, allocate_info, command_buffers);
  if (result == VK_SUCCESS && allocate_info != nullptr &&
      command_buffers != nullptr) {
    RegisterHostCommandBuffers(device, allocate_info->commandPool,
                               allocate_info->commandBufferCount,
                               command_buffers);
  }
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(
    VkDevice device, VkCommandPool command_pool,
    std::uint32_t command_buffer_count,
    const VkCommandBuffer* command_buffers) {
  VulkanCallObservation observation("vkFreeCommandBuffers");
  const PFN_vkFreeCommandBuffers host_free =
      HostDispatchForDevice(device).free_command_buffers;
  if (host_free == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return;
  }
  host_free(device, command_pool, command_buffer_count, command_buffers);
  RemoveHostCommandBuffers(device, command_pool, command_buffer_count,
                           command_buffers);
  observation.SetResult(VK_SUCCESS);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(
    VkDevice device, VkCommandPool command_pool,
    const VkAllocationCallbacks* allocator) {
  VulkanCallObservation observation("vkDestroyCommandPool");
  const PFN_vkDestroyCommandPool host_destroy =
      HostDispatchForDevice(device).destroy_command_pool;
  if (host_destroy == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return;
  }
  host_destroy(device, command_pool, allocator);
  RemoveHostCommandPoolBindings(device, command_pool);
  observation.SetResult(VK_SUCCESS);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(
    VkCommandBuffer command_buffer,
    const VkCommandBufferBeginInfo* begin_info) {
  VulkanCallObservation observation("vkBeginCommandBuffer");
  const PFN_vkBeginCommandBuffer host_begin =
      HostDispatchForCommandBuffer(command_buffer).begin_command_buffer;
  if (host_begin == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_begin(command_buffer, begin_info);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkEndCommandBuffer(VkCommandBuffer command_buffer) {
  VulkanCallObservation observation("vkEndCommandBuffer");
  const PFN_vkEndCommandBuffer host_end =
      HostDispatchForCommandBuffer(command_buffer).end_command_buffer;
  if (host_end == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_end(command_buffer);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(
    VkCommandBuffer command_buffer, VkCommandBufferResetFlags flags) {
  VulkanCallObservation observation("vkResetCommandBuffer");
  const PFN_vkResetCommandBuffer host_reset =
      HostDispatchForCommandBuffer(command_buffer).reset_command_buffer;
  if (host_reset == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_reset(command_buffer, flags);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(
    VkDevice device, const VkSemaphoreWaitInfo* wait_info,
    std::uint64_t timeout) {
  VulkanCallObservation observation("vkWaitSemaphores");
  const auto host_wait = reinterpret_cast<PFN_vkWaitSemaphores>(
      HostDeviceProc(device, "vkWaitSemaphores"));
  return WaitForSemaphoresObserved("vkWaitSemaphores", host_wait, device,
                                   wait_info, timeout, &observation);
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphoresKHR(
    VkDevice device, const VkSemaphoreWaitInfo* wait_info,
    std::uint64_t timeout) {
  VulkanCallObservation observation("vkWaitSemaphoresKHR");
  const auto host_wait = reinterpret_cast<PFN_vkWaitSemaphoresKHR>(
      HostDeviceProc(device, "vkWaitSemaphoresKHR"));
  return WaitForSemaphoresObserved(
      "vkWaitSemaphoresKHR", reinterpret_cast<PFN_vkWaitSemaphores>(host_wait),
      device, wait_info, timeout, &observation);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
    VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo* submits,
    VkFence fence) {
  VulkanCallObservation observation("vkQueueSubmit");
  const PFN_vkQueueSubmit host_submit =
      HostDispatchForQueue(queue).queue_submit;
  if (host_submit == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = State().text_overlay.QueueSubmit(
      queue, submit_count, submits, fence, host_submit);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(
    VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo2* submits,
    VkFence fence) {
  VulkanCallObservation observation("vkQueueSubmit2");
  const PFN_vkQueueSubmit2 host_submit =
      HostDispatchForQueue(queue).queue_submit2;
  if (host_submit == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = State().text_overlay.QueueSubmit2(
      queue, submit_count, submits, fence, host_submit);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR(
    VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo2* submits,
    VkFence fence) {
  VulkanCallObservation observation("vkQueueSubmit2KHR");
  const PFN_vkQueueSubmit2KHR host_submit =
      HostDispatchForQueue(queue).queue_submit2_khr;
  if (host_submit == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = State().text_overlay.QueueSubmit2(
      queue, submit_count, submits, fence,
      reinterpret_cast<PFN_vkQueueSubmit2>(host_submit));
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueBindSparse(
    VkQueue queue, std::uint32_t bind_info_count,
    const VkBindSparseInfo* bind_info, VkFence fence) {
  VulkanCallObservation observation("vkQueueBindSparse");
  const PFN_vkQueueBindSparse host_bind =
      HostDispatchForQueue(queue).queue_bind_sparse;
  if (host_bind == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = State().text_overlay.QueueBindSparse(
      queue, bind_info_count, bind_info, fence, host_bind);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
  VulkanCallObservation observation("vkQueueWaitIdle");
  const PFN_vkQueueWaitIdle host_wait =
      HostDispatchForQueue(queue).queue_wait_idle;
  if (host_wait == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result =
      State().text_overlay.QueueWaitIdle(queue, host_wait);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
  VulkanCallObservation observation("vkDeviceWaitIdle");
  const auto host_wait = reinterpret_cast<PFN_vkDeviceWaitIdle>(
      HostDeviceProc(device, "vkDeviceWaitIdle"));
  if (host_wait == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result =
      State().text_overlay.DeviceWaitIdle(device, host_wait);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* capabilities) {
  PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR host_capabilities = nullptr;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    host_capabilities = state.host_surface_capabilities;
  }
  if (host_capabilities == nullptr || capabilities == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result =
      host_capabilities(physical_device, surface, capabilities);
  if (result != VK_SUCCESS ||
      (capabilities->currentExtent.width != UINT32_MAX &&
       capabilities->currentExtent.height != UINT32_MAX)) {
    return result;
  }

  const WindowDimensionFn window_width =
      ResolveProcessFunction<WindowDimensionFn>("mocktail_window_width");
  const WindowDimensionFn window_height =
      ResolveProcessFunction<WindowDimensionFn>("mocktail_window_height");
  if (window_width == nullptr || window_height == nullptr ||
      window_width() <= 0 || window_height() <= 0) {
    return result;
  }
  capabilities->currentExtent.width = std::clamp(
      static_cast<std::uint32_t>(window_width()),
      capabilities->minImageExtent.width, capabilities->maxImageExtent.width);
  capabilities->currentExtent.height = std::clamp(
      static_cast<std::uint32_t>(window_height()),
      capabilities->minImageExtent.height, capabilities->maxImageExtent.height);

  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.extent_translation_logged) {
    state.extent_translation_logged = true;
    std::fprintf(
        stderr, "  [vulkan] Android currentExtent translated to %ux%u\n",
        capabilities->currentExtent.width, capabilities->currentExtent.height);
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    std::uint32_t* present_mode_count, VkPresentModeKHR* present_modes) {
  if (present_mode_count == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR host_query = nullptr;
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    host_query = state.host_surface_present_modes;
  }
  if (host_query == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  std::uint32_t host_count = 0;
  VkResult result = host_query(physical_device, surface, &host_count, nullptr);
  if (result != VK_SUCCESS) {
    return result;
  }
  std::vector<VkPresentModeKHR> host_modes(host_count);
  if (host_count != 0) {
    result =
        host_query(physical_device, surface, &host_count, host_modes.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
      return result;
    }
    host_modes.resize(host_count);
  }
  const char* vsync = std::getenv("MOCKTAIL_VSYNC");
  const char* frame_rate = std::getenv("MOCKTAIL_FRAME_RATE_LIMIT");
  const mocktail::graphics::PresentModePolicy policy =
      mocktail::graphics::ResolvePresentModePolicy(
          vsync != nullptr ? vsync : "auto",
          frame_rate != nullptr ? frame_rate : "display");
  const std::vector<VkPresentModeKHR> visible =
      mocktail::graphics::FilterPresentModes(policy, host_modes);
  {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.present_policy_logged) {
      state.present_policy_logged = true;
      std::fprintf(stderr, "  [vulkan] present policy=%s modes=%zu\n",
                   mocktail::graphics::PresentModePolicyName(policy),
                   visible.size());
    }
  }
  if (present_modes == nullptr) {
    *present_mode_count = static_cast<std::uint32_t>(visible.size());
    return VK_SUCCESS;
  }
  const std::uint32_t capacity = *present_mode_count;
  const std::uint32_t copied =
      std::min(capacity, static_cast<std::uint32_t>(visible.size()));
  std::copy_n(visible.begin(), copied, present_modes);
  *present_mode_count = copied;
  return copied < visible.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* present_info) {
  VulkanCallObservation observation("vkQueuePresentKHR(adapter)");
  AdapterState& state = State();
  const PFN_vkQueuePresentKHR host_present =
      HostDispatchForQueue(queue).queue_present;
  const NotePresentFn note_present =
      state.note_present.load(std::memory_order_acquire);
  if (host_present == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = state.text_overlay.QueuePresent(
      queue, present_info, ObservedHostQueuePresent);
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    const NoteSurfaceOutOfDateFn note_surface_out_of_date =
        state.note_surface_out_of_date.load(std::memory_order_acquire);
    if (note_surface_out_of_date != nullptr) {
      note_surface_out_of_date();
    }
  }
  if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) &&
      note_present != nullptr) {
    note_present();
  }
  if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) &&
      present_info != nullptr && present_info->pResults != nullptr) {
    for (std::uint32_t index = 0; index < present_info->swapchainCount;
         ++index) {
      present_info->pResults[index] =
          NormalizeSwapchainResult(present_info->pResults[index]);
    }
  }
  const VkResult normalized_result = NormalizeSwapchainResult(result);
  observation.SetResult(normalized_result);
  return normalized_result;
}

}  // extern "C"
