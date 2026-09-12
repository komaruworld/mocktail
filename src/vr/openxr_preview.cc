#include "mocktail/vr/openxr_preview.h"

#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <dlfcn.h>
#include <signal.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace mocktail::vr {
namespace {

using Clock = std::chrono::steady_clock;
volatile sig_atomic_t interrupted = 0;
void Interrupt(int) { interrupted = 1; }

class SignalScope {
 public:
  SignalScope() {
    interrupted = 0;
    struct sigaction action{};
    action.sa_handler = Interrupt;
    sigemptyset(&action.sa_mask);
    have_int_ = sigaction(SIGINT, &action, &old_int_) == 0;
    have_term_ = sigaction(SIGTERM, &action, &old_term_) == 0;
  }
  ~SignalScope() {
    if (have_int_) sigaction(SIGINT, &old_int_, nullptr);
    if (have_term_) sigaction(SIGTERM, &old_term_, nullptr);
  }

 private:
  struct sigaction old_int_{}, old_term_{};
  bool have_int_ = false, have_term_ = false;
};

template <typename T>
T XrInfo(XrStructureType type) {
  T value{};
  value.type = type;
  return value;
}

template <typename T>
T VkInfo(VkStructureType type) {
  T value{};
  value.sType = type;
  return value;
}

void Require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void CheckVk(VkResult result, const char* operation) {
  Require(result == VK_SUCCESS,
          std::string(operation) + " failed: " + std::to_string(result));
}

// Every Vulkan call uses the host loader explicitly. Linking global vk*
// symbols here could accidentally enter Mocktail's Android Vulkan adapter.
struct Vulkan {
  void* library = nullptr;
  PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  std::uint32_t queue_family = 0;
  PFN_vkDestroyInstance DestroyInstance = nullptr;

#define MOCKTAIL_PREVIEW_VK_FUNCTIONS(X) \
  X(DestroyDevice)                       \
  X(GetDeviceQueue)                      \
  X(DeviceWaitIdle)                      \
  X(CreateImageView)                     \
  X(DestroyImageView)                    \
  X(CreateRenderPass)                    \
  X(DestroyRenderPass)                   \
  X(CreateFramebuffer)                   \
  X(DestroyFramebuffer)                  \
  X(CreateCommandPool)                   \
  X(DestroyCommandPool)                  \
  X(AllocateCommandBuffers)              \
  X(ResetCommandBuffer)                  \
  X(BeginCommandBuffer)                  \
  X(EndCommandBuffer)                    \
  X(CmdBeginRenderPass)                  \
  X(CmdEndRenderPass)                    \
  X(CmdClearAttachments)                 \
  X(CreateFence)                         \
  X(DestroyFence)                        \
  X(ResetFences)                         \
  X(WaitForFences)                       \
  X(QueueSubmit)
#define DECLARE(name) PFN_vk##name name = nullptr;
  MOCKTAIL_PREVIEW_VK_FUNCTIONS(DECLARE)
#undef DECLARE

  Vulkan() = default;
  Vulkan(const Vulkan&) = delete;
  Vulkan& operator=(const Vulkan&) = delete;
  ~Vulkan() {
    if (device && DestroyDevice) DestroyDevice(device, nullptr);
    if (instance && DestroyInstance) DestroyInstance(instance, nullptr);
    if (library) dlclose(library);
  }

  void OpenLoader() {
    library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    Require(library != nullptr,
            "Cannot load the host Vulkan loader libvulkan.so.1");
    get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(library, "vkGetInstanceProcAddr"));
    Require(get_instance_proc != nullptr,
            "Host vkGetInstanceProcAddr is missing");
  }

  template <typename T>
  T InstanceFunction(const char* name) {
    const auto function =
        reinterpret_cast<T>(get_instance_proc(instance, name));
    Require(function != nullptr,
            std::string("Missing Vulkan function: ") + name);
    return function;
  }

