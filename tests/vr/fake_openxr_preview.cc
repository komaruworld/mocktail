// Session contract fixture. Vulkan devices, images, rendering and readback are
// real; OpenXR scheduling/tracking are scripted to exercise error paths.
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

namespace {

bool Scenario(const char* name) {
  const char* value = std::getenv("MOCKTAIL_TEST_XR_SCENARIO");
  return value && std::strcmp(value, name) == 0;
}

void Trace(const char* event) {
  if (const char* path = std::getenv("MOCKTAIL_TEST_XR_TRACE")) {
    if (auto* file = std::fopen(path, "a")) {
      std::fprintf(file, "%s\n", event);
      std::fclose(file);
    }
  }
}

#define REQUIRE(value)                  \
  if (!(value)) {                       \
    Trace("contract-error: " #value);   \
    return XR_ERROR_VALIDATION_FAILURE; \
  }

struct Fixture {
  PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  std::uint32_t queue_family = 0;
  std::array<VkImage, 3> images{};
  std::array<VkDeviceMemory, 3> memory{};
  std::uint32_t width = 0, height = 0, index = 0, frames = 0;
  bool running = false, begun = false, acquired = false, waited = false;
  bool timeout_returned = false, pixels_verified = false;
  int local = 0, view = 0, swapchain = 0;
  XrSessionState state = XR_SESSION_STATE_IDLE;
  std::deque<XrSessionState> events;
} fixture;

XrSession Session() { return reinterpret_cast<XrSession>(&fixture); }
XrSwapchain Swapchain() {
  return reinterpret_cast<XrSwapchain>(&fixture.swapchain);
}
XrSpace Local() { return reinterpret_cast<XrSpace>(&fixture.local); }
XrSpace View() { return reinterpret_cast<XrSpace>(&fixture.view); }

template <typename T>
T Device(const char* name) {
  return reinterpret_cast<T>(fixture.get_device_proc(fixture.device, name));
}
#define VK(name, ...) Device<PFN_vk##name>("vk" #name)(__VA_ARGS__)

std::uint32_t MemoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties memory{};
  reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
      fixture.get_instance_proc(fixture.instance,
                                "vkGetPhysicalDeviceMemoryProperties"))(
      fixture.physical, &memory);
  for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
    if ((bits & (1u << i)) &&
        (memory.memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  return UINT32_MAX;
}

// Independently inspect submitted pixels, so a successful xrEndFrame alone
// cannot make an empty or unrendered texture pass the integration test.
bool VerifyPixels() {
  struct Resources {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    ~Resources() {
      VK(DeviceWaitIdle, fixture.device);
      if (pool) VK(DestroyCommandPool, fixture.device, pool, nullptr);
      if (buffer) VK(DestroyBuffer, fixture.device, buffer, nullptr);
      if (memory) VK(FreeMemory, fixture.device, memory, nullptr);
    }
  } resources;
  VkBufferCreateInfo buffer{};
  buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer.size = fixture.width * fixture.height * 4;
  buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (VK(CreateBuffer, fixture.device, &buffer, nullptr, &resources.buffer) !=
      VK_SUCCESS)
    return false;
  VkMemoryRequirements requirements{};
  VK(GetBufferMemoryRequirements, fixture.device, resources.buffer,
     &requirements);
  VkMemoryAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = MemoryType(
      requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (allocate.memoryTypeIndex == UINT32_MAX ||
      VK(AllocateMemory, fixture.device, &allocate, nullptr,
         &resources.memory) != VK_SUCCESS ||
      VK(BindBufferMemory, fixture.device, resources.buffer, resources.memory,
         0) != VK_SUCCESS)
    return false;
  VkCommandPoolCreateInfo pool{};
  pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool.queueFamilyIndex = fixture.queue_family;
  if (VK(CreateCommandPool, fixture.device, &pool, nullptr, &resources.pool) !=
      VK_SUCCESS)
    return false;
  VkCommandBufferAllocateInfo commands{};
  commands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  commands.commandPool = resources.pool;
  commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  commands.commandBufferCount = 1;
  VkCommandBuffer command = VK_NULL_HANDLE;
  if (VK(AllocateCommandBuffers, fixture.device, &commands, &command) !=
      VK_SUCCESS)
    return false;
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  if (VK(BeginCommandBuffer, command, &begin) != VK_SUCCESS) return false;
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex =
      VK_QUEUE_FAMILY_IGNORED;
  barrier.image = fixture.images[fixture.index];
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VK(CmdPipelineBarrier, command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {fixture.width, fixture.height, 1};
  VK(CmdCopyImageToBuffer, command, barrier.image,
     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resources.buffer, 1, &copy);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VK(CmdPipelineBarrier, command, VK_PIPELINE_STAGE_TRANSFER_BIT,
     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
     1, &barrier);
  if (VK(EndCommandBuffer, command) != VK_SUCCESS) return false;
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  if (VK(QueueSubmit, fixture.queue, 1, &submit, VK_NULL_HANDLE) !=
          VK_SUCCESS ||
      VK(QueueWaitIdle, fixture.queue) != VK_SUCCESS)
    return false;
  void* mapped = nullptr;
  if (VK(MapMemory, fixture.device, resources.memory, 0, VK_WHOLE_SIZE, 0,
         &mapped) != VK_SUCCESS)
    return false;
  const auto* bytes = static_cast<const unsigned char*>(mapped);
  const auto* cyan =
      bytes + (fixture.height / 16 * fixture.width + fixture.width / 16) * 4;
  const auto* purple = cyan + fixture.width / 2 * 4;
  const bool valid = cyan[1] > 60 && cyan[2] > 80 && cyan[0] < 15 &&
                     purple[0] > 40 && purple[1] < 35 && purple[2] > 80;
  VK(UnmapMemory, fixture.device, resources.memory);
  if (valid) Trace("pixels-verified");
  return valid;
}

XrResult XRAPI_CALL
CreateVulkanInstance(XrInstance, const XrVulkanInstanceCreateInfoKHR* info,
                     VkInstance* instance, VkResult* result) {
  fixture.get_instance_proc = info->pfnGetInstanceProcAddr;
  *result = reinterpret_cast<PFN_vkCreateInstance>(
      fixture.get_instance_proc(VK_NULL_HANDLE, "vkCreateInstance"))(
      info->vulkanCreateInfo, info->vulkanAllocator, instance);
  if (*result == VK_SUCCESS) {
    fixture.instance = *instance;
    fixture.get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        fixture.get_instance_proc(*instance, "vkGetDeviceProcAddr"));
  }
  return XR_SUCCESS;
}

XrResult XRAPI_CALL
GetVulkanDevice(XrInstance, const XrVulkanGraphicsDeviceGetInfoKHR* info,
                VkPhysicalDevice* device) {
  const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
      fixture.get_instance_proc(info->vulkanInstance,
                                "vkEnumeratePhysicalDevices"));
  std::uint32_t count = 0;
  if (enumerate(info->vulkanInstance, &count, nullptr) != VK_SUCCESS || !count)
    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  std::vector<VkPhysicalDevice> devices(count);
  if (enumerate(info->vulkanInstance, &count, devices.data()) != VK_SUCCESS)
    return XR_ERROR_RUNTIME_FAILURE;
  *device = fixture.physical = devices.front();
  return XR_SUCCESS;
}

XrResult XRAPI_CALL CreateVulkanDevice(XrInstance,
                                       const XrVulkanDeviceCreateInfoKHR* info,
                                       VkDevice* device, VkResult* result) {
  if (Scenario("device_failure")) {
    *result = VK_ERROR_FEATURE_NOT_PRESENT;
    return XR_SUCCESS;
  }
  *result = reinterpret_cast<PFN_vkCreateDevice>(fixture.get_instance_proc(
      fixture.instance, "vkCreateDevice"))(info->vulkanPhysicalDevice,
                                           info->vulkanCreateInfo,
                                           info->vulkanAllocator, device);
  if (*result == VK_SUCCESS) fixture.device = *device;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL EnumerateBlends(XrInstance, XrSystemId,
                                    XrViewConfigurationType,
                                    std::uint32_t capacity,
                                    std::uint32_t* count,
                                    XrEnvironmentBlendMode* blends) {
  *count = 1;
  if (capacity) blends[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL CreateSession(XrInstance, const XrSessionCreateInfo* info,
                                  XrSession* session) {
  if (Scenario("session_failure")) return XR_ERROR_INITIALIZATION_FAILED;
  const auto* binding =
      static_cast<const XrGraphicsBindingVulkan2KHR*>(info->next);
  REQUIRE(binding && binding->type == XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR);
  REQUIRE(binding->device == fixture.device &&
          binding->physicalDevice == fixture.physical);
  fixture.queue_family = binding->queueFamilyIndex;
  VK(GetDeviceQueue, fixture.device, binding->queueFamilyIndex,
     binding->queueIndex, &fixture.queue);
  *session = Session();
  fixture.events.push_back(XR_SESSION_STATE_READY);
  Trace("session-create");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL DestroySession(XrSession) {
  REQUIRE(fixture.images[0] == VK_NULL_HANDLE);
  Trace("session-destroy");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL PollEvent(XrInstance, XrEventDataBuffer* buffer) {
  if (fixture.events.empty()) return XR_EVENT_UNAVAILABLE;
  XrEventDataSessionStateChanged event{};
  event.type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
  event.session = Session();
  event.state = fixture.state = fixture.events.front();
  fixture.events.pop_front();
  std::memcpy(buffer, &event, sizeof(event));
  return XR_SUCCESS;
}

XrResult XRAPI_CALL BeginSession(XrSession, const XrSessionBeginInfo* begin) {
  REQUIRE(fixture.state == XR_SESSION_STATE_READY && !fixture.running);
  REQUIRE(begin->primaryViewConfigurationType ==
          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
  fixture.running = true;
  fixture.events.push_back(XR_SESSION_STATE_FOCUSED);
  Trace("session-begin");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL RequestExit(XrSession) {
  REQUIRE(fixture.running);
  fixture.events.push_back(XR_SESSION_STATE_STOPPING);
  Trace("session-exit-request");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL EndSession(XrSession) {
  REQUIRE(fixture.state == XR_SESSION_STATE_STOPPING && fixture.running);
  fixture.running = false;
  Trace("session-end");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL CreateSpace(XrSession,
                                const XrReferenceSpaceCreateInfo* info,
                                XrSpace* space) {
  REQUIRE(info->poseInReferenceSpace.orientation.w == 1);
  REQUIRE(info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ||
          info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW);
  *space = info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ? Local()
                                                                     : View();
  Trace("space-create");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL DestroySpace(XrSpace) {
  Trace("space-destroy");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL LocateSpace(XrSpace space, XrSpace base, XrTime,
                                XrSpaceLocation* location) {
  REQUIRE(space == View() && base == Local());
  location->locationFlags = Scenario("invalid_tracking") && fixture.frames == 0
                                ? 0
                                : XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                      XR_SPACE_LOCATION_POSITION_VALID_BIT;
  location->pose.orientation.w = 1;
  location->pose.position.x = static_cast<float>(fixture.frames) / 10;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL EnumerateFormats(XrSession, std::uint32_t capacity,
                                     std::uint32_t* count,
                                     std::int64_t* formats) {
  *count = 1;
  if (capacity) formats[0] = VK_FORMAT_R8G8B8A8_UNORM;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL CreateSwapchain(XrSession,
                                    const XrSwapchainCreateInfo* info,
                                    XrSwapchain* swapchain) {
  if (Scenario("swapchain_failure")) return XR_ERROR_RUNTIME_FAILURE;
  REQUIRE(info->sampleCount == 1 && info->arraySize == 1 &&
          info->faceCount == 1 && info->mipCount == 1);
  fixture.width = info->width;
  fixture.height = info->height;
  VkImageCreateInfo create{};
  create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  create.imageType = VK_IMAGE_TYPE_2D;
  create.format = static_cast<VkFormat>(info->format);
  create.extent = {info->width, info->height, 1};
  create.mipLevels = create.arrayLayers = 1;
  create.samples = VK_SAMPLE_COUNT_1_BIT;
  create.usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  for (std::size_t i = 0; i < fixture.images.size(); ++i) {
    REQUIRE(VK(CreateImage, fixture.device, &create, nullptr,
               &fixture.images[i]) == VK_SUCCESS);
    VkMemoryRequirements requirements{};
    VK(GetImageMemoryRequirements, fixture.device, fixture.images[i],
       &requirements);
    VkMemoryAllocateInfo memory{};
    memory.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memory.allocationSize = requirements.size;
    memory.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, 0);
    REQUIRE(memory.memoryTypeIndex != UINT32_MAX);
    REQUIRE(VK(AllocateMemory, fixture.device, &memory, nullptr,
               &fixture.memory[i]) == VK_SUCCESS);
    REQUIRE(VK(BindImageMemory, fixture.device, fixture.images[i],
               fixture.memory[i], 0) == VK_SUCCESS);
  }
  *swapchain = Swapchain();
  Trace("swapchain-create");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL DestroySwapchain(XrSwapchain) {
  for (std::size_t i = 0; i < fixture.images.size(); ++i) {
    if (fixture.images[i])
      VK(DestroyImage, fixture.device, fixture.images[i], nullptr);
    if (fixture.memory[i])
      VK(FreeMemory, fixture.device, fixture.memory[i], nullptr);
    fixture.images[i] = VK_NULL_HANDLE;
  }
  Trace("swapchain-destroy");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL EnumerateImages(XrSwapchain, std::uint32_t capacity,
                                    std::uint32_t* count,
                                    XrSwapchainImageBaseHeader* base) {
  *count = fixture.images.size();
  if (!capacity) return XR_SUCCESS;
  if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
  auto* images = reinterpret_cast<XrSwapchainImageVulkanKHR*>(base);
  for (std::uint32_t i = 0; i < *count; ++i) {
    REQUIRE(images[i].type == XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR);
    images[i].image = fixture.images[i];
  }
  return XR_SUCCESS;
}

XrResult XRAPI_CALL WaitFrame(XrSession, const XrFrameWaitInfo*,
                              XrFrameState* state) {
  REQUIRE(fixture.running && !fixture.begun);
  state->predictedDisplayTime = (fixture.frames + 1) * 16'666'666;
  state->predictedDisplayPeriod = 16'666'666;
  state->shouldRender = !(Scenario("skip_render") && fixture.frames == 0);
  Trace("frame-wait");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL BeginFrame(XrSession, const XrFrameBeginInfo*) {
  REQUIRE(fixture.running && !fixture.begun);
  fixture.begun = true;
  Trace("frame-begin");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL AcquireImage(XrSwapchain,
                                 const XrSwapchainImageAcquireInfo*,
                                 std::uint32_t* index) {
  REQUIRE(fixture.begun && !fixture.acquired);
  fixture.acquired = true;
  fixture.index = fixture.frames % fixture.images.size();
  *index = Scenario("bad_image_index") ? 1000 : fixture.index;
  Trace("image-acquire");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL WaitImage(XrSwapchain, const XrSwapchainImageWaitInfo*) {
  REQUIRE(fixture.acquired && !fixture.waited);
  if (Scenario("image_timeout") && !fixture.timeout_returned) {
    fixture.timeout_returned = true;
    Trace("image-timeout");
    return XR_TIMEOUT_EXPIRED;
  }
  fixture.waited = true;
  Trace("image-wait");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL ReleaseImage(XrSwapchain,
                                 const XrSwapchainImageReleaseInfo*) {
  REQUIRE(fixture.acquired && fixture.waited);
  fixture.acquired = fixture.waited = false;
  Trace("image-release");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL EndFrame(XrSession, const XrFrameEndInfo* end) {
  REQUIRE(fixture.begun && !fixture.acquired);
  REQUIRE(end->displayTime == (fixture.frames + 1) * 16'666'666);
  REQUIRE(end->environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
  if (end->layerCount) {
    REQUIRE(end->layerCount == 1 &&
            end->layers[0]->type == XR_TYPE_COMPOSITION_LAYER_QUAD);
    const auto* layer =
        reinterpret_cast<const XrCompositionLayerQuad*>(end->layers[0]);
    REQUIRE(layer->space == Local() &&
            layer->eyeVisibility == XR_EYE_VISIBILITY_BOTH);
    REQUIRE(layer->pose.position.z == -2 && layer->pose.orientation.w == 1);
    REQUIRE(layer->subImage.swapchain == Swapchain());
    if (!fixture.pixels_verified) {
      REQUIRE(VerifyPixels());
      fixture.pixels_verified = true;
    }
    Trace("layer-submit");
  } else {
    Trace("empty-submit");
  }
  fixture.begun = false;
  ++fixture.frames;
  if (Scenario("preview_auto_stop") && fixture.frames == 3)
    fixture.events.push_back(XR_SESSION_STATE_STOPPING);
  if (Scenario("session_loss"))
    fixture.events.push_back(XR_SESSION_STATE_LOSS_PENDING);
  Trace("frame-end");
  return XR_SUCCESS;
}

}  // namespace

PFN_xrVoidFunction MocktailTestPreviewFunction(const char* name) {
#define MAP(xr_name, implementation)    \
  if (std::strcmp(name, #xr_name) == 0) \
    return reinterpret_cast<PFN_xrVoidFunction>(implementation);
  MAP(xrCreateVulkanInstanceKHR, CreateVulkanInstance)
  MAP(xrGetVulkanGraphicsDevice2KHR, GetVulkanDevice)
  MAP(xrCreateVulkanDeviceKHR, CreateVulkanDevice)
  MAP(xrEnumerateEnvironmentBlendModes, EnumerateBlends)
  MAP(xrCreateSession, CreateSession)
  MAP(xrDestroySession, DestroySession)
  MAP(xrPollEvent, PollEvent)
  MAP(xrBeginSession, BeginSession)
  MAP(xrRequestExitSession, RequestExit)
  MAP(xrEndSession, EndSession)
  MAP(xrCreateReferenceSpace, CreateSpace)
  MAP(xrDestroySpace, DestroySpace)
  MAP(xrLocateSpace, LocateSpace)
  MAP(xrEnumerateSwapchainFormats, EnumerateFormats)
  MAP(xrCreateSwapchain, CreateSwapchain)
  MAP(xrDestroySwapchain, DestroySwapchain)
  MAP(xrEnumerateSwapchainImages, EnumerateImages)
  MAP(xrWaitFrame, WaitFrame)
  MAP(xrBeginFrame, BeginFrame)
  MAP(xrEndFrame, EndFrame)
  MAP(xrAcquireSwapchainImage, AcquireImage)
  MAP(xrWaitSwapchainImage, WaitImage)
  MAP(xrReleaseSwapchainImage, ReleaseImage)
#undef MAP
  return nullptr;
}
