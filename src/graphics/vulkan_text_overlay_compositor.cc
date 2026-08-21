#include "mocktail/graphics/vulkan_text_overlay_compositor.h"

#include <dlfcn.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/log.h>
#include <libplacebo/shaders/sampling.h>
#include <libplacebo/vulkan.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "mocktail/graphics/text_overlay_frame.h"

namespace mocktail {
namespace graphics {
namespace {

constexpr std::uint64_t kMaximumOverlayBytes = 256ULL * 1024ULL * 1024ULL;

using OverlayQueryFn = bool (*)(MocktailTextOverlayFrameInfo* frame);
using OverlayCopyFn = bool (*)(std::uint64_t revision, void* rgba,
                               std::size_t rgba_capacity);
using OverlayMayPresentFn = bool (*)();

template <typename Function>
Function ResolveDeviceFunction(PFN_vkGetDeviceProcAddr get_device_proc_addr,
                               VkDevice device, const char* name) {
  if (get_device_proc_addr == nullptr || device == VK_NULL_HANDLE ||
      name == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<Function>(get_device_proc_addr(device, name));
}

template <typename Structure>
const Structure* FindFeature(const VkPhysicalDeviceFeatures2* features,
                             VkStructureType type) {
  if (features == nullptr) {
    return nullptr;
  }
  const auto* current = static_cast<const VkBaseInStructure*>(features->pNext);
  while (current != nullptr) {
    if (current->sType == type) {
      return reinterpret_cast<const Structure*>(current);
    }
    current = current->pNext;
  }
  return nullptr;
}

bool RequiredImportFeaturesEnabled(const VkPhysicalDeviceFeatures2* features) {
  bool timeline_semaphore = false;
  bool host_query_reset = false;
  const auto* vulkan12 = FindFeature<VkPhysicalDeviceVulkan12Features>(
      features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
  if (vulkan12 != nullptr) {
    timeline_semaphore = vulkan12->timelineSemaphore == VK_TRUE;
    host_query_reset = vulkan12->hostQueryReset == VK_TRUE;
  }
  const auto* timeline = FindFeature<VkPhysicalDeviceTimelineSemaphoreFeatures>(
      features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
  if (timeline != nullptr) {
    timeline_semaphore = timeline->timelineSemaphore == VK_TRUE;
  }
  const auto* host_reset = FindFeature<VkPhysicalDeviceHostQueryResetFeatures>(
      features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES);
  if (host_reset != nullptr) {
    host_query_reset = host_reset->hostQueryReset == VK_TRUE;
  }
  return timeline_semaphore && host_query_reset;
}

bool IsSupportedSwapchainFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
      return true;
    default:
      return false;
  }
}

bool GraphicsFamilyCanUseSwapchain(
    std::uint32_t graphics_queue_family,
    const VkSwapchainCreateInfoKHR& create_info) {
  if (create_info.imageSharingMode == VK_SHARING_MODE_EXCLUSIVE) {
    return true;
  }
  if (create_info.imageSharingMode != VK_SHARING_MODE_CONCURRENT ||
      create_info.pQueueFamilyIndices == nullptr) {
    return false;
  }
  for (std::uint32_t index = 0; index < create_info.queueFamilyIndexCount;
       ++index) {
    if (create_info.pQueueFamilyIndices[index] == graphics_queue_family) {
      return true;
    }
  }
  return false;
}

void SecureClear(std::vector<std::uint8_t>* bytes) {
  if (bytes == nullptr) {
    return;
  }
  volatile std::uint8_t* data = bytes->empty() ? nullptr : bytes->data();
  for (std::size_t index = 0; index < bytes->size(); ++index) {
    data[index] = 0;
  }
  bytes->clear();
}

void LibplaceboLog(void*, enum pl_log_level level, const char* message) {
  if (level <= PL_LOG_WARN && message != nullptr) {
    std::fprintf(stderr, "  [vulkan-overlay] %s\n", message);
  }
}

}  // namespace

struct VulkanTextOverlayCompositor::Impl {
  struct ImageState {
    VkImage image = VK_NULL_HANDLE;
    pl_tex target = nullptr;
    VkSemaphore bridge = VK_NULL_HANDLE;
    VkSemaphore done = VK_NULL_HANDLE;
    bool held_by_host = true;
  };

  struct SwapchainState {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSwapchainKHR old_swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkImageUsageFlags usage = 0;
    bool enabled = true;
    std::vector<ImageState> images;
  };

  struct QueueState {
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    std::uint32_t index = 0;
  };

  struct DeviceState {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t api_version = 0;
    std::uint32_t graphics_queue_family = 0;
    std::uint32_t graphics_queue_count = 0;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
    PFN_vkQueueSubmit queue_submit = nullptr;
    PFN_vkQueueWaitIdle queue_wait_idle = nullptr;
    PFN_vkDeviceWaitIdle device_wait_idle = nullptr;
    std::vector<std::string> extension_names;
    std::vector<const char*> extension_pointers;
    std::mutex queue_mutex;
    std::mutex overlay_mutex;
    pl_log log = nullptr;
    pl_vulkan vulkan = nullptr;
    pl_dispatch dispatch = nullptr;
    pl_tex source = nullptr;
    std::uint64_t source_revision = 0;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    bool enabled = true;
    bool dispatch_failure_logged = false;
    std::vector<QueueState> queues;
    std::vector<std::unique_ptr<SwapchainState>> swapchains;
  };