  void LoadDeviceFunctions() {
#define LOAD(name)                                                            \
  name = reinterpret_cast<PFN_vk##name>(get_device_proc(device, "vk" #name)); \
  Require(name != nullptr, "Missing Vulkan function: vk" #name);
    MOCKTAIL_PREVIEW_VK_FUNCTIONS(LOAD)
#undef LOAD
    GetDeviceQueue(device, queue_family, 0, &queue);
  }
};
#undef MOCKTAIL_PREVIEW_VK_FUNCTIONS

class Preview {
 public:
  Preview() = default;
  Preview(const Preview&) = delete;
  Preview& operator=(const Preview&) = delete;
  ~Preview() {
    // The runtime still needs our VkDevice when destroying its
    // swapchain/session.
    if (vk_.device && vk_.DeviceWaitIdle) (void)vk_.DeviceWaitIdle(vk_.device);
    for (auto framebuffer : framebuffers_)
      vk_.DestroyFramebuffer(vk_.device, framebuffer, nullptr);
    for (auto view : image_views_)
      vk_.DestroyImageView(vk_.device, view, nullptr);
    if (render_pass_) vk_.DestroyRenderPass(vk_.device, render_pass_, nullptr);
    if (fence_) vk_.DestroyFence(vk_.device, fence_, nullptr);
    if (pool_) vk_.DestroyCommandPool(vk_.device, pool_, nullptr);
    if (swapchain_) (void)xrDestroySwapchain(swapchain_);
    if (view_space_) (void)xrDestroySpace(view_space_);
    if (local_space_) (void)xrDestroySpace(local_space_);
    if (session_) (void)xrDestroySession(session_);
    if (instance_) (void)xrDestroyInstance(instance_);
  }

