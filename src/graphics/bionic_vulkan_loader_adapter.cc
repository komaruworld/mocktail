// Android Vulkan loader ABI -> host Vulkan loader + SDL3 WSI.

#include <dlfcn.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mocktail/graphics/android_vulkan_wsi_adapter.h"
#include "mocktail/graphics/present_mode_policy.h"
#include "mocktail/graphics/vulkan_text_overlay_compositor.h"
#include "mocktail/platform/platform_runtime.h"

extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL
mocktail_vulkan_idle_synchronized(PFN_vkDeviceWaitIdle raw, VkDevice device);
VKAPI_ATTR VkResult VKAPI_CALL mocktail_vulkan_submit_synchronized(
    PFN_vkQueueSubmit raw, VkDevice device, VkQueue queue, uint32_t count,
    const VkSubmitInfo *submits, VkFence fence);

VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo *create_info,
                 const VkAllocationCallbacks *allocator, VkInstance *instance);
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
    VkDevice device, const VkAcquireNextImageInfoKHR *acquire_info,
    std::uint32_t *image_index);
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice device,
                                               std::uint32_t fence_count,
                                               const VkFence *fences,
                                               VkBool32 wait_all,
                                               std::uint64_t timeout);
VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device,
                                             std::uint32_t fence_count,
                                             const VkFence *fences);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(
    VkDevice device, const VkCommandPoolCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkCommandPool *pool);
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(
    VkCommandBuffer command, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t memory_count,
    const VkMemoryBarrier *memory, uint32_t buffer_count,
    const VkBufferMemoryBarrier *buffers, uint32_t image_count,
    const VkImageMemoryBarrier *images);
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer command,
                                                 const VkDependencyInfo *info);
VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier2KHR(VkCommandBuffer command, const VkDependencyInfo *info);
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
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(
    VkDevice device, const VkImageCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkImage* image);
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image,
                                          const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkImageView* view);
VKAPI_ATTR void VKAPI_CALL
vkDestroyImageView(VkDevice device, VkImageView image_view,
                   const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(
    VkDevice device, const VkFramebufferCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkFramebuffer* framebuffer);
VKAPI_ATTR void VKAPI_CALL
vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer,
                     const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(
    VkDevice device, const VkRenderPassCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkRenderPass* render_pass);
VKAPI_ATTR void VKAPI_CALL
vkDestroyRenderPass(VkDevice device, VkRenderPass render_pass,
                    const VkAllocationCallbacks* allocator);
VKAPI_ATTR void VKAPI_CALL
vkCmdBeginRenderPass(VkCommandBuffer command_buffer,
                     const VkRenderPassBeginInfo* begin_info,
                     VkSubpassContents contents);
}

namespace {

using BackendWindowFn = void* (*)();
using UsesDirectVulkanFn = bool (*)();
using NotePresentFn = void (*)();
using NoteHostPresentBeginFn = void (*)();
using NoteHostPresentEndFn = void (*)(std::int32_t);
using NoteVrPresentBeginFn = void (*)();
using VrXrCreateInstanceFn = bool (*)(const VkInstanceCreateInfo*,
                                      const VkAllocationCallbacks*,
                                      VkInstance*, VkResult*);
using VrXrCreateDeviceFn = bool (*)(VkPhysicalDevice,
                                    const VkDeviceCreateInfo*,
                                    const VkAllocationCallbacks*, VkDevice*,
                                    VkResult*);
using VrXrNoteDeviceDestroyedFn = void (*)(VkDevice);
using VrXrNoteQueueFn = void (*)(VkDevice, VkQueue, std::uint32_t,
                                 std::uint32_t);
using VrXrRecordImageFn = void (*)(VkDevice, VkImage,
                                   const VkImageCreateInfo*);
using VrXrRecordImageViewFn = void (*)(VkImageView, VkImage);
using VrXrRecordFramebufferFn = void (*)(VkFramebuffer,
                                         const VkFramebufferCreateInfo*);
using VrXrRecordRenderPassFn = void (*)(VkRenderPass,
                                        const VkRenderPassCreateInfo*);
using VrXrNoteDestroyImageFn = void (*)(VkImage);
using VrXrNoteDestroyImageViewFn = void (*)(VkImageView);
using VrXrNoteDestroyFramebufferFn = void (*)(VkFramebuffer);
using VrXrNoteRenderPassBeginFn = void (*)(VkCommandBuffer,
                                           const VkRenderPassBeginInfo*);
using VrXrNotePresentFn = void (*)(VkQueue, VkDevice);
using NoteVulkanCallBeginFn = std::uint64_t (*)(const char*);
using NoteVulkanCallEndFn = void (*)(std::uint64_t, std::int32_t);
using NoteSurfaceOutOfDateFn = void (*)();
using WindowDimensionFn = int (*)();

struct AdapterState {
  struct DeviceDispatch {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    PFN_vkQueuePresentKHR queue_present = nullptr;
    PFN_vkQueueSubmit queue_submit = nullptr;
    PFN_vkQueueSubmit2 queue_submit2 = nullptr;
    PFN_vkQueueSubmit2KHR queue_submit2_khr = nullptr;
    PFN_vkQueueBindSparse queue_bind_sparse = nullptr;
    PFN_vkQueueWaitIdle queue_wait_idle = nullptr;
    PFN_vkAcquireNextImageKHR acquire_next_image = nullptr;
    PFN_vkAcquireNextImage2KHR acquire_next_image2 = nullptr;
    PFN_vkWaitForFences wait_for_fences = nullptr;
    PFN_vkWaitSemaphores wait_semaphores = nullptr;
    PFN_vkWaitSemaphoresKHR wait_semaphores_khr = nullptr;
    PFN_vkDeviceWaitIdle device_wait_idle = nullptr;
    PFN_vkResetFences reset_fences = nullptr;
    PFN_vkResetCommandPool reset_command_pool = nullptr;
    PFN_vkGetQueryPoolResults get_query_pool_results = nullptr;
    PFN_vkAllocateCommandBuffers allocate_command_buffers = nullptr;
    PFN_vkFreeCommandBuffers free_command_buffers = nullptr;
    PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
    PFN_vkBeginCommandBuffer begin_command_buffer = nullptr;
    PFN_vkEndCommandBuffer end_command_buffer = nullptr;
    PFN_vkResetCommandBuffer reset_command_buffer = nullptr;
    PFN_vkCreateImage create_image = nullptr;
    PFN_vkDestroyImage destroy_image = nullptr;
    PFN_vkCreateImageView create_image_view = nullptr;
    PFN_vkDestroyImageView destroy_image_view = nullptr;
    PFN_vkCreateFramebuffer create_framebuffer = nullptr;
    PFN_vkDestroyFramebuffer destroy_framebuffer = nullptr;
    PFN_vkCreateRenderPass create_render_pass = nullptr;
    PFN_vkDestroyRenderPass destroy_render_pass = nullptr;
    PFN_vkCmdBeginRenderPass cmd_begin_render_pass = nullptr;
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
  std::atomic<PFN_vkGetDeviceProcAddr> host_get_device_proc_addr{nullptr};
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
  std::atomic<NoteVrPresentBeginFn> note_vr_present_begin{nullptr};
  std::atomic<VrXrCreateInstanceFn> vr_create_instance{nullptr};
  std::atomic<VrXrCreateDeviceFn> vr_create_device{nullptr};
  std::atomic<VrXrNoteDeviceDestroyedFn> vr_note_device_destroyed{nullptr};
  std::atomic<VrXrNoteQueueFn> vr_note_queue{nullptr};
  std::atomic<VrXrRecordImageFn> vr_record_image{nullptr};
  std::atomic<VrXrRecordImageViewFn> vr_record_image_view{nullptr};
  std::atomic<VrXrRecordFramebufferFn> vr_record_framebuffer{nullptr};
  std::atomic<VrXrRecordRenderPassFn> vr_record_render_pass{nullptr};
  std::atomic<VrXrNoteDestroyImageFn> vr_note_destroy_image{nullptr};
  std::atomic<VrXrNoteDestroyImageViewFn> vr_note_destroy_image_view{nullptr};
  std::atomic<VrXrNoteDestroyFramebufferFn>
      vr_note_destroy_framebuffer{nullptr};
  std::atomic<VrXrNoteRenderPassBeginFn> vr_note_render_pass_begin{nullptr};
  std::atomic<VrXrNotePresentFn> vr_note_present{nullptr};
  std::atomic<NoteVulkanCallBeginFn> note_vulkan_call_begin{nullptr};
  std::atomic<NoteVulkanCallEndFn> note_vulkan_call_end{nullptr};
  std::atomic<NoteSurfaceOutOfDateFn> note_surface_out_of_date{nullptr};
  bool extent_translation_logged = false;
  bool present_policy_logged = false;
  bool initialized = false;
};

static std::atomic<bool> g_vulkan_call_observation_active{false};

AdapterState& State() {
  static AdapterState state;
  return state;
}

template <typename Function>
Function ResolveProcessFunction(const char* name) {
  return reinterpret_cast<Function>(dlsym(RTLD_DEFAULT, name));
}

// Queue calls share the gate; device idle is exclusive.
// Release it before fence waits and backend callbacks.
std::shared_ptr<std::shared_mutex> DeviceIdleGate(VkDevice device) {
  static std::mutex registry_mutex;
  static std::unordered_map<VkDevice, std::shared_ptr<std::shared_mutex>>
      registry;
  std::lock_guard<std::mutex> lock(registry_mutex);
  auto &gate = registry[device];
  if (!gate)
    gate = std::make_shared<std::shared_mutex>();
  return gate;
}
std::shared_ptr<std::mutex> QueueCallMutex(VkQueue queue) {
  static std::mutex registry_mutex;
  static std::unordered_map<VkQueue, std::shared_ptr<std::mutex>> registry;
  std::lock_guard<std::mutex> lock(registry_mutex);
  auto &mutex = registry[queue];
  if (!mutex)
    mutex = std::make_shared<std::mutex>();
  return mutex;
}

void NoteVrSubmitted(VkQueue queue, uint32_t count,
                     const VkSubmitInfo *submits) {
  static const auto note = ResolveProcessFunction<void (*)(
      VkQueue, unsigned, const VkCommandBuffer *)>("mocktail_vr_xr_submitted");
  if (note && submits)
    for (uint32_t i = 0; i < count; ++i)
      note(queue, submits[i].commandBufferCount, submits[i].pCommandBuffers);
}
void NoteVrSubmitted2(VkQueue queue, uint32_t count,
                      const VkSubmitInfo2 *submits) {
  static const auto note = ResolveProcessFunction<void (*)(
      VkQueue, unsigned, const VkCommandBuffer *)>("mocktail_vr_xr_submitted");
  if (note && submits)
    for (uint32_t i = 0; i < count; ++i)
      for (uint32_t j = 0; j < submits[i].commandBufferInfoCount; ++j)
        note(queue, 1, &submits[i].pCommandBufferInfos[j].commandBuffer);
}

bool EnsureInitialized() {
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.initialized) {
    return true;
  }