  struct WorkItem {
    SwapchainState* swapchain = nullptr;
    ImageState* image = nullptr;
    pl_rect2df source_rect{};
    pl_rect2d target_rect{};
  };

  std::mutex mutex;
  std::vector<std::unique_ptr<DeviceState>> devices;
  OverlayMayPresentFn overlay_may_present =
      reinterpret_cast<OverlayMayPresentFn>(
          dlsym(RTLD_DEFAULT, "mocktail_text_overlay_may_present"));
  OverlayQueryFn overlay_query = nullptr;
  OverlayCopyFn overlay_copy = nullptr;

  static void LockQueue(void* context, std::uint32_t, std::uint32_t) {
    if (context != nullptr) {
      static_cast<DeviceState*>(context)->queue_mutex.lock();
    }
  }

  static void UnlockQueue(void* context, std::uint32_t, std::uint32_t) {
    if (context != nullptr) {
      static_cast<DeviceState*>(context)->queue_mutex.unlock();
    }
  }

  DeviceState* FindDevice(VkDevice device) {
    for (const auto& candidate : devices) {
      if (candidate->device == device) {
        return candidate.get();
      }
    }
    return nullptr;
  }

  DeviceState* FindQueue(VkQueue queue, QueueState** queue_state) {
    for (const auto& device : devices) {
      for (QueueState& candidate : device->queues) {
        if (candidate.queue == queue) {
          if (queue_state != nullptr) {
            *queue_state = &candidate;
          }
          return device.get();
        }
      }
    }
    return nullptr;
  }

  SwapchainState* FindSwapchain(DeviceState* device, VkSwapchainKHR swapchain) {
    if (device == nullptr) {
      return nullptr;
    }
    for (const auto& candidate : device->swapchains) {
      if (candidate->swapchain == swapchain) {
        return candidate.get();
      }
    }
    return nullptr;
  }

  bool ResolveOverlayCallbacks() {
    if (overlay_query == nullptr) {
      overlay_query = reinterpret_cast<OverlayQueryFn>(
          dlsym(RTLD_DEFAULT, "mocktail_text_overlay_query"));
    }
    if (overlay_copy == nullptr) {
      overlay_copy = reinterpret_cast<OverlayCopyFn>(
          dlsym(RTLD_DEFAULT, "mocktail_text_overlay_copy"));
    }
    return overlay_query != nullptr && overlay_copy != nullptr;
  }

  void FinishDevice(DeviceState* device) {
    if (device == nullptr) {
      return;
    }
    if (device->vulkan != nullptr) {
      (void)pl_gpu_finish(device->vulkan->gpu);
    }
    if (device->device_wait_idle != nullptr) {
      std::lock_guard<std::mutex> queue_lock(device->queue_mutex);
      (void)device->device_wait_idle(device->device);
    }
  }

  void DestroyImage(DeviceState* device, ImageState* image) {
    if (device == nullptr || image == nullptr || device->vulkan == nullptr) {
      return;
    }
    if (image->target != nullptr) {
      pl_tex_destroy(device->vulkan->gpu, &image->target);
    }
    if (image->bridge != VK_NULL_HANDLE) {
      pl_vulkan_sem_destroy(device->vulkan->gpu, &image->bridge);
    }
    if (image->done != VK_NULL_HANDLE) {
      pl_vulkan_sem_destroy(device->vulkan->gpu, &image->done);
    }
  }

  void DestroySwapchainResources(DeviceState* device, SwapchainState* swapchain,
                                 bool wait_for_idle) {
    if (device == nullptr || swapchain == nullptr) {
      return;
    }
    if (wait_for_idle) {
      FinishDevice(device);
    }
    for (ImageState& image : swapchain->images) {
      DestroyImage(device, &image);
    }
    swapchain->images.clear();
  }

  void DestroyDeviceResources(DeviceState* device) {
    if (device == nullptr) {
      return;
    }
    FinishDevice(device);
    for (const auto& swapchain : device->swapchains) {
      DestroySwapchainResources(device, swapchain.get(), false);
    }
    device->swapchains.clear();
    if (device->source != nullptr && device->vulkan != nullptr) {
      pl_tex_destroy(device->vulkan->gpu, &device->source);
    }
    if (device->dispatch != nullptr) {
      pl_dispatch_destroy(&device->dispatch);
    }
    if (device->vulkan != nullptr) {
      pl_vulkan_destroy(&device->vulkan);
    }
    if (device->log != nullptr) {
      pl_log_destroy(&device->log);
    }
  }