  void Initialize() {
    const char* extension = XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
    auto info = XrInfo<XrInstanceCreateInfo>(XR_TYPE_INSTANCE_CREATE_INFO);
    std::strcpy(info.applicationInfo.applicationName, "Mocktail VR preview");
    std::strcpy(info.applicationInfo.engineName, "Mocktail");
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = &extension;
    Check(xrCreateInstance(&info, &instance_), "xrCreateInstance");
    auto system = XrInfo<XrSystemGetInfo>(XR_TYPE_SYSTEM_GET_INFO);
    system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    Check(xrGetSystem(instance_, &system, &system_), "xrGetSystem");

    const auto views = Enumerate<XrViewConfigurationView>(
        XR_TYPE_VIEW_CONFIGURATION_VIEW,
        [&](auto capacity, auto* count, auto* data) {
          return xrEnumerateViewConfigurationViews(
              instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
              capacity, count, data);
        });
    Require(views.size() == 2,
            "The VR preview requires PRIMARY_STEREO support");
    const auto blends = Enumerate<XrEnvironmentBlendMode>(
        XR_TYPE_UNKNOWN, [&](auto capacity, auto* count, auto* data) {
          return xrEnumerateEnvironmentBlendModes(
              instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
              capacity, count, data);
        });
    Require(!blends.empty(), "The runtime exposes no environment blend modes");
    blend_ = std::find(blends.begin(), blends.end(),
                       XR_ENVIRONMENT_BLEND_MODE_OPAQUE) != blends.end()
                 ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE
                 : blends.front();
    CreateGraphics();
    auto binding = XrInfo<XrGraphicsBindingVulkan2KHR>(
        XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR);
    binding.instance = vk_.instance;
    binding.physicalDevice = vk_.physical_device;
    binding.device = vk_.device;
    binding.queueFamilyIndex = vk_.queue_family;
    auto session_info =
        XrInfo<XrSessionCreateInfo>(XR_TYPE_SESSION_CREATE_INFO);
    session_info.next = &binding;
    session_info.systemId = system_;
    Check(xrCreateSession(instance_, &session_info, &session_),
          "xrCreateSession");
    auto space_info =
        XrInfo<XrReferenceSpaceCreateInfo>(XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
    space_info.poseInReferenceSpace.orientation.w = 1;
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    Check(xrCreateReferenceSpace(session_, &space_info, &local_space_),
          "xrCreateReferenceSpace LOCAL");
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    Check(xrCreateReferenceSpace(session_, &space_info, &view_space_),
          "xrCreateReferenceSpace VIEW");
    CreateImages();
  }

  void Run(const PreviewOptions& options, PreviewResult* report) {
    const auto ready_deadline = Clock::now() + std::chrono::seconds(30);
    auto exit_deadline = Clock::time_point::max();
    bool running = false, exit_requested = false;
    while (true) {
      while (true) {
        auto event = XrInfo<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
        const auto result = xrPollEvent(instance_, &event);
        if (result == XR_EVENT_UNAVAILABLE) break;
        Check(result, "xrPollEvent");
        if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
          throw std::runtime_error("The OpenXR runtime was lost");
        if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;
        const auto& changed =
            reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
        if (changed.session != session_) continue;
        switch (changed.state) {
          case XR_SESSION_STATE_READY: {
            auto begin = XrInfo<XrSessionBeginInfo>(XR_TYPE_SESSION_BEGIN_INFO);
            begin.primaryViewConfigurationType =
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            Check(xrBeginSession(session_, &begin), "xrBeginSession");
            running = true;
            std::cerr << "[VR] Session started. A test panel is fixed two "
                         "metres ahead.\n";
            break;
          }
          case XR_SESSION_STATE_STOPPING:
            if (running) Check(xrEndSession(session_), "xrEndSession");
            return;
          case XR_SESSION_STATE_EXITING:
            return;
          case XR_SESSION_STATE_LOSS_PENDING:
            throw std::runtime_error(
                "The OpenXR session was lost; reconnect and launch again");
          default:
            break;
        }
      }
      if (interrupted ||
          (options.frame_limit && report->frames >= options.frame_limit)) {
        if (!running) return;
        if (!exit_requested) {
          Check(xrRequestExitSession(session_), "xrRequestExitSession");
          exit_requested = true;
          exit_deadline = Clock::now() + std::chrono::seconds(5);
          // Consume an immediate STOPPING event before starting another frame.
          continue;
        }
      }
      Require(Clock::now() < exit_deadline,
              "The runtime did not stop the VR session after an exit request");
      if (!running) {
        Require(Clock::now() < ready_deadline,
                "Timed out waiting for the VR session to become READY");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      Frame(report);
    }
  }

 private:
  void Check(XrResult result, const char* operation) const {
    if (XR_SUCCEEDED(result)) return;
    char text[XR_MAX_RESULT_STRING_SIZE]{};
    if (instance_) (void)xrResultToString(instance_, result, text);
    if (!text[0] && result == XR_ERROR_RUNTIME_UNAVAILABLE)
      std::strcpy(text, "XR_ERROR_RUNTIME_UNAVAILABLE");
    std::string message =
        std::string(operation) +
        " failed: " + (text[0] ? std::string(text) : std::to_string(result));
    if (result == XR_ERROR_RUNTIME_UNAVAILABLE)
      message += ". No active OpenXR runtime was found. Start WiVRn for a real "
                 "headset. Without a headset, run mocktail-vr-simulated from "
                 "this build directory.";
    else if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
      message += ". Connect a headset or select a simulated runtime.";
    else if (result == XR_ERROR_EXTENSION_NOT_PRESENT)
      message += ". XR_KHR_vulkan_enable2 is required.";
    throw std::runtime_error(message);
  }

  template <typename T>
  T Function(const char* name) {
    PFN_xrVoidFunction function = nullptr;
    Check(xrGetInstanceProcAddr(instance_, name, &function), name);
    Require(function != nullptr,
            std::string("Missing OpenXR function: ") + name);
    return reinterpret_cast<T>(function);
  }

  template <typename T, typename F>
  std::vector<T> Enumerate(XrStructureType type, F function) {
    for (int retry = 0; retry < 3; ++retry) {
      std::uint32_t count = 0;
      Check(function(0, &count, static_cast<T*>(nullptr)),
            "OpenXR enumeration count");
      Require(count <= 4096, "OpenXR enumeration exceeds the supported size");
      std::vector<T> values(count);
      if constexpr (std::is_class_v<T>)
        for (auto& value : values) value.type = type;
      if (!count) return values;
      const auto result = function(count, &count, values.data());
      if (result == XR_ERROR_SIZE_INSUFFICIENT) continue;
      Check(result, "OpenXR enumeration data");
      Require(count <= values.size(),
              "OpenXR enumeration returned an invalid size");
      values.resize(count);
      return values;
    }
    throw std::runtime_error("OpenXR enumeration kept changing size");
  }

  void CreateGraphics() {
    auto requirements = XrInfo<XrGraphicsRequirementsVulkan2KHR>(
        XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR);
    Check(Function<PFN_xrGetVulkanGraphicsRequirements2KHR>(
              "xrGetVulkanGraphicsRequirements2KHR")(instance_, system_,
                                                     &requirements),
          "xrGetVulkanGraphicsRequirements2KHR");
    vk_.OpenLoader();
    auto application =
        VkInfo<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    application.pApplicationName = "Mocktail VR preview";
    const auto version = std::max<XrVersion>(
        requirements.minApiVersionSupported, XR_MAKE_VERSION(1, 0, 0));
    application.apiVersion = VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(version),
                                                 XR_VERSION_MINOR(version), 0);
    auto instance_info =
        VkInfo<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    instance_info.pApplicationInfo = &application;
    auto xr_instance_info = XrInfo<XrVulkanInstanceCreateInfoKHR>(
        XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR);
    xr_instance_info.systemId = system_;
    xr_instance_info.pfnGetInstanceProcAddr = vk_.get_instance_proc;
    xr_instance_info.vulkanCreateInfo = &instance_info;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    Check(Function<PFN_xrCreateVulkanInstanceKHR>("xrCreateVulkanInstanceKHR")(
              instance_, &xr_instance_info, &vk_.instance, &result),
          "xrCreateVulkanInstanceKHR");
    CheckVk(result, "vkCreateInstance");
    vk_.DestroyInstance =
        vk_.InstanceFunction<PFN_vkDestroyInstance>("vkDestroyInstance");
    vk_.get_device_proc =
        vk_.InstanceFunction<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    auto device_info = XrInfo<XrVulkanGraphicsDeviceGetInfoKHR>(
        XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR);
    device_info.systemId = system_;
    device_info.vulkanInstance = vk_.instance;
    Check(Function<PFN_xrGetVulkanGraphicsDevice2KHR>(
              "xrGetVulkanGraphicsDevice2KHR")(instance_, &device_info,
                                               &vk_.physical_device),
          "xrGetVulkanGraphicsDevice2KHR");
    VkPhysicalDeviceProperties properties{};
    vk_.InstanceFunction<PFN_vkGetPhysicalDeviceProperties>(
        "vkGetPhysicalDeviceProperties")(vk_.physical_device, &properties);
    Require(properties.apiVersion >= application.apiVersion,
            "The runtime-selected GPU does not support the required Vulkan "
            "version");
    const auto get_families =
        vk_.InstanceFunction<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            "vkGetPhysicalDeviceQueueFamilyProperties");
    std::uint32_t count = 0;
    get_families(vk_.physical_device, &count, nullptr);
    Require(count > 0 && count <= 256,
            "The runtime-selected GPU has no usable queue families");
    std::vector<VkQueueFamilyProperties> families(count);
    get_families(vk_.physical_device, &count, families.data());
    const auto family =
        std::find_if(families.begin(), families.end(), [](const auto& value) {
          return value.queueCount && (value.queueFlags & VK_QUEUE_GRAPHICS_BIT);
        });
    Require(family != families.end(),
            "The runtime-selected GPU has no graphics queue");
    vk_.queue_family = static_cast<std::uint32_t>(family - families.begin());
    const float priority = 1;
    auto queue = VkInfo<VkDeviceQueueCreateInfo>(
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    queue.queueFamilyIndex = vk_.queue_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    auto create =
        VkInfo<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queue;
    auto xr_create = XrInfo<XrVulkanDeviceCreateInfoKHR>(
        XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR);
    xr_create.systemId = system_;
    xr_create.pfnGetInstanceProcAddr = vk_.get_instance_proc;
    xr_create.vulkanPhysicalDevice = vk_.physical_device;
    xr_create.vulkanCreateInfo = &create;
    Check(Function<PFN_xrCreateVulkanDeviceKHR>("xrCreateVulkanDeviceKHR")(
              instance_, &xr_create, &vk_.device, &result),
          "xrCreateVulkanDeviceKHR");
    CheckVk(result, "vkCreateDevice");
    vk_.LoadDeviceFunctions();
    std::cerr << "[VR] Vulkan device: " << properties.deviceName << '\n';
  }

  void CreateImages() {
    const auto formats = Enumerate<std::int64_t>(
        XR_TYPE_UNKNOWN, [&](auto capacity, auto* count, auto* data) {
          return xrEnumerateSwapchainFormats(session_, capacity, count, data);
        });
    VkFormat format = VK_FORMAT_UNDEFINED;
    for (auto candidate :
         {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
          VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
      if (std::find(formats.begin(), formats.end(), candidate) !=
          formats.end()) {
        format = candidate;
        break;
      }
    }
    Require(format != VK_FORMAT_UNDEFINED,
            "The runtime exposes no supported RGBA swapchain format");
    auto system_properties =
        XrInfo<XrSystemProperties>(XR_TYPE_SYSTEM_PROPERTIES);
    Check(xrGetSystemProperties(instance_, system_, &system_properties),
          "xrGetSystemProperties");
    width_ = std::min(
        512u, system_properties.graphicsProperties.maxSwapchainImageWidth);
    height_ = std::min(
        512u, system_properties.graphicsProperties.maxSwapchainImageHeight);
    Require(width_ >= 64 && height_ >= 64 &&
                system_properties.graphicsProperties.maxLayerCount > 0,
            "The runtime cannot display the preview panel");
    auto swapchain =
        XrInfo<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
    swapchain.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain.format = format;
    swapchain.sampleCount = 1;
    swapchain.width = width_;
    swapchain.height = height_;
    swapchain.faceCount = swapchain.arraySize = swapchain.mipCount = 1;
    Check(xrCreateSwapchain(session_, &swapchain, &swapchain_),
          "xrCreateSwapchain");
    const auto images = Enumerate<XrSwapchainImageVulkanKHR>(
        XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR,
        [&](auto capacity, auto* count, auto* data) {
          return xrEnumerateSwapchainImages(
              swapchain_, capacity, count,
              reinterpret_cast<XrSwapchainImageBaseHeader*>(data));
        });
    Require(!images.empty(), "The runtime returned an empty swapchain");

    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference reference{0,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = dependency.dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    auto pass = VkInfo<VkRenderPassCreateInfo>(
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    pass.attachmentCount = 1;
    pass.pAttachments = &attachment;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 1;
    pass.pDependencies = &dependency;
    CheckVk(vk_.CreateRenderPass(vk_.device, &pass, nullptr, &render_pass_),
            "vkCreateRenderPass");
    for (const auto& image : images) {
      auto view_info = VkInfo<VkImageViewCreateInfo>(
          VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
      view_info.image = image.image;
      view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_info.format = format;
      view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView view = VK_NULL_HANDLE;
      CheckVk(vk_.CreateImageView(vk_.device, &view_info, nullptr, &view),
              "vkCreateImageView");
      image_views_.push_back(view);
      auto framebuffer_info = VkInfo<VkFramebufferCreateInfo>(
          VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
      framebuffer_info.renderPass = render_pass_;
      framebuffer_info.attachmentCount = 1;
      framebuffer_info.pAttachments = &image_views_.back();
      framebuffer_info.width = width_;
      framebuffer_info.height = height_;
      framebuffer_info.layers = 1;
      VkFramebuffer framebuffer = VK_NULL_HANDLE;
      CheckVk(vk_.CreateFramebuffer(vk_.device, &framebuffer_info, nullptr,
                                    &framebuffer),
              "vkCreateFramebuffer");
      framebuffers_.push_back(framebuffer);
    }
    auto pool = VkInfo<VkCommandPoolCreateInfo>(
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = vk_.queue_family;
    CheckVk(vk_.CreateCommandPool(vk_.device, &pool, nullptr, &pool_),
            "vkCreateCommandPool");
    auto allocate = VkInfo<VkCommandBufferAllocateInfo>(
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    allocate.commandPool = pool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    CheckVk(vk_.AllocateCommandBuffers(vk_.device, &allocate, &command_),
            "vkAllocateCommandBuffers");
    auto fence = VkInfo<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    CheckVk(vk_.CreateFence(vk_.device, &fence, nullptr, &fence_),
            "vkCreateFence");
  }

  void Render(std::uint32_t image, std::uint64_t frame) {
    Require(image < framebuffers_.size(),
            "The runtime acquired an invalid swapchain image index");
    CheckVk(vk_.ResetCommandBuffer(command_, 0), "vkResetCommandBuffer");
    auto begin = VkInfo<VkCommandBufferBeginInfo>(
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVk(vk_.BeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer");
    VkClearValue background{};
    background.color = {{0.018f, 0.024f, 0.04f, 1.f}};
    auto pass =
        VkInfo<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    pass.renderPass = render_pass_;
    pass.framebuffer = framebuffers_[image];
    pass.renderArea.extent = {width_, height_};
    pass.clearValueCount = 1;
    pass.pClearValues = &background;
    vk_.CmdBeginRenderPass(command_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    auto rectangle = [&](std::uint32_t x, std::uint32_t y, std::uint32_t w,
                         std::uint32_t h, VkClearColorValue color) {
      VkClearAttachment attachment{};
      attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      attachment.clearValue.color = color;
      VkClearRect rect{
          {{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)},
           {w, h}},
          0,
          1};
      vk_.CmdClearAttachments(command_, 1, &attachment, 1, &rect);
    };
    for (std::uint32_t y = 0; y < 8; ++y)
      for (std::uint32_t x = 0; x < 8; ++x)
        if ((x + y) % 2 == 0)
          rectangle(x * width_ / 8, y * height_ / 8, width_ / 8, height_ / 8,
                    x < 4 ? VkClearColorValue{{0.025f, 0.3f, 0.38f, 1.f}}
                          : VkClearColorValue{{0.23f, 0.08f, 0.4f, 1.f}});
    rectangle(width_ / 2 - 1, height_ / 4, 2, height_ / 2,
              {{0.9f, 0.9f, 0.9f, 1.f}});
    rectangle(width_ / 4, height_ / 2 - 1, width_ / 2, 2,
              {{0.9f, 0.9f, 0.9f, 1.f}});
    rectangle(static_cast<std::uint32_t>(frame % (width_ - 16)), height_ - 12,
              16, 6, {{0.8f, 0.55f, 0.08f, 1.f}});
    vk_.CmdEndRenderPass(command_);
    CheckVk(vk_.EndCommandBuffer(command_), "vkEndCommandBuffer");
    CheckVk(vk_.ResetFences(vk_.device, 1, &fence_), "vkResetFences");
    auto submit = VkInfo<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_;
    CheckVk(vk_.QueueSubmit(vk_.queue, 1, &submit, fence_), "vkQueueSubmit");
    // One frame in flight for the first prototype: all GPU writes complete
    // before release and command-buffer reuse. No runtime-owned image is freed.
    CheckVk(
        vk_.WaitForFences(vk_.device, 1, &fence_, VK_TRUE, 5'000'000'000ULL),
        "vkWaitForFences");
  }

  void Frame(PreviewResult* report) {
    auto wait = XrInfo<XrFrameWaitInfo>(XR_TYPE_FRAME_WAIT_INFO);
    auto state = XrInfo<XrFrameState>(XR_TYPE_FRAME_STATE);
    Check(xrWaitFrame(session_, &wait, &state), "xrWaitFrame");
    auto begin = XrInfo<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO);
    Check(xrBeginFrame(session_, &begin), "xrBeginFrame");
    auto end = XrInfo<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
    end.displayTime = state.predictedDisplayTime;
    end.environmentBlendMode = blend_;
    bool frame_open = true, image_waited = false;
    try {
      auto head = XrInfo<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
      Check(xrLocateSpace(view_space_, local_space_, state.predictedDisplayTime,
                          &head),
            "xrLocateSpace");
      constexpr auto valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                             XR_SPACE_LOCATION_POSITION_VALID_BIT;
      const bool pose_valid = (head.locationFlags & valid) == valid;
      if (pose_valid) {
        ++report->tracked_frames;
        report->head_position = {head.pose.position.x, head.pose.position.y,
                                 head.pose.position.z};
        report->head_orientation = {
            head.pose.orientation.x, head.pose.orientation.y,
            head.pose.orientation.z, head.pose.orientation.w};
      }
      auto layer =
          XrInfo<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD);
      const XrCompositionLayerBaseHeader* layer_pointer =
          reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
      if (state.shouldRender && pose_valid) {
        auto acquire = XrInfo<XrSwapchainImageAcquireInfo>(
            XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
        std::uint32_t image = 0;
        Check(xrAcquireSwapchainImage(swapchain_, &acquire, &image),
              "xrAcquireSwapchainImage");
        auto image_wait =
            XrInfo<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
        image_wait.timeout = 50'000'000;
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (true) {
          const auto result = xrWaitSwapchainImage(swapchain_, &image_wait);
          if (result != XR_TIMEOUT_EXPIRED) {
            Check(result, "xrWaitSwapchainImage");
            break;
          }
          Require(!interrupted && Clock::now() < deadline,
                  "Timed out waiting for an OpenXR swapchain image");
        }
        image_waited = true;
        Render(image, report->frames);
        auto release = XrInfo<XrSwapchainImageReleaseInfo>(
            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
        image_waited = false;
        Check(xrReleaseSwapchainImage(swapchain_, &release),
              "xrReleaseSwapchainImage");
        layer.space = local_space_;
        layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        layer.subImage.swapchain = swapchain_;
        layer.subImage.imageRect.extent = {static_cast<std::int32_t>(width_),
                                           static_cast<std::int32_t>(height_)};
        layer.pose.orientation.w = 1;
        layer.pose.position.z = -2;
        layer.size = {1.4f, 1.4f};
        end.layerCount = 1;
        end.layers = &layer_pointer;
      }
      frame_open = false;
      Check(xrEndFrame(session_, &end), "xrEndFrame");
      ++report->frames;
      if (end.layerCount) ++report->rendered_frames;
    } catch (...) {
      if (image_waited) {
        if (vk_.DeviceWaitIdle) (void)vk_.DeviceWaitIdle(vk_.device);
        auto release = XrInfo<XrSwapchainImageReleaseInfo>(
            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
        (void)xrReleaseSwapchainImage(swapchain_, &release);
      }
      if (frame_open) {
        end.layerCount = 0;
        end.layers = nullptr;
        (void)xrEndFrame(session_, &end);
      }
      throw;
    }
  }

  Vulkan vk_;
  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId system_ = XR_NULL_SYSTEM_ID;
  XrSession session_ = XR_NULL_HANDLE;
  XrSpace local_space_ = XR_NULL_HANDLE, view_space_ = XR_NULL_HANDLE;
  XrSwapchain swapchain_ = XR_NULL_HANDLE;
  XrEnvironmentBlendMode blend_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  std::uint32_t width_ = 512, height_ = 512;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  std::vector<VkImageView> image_views_;
  std::vector<VkFramebuffer> framebuffers_;
};

}  // namespace

PreviewResult RunOpenXrPreview(const PreviewOptions& options) {
  PreviewResult result;
  SignalScope signals;
  try {
    Preview preview;
    preview.Initialize();
    std::cerr << "[VR] Experimental test scene. Roblox rendering is not "
                 "connected. Ctrl+C exits.\n";
    preview.Run(options, &result);
    result.success = true;
    result.message = "VR preview closed";
  } catch (const std::exception& error) {
    result.message = error.what();
  }
  return result;
}

}  // namespace mocktail::vr