  const auto register_submit =
      ResolveProcessFunction<void (*)(VkResult(VKAPI_PTR *)(
          PFN_vkQueueSubmit, VkDevice, VkQueue, uint32_t, const VkSubmitInfo *,
          VkFence))>("mocktail_vr_xr_queue_submit_adapter");
  if (register_submit)
    register_submit(mocktail_vulkan_submit_synchronized);
  const auto register_idle = ResolveProcessFunction<void (*)(
      VkResult(VKAPI_PTR *)(PFN_vkDeviceWaitIdle, VkDevice))>(
      "mocktail_vr_xr_device_idle_adapter");
  if (register_idle)
    register_idle(mocktail_vulkan_idle_synchronized);

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
  state.note_vr_present_begin.store(
      ResolveProcessFunction<NoteVrPresentBeginFn>(
          "mocktail_vr_note_host_present_begin"),
      std::memory_order_release);
  state.vr_create_instance.store(
      ResolveProcessFunction<VrXrCreateInstanceFn>(
          "mocktail_vr_xr_create_vulkan_instance"),
      std::memory_order_release);
  state.vr_create_device.store(
      ResolveProcessFunction<VrXrCreateDeviceFn>(
          "mocktail_vr_xr_create_vulkan_device"),
      std::memory_order_release);
  state.vr_note_device_destroyed.store(
      ResolveProcessFunction<VrXrNoteDeviceDestroyedFn>(
          "mocktail_vr_xr_note_device_destroyed"),
      std::memory_order_release);
  state.vr_note_queue.store(
      ResolveProcessFunction<VrXrNoteQueueFn>("mocktail_vr_xr_note_queue"),
      std::memory_order_release);
  state.vr_record_image.store(
      ResolveProcessFunction<VrXrRecordImageFn>("mocktail_vr_xr_record_image"),
      std::memory_order_release);
  state.vr_record_image_view.store(
      ResolveProcessFunction<VrXrRecordImageViewFn>(
          "mocktail_vr_xr_record_image_view"),
      std::memory_order_release);
  state.vr_record_framebuffer.store(
      ResolveProcessFunction<VrXrRecordFramebufferFn>(
          "mocktail_vr_xr_record_framebuffer"),
      std::memory_order_release);
  state.vr_record_render_pass.store(
      ResolveProcessFunction<VrXrRecordRenderPassFn>(
          "mocktail_vr_xr_record_render_pass"),
      std::memory_order_release);
  state.vr_note_destroy_image.store(
      ResolveProcessFunction<VrXrNoteDestroyImageFn>(
          "mocktail_vr_xr_note_destroy_image"),
      std::memory_order_release);
  state.vr_note_destroy_image_view.store(
      ResolveProcessFunction<VrXrNoteDestroyImageViewFn>(
          "mocktail_vr_xr_note_destroy_image_view"),
      std::memory_order_release);
  state.vr_note_destroy_framebuffer.store(
      ResolveProcessFunction<VrXrNoteDestroyFramebufferFn>(
          "mocktail_vr_xr_note_destroy_framebuffer"),
      std::memory_order_release);
  state.vr_note_render_pass_begin.store(
      ResolveProcessFunction<VrXrNoteRenderPassBeginFn>(
          "mocktail_vr_xr_note_render_pass_begin"),
      std::memory_order_release);
  state.vr_note_present.store(
      ResolveProcessFunction<VrXrNotePresentFn>("mocktail_vr_xr_note_present"),
      std::memory_order_release);
  const auto call_begin = ResolveProcessFunction<NoteVulkanCallBeginFn>(
      "mocktail_window_note_vulkan_call_begin");
  const auto call_end = ResolveProcessFunction<NoteVulkanCallEndFn>(
      "mocktail_window_note_vulkan_call_end");
  state.note_vulkan_call_begin.store(call_begin, std::memory_order_release);
  state.note_vulkan_call_end.store(call_end, std::memory_order_release);
  g_vulkan_call_observation_active.store(
      call_begin != nullptr && call_end != nullptr, std::memory_order_release);
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
                 "  [vulkan] accepted VK_SUBOPTIMAL_KHR; the acquired image "
                 "remains usable\n");
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
  const PFN_vkGetDeviceProcAddr host_get_device_proc_addr =
      State().host_get_device_proc_addr.load(std::memory_order_acquire);
  return host_get_device_proc_addr != nullptr
             ? host_get_device_proc_addr(device, name)
             : nullptr;
}