  bool EnsureSource(DeviceState* device,
                    const MocktailTextOverlayFrameInfo& frame) {
    if (device == nullptr || device->vulkan == nullptr ||
        overlay_copy == nullptr) {
      return false;
    }
    if (device->source != nullptr &&
        device->source_revision == frame.revision &&
        device->source_width == frame.width &&
        device->source_height == frame.height) {
      return true;
    }

    if (frame.width == 0 || frame.height == 0 || frame.row_bytes == 0 ||
        frame.rgba_bytes == 0 || frame.rgba_bytes > kMaximumOverlayBytes ||
        frame.rgba_bytes > std::numeric_limits<std::size_t>::max() ||
        frame.row_bytes != static_cast<std::uint64_t>(frame.width) * 4ULL ||
        frame.rgba_bytes !=
            static_cast<std::uint64_t>(frame.row_bytes) * frame.height) {
      return false;
    }

    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(frame.rgba_bytes));
    if (!overlay_copy(frame.revision, pixels.data(), pixels.size())) {
      SecureClear(&pixels);
      return false;
    }

    bool uploaded = false;
    if (device->source != nullptr && device->source_width == frame.width &&
        device->source_height == frame.height) {
      pl_tex_transfer_params transfer{};
      transfer.tex = device->source;
      transfer.row_pitch = frame.row_bytes;
      transfer.ptr = pixels.data();
      transfer.no_import = true;
      uploaded = pl_tex_upload(device->vulkan->gpu, &transfer);
    } else {
      if (device->source != nullptr) {
        pl_tex_destroy(device->vulkan->gpu, &device->source);
      }
      const pl_fmt format = pl_find_fmt(device->vulkan->gpu, PL_FMT_UNORM, 4, 8,
                                        8, PL_FMT_CAP_SAMPLEABLE);
      if (format != nullptr) {
        pl_tex_params texture{};
        texture.w = static_cast<int>(frame.width);
        texture.h = static_cast<int>(frame.height);
        texture.format = format;
        texture.sampleable = true;
        texture.host_writable = true;
        texture.initial_data = pixels.data();
        device->source = pl_tex_create(device->vulkan->gpu, &texture);
      }
      uploaded = device->source != nullptr;
    }
    SecureClear(&pixels);
    if (!uploaded) {
      device->source_revision = 0;
      device->source_width = 0;
      device->source_height = 0;
      return false;
    }
    device->source_revision = frame.revision;
    device->source_width = frame.width;
    device->source_height = frame.height;
    return true;
  }

  bool BuildWorkItems(DeviceState* device, QueueState* queue,
                      const VkPresentInfoKHR& present_info,
                      const MocktailTextOverlayFrameInfo& frame,
                      std::vector<WorkItem>* work_items) {
    if (device == nullptr || queue == nullptr || work_items == nullptr ||
        queue->family != device->graphics_queue_family ||
        frame.coordinate_width == 0 || frame.coordinate_height == 0 ||
        present_info.swapchainCount == 0 ||
        present_info.pSwapchains == nullptr ||
        present_info.pImageIndices == nullptr) {
      return false;
    }

    for (std::uint32_t index = 0; index < present_info.swapchainCount;
         ++index) {
      SwapchainState* swapchain =
          FindSwapchain(device, present_info.pSwapchains[index]);
      if (swapchain == nullptr || !swapchain->enabled ||
          present_info.pImageIndices[index] >= swapchain->images.size()) {
        continue;
      }
      ImageState* image = &swapchain->images[present_info.pImageIndices[index]];
      if (!image->held_by_host || image->target == nullptr ||
          image->bridge == VK_NULL_HANDLE || image->done == VK_NULL_HANDLE) {
        continue;
      }
      const auto duplicate = std::find_if(
          work_items->begin(), work_items->end(),
          [image](const WorkItem& item) { return item.image == image; });
      if (duplicate != work_items->end()) {
        continue;
      }

      const std::int64_t source_x0 =
          std::max<std::int64_t>(0, -static_cast<std::int64_t>(frame.x));
      const std::int64_t source_y0 =
          std::max<std::int64_t>(0, -static_cast<std::int64_t>(frame.y));
      const std::int64_t logical_x0 = std::max<std::int64_t>(0, frame.x);
      const std::int64_t logical_y0 = std::max<std::int64_t>(0, frame.y);
      const std::int64_t visible_width = std::min<std::int64_t>(
          static_cast<std::int64_t>(frame.width) - source_x0,
          static_cast<std::int64_t>(frame.coordinate_width) - logical_x0);
      const std::int64_t visible_height = std::min<std::int64_t>(
          static_cast<std::int64_t>(frame.height) - source_y0,
          static_cast<std::int64_t>(frame.coordinate_height) - logical_y0);
      if (visible_width <= 0 || visible_height <= 0 ||
          source_x0 > std::numeric_limits<int>::max() ||
          source_y0 > std::numeric_limits<int>::max() ||
          visible_width > std::numeric_limits<int>::max() ||
          visible_height > std::numeric_limits<int>::max()) {
        continue;
      }

      const std::uint64_t target_x0 = static_cast<std::uint64_t>(logical_x0) *
                                      swapchain->extent.width /
                                      frame.coordinate_width;
      const std::uint64_t target_y0 = static_cast<std::uint64_t>(logical_y0) *
                                      swapchain->extent.height /
                                      frame.coordinate_height;
      const std::uint64_t logical_x1 =
          static_cast<std::uint64_t>(logical_x0 + visible_width);
      const std::uint64_t logical_y1 =
          static_cast<std::uint64_t>(logical_y0 + visible_height);
      const std::uint64_t target_x1 =
          (logical_x1 * swapchain->extent.width + frame.coordinate_width - 1) /
          frame.coordinate_width;
      const std::uint64_t target_y1 = (logical_y1 * swapchain->extent.height +
                                       frame.coordinate_height - 1) /
                                      frame.coordinate_height;
      if (target_x1 <= target_x0 || target_y1 <= target_y0 ||
          target_x1 > swapchain->extent.width ||
          target_y1 > swapchain->extent.height ||
          target_x1 >
              static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
          target_y1 >
              static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        continue;
      }

      WorkItem item;
      item.swapchain = swapchain;
      item.image = image;
      item.source_rect = {static_cast<float>(source_x0),
                          static_cast<float>(source_y0),
                          static_cast<float>(source_x0 + visible_width),
                          static_cast<float>(source_y0 + visible_height)};
      item.target_rect = {
          static_cast<int>(target_x0), static_cast<int>(target_y0),
          static_cast<int>(target_x1), static_cast<int>(target_y1)};
      work_items->push_back(item);
    }
    return !work_items->empty();
  }

  std::mutex* SharedQueueMutex(DeviceState* device,
                               QueueState* queue_state) {
    if (device == nullptr || queue_state == nullptr ||
        queue_state->family != device->graphics_queue_family ||
        queue_state->index >= device->graphics_queue_count) {
      return nullptr;
    }
    return &device->queue_mutex;
  }

  VkResult CallFallback(std::mutex* queue_mutex, VkQueue queue,
                        const VkPresentInfoKHR* present_info,
                        PFN_vkQueuePresentKHR fallback) {
    if (fallback == nullptr) {
      return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (queue_mutex == nullptr) {
      return fallback(queue, present_info);
    }
    std::lock_guard<std::mutex> queue_lock(*queue_mutex);
    return fallback(queue, present_info);
  }

  bool DispatchWork(DeviceState* device, const WorkItem& item) {
    if (device == nullptr || device->dispatch == nullptr ||
        device->source == nullptr || item.image == nullptr ||
        item.image->target == nullptr) {
      return false;
    }
    pl_shader shader = pl_dispatch_begin(device->dispatch);
    pl_sample_src source{};
    source.tex = device->source;
    source.rect = item.source_rect;
    source.address_mode = PL_TEX_ADDRESS_CLAMP;
    source.new_w = pl_rect_w(item.target_rect);
    source.new_h = pl_rect_h(item.target_rect);
    if (shader == nullptr || !pl_shader_sample_direct(shader, &source)) {
      if (shader != nullptr) {
        pl_dispatch_abort(device->dispatch, &shader);
      }
      return false;
    }
    pl_dispatch_params dispatch{};
    dispatch.shader = &shader;
    dispatch.target = item.image->target;
    dispatch.rect = item.target_rect;
    dispatch.blend_params = &pl_alpha_overlay;
    return pl_dispatch_finish(device->dispatch, &dispatch);
  }
};

VulkanTextOverlayCompositor::VulkanTextOverlayCompositor()
    : impl_(new (std::nothrow) Impl) {}

VulkanTextOverlayCompositor::~VulkanTextOverlayCompositor() {
  if (impl_ == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& device : impl_->devices) {
      std::lock_guard<std::mutex> overlay_lock(device->overlay_mutex);
      impl_->DestroyDeviceResources(device.get());
    }
    impl_->devices.clear();
  }
  delete impl_;
  impl_ = nullptr;
}

bool VulkanTextOverlayCompositor::RegisterDevice(
    VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
    std::uint32_t api_version, const char* const* enabled_extensions,
    std::uint32_t enabled_extension_count, std::uint32_t graphics_queue_family,
    std::uint32_t graphics_queue_count,
    const VkPhysicalDeviceFeatures2* enabled_features,
    PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    PFN_vkGetDeviceProcAddr get_device_proc_addr) {
  if (impl_ == nullptr || instance == VK_NULL_HANDLE ||
      physical_device == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
      api_version < VK_API_VERSION_1_2 || graphics_queue_count == 0 ||
      get_instance_proc_addr == nullptr || get_device_proc_addr == nullptr ||
      (enabled_extension_count != 0 && enabled_extensions == nullptr) ||
      enabled_extension_count >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      !RequiredImportFeaturesEnabled(enabled_features)) {
    return false;
  }

  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->FindDevice(device) != nullptr) {
    return true;
  }
  std::unique_ptr<Impl::DeviceState> state(new (std::nothrow)
                                               Impl::DeviceState);
  if (state == nullptr) {
    return false;
  }
  state->instance = instance;
  state->physical_device = physical_device;
  state->device = device;
  state->api_version = api_version;
  state->graphics_queue_family = graphics_queue_family;
  state->graphics_queue_count = graphics_queue_count;
  state->get_instance_proc_addr = get_instance_proc_addr;
  state->get_device_proc_addr = get_device_proc_addr;
  state->queue_submit = ResolveDeviceFunction<PFN_vkQueueSubmit>(
      get_device_proc_addr, device, "vkQueueSubmit");
  state->queue_wait_idle = ResolveDeviceFunction<PFN_vkQueueWaitIdle>(
      get_device_proc_addr, device, "vkQueueWaitIdle");
  state->device_wait_idle = ResolveDeviceFunction<PFN_vkDeviceWaitIdle>(
      get_device_proc_addr, device, "vkDeviceWaitIdle");
  if (state->queue_submit == nullptr || state->queue_wait_idle == nullptr ||
      state->device_wait_idle == nullptr) {
    return false;
  }
  state->extension_names.reserve(enabled_extension_count);
  for (std::uint32_t index = 0; index < enabled_extension_count; ++index) {
    if (enabled_extensions[index] == nullptr) {
      return false;
    }
    state->extension_names.emplace_back(enabled_extensions[index]);
  }
  state->extension_pointers.reserve(state->extension_names.size());
  for (const std::string& extension : state->extension_names) {
    state->extension_pointers.push_back(extension.c_str());
  }