static std::atomic<uint64_t> g_dispatch_generation{1};

struct ThreadQueueDispatchCache {
  uint64_t generation = 0;
  VkQueue queue = VK_NULL_HANDLE;
  AdapterState::DeviceDispatch dispatch{};
};
static thread_local ThreadQueueDispatchCache t_queue_cache;

struct ThreadDeviceDispatchCache {
  uint64_t generation = 0;
  VkDevice device = VK_NULL_HANDLE;
  AdapterState::DeviceDispatch dispatch{};
};
static thread_local ThreadDeviceDispatchCache t_device_cache;

struct ThreadCommandBufferDispatchCache {
  uint64_t generation = 0;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  AdapterState::DeviceDispatch dispatch{};
};
static thread_local ThreadCommandBufferDispatchCache t_command_buffer_cache;

void InvalidateFastDispatchCaches() {
  g_dispatch_generation.fetch_add(1, std::memory_order_release);
}

static const AdapterState::DeviceDispatch kEmptyDeviceDispatch{};

const AdapterState::DeviceDispatch& HostDispatchForQueue(VkQueue queue) {
  const uint64_t gen = g_dispatch_generation.load(std::memory_order_acquire);
  if (__builtin_expect(queue != VK_NULL_HANDLE &&
                           t_queue_cache.queue == queue &&
                           t_queue_cache.generation == gen,
                       1)) {
    return t_queue_cache.dispatch;
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto binding =
      std::find_if(state.queue_bindings.begin(), state.queue_bindings.end(),
                   [queue](const AdapterState::QueueBinding& candidate) {
                     return candidate.queue == queue;
                   });
  if (binding == state.queue_bindings.end()) {
    return kEmptyDeviceDispatch;
  }
  const auto dispatch = std::find_if(
      state.device_dispatches.begin(), state.device_dispatches.end(),
      [binding](const AdapterState::DeviceDispatch& candidate) {
        return candidate.device == binding->device;
      });
  if (dispatch != state.device_dispatches.end()) {
    t_queue_cache.generation = gen;
    t_queue_cache.queue = queue;
    t_queue_cache.dispatch = *dispatch;
    return t_queue_cache.dispatch;
  }
  return kEmptyDeviceDispatch;
}

const AdapterState::DeviceDispatch& HostDispatchForDevice(VkDevice device) {
  const uint64_t gen = g_dispatch_generation.load(std::memory_order_acquire);
  if (__builtin_expect(device != VK_NULL_HANDLE &&
                           t_device_cache.device == device &&
                           t_device_cache.generation == gen,
                       1)) {
    return t_device_cache.dispatch;
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto dispatch = std::find_if(
      state.device_dispatches.begin(), state.device_dispatches.end(),
      [device](const AdapterState::DeviceDispatch& candidate) {
        return candidate.device == device;
      });
  if (dispatch != state.device_dispatches.end()) {
    t_device_cache.generation = gen;
    t_device_cache.device = device;
    t_device_cache.dispatch = *dispatch;
    return t_device_cache.dispatch;
  }
  return kEmptyDeviceDispatch;
}

const AdapterState::DeviceDispatch& HostDispatchForCommandBuffer(
    VkCommandBuffer command_buffer) {
  const uint64_t gen = g_dispatch_generation.load(std::memory_order_acquire);
  if (__builtin_expect(command_buffer != VK_NULL_HANDLE &&
                           t_command_buffer_cache.command_buffer ==
                               command_buffer &&
                           t_command_buffer_cache.generation == gen,
                       1)) {
    return t_command_buffer_cache.dispatch;
  }
  AdapterState& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto binding = std::find_if(
      state.command_buffer_bindings.begin(),
      state.command_buffer_bindings.end(),
      [command_buffer](const AdapterState::CommandBufferBinding& candidate) {
        return candidate.command_buffer == command_buffer;
      });
  if (binding == state.command_buffer_bindings.end()) {
    return kEmptyDeviceDispatch;
  }
  const auto dispatch = std::find_if(
      state.device_dispatches.begin(), state.device_dispatches.end(),
      [binding](const AdapterState::DeviceDispatch& candidate) {
        return candidate.device == binding->device;
      });
  if (dispatch != state.device_dispatches.end()) {
    t_command_buffer_cache.generation = gen;
    t_command_buffer_cache.command_buffer = command_buffer;
    t_command_buffer_cache.dispatch = *dispatch;
    return t_command_buffer_cache.dispatch;
  }
  return kEmptyDeviceDispatch;
}

VkResult VKAPI_CALL LockedHostSubmit(VkQueue queue, uint32_t count,
                                     const VkSubmitInfo *submits,
                                     VkFence fence) {
  return mocktail_vulkan_submit_synchronized(
      HostDispatchForQueue(queue).queue_submit,
      HostDispatchForQueue(queue).device, queue, count, submits, fence);
}
VkResult VKAPI_CALL LockedHostSubmit2(VkQueue queue, uint32_t count,
                                      const VkSubmitInfo2 *submits,
                                      VkFence fence) {
  const auto raw = HostDispatchForQueue(queue).queue_submit2;
  const auto gate = DeviceIdleGate(HostDispatchForQueue(queue).device);
  std::shared_lock<std::shared_mutex> device_gate(*gate);
  const auto mutex = QueueCallMutex(queue);
  std::lock_guard<std::mutex> lock(*mutex);
  return raw(queue, count, submits, fence);
}
VkResult VKAPI_CALL LockedHostSubmit2KHR(VkQueue queue, uint32_t count,
                                         const VkSubmitInfo2 *submits,
                                         VkFence fence) {
  const auto raw = HostDispatchForQueue(queue).queue_submit2_khr;
  const auto gate = DeviceIdleGate(HostDispatchForQueue(queue).device);
  std::shared_lock<std::shared_mutex> device_gate(*gate);
  const auto mutex = QueueCallMutex(queue);
  std::lock_guard<std::mutex> lock(*mutex);
  return raw(queue, count, submits, fence);
}
VkResult VKAPI_CALL LockedHostBindSparse(VkQueue queue, uint32_t count,
                                         const VkBindSparseInfo *binds,
                                         VkFence fence) {
  const auto raw = HostDispatchForQueue(queue).queue_bind_sparse;
  const auto gate = DeviceIdleGate(HostDispatchForQueue(queue).device);
  std::shared_lock<std::shared_mutex> device_gate(*gate);
  const auto mutex = QueueCallMutex(queue);
  std::lock_guard<std::mutex> lock(*mutex);
  return raw(queue, count, binds, fence);
}
VkResult VKAPI_CALL LockedHostQueueWaitIdle(VkQueue queue) {
  const auto raw = HostDispatchForQueue(queue).queue_wait_idle;
  const auto gate = DeviceIdleGate(HostDispatchForQueue(queue).device);
  std::shared_lock<std::shared_mutex> device_gate(*gate);
  const auto mutex = QueueCallMutex(queue);
  std::lock_guard<std::mutex> lock(*mutex);
  return raw(queue);
}

VkResult VKAPI_CALL LockedHostDeviceWaitIdle(VkDevice device) {
  return mocktail_vulkan_idle_synchronized(
      HostDispatchForDevice(device).device_wait_idle, device);
}

void RegisterHostDeviceDispatch(VkDevice device,
                                VkPhysicalDevice physical_device,
                                PFN_vkGetDeviceProcAddr get_device_proc_addr) {
  if (device == VK_NULL_HANDLE || get_device_proc_addr == nullptr) {
    return;
  }
  AdapterState::DeviceDispatch dispatch;
  dispatch.device = device;
  dispatch.physical_device = physical_device;
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
  dispatch.acquire_next_image = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
      get_device_proc_addr(device, "vkAcquireNextImageKHR"));
  dispatch.acquire_next_image2 = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(
      get_device_proc_addr(device, "vkAcquireNextImage2KHR"));
  dispatch.wait_for_fences = reinterpret_cast<PFN_vkWaitForFences>(
      get_device_proc_addr(device, "vkWaitForFences"));
  dispatch.wait_semaphores = reinterpret_cast<PFN_vkWaitSemaphores>(
      get_device_proc_addr(device, "vkWaitSemaphores"));
  dispatch.wait_semaphores_khr = reinterpret_cast<PFN_vkWaitSemaphoresKHR>(
      get_device_proc_addr(device, "vkWaitSemaphoresKHR"));
  dispatch.device_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(
      get_device_proc_addr(device, "vkDeviceWaitIdle"));
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
  dispatch.create_image = reinterpret_cast<PFN_vkCreateImage>(
      get_device_proc_addr(device, "vkCreateImage"));
  dispatch.destroy_image = reinterpret_cast<PFN_vkDestroyImage>(
      get_device_proc_addr(device, "vkDestroyImage"));
  dispatch.create_image_view = reinterpret_cast<PFN_vkCreateImageView>(
      get_device_proc_addr(device, "vkCreateImageView"));
  dispatch.destroy_image_view = reinterpret_cast<PFN_vkDestroyImageView>(
      get_device_proc_addr(device, "vkDestroyImageView"));
  dispatch.create_framebuffer = reinterpret_cast<PFN_vkCreateFramebuffer>(
      get_device_proc_addr(device, "vkCreateFramebuffer"));
  dispatch.destroy_framebuffer = reinterpret_cast<PFN_vkDestroyFramebuffer>(
      get_device_proc_addr(device, "vkDestroyFramebuffer"));
  dispatch.create_render_pass = reinterpret_cast<PFN_vkCreateRenderPass>(
      get_device_proc_addr(device, "vkCreateRenderPass"));
  dispatch.destroy_render_pass = reinterpret_cast<PFN_vkDestroyRenderPass>(
      get_device_proc_addr(device, "vkDestroyRenderPass"));
  dispatch.cmd_begin_render_pass = reinterpret_cast<PFN_vkCmdBeginRenderPass>(
      get_device_proc_addr(device, "vkCmdBeginRenderPass"));

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
  InvalidateFastDispatchCaches();
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
  InvalidateFastDispatchCaches();
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
  InvalidateFastDispatchCaches();
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
  InvalidateFastDispatchCaches();
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
  InvalidateFastDispatchCaches();
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
  InvalidateFastDispatchCaches();
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
#define MOCKTAIL_VK_PROC(function)                                             \
  if (std::strcmp(name, #function) == 0) {                                     \
    return reinterpret_cast<PFN_vkVoidFunction>(function);                     \
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
  MOCKTAIL_VK_PROC(vkCreateCommandPool)
  MOCKTAIL_VK_PROC(vkCmdPipelineBarrier)
  MOCKTAIL_VK_PROC(vkCmdPipelineBarrier2)
  MOCKTAIL_VK_PROC(vkCmdPipelineBarrier2KHR)
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
  MOCKTAIL_VK_PROC(vkCreateImage)
  MOCKTAIL_VK_PROC(vkDestroyImage)
  MOCKTAIL_VK_PROC(vkCreateImageView)
  MOCKTAIL_VK_PROC(vkDestroyImageView)
  MOCKTAIL_VK_PROC(vkCreateFramebuffer)
  MOCKTAIL_VK_PROC(vkDestroyFramebuffer)
  MOCKTAIL_VK_PROC(vkCreateRenderPass)
  MOCKTAIL_VK_PROC(vkDestroyRenderPass)
  MOCKTAIL_VK_PROC(vkCmdBeginRenderPass)
#undef MOCKTAIL_VK_PROC
  return nullptr;
}

bool IsDeviceAdapterProc(const char *name) {
  return name != nullptr &&
         (std::strcmp(name, "vkDestroyDevice") == 0 ||
          std::strcmp(name, "vkGetDeviceQueue") == 0 ||
          std::strcmp(name, "vkGetDeviceQueue2") == 0 ||
          std::strcmp(name, "vkCreateSwapchainKHR") == 0 ||
          std::strcmp(name, "vkDestroySwapchainKHR") == 0 ||
          std::strcmp(name, "vkAcquireNextImageKHR") == 0 ||
          std::strcmp(name, "vkAcquireNextImage2KHR") == 0 ||
          std::strcmp(name, "vkWaitForFences") == 0 ||
          std::strcmp(name, "vkResetFences") == 0 ||
          std::strcmp(name, "vkCreateCommandPool") == 0 ||
          std::strcmp(name, "vkCmdPipelineBarrier") == 0 ||
          std::strcmp(name, "vkCmdPipelineBarrier2") == 0 ||
          std::strcmp(name, "vkCmdPipelineBarrier2KHR") == 0 ||
          std::strcmp(name, "vkResetCommandPool") == 0 ||
          std::strcmp(name, "vkGetQueryPoolResults") == 0 ||
          std::strcmp(name, "vkAllocateCommandBuffers") == 0 ||
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
          std::strcmp(name, "vkQueuePresentKHR") == 0 ||
          std::strcmp(name, "vkCreateImage") == 0 ||
          std::strcmp(name, "vkDestroyImage") == 0 ||
          std::strcmp(name, "vkCreateImageView") == 0 ||
          std::strcmp(name, "vkDestroyImageView") == 0 ||
          std::strcmp(name, "vkCreateFramebuffer") == 0 ||
          std::strcmp(name, "vkDestroyFramebuffer") == 0 ||
          std::strcmp(name, "vkCreateRenderPass") == 0 ||
          std::strcmp(name, "vkDestroyRenderPass") == 0 ||
          std::strcmp(name, "vkCmdBeginRenderPass") == 0);
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
    if (__builtin_expect(
            g_vulkan_call_observation_active.load(std::memory_order_relaxed),
            0)) {
      AdapterState& state = State();
      begin_ = state.note_vulkan_call_begin.load(std::memory_order_acquire);
      end_ = state.note_vulkan_call_end.load(std::memory_order_acquire);
      if (begin_ != nullptr && end_ != nullptr) {
        sequence_ = begin_(call_name);
      }
    }
  }

  ~VulkanCallObservation() {
    if (__builtin_expect(sequence_ != 0 && end_ != nullptr, 0)) {
      end_(sequence_, static_cast<std::int32_t>(result_));
    }
  }

  VulkanCallObservation(const VulkanCallObservation&) = delete;
  VulkanCallObservation& operator=(const VulkanCallObservation&) = delete;

  void SetResult(VkResult result) {
    result_ = result;
    if (__builtin_expect(
            result < VK_SUCCESS && result != VK_ERROR_OUT_OF_DATE_KHR, 0)) {
      std::uint64_t occurrence = 0;
      if (ShouldLogNegativeVulkanResult(result, &occurrence)) {
        std::fprintf(stderr,
                     "  [vulkan] unexpected negative result: call=%s "
                     "result=%d device_lost=%u occurrence=%llu\n",
                     call_name_ != nullptr ? call_name_ : "unknown",
                     static_cast<int>(result),
                     result == VK_ERROR_DEVICE_LOST ? 1U : 0U,
                     static_cast<unsigned long long>(occurrence));
      }
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
  static_cast<void>(call_name);
  const VkResult result = host_wait(device, wait_info, timeout);
  if (observation != nullptr) {
    observation->SetResult(result);
  }
  return result;
}

bool FpsTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("MOCKTAIL_TRACE_FPS");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

std::uint64_t MonotonicNanos() {
  timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

struct FpsWaitTrace {
  const char* name = nullptr;
  std::atomic<std::uint64_t> samples{0};
  std::atomic<std::uint64_t> total_ns{0};
  std::atomic<std::uint64_t> max_ns{0};
  std::atomic<std::uint64_t> window_start_ns{0};

  void Record(std::uint64_t start_ns) {
    if (name == nullptr || start_ns == 0) {
      return;
    }
    const std::uint64_t wait_ns = MonotonicNanos() - start_ns;
    total_ns.fetch_add(wait_ns, std::memory_order_relaxed);
    std::uint64_t max_wait = max_ns.load(std::memory_order_relaxed);
    while (wait_ns > max_wait &&
           !max_ns.compare_exchange_weak(max_wait, wait_ns,
                                         std::memory_order_relaxed)) {
    }
    const std::uint64_t n = samples.fetch_add(1, std::memory_order_relaxed) + 1;
    std::uint64_t window = window_start_ns.load(std::memory_order_relaxed);
    if (window == 0) {
      window_start_ns.compare_exchange_strong(window, start_ns,
                                              std::memory_order_relaxed);
      window = window_start_ns.load(std::memory_order_relaxed);
    }
    if (start_ns - window < 1000000000ULL || n == 0) {
      return;
    }
    const std::uint64_t total = total_ns.exchange(0, std::memory_order_relaxed);
    const std::uint64_t peak = max_ns.exchange(0, std::memory_order_relaxed);
    const std::uint64_t count = samples.exchange(0, std::memory_order_relaxed);
    window_start_ns.store(start_ns, std::memory_order_relaxed);
    if (count == 0) {
      return;
    }
    std::fprintf(stderr, "  [fps] %s n=%llu avg=%llu us max=%llu us start_ns=%llu end_ns=%llu\n", name,
                 static_cast<unsigned long long>(count),
                 static_cast<unsigned long long>(total / count / 1000ULL),
                 static_cast<unsigned long long>(peak / 1000ULL),
                 static_cast<unsigned long long>(window),
                 static_cast<unsigned long long>(start_ns));
  }
};

FpsWaitTrace& PresentWaitTrace() {
  static FpsWaitTrace trace{"vkQueuePresentKHR"};
  return trace;
}

FpsWaitTrace& AcquireWaitTrace() {
  static FpsWaitTrace trace{"vkAcquireNextImageKHR"};
  return trace;
}

FpsWaitTrace& FenceWaitTrace() {
  static FpsWaitTrace trace{"vkWaitForFences"};
  return trace;
}

FpsWaitTrace& QueueIdleWaitTrace() {
  static FpsWaitTrace trace{"vkQueueWaitIdle"};
  return trace;
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
  const NoteVrPresentBeginFn vr_note_begin =
      state.note_vr_present_begin.load(std::memory_order_acquire);
  if (vr_note_begin != nullptr) {
    vr_note_begin();
  }
  const VrXrNotePresentFn vr_note_present =
      state.vr_note_present.load(std::memory_order_acquire);
  VkPresentInfoKHR synchronized{};
  if (vr_note_present != nullptr) {
    static const auto prepare = ResolveProcessFunction<VkResult (*)(
        VkQueue, VkDevice, const VkPresentInfoKHR *)>(
        "mocktail_vr_xr_prepare_present");
    // Mirror/overlay may have consumed waits; use the final dependency list.
    const VkResult prepared =
        prepare
            ? prepare(queue, HostDispatchForQueue(queue).device, present_info)
            : VK_NOT_READY;
    if (prepared < 0)
      return prepared;
    if (prepared == VK_SUCCESS) {
      vr_note_present(queue, HostDispatchForQueue(queue).device);
      synchronized = *present_info;
      synchronized.waitSemaphoreCount = 0;
      synchronized.pWaitSemaphores = nullptr;
      present_info = &synchronized;
    }
  }
  const bool fps_trace = FpsTraceEnabled();
  const std::uint64_t present_start_ns = fps_trace ? MonotonicNanos() : 0;
  const VkResult result = [&] {
    const auto gate = DeviceIdleGate(HostDispatchForQueue(queue).device);
    std::shared_lock<std::shared_mutex> device_gate(*gate);
    const auto mutex = QueueCallMutex(queue);
    std::lock_guard<std::mutex> lock(*mutex);
    return host_present(queue, present_info);
  }();
  if (fps_trace) {
    PresentWaitTrace().Record(present_start_ns);
  }
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
  VkResult result = VK_ERROR_INITIALIZATION_FAILED;
  bool vr_handled = false;
  const auto vr_create_instance =
      State().vr_create_instance.load(std::memory_order_acquire);
  if (vr_create_instance != nullptr) {
    // The XR backend creates the instance through xrCreateVulkanInstanceKHR
    // so the runtime's required instance extensions are enabled on the very
    // instance Roblox renders with.
    vr_handled =
        vr_create_instance(&host_info, allocator, instance, &result);
  }
  if (!vr_handled) {
    result = host_create(&host_info, allocator, instance);
  }
  if (result == VK_SUCCESS) {
    AdapterState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.active_instance_count;
    state.latest_instance = *instance;
    state.host_get_device_proc_addr.store(
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            state.host_get_instance_proc_addr(*instance, "vkGetDeviceProcAddr")),
        std::memory_order_release);
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
  const auto note = ResolveProcessFunction<void (*)(VkInstance)>(
      "mocktail_vr_xr_note_instance_destroyed");
  if (note != nullptr) note(instance);

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
    state.host_get_device_proc_addr.store(nullptr, std::memory_order_release);
    state.device_dispatches.clear();
    state.queue_bindings.clear();
    state.command_buffer_bindings.clear();
    InvalidateFastDispatchCaches();
    state.host_surface_capabilities = nullptr;
    state.host_surface_present_modes = nullptr;
    state.latest_instance = VK_NULL_HANDLE;
    state.note_present.store(nullptr, std::memory_order_release);
    state.note_host_present_begin.store(nullptr, std::memory_order_release);
    state.note_host_present_end.store(nullptr, std::memory_order_release);
    state.note_vr_present_begin.store(nullptr, std::memory_order_release);
    state.vr_create_instance.store(nullptr, std::memory_order_release);
    state.vr_create_device.store(nullptr, std::memory_order_release);
    state.vr_note_device_destroyed.store(nullptr, std::memory_order_release);
    state.vr_note_queue.store(nullptr, std::memory_order_release);
    state.vr_record_image.store(nullptr, std::memory_order_release);
    state.vr_record_image_view.store(nullptr, std::memory_order_release);
    state.vr_record_framebuffer.store(nullptr, std::memory_order_release);
    state.vr_record_render_pass.store(nullptr, std::memory_order_release);
    state.vr_note_destroy_image.store(nullptr, std::memory_order_release);
    state.vr_note_destroy_image_view.store(nullptr, std::memory_order_release);
    state.vr_note_destroy_framebuffer.store(nullptr,
                                            std::memory_order_release);
    state.vr_note_render_pass_begin.store(nullptr, std::memory_order_release);
    state.vr_note_present.store(nullptr, std::memory_order_release);
    state.note_vulkan_call_begin.store(nullptr, std::memory_order_release);
    state.note_vulkan_call_end.store(nullptr, std::memory_order_release);
    g_vulkan_call_observation_active.store(false, std::memory_order_release);
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
  const PFN_vkGetDeviceProcAddr host_get_device_proc_addr =
      State().host_get_device_proc_addr.load(std::memory_order_acquire);
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
    host_get_device_proc_addr =
        state.host_get_device_proc_addr.load(std::memory_order_acquire);
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

  VkResult result = VK_ERROR_INITIALIZATION_FAILED;
  bool vr_handled = false;
  const auto vr_create_device =
      State().vr_create_device.load(std::memory_order_acquire);
  if (vr_create_device != nullptr) {
    // The XR backend creates the device through xrCreateVulkanDeviceKHR and
    // binds the OpenXR session to it; falling back afterwards would create a
    // second, XR-incompatible device, so the hook result is final.
    vr_handled = vr_create_device(physical_device, &host_info, allocator,
                                  device, &result);
  }
  if (!vr_handled) {
    result = host_create(physical_device, &host_info, allocator, device);
  }
  if (result != VK_SUCCESS) {
    return result;
  }
  RegisterHostDeviceDispatch(*device, physical_device,
                             host_get_device_proc_addr);

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
  const auto vr_note_device_destroyed =
      State().vr_note_device_destroyed.load(std::memory_order_acquire);
  if (vr_note_device_destroyed != nullptr) {
    // Runs while the device is still valid so the XR backend can destroy
    // its session and swapchain resources first.
    vr_note_device_destroyed(device);
  }
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
    const auto vr_note_queue =
        State().vr_note_queue.load(std::memory_order_acquire);
    if (vr_note_queue != nullptr) {
      vr_note_queue(device, *queue, queue_family_index, queue_index);
    }
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
    const auto vr_note_queue =
        State().vr_note_queue.load(std::memory_order_acquire);
    if (vr_note_queue != nullptr) {
      vr_note_queue(device, *queue, queue_info->queueFamilyIndex,
                    queue_info->queueIndex);
    }
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
  VkSwapchainCreateInfoKHR host_info = *create_info;
  unsigned vr_families[256]{};
  bool converted_to_concurrent = false;
  static const auto shared_families =
      ResolveProcessFunction<unsigned (*)(VkDevice, unsigned, unsigned *)>(
          "mocktail_vr_desktop_queue_families");
  if (shared_families &&
      host_info.imageSharingMode == VK_SHARING_MODE_EXCLUSIVE) {
    const unsigned count = shared_families(device, 256, vr_families);
    if (count > 1) {
      host_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
      host_info.queueFamilyIndexCount = count;
      host_info.pQueueFamilyIndices = vr_families;
      converted_to_concurrent = true;
    }
  }
  static const auto vr_desktop_enabled =
      ResolveProcessFunction<bool (*)()>("mocktail_vr_desktop_enabled");
  if (vr_desktop_enabled && vr_desktop_enabled()) {
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR caps;
    { std::lock_guard<std::mutex> lock(State().mutex); caps = State().host_surface_capabilities; }
    VkSurfaceCapabilitiesKHR supported{};
    if (caps && caps(HostDispatchForDevice(device).physical_device, host_info.surface,
                     &supported) == VK_SUCCESS &&
        (supported.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
      host_info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  const mocktail::graphics::PresentModePolicy present_policy =
      mocktail::graphics::CachedPresentModePolicy();
  if (present_policy != mocktail::graphics::PresentModePolicy::kHostDefault) {
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR host_caps = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR host_modes_query = nullptr;
    {
      AdapterState& state = State();
      std::lock_guard<std::mutex> lock(state.mutex);
      host_caps = state.host_surface_capabilities;
      host_modes_query = state.host_surface_present_modes;
    }
    const VkPhysicalDevice physical_device =
        HostDispatchForDevice(device).physical_device;
    if (host_modes_query != nullptr && physical_device != VK_NULL_HANDLE) {
      std::uint32_t host_count = 0;
      if (host_modes_query(physical_device, create_info->surface, &host_count,
                           nullptr) == VK_SUCCESS &&
          host_count != 0) {
        std::vector<VkPresentModeKHR> host_modes(host_count);
        const VkResult modes_result = host_modes_query(
            physical_device, create_info->surface, &host_count,
            host_modes.data());
        if (modes_result == VK_SUCCESS || modes_result == VK_INCOMPLETE) {
          host_modes.resize(host_count);
          const std::vector<VkPresentModeKHR> selected =
              mocktail::graphics::FilterPresentModes(present_policy,
                                                     host_modes);
          if (!selected.empty()) {
            static std::atomic<bool> mode_logged{false};
            if (!mode_logged.exchange(true, std::memory_order_relaxed)) {
              std::fprintf(
                  stderr,
                  "  [vulkan] swapchain presentMode requested=%s applied=%s\n",
                  mocktail::graphics::PresentModeKhrName(
                      create_info->presentMode),
                  mocktail::graphics::PresentModeKhrName(selected.front()));
            }
            host_info.presentMode = selected.front();
          }
        }
      }
    }
    VkSurfaceCapabilitiesKHR capabilities{};
    if (host_caps != nullptr && physical_device != VK_NULL_HANDLE &&
        host_caps(physical_device, create_info->surface, &capabilities) ==
            VK_SUCCESS) {
      const std::uint32_t preferred =
          mocktail::graphics::PreferSwapchainMinImageCount(
              present_policy, create_info->minImageCount,
              capabilities.minImageCount, capabilities.maxImageCount);
      if (preferred != host_info.minImageCount) {
        host_info.minImageCount = preferred;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
          std::fprintf(stderr,
                       "  [vulkan] present policy=%s swapchain "
                       "minImageCount=%u (requested=%u max=%u)\n",
                       mocktail::graphics::PresentModePolicyName(
                           present_policy),
                       preferred, create_info->minImageCount,
                       capabilities.maxImageCount);
        }
      }
    }
  }
  const VkResult result =
      host_create(device, &host_info, allocator, swapchain);
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
    static const auto record = ResolveProcessFunction<void (*)(
        VkDevice, VkSwapchainKHR, const VkSwapchainCreateInfoKHR *,
        const VkImage *, unsigned, bool)>(
        "mocktail_vr_desktop_swapchain_shared");
    if (record)
      record(device, *swapchain, &host_info, images.data(), image_count,
             converted_to_concurrent);
    (void)State().text_overlay.RegisterSwapchain(device, *swapchain, host_info,
                                                 images.data(), image_count);
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                      const VkAllocationCallbacks* allocator) {
  const auto host_destroy = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
      HostDeviceProc(device, "vkDestroySwapchainKHR"));
  static const auto record = ResolveProcessFunction<void (*)(VkDevice, VkSwapchainKHR,
      const VkSwapchainCreateInfoKHR*, const VkImage*, unsigned)>("mocktail_vr_desktop_swapchain");
  if (record) record(device, swapchain, nullptr, nullptr, 0);
  State().text_overlay.DestroySwapchain(device, swapchain);
  if (host_destroy != nullptr) {
    host_destroy(device, swapchain, allocator);
  }
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, std::uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, std::uint32_t* image_index) {
  VulkanCallObservation observation("vkAcquireNextImageKHR");
  const PFN_vkAcquireNextImageKHR host_acquire =
      HostDispatchForDevice(device).acquire_next_image;
  if (host_acquire == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const bool fps_trace = FpsTraceEnabled();
  const std::uint64_t acquire_start_ns = fps_trace ? MonotonicNanos() : 0;
  const VkResult host_result = host_acquire(
      device, swapchain, timeout, semaphore, fence, image_index);
  if (fps_trace) {
    AcquireWaitTrace().Record(acquire_start_ns);
  }
  const VkResult result = NormalizeSwapchainResult(host_result);
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
  const PFN_vkAcquireNextImage2KHR host_acquire =
      HostDispatchForDevice(device).acquire_next_image2;
  if (host_acquire == nullptr || acquire_info == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult host_result =
      host_acquire(device, acquire_info, image_index);
  const VkResult result = NormalizeSwapchainResult(host_result);
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
  const PFN_vkWaitForFences host_wait =
      HostDispatchForDevice(device).wait_for_fences;
  if (host_wait == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const bool fps_trace = FpsTraceEnabled();
  const std::uint64_t fence_start_ns = fps_trace ? MonotonicNanos() : 0;
  const VkResult result =
      host_wait(device, fence_count, fences, wait_all, timeout);
  if (fps_trace) {
    FenceWaitTrace().Record(fence_start_ns);
  }
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
  static const auto note = ResolveProcessFunction<void (*)(VkCommandPool)>(
      "mocktail_vr_xr_reset_pool");
  if (result == VK_SUCCESS && note)
    note(command_pool);
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
    static const auto note = ResolveProcessFunction<void (*)(
        VkDevice, VkCommandPool, unsigned, const VkCommandBuffer *)>(
        "mocktail_vr_xr_command_buffers");
    if (note)
      note(device, allocate_info->commandPool,
           allocate_info->commandBufferCount, command_buffers);
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
  static const auto note = ResolveProcessFunction<void (*)(
      VkDevice, VkCommandPool, unsigned, const VkCommandBuffer *)>(
      "mocktail_vr_xr_command_buffers");
  if (note)
    note(device, VK_NULL_HANDLE, command_buffer_count, command_buffers);
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
  static const auto note =
      ResolveProcessFunction<void (*)(VkDevice, VkCommandPool, unsigned)>(
          "mocktail_vr_xr_command_pool");
  if (note)
    note(device, command_pool, UINT32_MAX);
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
  static const auto note = ResolveProcessFunction<void (*)(VkCommandBuffer)>(
      "mocktail_vr_xr_reset_command");
  if (result == VK_SUCCESS && note)
    note(command_buffer);
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
  static const auto note = ResolveProcessFunction<void (*)(VkCommandBuffer)>(
      "mocktail_vr_xr_reset_command");
  if (result == VK_SUCCESS && note)
    note(command_buffer);
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(
    VkDevice device, const VkSemaphoreWaitInfo* wait_info,
    std::uint64_t timeout) {
  VulkanCallObservation observation("vkWaitSemaphores");
  const PFN_vkWaitSemaphores host_wait =
      HostDispatchForDevice(device).wait_semaphores;
  return WaitForSemaphoresObserved("vkWaitSemaphores", host_wait, device,
                                   wait_info, timeout, &observation);
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphoresKHR(
    VkDevice device, const VkSemaphoreWaitInfo* wait_info,
    std::uint64_t timeout) {
  VulkanCallObservation observation("vkWaitSemaphoresKHR");
  const PFN_vkWaitSemaphoresKHR host_wait =
      HostDispatchForDevice(device).wait_semaphores_khr;
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
      queue, submit_count, submits, fence, LockedHostSubmit);
  if (result == VK_SUCCESS)
    NoteVrSubmitted(queue, submit_count, submits);
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
      queue, submit_count, submits, fence, LockedHostSubmit2);
  if (result == VK_SUCCESS)
    NoteVrSubmitted2(queue, submit_count, submits);
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
      queue, submit_count, submits, fence, LockedHostSubmit2KHR);
  if (result == VK_SUCCESS)
    NoteVrSubmitted2(queue, submit_count, submits);
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
      queue, bind_info_count, bind_info, fence, LockedHostBindSparse);
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
  const bool fps_trace = FpsTraceEnabled();
  const std::uint64_t idle_start_ns = fps_trace ? MonotonicNanos() : 0;
  const VkResult result =
      State().text_overlay.QueueWaitIdle(queue, LockedHostQueueWaitIdle);
  if (fps_trace) {
    QueueIdleWaitTrace().Record(idle_start_ns);
  }
  observation.SetResult(result);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
  VulkanCallObservation observation("vkDeviceWaitIdle");
  const PFN_vkDeviceWaitIdle host_wait =
      HostDispatchForDevice(device).device_wait_idle;
  if (host_wait == nullptr) {
    observation.SetResult(VK_ERROR_INITIALIZATION_FAILED);
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result =
      State().text_overlay.DeviceWaitIdle(device, LockedHostDeviceWaitIdle);
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
  const mocktail::graphics::PresentModePolicy policy = mocktail::graphics::CachedPresentModePolicy();
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
  static const auto mirror = ResolveProcessFunction<VkResult (*)(VkQueue, const VkPresentInfoKHR*)>(
      "mocktail_vr_desktop_present");
  VkPresentInfoKHR mirrored{};
  if (mirror && present_info) {
    const VkResult mirror_result = mirror(queue, present_info);
    if (mirror_result < 0) return mirror_result;
    if (mirror_result == VK_SUCCESS) {
      mirrored = *present_info;
      mirrored.waitSemaphoreCount = 0;
      mirrored.pWaitSemaphores = nullptr;
      present_info = &mirrored;
    }
  }
  const VkResult result = state.text_overlay.QueuePresent(
      queue, present_info, ObservedHostQueuePresent);
  const VkResult normalized_result = NormalizeSwapchainResult(result);
  if (normalized_result == VK_ERROR_OUT_OF_DATE_KHR) {
    const NoteSurfaceOutOfDateFn note_surface_out_of_date =
        state.note_surface_out_of_date.load(std::memory_order_acquire);
    if (note_surface_out_of_date != nullptr) {
      note_surface_out_of_date();
    }
  }
  if (normalized_result == VK_SUCCESS && note_present != nullptr) {
    note_present();
  }
  if (present_info != nullptr && present_info->pResults != nullptr) {
    for (std::uint32_t index = 0; index < present_info->swapchainCount;
         ++index) {
      present_info->pResults[index] =
          NormalizeSwapchainResult(present_info->pResults[index]);
    }
  }
  observation.SetResult(normalized_result);
  return normalized_result;
}

// ---- VR backend resource provenance wrappers -------------------------------
// Thin pass-throughs to the host dispatch with optional notifications for the
// experimental OpenXR backend. Creation/destruction calls are rare; the only
// per-frame entry (vkCmdBeginRenderPass) costs one atomic load and stops
// doing work once both eye bindings are established.

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(
    VkDevice device, const VkImageCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkImage* image) {
  const PFN_vkCreateImage host_create =
      HostDispatchForDevice(device).create_image;
  if (host_create == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_create(device, create_info, allocator, image);
  const auto record = State().vr_record_image.load(std::memory_order_acquire);
  if (result == VK_SUCCESS && record != nullptr) {
    record(device, *image, create_info);
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image,
                                          const VkAllocationCallbacks* allocator) {
  const auto note = State().vr_note_destroy_image.load(std::memory_order_acquire);
  if (note != nullptr && image != VK_NULL_HANDLE) {
    note(image);
  }
  const PFN_vkDestroyImage host_destroy =
      HostDispatchForDevice(device).destroy_image;
  if (host_destroy != nullptr) {
    host_destroy(device, image, allocator);
  }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkImageView* view) {
  const PFN_vkCreateImageView host_create =
      HostDispatchForDevice(device).create_image_view;
  if (host_create == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_create(device, create_info, allocator, view);
  const auto record =
      State().vr_record_image_view.load(std::memory_order_acquire);
  if (result == VK_SUCCESS && record != nullptr && create_info != nullptr) {
    record(*view, create_info->image);
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyImageView(VkDevice device, VkImageView image_view,
                   const VkAllocationCallbacks* allocator) {
  const auto note =
      State().vr_note_destroy_image_view.load(std::memory_order_acquire);
  if (note != nullptr && image_view != VK_NULL_HANDLE) {
    note(image_view);
  }
  const PFN_vkDestroyImageView host_destroy =
      HostDispatchForDevice(device).destroy_image_view;
  if (host_destroy != nullptr) {
    host_destroy(device, image_view, allocator);
  }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(
    VkDevice device, const VkFramebufferCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkFramebuffer* framebuffer) {
  const PFN_vkCreateFramebuffer host_create =
      HostDispatchForDevice(device).create_framebuffer;
  if (host_create == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_create(device, create_info, allocator, framebuffer);
  const auto record =
      State().vr_record_framebuffer.load(std::memory_order_acquire);
  if (result == VK_SUCCESS && record != nullptr) {
    record(*framebuffer, create_info);
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer,
                     const VkAllocationCallbacks* allocator) {
  const auto note =
      State().vr_note_destroy_framebuffer.load(std::memory_order_acquire);
  if (note != nullptr && framebuffer != VK_NULL_HANDLE) {
    note(framebuffer);
  }
  const PFN_vkDestroyFramebuffer host_destroy =
      HostDispatchForDevice(device).destroy_framebuffer;
  if (host_destroy != nullptr) {
    host_destroy(device, framebuffer, allocator);
  }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(
    VkDevice device, const VkRenderPassCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkRenderPass* render_pass) {
  const PFN_vkCreateRenderPass host_create =
      HostDispatchForDevice(device).create_render_pass;
  if (host_create == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_create(device, create_info, allocator, render_pass);
  const auto record =
      State().vr_record_render_pass.load(std::memory_order_acquire);
  if (result == VK_SUCCESS && record != nullptr) {
    record(*render_pass, create_info);
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL
vkDestroyRenderPass(VkDevice device, VkRenderPass render_pass,
                    const VkAllocationCallbacks* allocator) {
  const PFN_vkDestroyRenderPass host_destroy =
      HostDispatchForDevice(device).destroy_render_pass;
  if (host_destroy != nullptr) {
    host_destroy(device, render_pass, allocator);
  }
}

VKAPI_ATTR void VKAPI_CALL
vkCmdBeginRenderPass(VkCommandBuffer command_buffer,
                     const VkRenderPassBeginInfo* begin_info,
                     VkSubpassContents contents) {
  const auto note =
      State().vr_note_render_pass_begin.load(std::memory_order_acquire);
  if (note != nullptr) {
    note(command_buffer, begin_info);
  }
  const PFN_vkCmdBeginRenderPass host_begin =
      HostDispatchForCommandBuffer(command_buffer).cmd_begin_render_pass;
  if (host_begin != nullptr) {
    host_begin(command_buffer, begin_info, contents);
  }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(
    VkDevice device, const VkCommandPoolCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkCommandPool *pool) {
  const auto create = reinterpret_cast<PFN_vkCreateCommandPool>(
      HostDeviceProc(device, "vkCreateCommandPool"));
  if (!create)
    return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = create(device, info, allocator, pool);
  static const auto note =
      ResolveProcessFunction<void (*)(VkDevice, VkCommandPool, unsigned)>(
          "mocktail_vr_xr_command_pool");
  if (result == VK_SUCCESS && note && info && pool)
    note(device, *pool, info->queueFamilyIndex);
  return result;
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(
    VkCommandBuffer command, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t memory_count,
    const VkMemoryBarrier *memory, uint32_t buffer_count,
    const VkBufferMemoryBarrier *buffers, uint32_t image_count,
    const VkImageMemoryBarrier *images) {
  const auto barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(
      HostDeviceProc(HostDispatchForCommandBuffer(command).device,
                     "vkCmdPipelineBarrier"));
  if (!barrier)
    return;
  static const auto normalize = ResolveProcessFunction<VkImageLayout (*)(
      VkCommandBuffer, VkImage, unsigned *, unsigned *, VkImageLayout *,
      VkImageLayout)>("mocktail_vr_xr_desktop_barrier");
  std::vector<VkImageMemoryBarrier> adjusted;
  if (normalize && images && image_count) {
    adjusted.assign(images, images + image_count);
    for (auto &image : adjusted)
      image.newLayout = normalize(
          command, image.image, &image.srcQueueFamilyIndex,
          &image.dstQueueFamilyIndex, &image.oldLayout, image.newLayout);
    images = adjusted.data();
  }
  barrier(command, src, dst, flags, memory_count, memory, buffer_count, buffers,
          image_count, images);
}

static void ForwardVrBarrier2(VkCommandBuffer command,
                              const VkDependencyInfo *info, const char *name) {
  const auto barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      HostDeviceProc(HostDispatchForCommandBuffer(command).device, name));
  if (!barrier || !info)
    return;
  static const auto normalize = ResolveProcessFunction<VkImageLayout (*)(
      VkCommandBuffer, VkImage, unsigned *, unsigned *, VkImageLayout *,
      VkImageLayout)>("mocktail_vr_xr_desktop_barrier");
  VkDependencyInfo adjusted_info = *info;
  std::vector<VkImageMemoryBarrier2> adjusted;
  if (normalize && info->pImageMemoryBarriers &&
      info->imageMemoryBarrierCount) {
    adjusted.assign(info->pImageMemoryBarriers,
                    info->pImageMemoryBarriers + info->imageMemoryBarrierCount);
    for (auto &image : adjusted)
      image.newLayout = normalize(
          command, image.image, &image.srcQueueFamilyIndex,
          &image.dstQueueFamilyIndex, &image.oldLayout, image.newLayout);
    adjusted_info.pImageMemoryBarriers = adjusted.data();
  }
  barrier(command, &adjusted_info);
}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer command,
                                                 const VkDependencyInfo *info) {
  ForwardVrBarrier2(command, info, "vkCmdPipelineBarrier2");
}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2KHR(
    VkCommandBuffer command, const VkDependencyInfo *info) {
  ForwardVrBarrier2(command, info, "vkCmdPipelineBarrier2KHR");
}

VKAPI_ATTR VkResult VKAPI_CALL mocktail_vulkan_submit_synchronized(
    PFN_vkQueueSubmit raw, VkDevice device, VkQueue queue, uint32_t count,
    const VkSubmitInfo *submits, VkFence fence) {
  if (!raw)
    return VK_ERROR_INITIALIZATION_FAILED;
  const auto gate = DeviceIdleGate(device);
  std::shared_lock<std::shared_mutex> device_gate(*gate);
  const auto mutex = QueueCallMutex(queue);
  std::lock_guard<std::mutex> lock(*mutex);
  return raw(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL
mocktail_vulkan_idle_synchronized(PFN_vkDeviceWaitIdle raw, VkDevice device) {
  if (!raw)
    return VK_ERROR_INITIALIZATION_FAILED;
  const auto gate = DeviceIdleGate(device);
  std::unique_lock<std::shared_mutex> device_gate(*gate);
  return raw(device);
}

} // extern "C"