  pl_log_params log_params{};
  log_params.log_cb = &LibplaceboLog;
  log_params.log_level = PL_LOG_WARN;
  state->log = pl_log_create(PL_API_VER, &log_params);
  if (state->log == nullptr) {
    return false;
  }
  pl_vulkan_import_params import_params{};
  import_params.instance = instance;
  import_params.get_proc_addr = get_instance_proc_addr;
  import_params.phys_device = physical_device;
  import_params.device = device;
  import_params.extensions = state->extension_pointers.data();
  import_params.num_extensions =
      static_cast<int>(state->extension_pointers.size());
  // libplacebo queue ranges always start at queue index zero. Restrict it to
  // that one queue so it cannot submit through an application queue that the
  // adapter did not explicitly synchronize with.
  import_params.queue_graphics = {graphics_queue_family, 1};
  import_params.features = &pl_vulkan_required_features;
  import_params.lock_queue = &Impl::LockQueue;
  import_params.unlock_queue = &Impl::UnlockQueue;
  import_params.queue_ctx = state.get();
  import_params.no_compute = true;
  import_params.max_api_version = api_version;
  state->vulkan = pl_vulkan_import(state->log, &import_params);
  if (state->vulkan == nullptr) {
    pl_log_destroy(&state->log);
    return false;
  }
  state->dispatch = pl_dispatch_create(state->log, state->vulkan->gpu);
  if (state->dispatch == nullptr) {
    pl_vulkan_destroy(&state->vulkan);
    pl_log_destroy(&state->log);
    return false;
  }
  impl_->devices.push_back(std::move(state));
  std::fprintf(stderr,
               "  [vulkan-overlay] imported host VkDevice with libplacebo "
               "(graphics queue family %u)\n",
               graphics_queue_family);
  return true;
}

void VulkanTextOverlayCompositor::DestroyDevice(VkDevice device) {
  if (impl_ == nullptr || device == VK_NULL_HANDLE) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto iterator = std::find_if(
      impl_->devices.begin(), impl_->devices.end(),
      [device](const auto& candidate) { return candidate->device == device; });
  if (iterator == impl_->devices.end()) {
    return;
  }
  std::lock_guard<std::mutex> overlay_lock((*iterator)->overlay_mutex);
  impl_->DestroyDeviceResources(iterator->get());
  impl_->devices.erase(iterator);
}

bool VulkanTextOverlayCompositor::RegisterQueue(VkDevice device, VkQueue queue,
                                                std::uint32_t queue_family,
                                                std::uint32_t queue_index) {
  if (impl_ == nullptr || device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Impl::DeviceState* state = impl_->FindDevice(device);
  if (state == nullptr) {
    return false;
  }
  for (Impl::QueueState& registered : state->queues) {
    if (registered.queue == queue) {
      registered.family = queue_family;
      registered.index = queue_index;
      return true;
    }
  }
  state->queues.push_back({queue, queue_family, queue_index});
  return true;
}

bool VulkanTextOverlayCompositor::RegisterSwapchain(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkSwapchainCreateInfoKHR& create_info, const VkImage* images,
    std::uint32_t image_count) {
  if (impl_ == nullptr || device == VK_NULL_HANDLE ||
      swapchain == VK_NULL_HANDLE || images == nullptr || image_count == 0 ||
      create_info.imageExtent.width == 0 ||
      create_info.imageExtent.height == 0 ||
      create_info.imageExtent.width >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      create_info.imageExtent.height >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      create_info.imageArrayLayers != 1 ||
      (create_info.imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0 ||
      create_info.preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
      !IsSupportedSwapchainFormat(create_info.imageFormat)) {
    return false;
  }

  std::lock_guard<std::mutex> lock(impl_->mutex);
  Impl::DeviceState* state = impl_->FindDevice(device);
  if (state == nullptr || state->vulkan == nullptr ||
      !GraphicsFamilyCanUseSwapchain(state->graphics_queue_family,
                                     create_info)) {
    return false;
  }
  std::lock_guard<std::mutex> overlay_lock(state->overlay_mutex);
  if (impl_->FindSwapchain(state, swapchain) != nullptr) {
    return true;
  }

  std::unique_ptr<Impl::SwapchainState> candidate(new (std::nothrow)
                                                      Impl::SwapchainState);
  if (candidate == nullptr) {
    return false;
  }
  candidate->swapchain = swapchain;
  candidate->old_swapchain = create_info.oldSwapchain;
  candidate->format = create_info.imageFormat;
  candidate->extent = create_info.imageExtent;
  candidate->usage = create_info.imageUsage;
  candidate->images.reserve(image_count);
  for (std::uint32_t index = 0; index < image_count; ++index) {
    if (images[index] == VK_NULL_HANDLE) {
      impl_->DestroySwapchainResources(state, candidate.get(), false);
      return false;
    }
    Impl::ImageState image;
    image.image = images[index];
    pl_vulkan_wrap_params wrap{};
    wrap.image = image.image;
    wrap.width = static_cast<int>(candidate->extent.width);
    wrap.height = static_cast<int>(candidate->extent.height);
    wrap.format = candidate->format;
    wrap.usage = candidate->usage;
    image.target = pl_vulkan_wrap(state->vulkan->gpu, &wrap);
    if (image.target == nullptr || !image.target->params.renderable ||
        image.target->params.format == nullptr ||
        (image.target->params.format->caps & PL_FMT_CAP_BLENDABLE) == 0) {
      if (image.target != nullptr) {
        pl_tex_destroy(state->vulkan->gpu, &image.target);
      }
      impl_->DestroySwapchainResources(state, candidate.get(), false);
      return false;
    }
    pl_vulkan_sem_params semaphore{};
    semaphore.type = VK_SEMAPHORE_TYPE_BINARY;
    image.bridge = pl_vulkan_sem_create(state->vulkan->gpu, &semaphore);
    image.done = pl_vulkan_sem_create(state->vulkan->gpu, &semaphore);
    if (image.bridge == VK_NULL_HANDLE || image.done == VK_NULL_HANDLE) {
      impl_->DestroyImage(state, &image);
      impl_->DestroySwapchainResources(state, candidate.get(), false);
      return false;
    }
    candidate->images.push_back(image);
  }

  state->swapchains.push_back(std::move(candidate));
  std::fprintf(stderr,
               "  [vulkan-overlay] wrapped %u swapchain images for "
               "same-surface text\n",
               image_count);
  return true;
}

void VulkanTextOverlayCompositor::DestroySwapchain(VkDevice device,
                                                   VkSwapchainKHR swapchain) {
  if (impl_ == nullptr || device == VK_NULL_HANDLE ||
      swapchain == VK_NULL_HANDLE) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Impl::DeviceState* state = impl_->FindDevice(device);
  if (state == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> overlay_lock(state->overlay_mutex);
  const auto iterator =
      std::find_if(state->swapchains.begin(), state->swapchains.end(),
                   [swapchain](const auto& candidate) {
                     return candidate->swapchain == swapchain;
                   });
  if (iterator == state->swapchains.end()) {
    return;
  }
  impl_->DestroySwapchainResources(state, iterator->get(), true);
  state->swapchains.erase(iterator);
}

VkResult VulkanTextOverlayCompositor::QueueSubmit(
    VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo* submits,
    VkFence fence, PFN_vkQueueSubmit fallback) {
  if (fallback == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (impl_ == nullptr || queue == VK_NULL_HANDLE) {
    return fallback(queue, submit_count, submits, fence);
  }
  std::mutex* shared_queue_mutex = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::QueueState* queue_state = nullptr;
    Impl::DeviceState* device = impl_->FindQueue(queue, &queue_state);
    shared_queue_mutex = impl_->SharedQueueMutex(device, queue_state);
  }
  if (shared_queue_mutex == nullptr) {
    return fallback(queue, submit_count, submits, fence);
  }
  std::lock_guard<std::mutex> queue_lock(*shared_queue_mutex);
  return fallback(queue, submit_count, submits, fence);
}

VkResult VulkanTextOverlayCompositor::QueueSubmit2(
    VkQueue queue, std::uint32_t submit_count, const VkSubmitInfo2* submits,
    VkFence fence, PFN_vkQueueSubmit2 fallback) {
  if (fallback == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (impl_ == nullptr || queue == VK_NULL_HANDLE) {
    return fallback(queue, submit_count, submits, fence);
  }
  std::mutex* shared_queue_mutex = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::QueueState* queue_state = nullptr;
    Impl::DeviceState* device = impl_->FindQueue(queue, &queue_state);
    shared_queue_mutex = impl_->SharedQueueMutex(device, queue_state);
  }
  if (shared_queue_mutex == nullptr) {
    return fallback(queue, submit_count, submits, fence);
  }
  std::lock_guard<std::mutex> queue_lock(*shared_queue_mutex);
  return fallback(queue, submit_count, submits, fence);
}

VkResult VulkanTextOverlayCompositor::QueueBindSparse(
    VkQueue queue, std::uint32_t bind_info_count,
    const VkBindSparseInfo* bind_info, VkFence fence,
    PFN_vkQueueBindSparse fallback) {
  if (fallback == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (impl_ == nullptr || queue == VK_NULL_HANDLE) {
    return fallback(queue, bind_info_count, bind_info, fence);
  }
  std::mutex* shared_queue_mutex = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::QueueState* queue_state = nullptr;
    Impl::DeviceState* device = impl_->FindQueue(queue, &queue_state);
    shared_queue_mutex = impl_->SharedQueueMutex(device, queue_state);
  }
  if (shared_queue_mutex == nullptr) {
    return fallback(queue, bind_info_count, bind_info, fence);
  }
  std::lock_guard<std::mutex> queue_lock(*shared_queue_mutex);
  return fallback(queue, bind_info_count, bind_info, fence);
}

VkResult VulkanTextOverlayCompositor::QueueWaitIdle(
    VkQueue queue, PFN_vkQueueWaitIdle fallback) {
  if (fallback == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (impl_ == nullptr || queue == VK_NULL_HANDLE) {
    return fallback(queue);
  }
  std::mutex* shared_queue_mutex = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::QueueState* queue_state = nullptr;
    Impl::DeviceState* device = impl_->FindQueue(queue, &queue_state);
    shared_queue_mutex = impl_->SharedQueueMutex(device, queue_state);
  }
  if (shared_queue_mutex == nullptr) {
    return fallback(queue);
  }
  std::lock_guard<std::mutex> queue_lock(*shared_queue_mutex);
  return fallback(queue);
}

VkResult VulkanTextOverlayCompositor::DeviceWaitIdle(
    VkDevice device, PFN_vkDeviceWaitIdle fallback) {
  if (fallback == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (impl_ == nullptr || device == VK_NULL_HANDLE) {
    return fallback(device);
  }
  std::mutex* shared_queue_mutex = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::DeviceState* state = impl_->FindDevice(device);
    if (state != nullptr && state->graphics_queue_count != 0) {
      shared_queue_mutex = &state->queue_mutex;
    }
  }
  if (shared_queue_mutex == nullptr) {
    return fallback(device);
  }
  std::lock_guard<std::mutex> queue_lock(*shared_queue_mutex);
  return fallback(device);
}

VkResult VulkanTextOverlayCompositor::QueuePresent(
    VkQueue queue, const VkPresentInfoKHR* present_info,
    PFN_vkQueuePresentKHR fallback) {
  if (fallback == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (impl_ == nullptr || queue == VK_NULL_HANDLE || present_info == nullptr) {
    return fallback(queue, present_info);
  }
  // No compositor work has consumed the application's wait semaphores yet,
  // so an inactive overlay forwards the original present. It still takes the
  // imported queue lock because libplacebo and the application share VkQueue.
  if (impl_->overlay_may_present != nullptr &&
      !impl_->overlay_may_present()) {
    std::mutex* shared_queue_mutex = nullptr;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      Impl::QueueState* queue_state = nullptr;
      Impl::DeviceState* device = impl_->FindQueue(queue, &queue_state);
      shared_queue_mutex = impl_->SharedQueueMutex(device, queue_state);
    }
    if (shared_queue_mutex == nullptr) {
      return fallback(queue, present_info);
    }
    std::lock_guard<std::mutex> queue_lock(*shared_queue_mutex);
    return fallback(queue, present_info);
  }

  Impl::DeviceState* device = nullptr;
  Impl::QueueState queue_state{};
  std::mutex* shared_queue_mutex = nullptr;
  bool overlay_callbacks_available = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::QueueState* registered_queue = nullptr;
    device = impl_->FindQueue(queue, &registered_queue);
    if (device != nullptr && registered_queue != nullptr) {
      queue_state = *registered_queue;
      shared_queue_mutex =
          impl_->SharedQueueMutex(device, registered_queue);
      overlay_callbacks_available = impl_->ResolveOverlayCallbacks();
    }
  }
  if (device == nullptr || !device->enabled ||
      !overlay_callbacks_available) {
    return impl_->CallFallback(shared_queue_mutex, queue, present_info,
                               fallback);
  }

  // Device and swapchain lifetime are externally synchronized by Vulkan. A
  // per-device overlay lock protects libplacebo state without holding the
  // global registry lock across a potentially blocking host queue call.
  std::lock_guard<std::mutex> overlay_lock(device->overlay_mutex);

  MocktailTextOverlayFrameInfo frame{};
  if (!impl_->overlay_query(&frame) ||
      frame.abi_version != MocktailTextOverlayFrameInfo::kAbiVersion ||
      frame.visible == 0 || frame.revision == 0 ||
      !impl_->EnsureSource(device, frame)) {
    return impl_->CallFallback(shared_queue_mutex, queue, present_info,
                               fallback);
  }

  std::vector<Impl::WorkItem> work_items;
  work_items.reserve(present_info->swapchainCount);
  if (!impl_->BuildWorkItems(device, &queue_state, *present_info, frame,
                             &work_items)) {
    return impl_->CallFallback(shared_queue_mutex, queue, present_info,
                               fallback);
  }

  std::vector<VkSemaphore> bridge_semaphores;
  bridge_semaphores.reserve(work_items.size());
  for (const Impl::WorkItem& item : work_items) {
    bridge_semaphores.push_back(item.image->bridge);
  }
  std::vector<VkPipelineStageFlags> wait_stages(
      present_info->waitSemaphoreCount,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  VkSubmitInfo bridge_submit{};
  bridge_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  bridge_submit.waitSemaphoreCount = present_info->waitSemaphoreCount;
  bridge_submit.pWaitSemaphores = present_info->pWaitSemaphores;
  bridge_submit.pWaitDstStageMask = wait_stages.data();
  bridge_submit.signalSemaphoreCount =
      static_cast<std::uint32_t>(bridge_semaphores.size());
  bridge_submit.pSignalSemaphores = bridge_semaphores.data();
  VkResult submit_result = VK_ERROR_INITIALIZATION_FAILED;
  if (shared_queue_mutex != nullptr) {
    std::lock_guard<std::mutex> queue_lock(*shared_queue_mutex);
    submit_result =
        device->queue_submit(queue, 1, &bridge_submit, VK_NULL_HANDLE);
  } else {
    submit_result =
        device->queue_submit(queue, 1, &bridge_submit, VK_NULL_HANDLE);
  }
  if (submit_result != VK_SUCCESS) {
    return impl_->CallFallback(shared_queue_mutex, queue, present_info,
                               fallback);
  }

  pl_dispatch_reset_frame(device->dispatch);
  std::vector<VkSemaphore> done_semaphores;
  done_semaphores.reserve(work_items.size());
  bool all_images_held = true;
  for (Impl::WorkItem& item : work_items) {
    pl_vulkan_release_params release{};
    release.tex = item.image->target;
    release.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    release.qf = device->graphics_queue_family;
    release.semaphore = {item.image->bridge, 0};
    pl_vulkan_release_ex(device->vulkan->gpu, &release);
    item.image->held_by_host = false;

    const bool dispatched = impl_->DispatchWork(device, item);
    if (!dispatched && !device->dispatch_failure_logged) {
      device->dispatch_failure_logged = true;
      std::fprintf(stderr,
                   "  [vulkan-overlay] alpha dispatch failed; presenting the "
                   "held image without the text update\n");
    }
    pl_vulkan_hold_params hold{};
    hold.tex = item.image->target;
    hold.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    hold.qf = device->graphics_queue_family;
    hold.semaphore = {item.image->done, 0};
    bool held = pl_vulkan_hold_ex(device->vulkan->gpu, &hold);
    if (!held) {
      // A failed dispatch may temporarily leave libplacebo unable to emit the
      // hand-off. Drain its pending work once and retry before falling back to
      // the emergency zero-wait presentation path.
      (void)pl_gpu_finish(device->vulkan->gpu);
      held = pl_vulkan_hold_ex(device->vulkan->gpu, &hold);
    }
    item.image->held_by_host = held;
    all_images_held = all_images_held && held;
    if (held) {
      done_semaphores.push_back(item.image->done);
    } else {
      item.swapchain->enabled = false;
    }
  }

  VkPresentInfoKHR host_present = *present_info;
  if (!all_images_held) {
    // The original waits were consumed by bridge_submit, so forwarding them
    // is no longer legal. Drain both owners and fail closed to a zero-wait
    // present; affected swapchains remain disabled until recreation.
    (void)pl_gpu_finish(device->vulkan->gpu);
    if (shared_queue_mutex != nullptr) {
      std::lock_guard<std::mutex> queue_lock(*shared_queue_mutex);
      (void)device->queue_wait_idle(queue);
    } else {
      (void)device->queue_wait_idle(queue);
    }
    host_present.waitSemaphoreCount = 0;
    host_present.pWaitSemaphores = nullptr;
    for (Impl::WorkItem& item : work_items) {
      // Successful hold semaphores are not consumed by this
      // emergency present, so none of this generation's semaphore pairs may
      // be recycled.
      item.swapchain->enabled = false;
    }
  } else {
    host_present.waitSemaphoreCount =
        static_cast<std::uint32_t>(done_semaphores.size());
    host_present.pWaitSemaphores = done_semaphores.data();
  }
  const VkResult present_result =
      impl_->CallFallback(shared_queue_mutex, queue, &host_present, fallback);
  bool waits_consumed =
      present_result == VK_SUCCESS || present_result == VK_SUBOPTIMAL_KHR;
  if (host_present.pResults != nullptr) {
    for (std::uint32_t index = 0; index < host_present.swapchainCount;
         ++index) {
      waits_consumed =
          waits_consumed && (host_present.pResults[index] == VK_SUCCESS ||
                             host_present.pResults[index] == VK_SUBOPTIMAL_KHR);
    }
  }
  if (!waits_consumed) {
    // On presentation failure Vulkan does not give us a portable guarantee
    // that every binary wait was consumed. Keep the resources alive, but do
    // not signal either semaphore again before swapchain destruction.
    for (Impl::WorkItem& item : work_items) {
      item.swapchain->enabled = false;
    }
  }
  return present_result;
}

}  // namespace graphics
}  // namespace mocktail
