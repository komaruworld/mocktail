#include "mocktail/vr/openxr_backend.h"
#include "mocktail/vr/vr_perf.h"
#include "mocktail/vr/vr_pose_math.h"

#include "vr/gles_transport.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace mocktail::vr {
namespace {

using Clock = std::chrono::steady_clock;

void Log(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  std::vfprintf(stderr, format, arguments);
  va_end(arguments);
  std::fflush(stderr);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename T>
T XrInfo(XrStructureType type) {
  T value{};
  value.type = type;
  return value;
}

std::uint64_t EnvUnsigned(const char* name, std::uint64_t fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  const unsigned long long parsed = std::strtoull(value, nullptr, 10);
  return parsed > 0 ? static_cast<std::uint64_t>(parsed) : fallback;
}

double EnvDouble(const char* name, double fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  return std::strtod(value, nullptr);
}

std::string EnvString(const char* name, const char* fallback) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' ? std::string(value)
                                              : std::string(fallback);
}

// Coordinate and pose math is shared with the unit tests through
// vr_pose_math.h, which documents every space and unit in the camera path, so
// the audited conventions cannot silently diverge from the backend.
using pose::Pose;
using pose::PoseInverse;
using pose::PoseMultiply;
using pose::Quat;
using pose::QuatFromYawPitch;
using pose::Vec3;

Pose XrToPose(const XrPosef& pose) {
  Pose result;
  result.position = Vec3{pose.position.x, pose.position.y, pose.position.z};
  result.orientation =
      Quat{pose.orientation.x, pose.orientation.y, pose.orientation.z,
           pose.orientation.w};
  return result;
}

// Bridge provenance exports (same-process direct linkage; the evidence layer
// resolves the identical symbols through dlsym).
extern "C" void* mocktail_vr_initializing_object();
extern "C" bool mocktail_vr_current_eye(void** self, int* eye,
                                        void** framebuffer);
extern "C" void mocktail_vr_consume_eye();

bool IsColorFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
      return false;
    default:
      return true;
  }
}

bool FormatIsCopyCompatible(VkFormat a, VkFormat b) {
  return a == b;
}

OpenXrBackend* g_backend = nullptr;
std::mutex g_backend_slot_mutex;

}  // namespace

struct OpenXrBackend::VkProcs {
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
  PFN_vkCreateBuffer CreateBuffer = nullptr;
  PFN_vkDestroyBuffer DestroyBuffer = nullptr;
  PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
  PFN_vkAllocateMemory AllocateMemory = nullptr;
  PFN_vkFreeMemory FreeMemory = nullptr;
  PFN_vkBindBufferMemory BindBufferMemory = nullptr;
  PFN_vkMapMemory MapMemory = nullptr;
  PFN_vkUnmapMemory UnmapMemory = nullptr;
  PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
  PFN_vkCreateImageView CreateImageView = nullptr;
  PFN_vkDestroyImageView DestroyImageView = nullptr;
  PFN_vkCreateCommandPool CreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
  PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
  PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyImage CmdCopyImage = nullptr;
  PFN_vkCmdBlitImage CmdBlitImage = nullptr;
  PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
  PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
  PFN_vkCreateFence CreateFence = nullptr;
  PFN_vkDestroyFence DestroyFence = nullptr;
  PFN_vkResetFences ResetFences = nullptr;
  PFN_vkWaitForFences WaitForFences = nullptr;
  PFN_vkQueueSubmit QueueSubmit = nullptr;
  PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties =
      nullptr;
  PFN_vkGetPhysicalDeviceQueueFamilyProperties
      GetPhysicalDeviceQueueFamilyProperties = nullptr;
  PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
};

namespace {

// Records elapsed monotonic time into the opt-in perf collector. A no-op
// unless MOCKTAIL_VR_PERF is set, so the steady-state path is unaffected.
class ScopedStageTimer final {
 public:
  explicit ScopedStageTimer(perf::Stage stage)
      : stage_(stage), start_(perf::Enabled() ? perf::NowNs() : 0) {}
  ~ScopedStageTimer() {
    if (start_ != 0) {
      const std::uint64_t end = perf::NowNs();
      if (end >= start_) {
        perf::ProcessCollector().Record(stage_, end - start_);
      }
    }
  }
  ScopedStageTimer(const ScopedStageTimer&) = delete;
  ScopedStageTimer& operator=(const ScopedStageTimer&) = delete;

 private:
  perf::Stage stage_;
  std::uint64_t start_;
};

std::string XrResultText(XrInstance instance, XrResult result) {
  char text[XR_MAX_RESULT_STRING_SIZE]{};
  if (instance != XR_NULL_HANDLE) {
    (void)xrResultToString(instance, result, text);
  }
  if (!text[0]) {
    return std::to_string(static_cast<int>(result));
  }
  return text;
}

struct XrCallError : std::runtime_error {
  XrResult result;
  XrCallError(XrResult value, const std::string& message)
      : std::runtime_error(message), result(value) {}
};

void CheckXr(XrInstance instance, XrResult result, const char* operation) {
  if (XR_SUCCEEDED(result)) {
    return;
  }
  std::string message =
      std::string(operation) + " failed: " + XrResultText(instance, result);
  throw XrCallError(result, message);
}

}  // namespace

OpenXrBackend::~OpenXrBackend() { Disarm(); }

OpenXrBackend* ActiveVrBackend() {
  std::lock_guard<std::mutex> lock(g_backend_slot_mutex);
  return g_backend;
}

Status OpenXrBackend::Arm(VrGraphicsApi graphics_api) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (armed_.load(std::memory_order_acquire)) {
    return Status::Ok();
  }
  {
    std::lock_guard<std::mutex> slot_lock(g_backend_slot_mutex);
    if (g_backend != nullptr && g_backend != this) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "another OpenXR backend owns the process");
    }
    g_backend = this;
  }
  try {
    const std::string user_manifest = EnvString("XR_RUNTIME_JSON", "");
    const auto preference = EnvString("MOCKTAIL_VR_RUNTIME", "auto");
    Require(
        preference == "auto" || preference == "system" ||
            preference == "wivrn" || preference == "steamvr" ||
            preference == "alvr",
        "MOCKTAIL_VR_RUNTIME must be auto, system, wivrn, steamvr, or alvr");
    const auto discovery =
        user_manifest.empty()
            ? DiscoverVrRuntime(
                  preference, EnvString("HOME", ""),
                  EnvString("XDG_CONFIG_HOME", ""),
                  EnvString("XDG_CONFIG_DIRS", ""),
                  EnvString("XDG_RUNTIME_DIR",
                            ("/run/user/" + std::to_string(getuid())).c_str()))
            : VrRuntimeDiscovery{{}, "explicit XR_RUNTIME_JSON override"};
    const auto manifest =
        SelectVrRuntimeManifest(user_manifest, discovery.candidates);
    Log("  [vr-backend] runtime selection: %s\n", discovery.reason.c_str());
    Require(!manifest.empty() ||
                (preference != "steamvr" && preference != "alvr" &&
                 preference != "wivrn"),
            "Selected VR runtime was not found. For ALVR, install SteamVR and "
            "set XR_RUNTIME_JSON "
            "to its steamxr_linux64.json if automatic discovery cannot find "
            "the installation. "
            "For WiVRn, install its native runtime or select its manifest "
            "explicitly.");
    if (!manifest.empty()) {
      Require(setenv("XR_RUNTIME_JSON", manifest.c_str(), 1) == 0,
              "Cannot select the OpenXR runtime");
      Log("  [vr-backend] runtime manifest: %s\n", manifest.c_str());
    } else {
      Log("  [vr-backend] using registered OpenXR runtime; connect the headset "
          "in WiVRn or SteamVR/ALVR first\n");
    }
    graphics_api_ = graphics_api;
    if (graphics_api_ == VrGraphicsApi::kVulkan) {
      vk_loader_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
      Require(vk_loader_ != nullptr,
              "Cannot load the host Vulkan loader libvulkan.so.1");
      auto loader_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
          dlsym(vk_loader_, "vkGetInstanceProcAddr"));
      Require(loader_gipa != nullptr, "Host vkGetInstanceProcAddr is missing");
    }
    const char *vk_extensions[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    const char *gl_extensions[] = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
                                   XR_MNDX_EGL_ENABLE_EXTENSION_NAME};
    auto info = XrInfo<XrInstanceCreateInfo>(XR_TYPE_INSTANCE_CREATE_INFO);
    std::strcpy(info.applicationInfo.applicationName, "Mocktail Roblox VR");
    std::strcpy(info.applicationInfo.engineName, "Mocktail");
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount =
        graphics_api_ == VrGraphicsApi::kVulkan ? 1 : 2;
    info.enabledExtensionNames =
        graphics_api_ == VrGraphicsApi::kVulkan ? vk_extensions : gl_extensions;
    XrInstance instance = XR_NULL_HANDLE;
    const XrResult instance_result = xrCreateInstance(&info, &instance);
    if (instance_result == XR_ERROR_RUNTIME_UNAVAILABLE) {
      throw std::runtime_error("xrCreateInstance: " +
          VrRuntimeUnavailableHint(!user_manifest.empty(), manifest));
    }
    if (instance_result == XR_ERROR_EXTENSION_NOT_PRESENT &&
        graphics_api_ == VrGraphicsApi::kOpenGles) {
      throw std::runtime_error(
          "OpenGL VR requires runtime extensions XR_KHR_opengl_es_enable and "
          "XR_MNDX_egl_enable; select direct-vulkan if the runtime does not "
          "support EGL.");
    }
    CheckXr(XR_NULL_HANDLE, instance_result, "xrCreateInstance");
    xr_instance_ = instance;

    XrInstanceProperties properties =
        XrInfo<XrInstanceProperties>(XR_TYPE_INSTANCE_PROPERTIES);
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &properties))) {
      runtime_name_ = properties.runtimeName;
      Log("  [vr-backend] OpenXR runtime: %s %u.%u.%u\n",
          properties.runtimeName,
          XR_VERSION_MAJOR(properties.runtimeVersion),
          XR_VERSION_MINOR(properties.runtimeVersion),
          XR_VERSION_PATCH(properties.runtimeVersion));
    }

    auto system = XrInfo<XrSystemGetInfo>(XR_TYPE_SYSTEM_GET_INFO);
    system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const XrResult system_result =
        xrGetSystem(instance, &system, &xr_system_);
    if (system_result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
      // The runtime answered but exposes no HMD: with WiVRn this is the exact
      // state while the server runs with no headset connected.
      throw std::runtime_error(
          "The OpenXR runtime is reachable but reports no connected headset "
          "(XR_ERROR_FORM_FACTOR_UNAVAILABLE). Start WiVRn or SteamVR with "
          "ALVR "
          "and connect the headset first; VR applications can only run while "
          "the headset connection is established.");
    }
    CheckXr(instance, system_result, "xrGetSystem");
    Require(xr_system_ != XR_NULL_SYSTEM_ID,
            "The OpenXR runtime exposes no head-mounted display system");

    if (graphics_api_ == VrGraphicsApi::kVulkan) {
      auto graphics_requirements = XrInfo<XrGraphicsRequirementsVulkan2KHR>(
          XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR);
      auto get_requirements =
          reinterpret_cast<PFN_xrGetVulkanGraphicsRequirements2KHR>(
              ResolveFunction("xrGetVulkanGraphicsRequirements2KHR"));
      CheckXr(instance,
              get_requirements(instance, xr_system_, &graphics_requirements),
              "xrGetVulkanGraphicsRequirements2KHR");
      Log("  [vr-backend] runtime Vulkan requirement: %u.%u - %u.%u\n",
          XR_VERSION_MAJOR(graphics_requirements.minApiVersionSupported),
          XR_VERSION_MINOR(graphics_requirements.minApiVersionSupported),
          XR_VERSION_MAJOR(graphics_requirements.maxApiVersionSupported),
          XR_VERSION_MINOR(graphics_requirements.maxApiVersionSupported));

    } else {
      auto requirements = XrInfo<XrGraphicsRequirementsOpenGLESKHR>(
          XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR);
      auto get = reinterpret_cast<PFN_xrGetOpenGLESGraphicsRequirementsKHR>(
          ResolveFunction("xrGetOpenGLESGraphicsRequirementsKHR"));
      CheckXr(instance, get(instance, xr_system_, &requirements),
              "xrGetOpenGLESGraphicsRequirementsKHR");
      gl_min_version_ = requirements.minApiVersionSupported;
      gl_max_version_ = requirements.maxApiVersionSupported;
    }
    // Controller actions are instance-level: create them once here so every
    // session (including a recreated one) can attach without re-suggesting
    // bindings. A failure degrades to no controller input, not to no XR.
    std::lock_guard<std::mutex> controller_lock(controller_mutex_);
    const Status actions_status = actions_.Create(instance);
    actions_created_ = actions_status.ok();
    if (!actions_created_) {
      Log("  [vr-backend] controller actions unavailable: %s (XR output "
          "continues without controller input)\n",
          actions_status.message().c_str());
    }
    armed_.store(true, std::memory_order_release);
    Log("  [vr-backend] armed: OpenXR instance/system ready; awaiting guest "
        "%s\n",
        graphics_api_ == VrGraphicsApi::kVulkan ? "Vulkan device"
                                                : "OpenGL ES / EGL context");
    return Status::Ok();
  } catch (const std::exception& error) {
    DisarmLocked();
    return Status::Error(StatusCode::kUnavailable, error.what());
  }
}

void OpenXrBackend::Disarm() {
  std::lock_guard<std::mutex> lock(mutex_);
  DisarmLocked();
}

void OpenXrBackend::DisarmLocked() {
  TeardownSessionLocked("disarm");
  { std::lock_guard<std::mutex> lock(controller_mutex_);
    actions_.Destroy();
    actions_created_ = false; }
  if (gles_)
    gles_->Destroy();
  delete gles_;
  gles_ = nullptr;
  if (xr_instance_ != nullptr) {
    const auto instance = static_cast<XrInstance>(xr_instance_);
    (void)xrDestroyInstance(instance);
    xr_instance_ = nullptr;
  }
  xr_system_ = 0;
  runtime_name_.clear();
  vk_instance_ = VK_NULL_HANDLE;
  if (vk_loader_ != nullptr) {
    dlclose(vk_loader_);
    vk_loader_ = nullptr;
  }
  armed_.store(false, std::memory_order_release);
  recording_.store(false, std::memory_order_release);
  eyes_bound_.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> slot_lock(g_backend_slot_mutex);
    if (g_backend == this) {
      g_backend = nullptr;
    }
  }
}

std::string OpenXrBackend::runtime_name() const {
  std::lock_guard<std::mutex> lock(mutex_);
  // Captured once at xrCreateInstance; reporting the cached value avoids a
  // second xrGetInstanceProperties call and re-entrancy from the bind-time
  // diagnostics that already hold the mutex.
  return runtime_name_;
}

void* OpenXrBackend::ResolveFunction(const char* name) const {
  PFN_xrVoidFunction function = nullptr;
  CheckXr(static_cast<XrInstance>(xr_instance_),
          xrGetInstanceProcAddr(static_cast<XrInstance>(xr_instance_), name,
                                &function),
          name);
  Require(function != nullptr, std::string("Missing OpenXR function: ") + name);
  return reinterpret_cast<void*>(function);
}

bool OpenXrBackend::CreateVulkanInstance(
    const VkInstanceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkInstance* instance,
    VkResult* result) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!armed_.load(std::memory_order_acquire) || xr_instance_ == nullptr ||
      vk_loader_ == nullptr) {
    return false;
  }
  try {
    auto loader_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(vk_loader_, "vkGetInstanceProcAddr"));
    Require(loader_gipa != nullptr, "Host vkGetInstanceProcAddr is missing");
    auto xr_create = reinterpret_cast<PFN_xrCreateVulkanInstanceKHR>(
        ResolveFunction("xrCreateVulkanInstanceKHR"));
    auto xr_info = XrInfo<XrVulkanInstanceCreateInfoKHR>(
        XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR);
    xr_info.systemId = xr_system_;
    xr_info.pfnGetInstanceProcAddr = loader_gipa;
    xr_info.vulkanCreateInfo = create_info;
    VkInstance created = VK_NULL_HANDLE;
    VkResult vk_result = VK_ERROR_INITIALIZATION_FAILED;
    CheckXr(static_cast<XrInstance>(xr_instance_),
            xr_create(static_cast<XrInstance>(xr_instance_), &xr_info,
                      &created, &vk_result),
            "xrCreateVulkanInstanceKHR");
    if (vk_result != VK_SUCCESS) {
      *result = vk_result;
      *instance = VK_NULL_HANDLE;
      return true;
    }
    vk_instance_ = created;
    *instance = created;
    *result = VK_SUCCESS;
    Log("  [vr-backend] guest VkInstance created through the OpenXR runtime\n");
    return true;
  } catch (const std::exception& error) {
    Log("  [vr-backend] XR instance creation hook failed: %s; falling back "
        "to the plain host loader path\n",
        error.what());
    return false;
  }
}

bool OpenXrBackend::CreateVulkanDevice(VkPhysicalDevice physical_device,
                                       const VkDeviceCreateInfo* create_info,
                                       const VkAllocationCallbacks* allocator,
                                       VkDevice* device, VkResult* result) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!armed_.load(std::memory_order_acquire) || xr_instance_ == nullptr ||
      vk_instance_ == VK_NULL_HANDLE || vk_loader_ == nullptr) {
    return false;
  }
  try {
    auto loader_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(vk_loader_, "vkGetInstanceProcAddr"));
    Require(loader_gipa != nullptr, "Host vkGetInstanceProcAddr is missing");

    auto get_graphics_device =
        reinterpret_cast<PFN_xrGetVulkanGraphicsDevice2KHR>(
            ResolveFunction("xrGetVulkanGraphicsDevice2KHR"));
    auto device_get_info = XrInfo<XrVulkanGraphicsDeviceGetInfoKHR>(
        XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR);
    device_get_info.systemId = xr_system_;
    device_get_info.vulkanInstance = vk_instance_;
    VkPhysicalDevice runtime_device = VK_NULL_HANDLE;
    CheckXr(static_cast<XrInstance>(xr_instance_),
            get_graphics_device(static_cast<XrInstance>(xr_instance_),
                                &device_get_info, &runtime_device),
            "xrGetVulkanGraphicsDevice2KHR");
    if (runtime_device != physical_device) {
      *device = VK_NULL_HANDLE;
      *result = VK_ERROR_INCOMPATIBLE_DRIVER;
      Log("  [vr-backend] refusing a guest GPU different from the OpenXR GPU\n");
      return true;
    }

    auto xr_create = reinterpret_cast<PFN_xrCreateVulkanDeviceKHR>(
        ResolveFunction("xrCreateVulkanDeviceKHR"));
    auto xr_info = XrInfo<XrVulkanDeviceCreateInfoKHR>(
        XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR);
    xr_info.systemId = xr_system_;
    xr_info.pfnGetInstanceProcAddr = loader_gipa;
    xr_info.vulkanPhysicalDevice = physical_device;
    xr_info.vulkanCreateInfo = create_info;
    VkDevice created = VK_NULL_HANDLE;
    VkResult vk_result = VK_ERROR_INITIALIZATION_FAILED;
    CheckXr(static_cast<XrInstance>(xr_instance_),
            xr_create(static_cast<XrInstance>(xr_instance_), &xr_info, &created,
                      &vk_result),
            "xrCreateVulkanDeviceKHR");
    if (vk_result != VK_SUCCESS) {
      *result = vk_result;
      *device = VK_NULL_HANDLE;
      return true;
    }
    *device = created;
    *result = VK_SUCCESS;
    // From here the runtime-created device is the guest device; never fall
    // back to a second plain vkCreateDevice. Session setup failures degrade
    // to the native-stereo diagnostic path with an explicit log.
    try {
      SetupDeviceAfterCreationLocked(physical_device, created, create_info);
    } catch (const std::exception& setup_error) {
      Log("  [vr-backend] XR OUTPUT UNAVAILABLE: %s. Roblox continues in "
          "native-stereo diagnostic mode.\n",
          setup_error.what());
    }
    return true;
  } catch (const std::exception& error) {
    Log("  [vr-backend] XR device creation hook failed: %s; falling back to "
        "the plain host loader path\n",
        error.what());
    return false;
  }
}

void OpenXrBackend::SetupDeviceAfterCreationLocked(
    VkPhysicalDevice physical_device, VkDevice created,
    const VkDeviceCreateInfo* create_info) {
    vk_device_ = created;
    vk_physical_device_ = physical_device;
    auto loader_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(vk_loader_, "vkGetInstanceProcAddr"));
    Require(loader_gipa != nullptr, "Host vkGetInstanceProcAddr is missing");
    delete vk_;
    vk_ = new VkProcs();
    vk_->get_instance_proc_addr = loader_gipa;
    vk_->get_device_proc_addr =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(loader_gipa(
            vk_instance_, "vkGetDeviceProcAddr"));
    Require(vk_->get_device_proc_addr != nullptr,
            "Host vkGetDeviceProcAddr is missing");
#define MOCKTAIL_BACKEND_VK_FN(name)                                          \
    vk_->name = reinterpret_cast<PFN_vk##name>(                               \
        vk_->get_device_proc_addr(created, "vk" #name));                      \
    Require(vk_->name != nullptr, "Missing Vulkan function: vk" #name);
    MOCKTAIL_BACKEND_VK_FN(CreateBuffer)
    MOCKTAIL_BACKEND_VK_FN(DestroyBuffer)
    MOCKTAIL_BACKEND_VK_FN(GetBufferMemoryRequirements)
    MOCKTAIL_BACKEND_VK_FN(AllocateMemory)
    MOCKTAIL_BACKEND_VK_FN(FreeMemory)
    MOCKTAIL_BACKEND_VK_FN(BindBufferMemory)
    MOCKTAIL_BACKEND_VK_FN(MapMemory)
    MOCKTAIL_BACKEND_VK_FN(UnmapMemory)
    MOCKTAIL_BACKEND_VK_FN(CmdCopyImageToBuffer)
    MOCKTAIL_BACKEND_VK_FN(CreateImageView)
    MOCKTAIL_BACKEND_VK_FN(DestroyImageView)
    MOCKTAIL_BACKEND_VK_FN(CreateCommandPool)
    MOCKTAIL_BACKEND_VK_FN(DestroyCommandPool)
    MOCKTAIL_BACKEND_VK_FN(AllocateCommandBuffers)
    MOCKTAIL_BACKEND_VK_FN(ResetCommandBuffer)
    MOCKTAIL_BACKEND_VK_FN(BeginCommandBuffer)
    MOCKTAIL_BACKEND_VK_FN(EndCommandBuffer)
    MOCKTAIL_BACKEND_VK_FN(CmdPipelineBarrier)
    MOCKTAIL_BACKEND_VK_FN(CmdCopyImage)
    MOCKTAIL_BACKEND_VK_FN(CmdBlitImage)
    MOCKTAIL_BACKEND_VK_FN(CmdClearColorImage)
    MOCKTAIL_BACKEND_VK_FN(CreateFence)
    MOCKTAIL_BACKEND_VK_FN(DestroyFence)
    MOCKTAIL_BACKEND_VK_FN(ResetFences)
    MOCKTAIL_BACKEND_VK_FN(WaitForFences)
    MOCKTAIL_BACKEND_VK_FN(QueueSubmit)
    MOCKTAIL_BACKEND_VK_FN(DeviceWaitIdle)
#undef MOCKTAIL_BACKEND_VK_FN
#define MOCKTAIL_BACKEND_VK_INSTANCE_FN(name)                                 \
    vk_->name = reinterpret_cast<PFN_vk##name>(loader_gipa(                   \
        vk_instance_, "vk" #name));                                           \
    Require(vk_->name != nullptr, "Missing Vulkan function: vk" #name);
    MOCKTAIL_BACKEND_VK_INSTANCE_FN(GetPhysicalDeviceMemoryProperties)
    MOCKTAIL_BACKEND_VK_INSTANCE_FN(GetPhysicalDeviceQueueFamilyProperties)
    MOCKTAIL_BACKEND_VK_INSTANCE_FN(GetPhysicalDeviceProperties)
    MOCKTAIL_BACKEND_VK_INSTANCE_FN(GetPhysicalDeviceFormatProperties)
#undef MOCKTAIL_BACKEND_VK_INSTANCE_FN

    VkPhysicalDeviceProperties device_properties{};
    vk_->GetPhysicalDeviceProperties(physical_device, &device_properties);

    std::string error;
    if (!InitializeSessionLocked(physical_device, created, create_info,
                                 &error)) {
      Log("  [vr-backend] XR OUTPUT UNAVAILABLE on GPU '%s': %s. Roblox "
          "continues in native-stereo diagnostic mode.\n",
          device_properties.deviceName, error.c_str());
      return;
    }
    // One-time plain-text diagnostics required for headset acceptance: which
    // runtime answered, which pose source drives the camera, and which GPU owns
    // the session. No credentials, tokens or launch parameters are included.
    Log("  [vr-backend] XR session bound to guest VkDevice: gpu=%s runtime=%s "
        "pose_source=%s\n",
        device_properties.deviceName, runtime_name_.c_str(),
        EnvString("MOCKTAIL_VR_POSE_SOURCE", "runtime").c_str());
}

bool OpenXrBackend::InitializeSessionLocked(
    VkPhysicalDevice physical_device, VkDevice device,
    const VkDeviceCreateInfo* create_info, std::string* error) {
  try {
    // Pick the queue family the guest gave graphics queues on; the XR binding
    // must name a family of this very device.
    std::uint32_t family_count = 0;
    vk_->GetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count,
                                                nullptr);
    Require(family_count > 0 && family_count <= 256,
            "the GPU has no usable queue families");
    std::vector<VkQueueFamilyProperties> families(family_count);
    vk_->GetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count,
                                                families.data());
    std::uint32_t chosen_family = create_info ? UINT32_MAX : vk_queue_family_;
    for (std::uint32_t index = 0; create_info && index < create_info->queueCreateInfoCount;
         ++index) {
      const VkDeviceQueueCreateInfo& queue_info =
          create_info->pQueueCreateInfos[index];
      if (queue_info.queueFamilyIndex >= family_count ||
          queue_info.queueCount == 0) {
        continue;
      }
      if (families[queue_info.queueFamilyIndex].queueFlags &
          VK_QUEUE_GRAPHICS_BIT) {
        chosen_family = queue_info.queueFamilyIndex;
        break;
      }
    }
    Require(chosen_family < family_count && (families[chosen_family].queueFlags & VK_QUEUE_GRAPHICS_BIT),
            "the guest device requested no graphics queue family");
    vk_queue_family_ = chosen_family;

    auto binding = XrInfo<XrGraphicsBindingVulkan2KHR>(
        XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR);
    binding.instance = vk_instance_;
    binding.physicalDevice = physical_device;
    binding.device = device;
    binding.queueFamilyIndex = chosen_family;
    binding.queueIndex = 0;
    auto session_info =
        XrInfo<XrSessionCreateInfo>(XR_TYPE_SESSION_CREATE_INFO);
    session_info.next = &binding;
    session_info.systemId = xr_system_;
    XrSession session = XR_NULL_HANDLE;
    CheckXr(static_cast<XrInstance>(xr_instance_),
            xrCreateSession(static_cast<XrInstance>(xr_instance_),
                            &session_info, &session),
            "xrCreateSession");
    xr_session_ = session;

    return InitializeSessionResourcesLocked(error);
  } catch (const std::exception &exception) {
    *error = exception.what();
    TeardownSessionLocked("session init failure", session_recovery_pending_);
    return false;
  }
}

Status OpenXrBackend::AttachGlesContext(void *display, void *config,
                                        void *context, void *egl_get_proc,
                                        void *(*resolve)(const char *)) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!armed_ || graphics_api_ != VrGraphicsApi::kOpenGles)
    return Status::Ok();
  try {
    if (gles_) {
      Require(gles_->context() == context && gles_->display() == display &&
                  gles_->IsCurrent(),
              "OpenGL VR cannot switch to a different live EGL context");
      gles_->MakeCurrent();
      return Status::Ok();
    }
    gles_ = new GlesTransport;
    std::string error;
    Require(gles_->Initialize(display, config, context, egl_get_proc, resolve,
                              &error),
            error);
    Require(gles_->version() >= gl_min_version_ &&
                gles_->version() <= gl_max_version_,
            "EGL context version is outside the OpenXR runtime's OpenGL ES "
            "requirements");
    Require(InitializeGlesSessionLocked(&error), error);
    Log("  [vr-backend] bound OpenGL ES context to OpenXR; GPU eye copies "
        "enabled, pose_source=%s\n",
        EnvString("MOCKTAIL_VR_POSE_SOURCE", "runtime").c_str());
    return Status::Ok();
  } catch (const std::exception &error) {
    TeardownSessionLocked("EGL binding failed");
    if (gles_)
      gles_->Destroy();
    delete gles_;
    gles_ = nullptr;
    return Status::Error(StatusCode::kUnavailable, error.what());
  }
}

bool OpenXrBackend::InitializeGlesSessionLocked(std::string *error) {
  try {
    Require(gles_ && gles_->IsCurrent(),
            "OpenXR EGL session creation requires the current guest context");
    auto binding =
        XrInfo<XrGraphicsBindingEGLMNDX>(XR_TYPE_GRAPHICS_BINDING_EGL_MNDX);
    binding.display = gles_->display();
    binding.config = gles_->config();
    binding.context = gles_->context();
    binding.getProcAddress =
        reinterpret_cast<PFN_xrEglGetProcAddressMNDX>(gles_->egl_get_proc());
    auto info = XrInfo<XrSessionCreateInfo>(XR_TYPE_SESSION_CREATE_INFO);
    info.next = &binding;
    info.systemId = xr_system_;
    XrSession session = XR_NULL_HANDLE;
    CheckXr(
        static_cast<XrInstance>(xr_instance_),
        xrCreateSession(static_cast<XrInstance>(xr_instance_), &info, &session),
        "xrCreateSession EGL");
    xr_session_ = session;
    return InitializeSessionResourcesLocked(error);
  } catch (const std::exception &exception) {
    *error = exception.what();
    TeardownSessionLocked("EGL session init failure",
                          session_recovery_pending_);
    return false;
  }
}

void OpenXrBackend::DetachGlesContext(void *context) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!gles_ || gles_->context() != context)
    return;
  TeardownSessionLocked("EGL context destruction");
  gles_->Destroy();
  delete gles_;
  gles_ = nullptr;
}

void *OpenXrBackend::WrapGlesProcAddress(const char *name, void *raw) {
  if (!armed_ || graphics_api_ != VrGraphicsApi::kOpenGles || !gles_)
    return raw;
  return GlesTransport::Wrap(name, raw);
}

void OpenXrBackend::NoteGlesPresent() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!armed_ || graphics_api_ != VrGraphicsApi::kOpenGles || !gles_ ||
        !gles_->IsCurrent())
      return;
    for (int i = 0; i < 2; ++i) {
      const auto &eye = gles_->eye(i);
      eyes_[i].valid = eye.framebuffer != 0;
      eyes_[i].frame = eye.frame;
      eyes_[i].owner = eye.owner;
      eyes_[i].width = eye.width;
      eyes_[i].height = eye.height;
    }
    const auto &left = gles_->eye(0);
    const auto &right = gles_->eye(1);
    eyes_bound_ =
        left.framebuffer && right.framebuffer &&
        left.framebuffer != right.framebuffer && left.owner == right.owner &&
        (left.texture != right.texture || left.object != right.object);
  }
  NoteHostPresent(VK_NULL_HANDLE, VK_NULL_HANDLE);
}

bool OpenXrBackend::CopyGlesEyesIntoSwapchains() {
  if (!gles_)
    return false;
  unsigned textures[2] = {};
  for (int i = 0; i < 2; ++i) {
    const auto &slot = eye_swapchains_[i];
    if (!slot.image_acquired || slot.acquired_index >= slot.gl_images.size())
      return false;
    textures[i] = slot.gl_images[slot.acquired_index];
  }
  Require(gles_->Copy(textures, eye_swapchains_[0].width,
                      eye_swapchains_[0].height),
          "Cannot copy GLES eyes into runtime textures; verify EGL and OpenXR "
          "use the same GPU and support image sharing");
  return true;
}

bool OpenXrBackend::InitializeSessionResourcesLocked(std::string *error) {
  try {
    const auto session = static_cast<XrSession>(xr_session_);
    auto space_info = XrInfo<XrReferenceSpaceCreateInfo>(
        XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
    space_info.poseInReferenceSpace.orientation.w = 1;
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrSpace local_space = XR_NULL_HANDLE;
    CheckXr(static_cast<XrInstance>(xr_instance_),
            xrCreateReferenceSpace(session, &space_info, &local_space),
            "xrCreateReferenceSpace LOCAL");
    xr_local_space_ = local_space;
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XrSpace view_space = XR_NULL_HANDLE;
    CheckXr(static_cast<XrInstance>(xr_instance_),
            xrCreateReferenceSpace(session, &space_info, &view_space),
            "xrCreateReferenceSpace VIEW");
    xr_view_space_ = view_space;

    auto system_properties =
        XrInfo<XrSystemProperties>(XR_TYPE_SYSTEM_PROPERTIES);
    CheckXr(static_cast<XrInstance>(xr_instance_),
            xrGetSystemProperties(static_cast<XrInstance>(xr_instance_),
                                  xr_system_, &system_properties),
            "xrGetSystemProperties");
    Log("  [vr-backend] system '%s' max swapchain %ux%u views=%u\n",
        system_properties.systemName,
        system_properties.graphicsProperties.maxSwapchainImageWidth,
        system_properties.graphicsProperties.maxSwapchainImageHeight,
        system_properties.graphicsProperties.maxLayerCount);

    std::uint32_t blend_count = 0;
    CheckXr(static_cast<XrInstance>(xr_instance_),
            xrEnumerateEnvironmentBlendModes(
                static_cast<XrInstance>(xr_instance_), xr_system_,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &blend_count,
                nullptr),
            "xrEnumerateEnvironmentBlendModes count");
    Require(blend_count > 0 && blend_count <= 16,
            "the runtime exposes no environment blend modes");
    std::array<XrEnvironmentBlendMode, 16> blends{};
    CheckXr(static_cast<XrInstance>(xr_instance_),
            xrEnumerateEnvironmentBlendModes(
                static_cast<XrInstance>(xr_instance_), xr_system_,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, blend_count,
                &blend_count, blends.data()),
            "xrEnumerateEnvironmentBlendModes");
    xr_blend_mode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    for (std::uint32_t index = 0; index < blend_count; ++index) {
      if (blends[index] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
        xr_blend_mode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        break;
      }
      xr_blend_mode_ = static_cast<int>(blends[index]);
    }

    std::uint32_t view_count = 0;
    const XrInstance instance = static_cast<XrInstance>(xr_instance_);
    CheckXr(instance,
            xrEnumerateViewConfigurationViews(
                instance, static_cast<XrSystemId>(xr_system_),
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count,
                nullptr),
            "xrEnumerateViewConfigurationViews count");
    Require(view_count >= 2,
            "OpenXR primary stereo configuration exposes fewer than two views");

    std::vector<XrViewConfigurationView> config_views(view_count);
    for (auto& view : config_views) {
      view = XrInfo<XrViewConfigurationView>(XR_TYPE_VIEW_CONFIGURATION_VIEW);
    }

    CheckXr(instance,
            xrEnumerateViewConfigurationViews(
                instance, static_cast<XrSystemId>(xr_system_),
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count,
                &view_count, config_views.data()),
            "xrEnumerateViewConfigurationViews");

    xr_eye_width_[0] = config_views[0].recommendedImageRectWidth;
    xr_eye_height_[0] = config_views[0].recommendedImageRectHeight;
    xr_eye_width_[1] = config_views[1].recommendedImageRectWidth;
    xr_eye_height_[1] = config_views[1].recommendedImageRectHeight;

    Require(xr_eye_width_[0] != 0 && xr_eye_height_[0] != 0 &&
                xr_eye_width_[1] != 0 && xr_eye_height_[1] != 0,
            "OpenXR returned zero recommended stereo eye extent");

    Log("  [vr-backend] OpenXR recommended eye extents L=%ux%u R=%ux%u\n",
        xr_eye_width_[0], xr_eye_height_[0], xr_eye_width_[1],
        xr_eye_height_[1]);

    if (!CreateSwapchains(error)) {
      TeardownSessionLocked("swapchain failure", session_recovery_pending_);
      return false;
    }
    // Attach the action set to this session and create its pose spaces. A
    // failure leaves XR output working without controller input.
    std::lock_guard<std::mutex> controller_lock(controller_mutex_);
    if (actions_created_) {
      const Status attach_status =
          actions_.Attach(xr_instance_, xr_session_, xr_local_space_);
      if (!attach_status.ok()) {
        Log("  [vr-backend] controller actions not attached: %s\n",
            attach_status.message().c_str());
      }
    }
    recording_.store(true, std::memory_order_release);
    return true;
  } catch (const std::exception &exception) {
    *error = exception.what();
    TeardownSessionLocked("session init failure", session_recovery_pending_);
    return false;
  }
}
bool OpenXrBackend::CreateSwapchains(std::string* error) {
  try {
    const auto session = static_cast<XrSession>(xr_session_);
    std::uint32_t format_count = 0;
    CheckXr(static_cast<XrInstance>(xr_instance_),
            xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr),
            "xrEnumerateSwapchainFormats count");
    Require(format_count > 0 && format_count <= 256,
            "the runtime exposes no swapchain formats");
    std::vector<std::int64_t> formats(format_count);
    CheckXr(static_cast<XrInstance>(xr_instance_),
            xrEnumerateSwapchainFormats(session, format_count, &format_count,
                                        formats.data()),
            "xrEnumerateSwapchainFormats");
    // The swapchain format must accept the guest eye content. Prefer the
    // common 8-bit formats; the final choice is cross-checked against the
    // recorded eye images before the first copy (blit converts if needed).
    std::int64_t chosen = 0;
    const std::vector<std::int64_t> candidates =
        graphics_api_ == VrGraphicsApi::kVulkan
            ? std::vector<std::int64_t>{VK_FORMAT_B8G8R8A8_UNORM,
                                        VK_FORMAT_R8G8B8A8_UNORM,
                                        VK_FORMAT_B8G8R8A8_SRGB,
                                        VK_FORMAT_R8G8B8A8_SRGB}
            : std::vector<std::int64_t>{GL_RGBA8, GL_SRGB8_ALPHA8};
    for (const auto candidate : candidates) {
      if (std::find(formats.begin(), formats.end(),
                    static_cast<std::int64_t>(candidate)) != formats.end()) {
        chosen = candidate;
        break;
      }
    }
    Require(chosen != VK_FORMAT_UNDEFINED,
            "the runtime exposes no 8-bit RGBA swapchain format");

    const std::uint32_t width =
        std::max(xr_eye_width_[0], xr_eye_width_[1]);
    const std::uint32_t height =
        std::max(xr_eye_height_[0], xr_eye_height_[1]);
    for (int eye = 0; eye < 2; ++eye) {
      auto create_info =
          XrInfo<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
      create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                               XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
      if (!EnvString("MOCKTAIL_VR_XR_EVIDENCE_DIR", "").empty())
        create_info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
      create_info.format = static_cast<std::int64_t>(chosen);
      create_info.sampleCount = 1;
      create_info.width = width;
      create_info.height = height;
      create_info.faceCount = 1;
      create_info.arraySize = 1;
      create_info.mipCount = 1;
      XrSwapchain swapchain = XR_NULL_HANDLE;
      CheckXr(static_cast<XrInstance>(xr_instance_),
              xrCreateSwapchain(session, &create_info, &swapchain),
              "xrCreateSwapchain");
      EyeSwapchain &slot = eye_swapchains_[eye];
      slot.swapchain =
          swapchain; // Own immediately, including enumeration failures.
      std::uint32_t image_count = 0;
      CheckXr(static_cast<XrInstance>(xr_instance_),
              xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr),
              "xrEnumerateSwapchainImages count");
      Require(image_count > 0 && image_count <= 16,
              "the runtime returned an empty swapchain");
      if (graphics_api_ == VrGraphicsApi::kVulkan) {
        std::vector<XrSwapchainImageVulkanKHR> images(image_count);
        for (auto &image : images) {
          image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
        }
        CheckXr(
            static_cast<XrInstance>(xr_instance_),
            xrEnumerateSwapchainImages(
                swapchain, image_count, &image_count,
                reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data())),
            "xrEnumerateSwapchainImages");
        slot.images.clear();
        for (const auto &image : images) {
          slot.images.push_back(image.image);
        }
      } else {
        std::vector<XrSwapchainImageOpenGLESKHR> images(image_count);
        for (auto &image : images)
          image.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        CheckXr(
            static_cast<XrInstance>(xr_instance_),
            xrEnumerateSwapchainImages(
                swapchain, image_count, &image_count,
                reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data())),
            "xrEnumerateSwapchainImages GLES");
        slot.gl_images.clear();
        for (const auto &image : images) {
          slot.gl_images.push_back(image.image);
        }
      }
      slot.width = width;
      slot.height = height;
      slot.format = static_cast<VkFormat>(chosen);
      Log("  [vr-backend] eye%d swapchain: %ux%u format=%d images=%u\n", eye,
          width, height, static_cast<int>(chosen), image_count);
    }
    return true;
  } catch (const std::exception& exception) {
    *error = exception.what();
    return false;
  }
}

void OpenXrBackend::DestroySwapchains() {
  for (EyeSwapchain& slot : eye_swapchains_) {
    slot.images.clear();
    slot.gl_images.clear();
    if (slot.swapchain != nullptr) {
      (void)xrDestroySwapchain(static_cast<XrSwapchain>(slot.swapchain));
      slot.swapchain = nullptr;
    }
    slot.image_acquired = false;
    slot.acquired_index = 0;
  }
}

void OpenXrBackend::TeardownSessionLocked(const char* reason, bool preserve_device) {
  if (xr_session_ != nullptr) {
    const auto session = static_cast<XrSession>(xr_session_);
    if (frame_open_) {
      auto end = XrInfo<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
      end.displayTime = frame_display_time_;
      end.environmentBlendMode = static_cast<XrEnvironmentBlendMode>(xr_blend_mode_);
      (void)xrEndFrame(session, &end);
      frame_open_ = false;
    }
    // Destruction also ends a running session; xrEndSession is legal only in
    // STOPPING (handled by PollSessionEvents).
    if (vk_ && vk_device_) {
      (void)vk_->DeviceWaitIdle(vk_device_);
      // Mirror staging lives on this device; free it before the device goes.
      mirror_.DestroyStaging(MirrorProcs());
    }
    mirror_.Close();
    DestroyCopyResources();
    DestroySwapchains();
    // Action pose spaces belong to this session; detach before the session and
    // its spaces go away so a recreated session attaches cleanly.
    { std::lock_guard<std::mutex> lock(controller_mutex_); actions_.Detach(); }
    if (xr_view_space_ != nullptr) {
      (void)xrDestroySpace(static_cast<XrSpace>(xr_view_space_));
      xr_view_space_ = nullptr;
    }
    if (xr_local_space_ != nullptr) {
      (void)xrDestroySpace(static_cast<XrSpace>(xr_local_space_));
      xr_local_space_ = nullptr;
    }
    (void)xrDestroySession(session);
    xr_session_ = nullptr;
    Log("  [vr-backend] XR session torn down (%s)\n", reason);
  }
  session_running_.store(false, std::memory_order_relaxed);
  recording_.store(false, std::memory_order_release);
  eyes_bound_.store(false, std::memory_order_release);
  frame_open_ = false;
  frame_views_valid_ = false;
  visibility_lost_ = false;
  canted_rejection_logged_ = false;
  eyes_[0] = {};
  eyes_[1] = {};
  if (gles_)
    gles_->ResetEyes();
  applied_poses_.clear();
  published_pose_.valid = false;
  if (preserve_device) {
    // Resource callbacks must continue tracking destruction/recreation while
    // disconnected. Keep provenance, never resurrect a destroyed guest image.
    recording_.store(true, std::memory_order_release);
    session_recovery_pending_ = true;
    return;
  }
  session_recovery_pending_ = false;
  next_session_retry_ns_ = 0;
  images_.clear();
  desktop_swapchains_.clear();
  image_views_.clear();
  framebuffer_views_.clear();
  framebuffer_render_pass_.clear();
  render_pass_final_layouts_.clear();
  vk_device_ = VK_NULL_HANDLE;
  vk_physical_device_ = VK_NULL_HANDLE;
  queue_families_.clear();
  delete vk_;
  vk_ = nullptr;
}

void OpenXrBackend::NoteInstanceDestroyed(VkInstance instance) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (instance == vk_instance_) {
    TeardownSessionLocked("guest VkInstance destroyed");
    vk_instance_ = VK_NULL_HANDLE;
  }
}

void OpenXrBackend::NoteDeviceDestroyed(VkDevice device) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (device != VK_NULL_HANDLE && device == vk_device_) {
    // Tear down while the device is still valid; the adapter notifies before
    // the host destroy call.
    TeardownSessionLocked("guest VkDevice destroyed");
  }
}

void OpenXrBackend::NoteQueue(VkDevice device, VkQueue queue,
                              std::uint32_t family, std::uint32_t index) {
  static_cast<void>(index);
  std::lock_guard<std::mutex> lock(mutex_);
  if (device == vk_device_ && queue != VK_NULL_HANDLE) {
    queue_families_[queue] = family;
  }
}

void OpenXrBackend::RecordImage(VkDevice device, VkImage image,
                                const VkImageCreateInfo* info) {
  if (!recording_.load(std::memory_order_acquire) || info == nullptr) {
    return;
  }
  void* owner = mocktail_vr_initializing_object();
  if (owner == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  ImageRecord record{};
  record.image = image;
  record.device = device;
  record.width = info->extent.width;
  record.height = info->extent.height;
  record.format = info->format;
  record.usage = info->usage;
  record.owner = owner;
  record.order = ++image_order_;
  images_[image] = record;
}

void OpenXrBackend::RecordImageView(VkImageView view, VkImage image) {
  if (!recording_.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  image_views_[view] = image;
}

void OpenXrBackend::RecordFramebuffer(VkFramebuffer framebuffer,
                                      const VkFramebufferCreateInfo* info) {
  if (!recording_.load(std::memory_order_acquire) || info == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<VkImageView> views;
  views.reserve(info->attachmentCount);
  for (std::uint32_t index = 0; index < info->attachmentCount; ++index) {
    views.push_back(info->pAttachments[index]);
  }
  framebuffer_views_[framebuffer] = std::move(views);
  framebuffer_render_pass_[framebuffer] = info->renderPass;
}

void OpenXrBackend::RecordRenderPass(VkRenderPass render_pass,
                                     const VkRenderPassCreateInfo* info) {
  if (!recording_.load(std::memory_order_acquire) || info == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<VkImageLayout> layouts;
  layouts.reserve(info->attachmentCount);
  for (std::uint32_t index = 0; index < info->attachmentCount; ++index) {
    layouts.push_back(info->pAttachments[index].finalLayout);
  }
  render_pass_final_layouts_[render_pass] = std::move(layouts);
}

void OpenXrBackend::NoteDestroyImage(VkImage image) {
  if (!recording_.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  images_.erase(image);
  for (int index = 0; index < 2; ++index) {
    if (eyes_[index].valid && eyes_[index].image == image) {
      Log("  [vr-backend] eye%d image destroyed; eye binding reset\n", index);
      eyes_[index] = {};
      eyes_bound_.store(false, std::memory_order_release);
    }
  }
}

void OpenXrBackend::NoteDestroyImageView(VkImageView view) {
  if (!recording_.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  image_views_.erase(view);
}

void OpenXrBackend::NoteDestroyFramebuffer(VkFramebuffer framebuffer) {
  if (!recording_.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  framebuffer_views_.erase(framebuffer);
  framebuffer_render_pass_.erase(framebuffer);
  for (int index = 0; index < 2; ++index) {
    if (eyes_[index].valid && eyes_[index].framebuffer == framebuffer) {
      Log("  [vr-backend] eye%d framebuffer destroyed; eye binding reset\n",
          index);
      eyes_[index] = {};
      eyes_bound_.store(false, std::memory_order_release);
    }
  }
}

void OpenXrBackend::NoteRenderPassBegin(VkCommandBuffer command_buffer,
                                        const VkRenderPassBeginInfo* info) {
  static_cast<void>(command_buffer);
  if (!recording_.load(std::memory_order_acquire) || info == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  void* owner = nullptr;
  void* guest_framebuffer = nullptr;
  int eye_index = -1;
  if (!mocktail_vr_current_eye(&owner, &eye_index, &guest_framebuffer) ||
      eye_index < 0 || eye_index > 1) {
    return;
  }
  const auto attachments = framebuffer_views_.find(info->framebuffer);
  if (attachments == framebuffer_views_.end()) {
    return;
  }
  const ImageRecord* target = nullptr;
  bool ambiguous = false;
  std::size_t target_attachment = 0;
  for (std::size_t index = 0; index < attachments->second.size(); ++index) {
    const auto source = image_views_.find(attachments->second[index]);
    if (source == image_views_.end()) {
      continue;
    }
    const auto image = images_.find(source->second);
    if (image == images_.end() || image->second.owner != owner || image->second.device != vk_device_ ||
        !IsColorFormat(image->second.format) ||
        (image->second.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0 ||
        (image->second.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
      continue;
    }
    if (target != nullptr) {
      ambiguous = true;
    }
    target = &image->second;
    target_attachment = index;
  }
  if (target == nullptr || ambiguous) {
    return;
  }
  const auto layouts = render_pass_final_layouts_.find(info->renderPass);
  if (layouts == render_pass_final_layouts_.end() ||
      target_attachment >= layouts->second.size() ||
      layouts->second[target_attachment] == VK_IMAGE_LAYOUT_UNDEFINED) return;
  EyeBinding& binding = eyes_[eye_index];
  if ((eyes_[0].valid && eyes_[0].owner != owner) ||
      (eyes_[1].valid && eyes_[1].owner != owner)) {
    // A new DebugDeviceVR generation replaced the old one; drop both eyes and
    // rebind from the live provenance.
    eyes_[0] = {};
    eyes_[1] = {};
    eyes_bound_.store(false, std::memory_order_release);
  }
  const bool changed = !binding.valid || binding.image != target->image ||
                       binding.framebuffer != info->framebuffer;
  binding.valid = true;
  binding.frame = published_pose_.frame;
  binding.image = target->image;
  binding.framebuffer = info->framebuffer;
  binding.owner = owner;
  binding.width = target->width;
  binding.height = target->height;
  binding.format = target->format;
  binding.final_layout = layouts->second[target_attachment];
  if (changed) Log("  [vr-backend][evidence] eye-bound eye=%d owner=%p guest_fb=%p "
      "vk_fb=%p vk_image=%p extent=%ux%u format=%d final_layout=%d "
      "provenance=guest_initializer_and_eye_getter\n",
      eye_index, owner, guest_framebuffer,
      static_cast<void*>(info->framebuffer), static_cast<void*>(target->image),
      target->width, target->height, static_cast<int>(target->format),
      static_cast<int>(binding.final_layout));
  // Evidence layer also observes this request; do not consume it here.
  if (eyes_[0].valid && eyes_[1].valid &&
      eyes_[0].image != eyes_[1].image && eyes_[0].owner == eyes_[1].owner &&
      eyes_[0].width > 0 && eyes_[1].width > 0 &&
      eyes_[0].width == eyes_[1].width &&
      eyes_[0].height == eyes_[1].height) {
    eyes_bound_.store(true, std::memory_order_release);
    if (changed) Log("  [vr-backend] both eyes bound and verified distinct\n");
  }
}

void OpenXrBackend::NotePoseApplied(void* owner, std::uint64_t frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (published_pose_.valid && published_pose_.frame == frame)
    applied_poses_[owner] = frame;
}

ScriptedPoseSample OpenXrBackend::ComputeScriptedPose(
    std::uint64_t frame) const {
  // Deterministic head motion: position translates along X and Z, yaw and
  // pitch rotate independently. Periods/amplitudes are env-configurable so
  // acceptance runs can verify each axis separately.
  const double period_position =
      static_cast<double>(std::max<std::uint64_t>(1, EnvUnsigned("MOCKTAIL_VR_POSE_PERIOD_POSITION", 240)));
  const double period_rotation =
      static_cast<double>(std::max<std::uint64_t>(1, EnvUnsigned("MOCKTAIL_VR_POSE_PERIOD_ROTATION", 360)));
  const double amplitude_position =
      EnvDouble("MOCKTAIL_VR_POSE_AMPLITUDE_POSITION", 0.25);
  const double amplitude_yaw =
      EnvDouble("MOCKTAIL_VR_POSE_AMPLITUDE_YAW", 0.5);   // ~28.6 degrees
  const double amplitude_pitch =
      EnvDouble("MOCKTAIL_VR_POSE_AMPLITUDE_PITCH", 0.17);  // ~10 degrees
  const double phase_position =
      2.0 * M_PI * static_cast<double>(frame % static_cast<std::uint64_t>(period_position)) /
      period_position;
  const double phase_rotation =
      2.0 * M_PI * static_cast<double>(frame % static_cast<std::uint64_t>(period_rotation)) /
      period_rotation;
  ScriptedPoseSample sample;
  sample.position[0] =
      static_cast<float>(amplitude_position * std::sin(phase_position));
  sample.position[1] = 0.f;
  sample.position[2] =
      static_cast<float>(-0.3 * amplitude_position * std::cos(phase_position));
  const float yaw = static_cast<float>(amplitude_yaw * std::sin(phase_rotation));
  const float pitch =
      static_cast<float>(amplitude_pitch * std::sin(2.0 * phase_rotation));
  const Quat orientation = QuatFromYawPitch(yaw, pitch);
  sample.orientation[0] = orientation.x;
  sample.orientation[1] = orientation.y;
  sample.orientation[2] = orientation.z;
  sample.orientation[3] = orientation.w;
  sample.frame = frame;
  sample.valid = true;
  return sample;
}

ScriptedPoseSample OpenXrBackend::PublishedHeadPose() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return published_pose_;
}

bool OpenXrBackend::RecommendedEyeExtent(std::uint32_t* width,
                                         std::uint32_t* height) const {
  if (width == nullptr || height == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (xr_instance_ == nullptr || xr_system_ == 0 ||
      xr_eye_width_[0] == 0 || xr_eye_height_[0] == 0 ||
      xr_eye_width_[1] == 0 || xr_eye_height_[1] == 0) {
    return false;
  }
  *width = std::max(xr_eye_width_[0], xr_eye_width_[1]);
  *height = std::max(xr_eye_height_[0], xr_eye_height_[1]);
  return true;
}

bool OpenXrBackend::TakeControllerDelivery(ControllerDelivery* out) {
  if (out == nullptr) {
    return false;
  }
  // controller_mutex_ (never mutex_): the frame cycle holds mutex_ across
  // xrWaitFrame runtime pacing, and this drain runs on the window thread.
  std::lock_guard<std::mutex> lock(controller_mutex_);
  return actions_.TakeDelivery(out);
}

ControllerSnapshot OpenXrBackend::ControllerSnapshotForFrame() const {
  std::lock_guard<std::mutex> lock(controller_mutex_);
  return actions_.snapshot();
}

void OpenXrBackend::RequestControllerHaptics(int hand, float amplitude,
                                             std::uint64_t duration_ns,
                                             float frequency_hz) {
  std::lock_guard<std::mutex> lock(controller_mutex_);
  if (actions_created_) {
    actions_.RequestHaptics(hand, amplitude, duration_ns, frequency_hz);
  }
}

void OpenXrBackend::StopControllerHaptics(int hand) {
  std::lock_guard<std::mutex> lock(controller_mutex_);
  if (actions_created_) {
    actions_.StopHaptics(hand);
  }
}

void OpenXrBackend::PollSessionEvents() {
  if (xr_instance_ == nullptr) {
    return;
  }
  const auto instance = static_cast<XrInstance>(xr_instance_);
  while (true) {
    auto event = XrInfo<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
    const XrResult result = xrPollEvent(instance, &event);
    if (result == XR_EVENT_UNAVAILABLE) {
      break;
    }
    if (XR_FAILED(result)) {
      Log("  [vr-backend] xrPollEvent failed: %s\n",
          XrResultText(instance, result).c_str());
      break;
    }
    if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
      Log("  [vr-backend] OpenXR instance loss pending; XR output stopped\n");
      TeardownSessionLocked("instance loss pending");
      return;
    }
    if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
      const auto& change =
          reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(event);
      const bool affects_us =
          change.session == static_cast<XrSession>(xr_session_) &&
          (change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ||
           change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW);
      // poseInPreviousSpace is undefined when poseValid is false. No application
      // origin offset is applied: xrLocate* uses the requested display time.
      Log("  [vr-backend] reference space change pending: type=%d time=%lld poseValid=%d\n",
          static_cast<int>(change.referenceSpaceType),
          static_cast<long long>(change.changeTime), change.poseValid);
      if (affects_us) HandleReferenceSpaceChange(change.changeTime);
      perf::ProcessCollector().Count(perf::Counter::kReferenceSpaceChanges);
      continue;
    }
    if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
      { std::lock_guard<std::mutex> lock(controller_mutex_); actions_.NoteProfileChanged(); }
      Log("  [vr-backend] controller interaction profile changed\n");
      continue;
    }
    if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      continue;
    }
    const auto& changed =
        reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
    if (changed.session != static_cast<XrSession>(xr_session_)) {
      continue;
    }
    HandleSessionState(static_cast<int>(changed.state));
    if (xr_session_ == nullptr) return;
  }
}

void OpenXrBackend::HandleSessionState(int state) {
  if (state != XR_SESSION_STATE_FOCUSED) {
    std::lock_guard<std::mutex> lock(controller_mutex_);
    actions_.ResetInput();
    published_pose_.hands[0] = {};
    published_pose_.hands[1] = {};
    published_pose_.controllers_connected = false;
    std::fill(std::begin(published_pose_.controller_channels), std::end(published_pose_.controller_channels), 0.f);
  }
  switch (static_cast<XrSessionState>(state)) {
      case XR_SESSION_STATE_READY: {
        auto begin =
            XrInfo<XrSessionBeginInfo>(XR_TYPE_SESSION_BEGIN_INFO);
        begin.primaryViewConfigurationType =
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        const XrResult begin_result =
            xrBeginSession(static_cast<XrSession>(xr_session_), &begin);
        if (XR_SUCCEEDED(begin_result)) {
          session_running_.store(true, std::memory_order_release);
          visibility_lost_ = false;
          Log("  [vr-backend] XR session READY; projection submission "
              "starts on the next frame\n");
        } else {
          Log("  [vr-backend] xrBeginSession failed: %s\n",
              XrResultText(static_cast<XrInstance>(xr_instance_), begin_result)
                  .c_str());
        }
        break;
      }
      case XR_SESSION_STATE_STOPPING: {
        if (frame_open_) {
          auto end = XrInfo<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
          end.displayTime = frame_display_time_;
          end.environmentBlendMode =
              static_cast<XrEnvironmentBlendMode>(xr_blend_mode_);
          (void)xrEndFrame(static_cast<XrSession>(xr_session_), &end);
          frame_open_ = false;
        }
        if (session_running_.load(std::memory_order_relaxed)) {
          (void)xrEndSession(static_cast<XrSession>(xr_session_));
        }
        session_running_.store(false, std::memory_order_release);
        Log("  [vr-backend] XR session STOPPING; submission paused\n");
        break;
      }
      case XR_SESSION_STATE_FOCUSED:
        Log("  [vr-backend] XR session FOCUSED\n");
        if (visibility_lost_) {
          // Returning from IDLE to a visible state: the frames rendered while
          // idle are stale, so the first frame after regain is invalidated.
          InvalidateInFlightPose("visibility regained (FOCUSED)");
        }
        visibility_lost_ = false;
        perf::ProcessCollector().Count(perf::Counter::kVisibilityGained);
        break;
      case XR_SESSION_STATE_VISIBLE:
        Log("  [vr-backend] XR session VISIBLE (compositor shows our layers "
            "without input focus)\n");
        if (visibility_lost_) {
          InvalidateInFlightPose("visibility regained (VISIBLE)");
        }
        visibility_lost_ = false;
        perf::ProcessCollector().Count(perf::Counter::kVisibilityGained);
        break;
      case XR_SESSION_STATE_SYNCHRONIZED:
      case XR_SESSION_STATE_IDLE:
        Log("  [vr-backend] XR session not visible (IDLE/SYNCHRONIZED; runtime may "
            "throttle frames)\n");
        visibility_lost_ = true;
        InvalidateInFlightPose("session not visible");
        perf::ProcessCollector().Count(perf::Counter::kVisibilityLost);
        break;
      case XR_SESSION_STATE_EXITING:
        Log("  [vr-backend] XR session EXITING\n");
        session_running_.store(false, std::memory_order_release);
        TeardownSessionLocked("session exiting");
        return;
      case XR_SESSION_STATE_LOSS_PENDING:
        Log("  [vr-backend] XR session LOSS_PENDING\n");
        session_running_.store(false, std::memory_order_release);
        TeardownSessionLocked("session loss pending", true);
        return;
      default:
        break;
    }
}

void OpenXrBackend::InvalidateInFlightPose(const char* reason) {
  // Mark the published pose invalid and clear the per-owner application record
  // so the frame cycle's can_submit gate fails. The rendered images of the
  // in-flight frame stay untouched (the guest owns them); xrEndFrame simply
  // submits no projection layer for this frame rather than retagging a stale
  // image with a pose describing a different origin.
  if (frame_open_ || published_pose_.valid) {
    Log("  [vr-backend] in-flight frame invalidated (%s); skipping its "
        "projection submission\n",
        reason);
  }
  published_pose_.valid = false;
  frame_views_valid_ = false;
  applied_poses_.clear();
  perf::ProcessCollector().Count(perf::Counter::kFramesInvalidated);
}

void OpenXrBackend::HandleReferenceSpaceChange(std::int64_t change_time) {
  // A future event does NOT invalidate an earlier predicted frame. Runtime
  // locates automatically use the new origin for times >= changeTime. Drop
  // only a frame already located when a late notification affects its time.
  if (frame_open_ && frame_views_valid_ &&
      static_cast<std::int64_t>(frame_display_time_) >= change_time) {
    InvalidateInFlightPose("late reference-space change affects rendered frame");
  }
}

bool OpenXrBackend::RecoverSessionLocked(std::uint64_t now_ns) {
  if (!session_recovery_pending_ ||
      (graphics_api_ == VrGraphicsApi::kVulkan
           ? (vk_device_ == VK_NULL_HANDLE || vk_ == nullptr)
           : gles_ == nullptr) ||
      xr_instance_ == nullptr || now_ns < next_session_retry_ns_)
    return false;
  next_session_retry_ns_ = now_ns + 1'000'000'000ULL;
  std::string error;
  // nullptr means reuse the verified queue family of this existing VkDevice.
  if (!(graphics_api_ == VrGraphicsApi::kVulkan
            ? InitializeSessionLocked(vk_physical_device_, vk_device_, nullptr,
                                      &error)
            : InitializeGlesSessionLocked(&error))) {
    Log("  [vr-backend] waiting to restore XR session: %s\n", error.c_str());
    return false;
  }
  session_recovery_pending_ = false;
  Log("  [vr-backend] XR session recreated on the existing guest device; awaiting READY\n");
  return true;
}

void OpenXrBackend::NoteHostPresent(VkQueue queue, VkDevice device) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!armed_.load(std::memory_order_acquire) || device != vk_device_) {
    return;
  }
  try {
    if (xr_session_ == nullptr && !RecoverSessionLocked(perf::NowNs())) return;
    PollSessionEvents();
    if (!session_running_.load(std::memory_order_acquire)) {
      return;
    }
    perf::ProcessCollector().Count(perf::Counter::kPresents);
    RunFrameCycle(queue, device);
    // Explicit, one-shot fault injection for the isolated correctness canary.
    // Never active in a normal authenticated/headset launch.
    const auto loss_frame = EnvUnsigned("MOCKTAIL_VR_TEST_SESSION_LOSS_AFTER", 0);
    if (!test_session_loss_injected_ && loss_frame > 0 &&
        EnvString("MOCKTAIL_ISOLATED_CANARY", "") == "1" &&
        submitted_frames_.load() >= loss_frame) {
      test_session_loss_injected_ = true;
      Log("  [vr-backend][test] injecting session loss on existing guest device\n");
      HandleSessionState(XR_SESSION_STATE_LOSS_PENDING);
    }
    perf::ProcessCollector().EmitIfDue(perf::NowNs(), "xr-frame-cycle");
  } catch (const XrCallError& error) {
    if (error.result == XR_ERROR_SESSION_LOST) {
      Log("  [vr-backend] XR session lost; scheduling recovery on the existing device\n");
      TeardownSessionLocked("XR_ERROR_SESSION_LOST", true);
    } else {
      Log("  [vr-backend] XR frame failed: %s; restart the client if the runtime instance was lost\n",
          error.what());
      TeardownSessionLocked("unrecoverable XR frame error");
    }
  } catch (const std::exception& error) {
    Log("  [vr-backend] frame cycle failed: %s; XR output stopped, native "
        "stereo continues\n",
        error.what());
    TeardownSessionLocked("frame cycle failure");
  }
}

void OpenXrBackend::RunFrameCycle(VkQueue queue, VkDevice device) {
  const ScopedStageTimer cycle_timer(perf::Stage::kFrameCycle);
  const auto session = static_cast<XrSession>(xr_session_);
  const auto instance = static_cast<XrInstance>(xr_instance_);

  // ---- finish the XR frame whose predicted display time covered the frame
  // Roblox just rendered (eye images are complete at present time).
  if (frame_open_) {
    auto end = XrInfo<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
    end.displayTime = frame_display_time_;
    end.environmentBlendMode =
        static_cast<XrEnvironmentBlendMode>(xr_blend_mode_);
    XrCompositionLayerProjection projection =
        XrInfo<XrCompositionLayerProjection>(
            XR_TYPE_COMPOSITION_LAYER_PROJECTION);
    std::array<XrCompositionLayerProjectionView, 2> views{};
    const XrCompositionLayerBaseHeader* layers[1] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
    const bool eyes_usable = eyes_bound_.load(std::memory_order_acquire) &&
                             eyes_[0].valid && eyes_[1].valid;
    const bool pose_ready = frame_views_valid_ && published_pose_.valid;
    const bool eye_frames_match =
        eyes_usable && pose_ready &&
        eyes_[0].frame == published_pose_.frame &&
        eyes_[1].frame == published_pose_.frame &&
        applied_poses_[eyes_[0].owner] == published_pose_.frame;
    const bool can_submit = frame_should_render_ &&
                            pose_ready && eye_frames_match && eyes_usable;
    if (!can_submit) {
      // One dominant reason per skipped frame, checked in pipeline order.
      if (visibility_lost_ && !frame_should_render_) {
        perf::ProcessCollector().Count(perf::Counter::kSkipVisibilityLost);
      } else if (!frame_should_render_) {
        perf::ProcessCollector().Count(perf::Counter::kSkipShouldRender);
      } else if (!pose_ready) {
        perf::ProcessCollector().Count(perf::Counter::kSkipPoseInvalid);
      } else if (!eyes_usable) {
        perf::ProcessCollector().Count(perf::Counter::kSkipEyesUnbound);
      } else {
        perf::ProcessCollector().Count(perf::Counter::kSkipEyeFrameMismatch);
      }
    }
    bool images_held[2] = {false, false};
    if (can_submit) {
      projection.space = static_cast<XrSpace>(xr_local_space_);
      projection.viewCount = 2;
      for (int eye = 0; eye < 2; ++eye) {
        std::uint32_t index = 0;
        {
          const ScopedStageTimer acquire_timer(perf::Stage::kSwapchainAcquire);
          auto acquire = XrInfo<XrSwapchainImageAcquireInfo>(
              XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
          CheckXr(instance,
                  xrAcquireSwapchainImage(
                      static_cast<XrSwapchain>(eye_swapchains_[eye].swapchain),
                      &acquire, &index),
                  "xrAcquireSwapchainImage");
        }
        {
          const ScopedStageTimer wait_timer(perf::Stage::kSwapchainWait);
          auto wait = XrInfo<XrSwapchainImageWaitInfo>(
              XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
          wait.timeout = 50'000'000;
          const auto deadline = Clock::now() + std::chrono::seconds(5);
          while (true) {
            const XrResult wait_result = xrWaitSwapchainImage(
                static_cast<XrSwapchain>(eye_swapchains_[eye].swapchain), &wait);
            if (wait_result != XR_TIMEOUT_EXPIRED) {
              CheckXr(instance, wait_result, "xrWaitSwapchainImage");
              break;
            }
            Require(Clock::now() < deadline,
                    "Timed out waiting for an OpenXR swapchain image");
          }
        }
        images_held[eye] = true;
        eye_swapchains_[eye].acquired_index = index;
        eye_swapchains_[eye].image_acquired = true;
      }
      const bool copied = [&] {
        const ScopedStageTimer copy_record(perf::Stage::kCopyRecord);
        return graphics_api_ == VrGraphicsApi::kVulkan
                   ? CopyEyesIntoSwapchains(queue)
                   : CopyGlesEyesIntoSwapchains();
      }() && [&] {
        const ScopedStageTimer evidence_timer(perf::Stage::kEvidence);
        return graphics_api_ == VrGraphicsApi::kOpenGles ||
               (CaptureEyeEvidence(queue, 0) && CaptureEyeEvidence(queue, 1));
      }();
      for (int eye = 0; eye < 2; ++eye) {
        if (images_held[eye]) {
          auto release = XrInfo<XrSwapchainImageReleaseInfo>(
              XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
          (void)xrReleaseSwapchainImage(
              static_cast<XrSwapchain>(eye_swapchains_[eye].swapchain),
              &release);
          eye_swapchains_[eye].image_acquired = false;
        }
      }
      if (copied) {
        for (int eye = 0; eye < 2; ++eye) {
          views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
          XrPosef submit_pose{};
          submit_pose.position = {frame_view_poses_[eye].position[0],
                                  frame_view_poses_[eye].position[1],
                                  frame_view_poses_[eye].position[2]};
          submit_pose.orientation = {frame_view_poses_[eye].orientation[0],
                                     frame_view_poses_[eye].orientation[1],
                                     frame_view_poses_[eye].orientation[2],
                                     frame_view_poses_[eye].orientation[3]};
          views[eye].pose = submit_pose;
          views[eye].fov = XrFovf{frame_views_[eye].fov[0],
                                  frame_views_[eye].fov[1],
                                  frame_views_[eye].fov[2],
                                  frame_views_[eye].fov[3]};
          views[eye].subImage.swapchain =
              static_cast<XrSwapchain>(eye_swapchains_[eye].swapchain);
          views[eye].subImage.imageRect.offset = {0, 0};
          views[eye].subImage.imageRect.extent = {
              static_cast<std::int32_t>(eye_swapchains_[eye].width),
              static_cast<std::int32_t>(eye_swapchains_[eye].height)};
          views[eye].subImage.imageArrayIndex = 0;
        }
        projection.views = views.data();
        end.layerCount = 1;
        end.layers = layers;
      }
    }
    CheckXr(instance, xrEndFrame(session, &end), "xrEndFrame");
    frame_open_ = false;
    if (end.layerCount > 0) {
      perf::ProcessCollector().Count(perf::Counter::kFramesSubmitted);
      const std::uint64_t submitted =
          submitted_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
      if (submitted == 1 || submitted % 120 == 0 ||
          (!EnvString("MOCKTAIL_VR_XR_EVIDENCE_DIR", "").empty() &&
           published_pose_.frame % std::max<std::uint64_t>(1,
               EnvUnsigned("MOCKTAIL_VR_XR_EVIDENCE_INTERVAL", 20)) == 0)) {
        if (graphics_api_ == VrGraphicsApi::kOpenGles) {
          Log("  [vr-backend][evidence] projection submitted=%llu frame=%llu "
              "graphics=GLES eye0_texture=%u eye1_texture=%u\n",
              static_cast<unsigned long long>(submitted),
              static_cast<unsigned long long>(published_pose_.frame),
              gles_->eye(0).object, gles_->eye(1).object);
        } else {
          Log("  [vr-backend][evidence] projection submitted=%llu frame=%llu "
              "eye0_image=%p "
              "eye1_image=%p eye0_swapchain_index=%u eye1_swapchain_index=%u\n",
              static_cast<unsigned long long>(submitted),
              static_cast<unsigned long long>(published_pose_.frame),
              static_cast<void *>(eyes_[0].image),
              static_cast<void *>(eyes_[1].image),
              eye_swapchains_[0].acquired_index,
              eye_swapchains_[1].acquired_index);
        }
      }
    } else {
      perf::ProcessCollector().Count(perf::Counter::kFramesSkipped);
      skipped_frames_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // ---- open the XR frame for the Roblox frame that is about to render.
  std::uint64_t predicted_display_time = 0;
  bool should_render = false;
  {
    const ScopedStageTimer wait_timer(perf::Stage::kXrWaitFrame);
    auto wait = XrInfo<XrFrameWaitInfo>(XR_TYPE_FRAME_WAIT_INFO);
    XrFrameState state = XrInfo<XrFrameState>(XR_TYPE_FRAME_STATE);
    CheckXr(instance, xrWaitFrame(session, &wait, &state), "xrWaitFrame");
    {
      const ScopedStageTimer begin_timer(perf::Stage::kXrBeginFrame);
      auto begin_info = XrInfo<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO);
      CheckXr(instance, xrBeginFrame(session, &begin_info), "xrBeginFrame");
    }
    predicted_display_time = state.predictedDisplayTime;
    should_render = state.shouldRender;
  }
  frame_open_ = true;
  frame_display_time_ = predicted_display_time;
  frame_should_render_ = should_render;
  applied_poses_.clear();
  frame_views_valid_ = false;
  perf::ProcessCollector().Count(perf::Counter::kFramesBegun);

  if (should_render) {
    auto locate_info = XrInfo<XrViewLocateInfo>(XR_TYPE_VIEW_LOCATE_INFO);
    locate_info.viewConfigurationType =
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate_info.displayTime = predicted_display_time;
    locate_info.space = static_cast<XrSpace>(xr_local_space_);
    XrViewState view_state = XrInfo<XrViewState>(XR_TYPE_VIEW_STATE);
    std::array<XrView, 2> xr_views{};
    xr_views[0].type = XR_TYPE_VIEW;
    xr_views[1].type = XR_TYPE_VIEW;
    std::uint32_t view_count = 0;
    {
      const ScopedStageTimer locate_timer(perf::Stage::kXrLocateViews);
      CheckXr(instance,
              xrLocateViews(session, &locate_info, &view_state, 2, &view_count,
                            xr_views.data()),
              "xrLocateViews");
    }
    constexpr XrViewStateFlags required_view_flags =
        XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    if (view_count == 2 &&
        (view_state.viewStateFlags & required_view_flags) ==
            required_view_flags) {
      {
        // Head pose in LOCAL space for the guest camera injection.
        XrSpaceLocation head = XrInfo<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
        const XrResult head_result = xrLocateSpace(
            static_cast<XrSpace>(xr_view_space_),
            static_cast<XrSpace>(xr_local_space_), predicted_display_time,
            &head);
        constexpr XrSpaceLocationFlags required_location =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
            XR_SPACE_LOCATION_POSITION_VALID_BIT;
        const bool head_valid =
            XR_SUCCEEDED(head_result) &&
            (head.locationFlags & required_location) == required_location;
        const std::string pose_source =
            EnvString("MOCKTAIL_VR_POSE_SOURCE", "runtime");
        Pose head_pose = head_valid ? XrToPose(head.pose) : Pose{};
        if (pose_source == "script") {
          const ScriptedPoseSample scripted =
              ComputeScriptedPose(++xr_frame_counter_);
          head_pose = Pose{Vec3{scripted.position[0], scripted.position[1],
                                scripted.position[2]},
                           Quat{scripted.orientation[0], scripted.orientation[1],
                                scripted.orientation[2],
                                scripted.orientation[3]}};
          published_pose_ = scripted;
          published_pose_.valid = head_valid;
          published_pose_.frame = xr_frame_counter_;
        } else {
          ScriptedPoseSample sample;
          sample.position[0] = head_pose.position.x;
          sample.position[1] = head_pose.position.y;
          sample.position[2] = head_pose.position.z;
          sample.orientation[0] = head_pose.orientation.x;
          sample.orientation[1] = head_pose.orientation.y;
          sample.orientation[2] = head_pose.orientation.z;
          sample.orientation[3] = head_pose.orientation.w;
          sample.frame = ++xr_frame_counter_;
          sample.valid = head_valid;
          published_pose_ = sample;
        }
        for (int eye = 0; eye < 2; ++eye) {
          const Pose runtime_eye = XrToPose(xr_views[eye].pose);
          // Preserve the runtime's inter-lens offsets while the scripted (or
          // runtime) head drives the motion.
          const Pose relative = head_valid || pose_source != "script"
                                    ? PoseMultiply(PoseInverse(head_valid
                                                                   ? XrToPose(head.pose)
                                                                   : Pose{}),
                                                   runtime_eye)
                                    : runtime_eye;
          // Without a valid head pose `relative` degenerates to the full
          // LOCAL-space eye pose, which is not an inter-lens offset. Publish
          // explicit zeroes so no consumer can read a stale tracking value.
          if (head_valid) {
            published_pose_.eye_offset[eye][0] = relative.position.x;
            published_pose_.eye_offset[eye][1] = relative.position.y;
            published_pose_.eye_offset[eye][2] = relative.position.z;
          } else {
            published_pose_.eye_offset[eye][0] = 0.f;
            published_pose_.eye_offset[eye][1] = 0.f;
            published_pose_.eye_offset[eye][2] = 0.f;
          }
          // The pinned guest state represents parallel eye orientations. A
          // canted runtime needs a different camera ABI; do not submit an
          // image with a projection pose that the guest cannot represent.
          // Reject once per binding with the measured components so a tester
          // sees why nothing reaches the headset instead of a silent freeze.
          // Only judged while the head pose itself is valid: during tracking
          // loss `relative` degenerates to the full LOCAL-space eye pose and
          // its rotation is not a canted-eye signal.
          if (head_valid &&
              !pose::EyeOrientationIsParallel(relative.orientation)) {
            published_pose_.valid = false;
            if (!canted_rejection_logged_) {
              canted_rejection_logged_ = true;
              Log("  [vr-backend] UNSUPPORTED CONFIGURATION: the runtime "
                  "reports canted eye orientations (eye%d relative rotation "
                  "xyz=(%.5f,%.5f,%.5f), tolerance %.3f). The exact-build "
                  "guest camera ABI represents only parallel eye orientations, "
                  "so XR submission is rejected rather than showing images "
                  "with a mismatched projection. This is a known unmet "
                  "compatibility criterion for this headset/runtime "
                  "combination.\n",
                  eye, relative.orientation.x, relative.orientation.y,
                  relative.orientation.z, pose::kCantedTolerance);
            }
          }
          const Pose final_eye = PoseMultiply(head_pose, relative);
          frame_view_poses_[eye].position[0] = final_eye.position.x;
          frame_view_poses_[eye].position[1] = final_eye.position.y;
          frame_view_poses_[eye].position[2] = final_eye.position.z;
          frame_view_poses_[eye].orientation[0] = final_eye.orientation.x;
          frame_view_poses_[eye].orientation[1] = final_eye.orientation.y;
          frame_view_poses_[eye].orientation[2] = final_eye.orientation.z;
          frame_view_poses_[eye].orientation[3] = final_eye.orientation.w;
          frame_views_[eye].position[0] = final_eye.position.x;
          frame_views_[eye].position[1] = final_eye.position.y;
          frame_views_[eye].position[2] = final_eye.position.z;
          frame_views_[eye].orientation[0] = final_eye.orientation.x;
          frame_views_[eye].orientation[1] = final_eye.orientation.y;
          frame_views_[eye].orientation[2] = final_eye.orientation.z;
          frame_views_[eye].orientation[3] = final_eye.orientation.w;
          frame_views_[eye].fov[0] = xr_views[eye].fov.angleLeft;
          frame_views_[eye].fov[1] = xr_views[eye].fov.angleRight;
          frame_views_[eye].fov[2] = xr_views[eye].fov.angleUp;
          frame_views_[eye].fov[3] = xr_views[eye].fov.angleDown;
          std::copy_n(frame_views_[eye].fov, 4, published_pose_.eye_fov[eye]);
          frame_views_[eye].valid = true;
        }
        frame_views_valid_ = published_pose_.valid;
        // Head-pose evidence cadence is tunable so the scripted-motion canary
        // can observe at least two distinct samples within a short run; the
        // production default stays coarse to avoid per-frame log spam.
        static const std::uint64_t pose_log_interval = [] {
          const std::uint64_t value =
              EnvUnsigned("MOCKTAIL_VR_POSE_LOG_INTERVAL", 240);
          return value == 0 ? 240 : value;
        }();
        if (xr_frame_counter_ == 1 || xr_frame_counter_ % pose_log_interval == 0) {
          Log("  [vr-backend][evidence] head-pose frame=%llu pos=(%.4f,%.4f,"
              "%.4f) ori=(%.4f,%.4f,%.4f,%.4f) source=%s predicted=%lld\n",
              static_cast<unsigned long long>(xr_frame_counter_),
              published_pose_.position[0], published_pose_.position[1],
              published_pose_.position[2], published_pose_.orientation[0],
              published_pose_.orientation[1], published_pose_.orientation[2],
              published_pose_.orientation[3], pose_source.c_str(),
              static_cast<long long>(predicted_display_time));
        }
      }
    }
    if (!frame_views_valid_) {
      // Invalid tracking is never published as a fresh valid pose.
      published_pose_.valid = false;
    }
  } else {
    published_pose_.valid = false;
  }

  // Sync controllers exactly once per frame at the same predicted display time
  // used for the head/eyes, then fold the resulting hand poses into the
  // already-published frame. This adds no second frame wait and no per-getter
  // polling; the device bridge reads published_pose_.hands when it injects the
  // state for this frame.
  SyncControllersLocked(predicted_display_time);
}

void OpenXrBackend::SyncControllersLocked(std::uint64_t predicted_display_time) {
  std::lock_guard<std::mutex> lock(controller_mutex_);
  published_pose_.hands[0] = {};
  published_pose_.hands[1] = {};
  published_pose_.controllers_connected = false;
  std::fill(std::begin(published_pose_.controller_channels), std::end(published_pose_.controller_channels), 0.f);
  if (!actions_created_ || !actions_.attached()) {
    return;
  }
  const ScopedStageTimer sync_timer(perf::Stage::kControllerSync);
  const bool synced = actions_.Sync(xr_session_, predicted_display_time,
                                    published_pose_.frame, 0);
  if (!synced) {
    // Hands were invalidated above; Sync queued the input reset.
    return;
  }
  // Fold this frame's hand poses into the published sample. Only valid when
  // the head pose for this frame is itself valid, so hands and head always
  // describe the same predicted instant.
  if (published_pose_.valid) {
    published_pose_.hands[0] = actions_.hand(0);
    published_pose_.hands[1] = actions_.hand(1);
    published_pose_.controllers_connected = EncodeNativeControllerChannels(
        actions_.snapshot(), published_pose_.controller_channels);
  } else {
    published_pose_.hands[0] = VrHandPose{};
    published_pose_.hands[1] = VrHandPose{};
  }
  perf::ProcessCollector().Count(perf::Counter::kControllerSyncs);
}

bool OpenXrBackend::EnsureCopyResources(VkDevice device,
                                        std::uint32_t queue_family,
                                        std::string* error) {
  try {
    if (copy_pool_ != VK_NULL_HANDLE && copy_command_ != VK_NULL_HANDLE &&
        copy_fence_ != VK_NULL_HANDLE) {
      return true;
    }
    if (copy_pool_ == VK_NULL_HANDLE) {
      VkCommandPoolCreateInfo pool_info{};
      pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pool_info.queueFamilyIndex = queue_family;
      Require(vk_->CreateCommandPool(device, &pool_info, nullptr,
                                     &copy_pool_) == VK_SUCCESS,
              "vkCreateCommandPool failed for XR copies");
    }
    if (copy_command_ == VK_NULL_HANDLE) {
      VkCommandBufferAllocateInfo allocate_info{};
      allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      allocate_info.commandPool = copy_pool_;
      allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      allocate_info.commandBufferCount = 1;
      Require(vk_->AllocateCommandBuffers(device, &allocate_info,
                                          &copy_command_) == VK_SUCCESS,
              "vkAllocateCommandBuffers failed for XR copies");
    }
    if (copy_fence_ == VK_NULL_HANDLE) {
      VkFenceCreateInfo fence_info{};
      fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      Require(vk_->CreateFence(device, &fence_info, nullptr, &copy_fence_) ==
                VK_SUCCESS,
              "vkCreateFence failed for XR copies");
    }
    return true;
  } catch (const std::exception& exception) {
    *error = exception.what();
    return false;
  }
}

void OpenXrBackend::DestroyCopyResources() {
  if (vk_ == nullptr || vk_device_ == VK_NULL_HANDLE) {
    copy_pool_ = VK_NULL_HANDLE;
    copy_command_ = VK_NULL_HANDLE;
    copy_fence_ = VK_NULL_HANDLE;
    return;
  }
  if (copy_fence_ != VK_NULL_HANDLE) {
    vk_->DestroyFence(vk_device_, copy_fence_, nullptr);
    copy_fence_ = VK_NULL_HANDLE;
  }
  if (copy_pool_ != VK_NULL_HANDLE) {
    // Frees copy_command_ as well.
    vk_->DestroyCommandPool(vk_device_, copy_pool_, nullptr);
    copy_pool_ = VK_NULL_HANDLE;
    copy_command_ = VK_NULL_HANDLE;
  }
}

bool OpenXrBackend::CopyEyesIntoSwapchains(VkQueue queue) {
  std::string error;
  const auto family_entry = queue_families_.find(queue);
  const std::uint32_t family =
      family_entry != queue_families_.end() ? family_entry->second
                                            : vk_queue_family_;
  if (!EnsureCopyResources(vk_device_, family, &error)) {
    Log("  [vr-backend] copy resources unavailable: %s\n", error.c_str());
    return false;
  }
  // Both eyes must match their swapchain extents; a mismatch is a hard skip
  // of the whole batched submission, never a half-copied frame.
  const VkImage source[2] = {eyes_[0].image, eyes_[1].image};
  for (int eye = 0; eye < 2; ++eye) {
    const EyeSwapchain& swapchain = eye_swapchains_[eye];
    if (!swapchain.image_acquired || swapchain.images.empty() ||
        swapchain.acquired_index >= swapchain.images.size()) {
      return false;
    }
    if (eyes_[eye].width != swapchain.width ||
        eyes_[eye].height != swapchain.height) {
      Log("  [vr-backend] eye%d extent %ux%u does not match swapchain %ux%u\n",
          eye, eyes_[eye].width, eyes_[eye].height, swapchain.width,
          swapchain.height);
      return false;
    }
  }
  const VkImage destination[2] = {
      eye_swapchains_[0].images[eye_swapchains_[0].acquired_index],
      eye_swapchains_[1].images[eye_swapchains_[1].acquired_index]};
  EnsureMirrorForExtent(eyes_[0].width, eyes_[0].height);

  if (vk_->ResetCommandBuffer(copy_command_, 0) != VK_SUCCESS) {
    return false;
  }
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vk_->BeginCommandBuffer(copy_command_, &begin) != VK_SUCCESS) {
    return false;
  }

  // Phase 1: transition both sources to TRANSFER_SRC and both swapchain
  // destinations to TRANSFER_DST in one barrier batch.
  std::array<VkImageMemoryBarrier, 4> prepare{};
  for (int eye = 0; eye < 2; ++eye) {
    VkImageMemoryBarrier& source_barrier = prepare[eye];
    source_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    source_barrier.srcAccessMask =
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    source_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    source_barrier.oldLayout = eyes_[eye].final_layout;
    source_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    source_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source_barrier.image = source[eye];
    source_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier& destination_barrier = prepare[2 + eye];
    destination_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    destination_barrier.srcAccessMask = 0;
    destination_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    destination_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    destination_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    destination_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    destination_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    destination_barrier.image = destination[eye];
    destination_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                            1};
  }
  vk_->CmdPipelineBarrier(copy_command_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, static_cast<std::uint32_t>(prepare.size()),
                          prepare.data());

  // Phase 2: both eye copies (plus the mirror readbacks when enabled) in one
  // recording.
  const VkImageSubresourceLayers subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                             1};
  for (int eye = 0; eye < 2; ++eye) {
    const EyeBinding& binding = eyes_[eye];
    const EyeSwapchain& swapchain = eye_swapchains_[eye];
    if (FormatIsCopyCompatible(binding.format, swapchain.format)) {
      VkImageCopy region{};
      region.srcSubresource = subresource;
      region.dstSubresource = subresource;
      region.extent = {binding.width, binding.height, 1};
      vk_->CmdCopyImage(copy_command_, source[eye],
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination[eye],
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else {
      VkImageBlit region{};
      region.srcSubresource = subresource;
      region.dstSubresource = subresource;
      region.srcOffsets[1] = {static_cast<std::int32_t>(binding.width),
                              static_cast<std::int32_t>(binding.height), 1};
      region.dstOffsets[1] = region.srcOffsets[1];
      vk_->CmdBlitImage(copy_command_, source[eye],
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination[eye],
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                        VK_FILTER_NEAREST);
    }
    if (mirror_.enabled()) {
      // Reads the source while it is already TRANSFER_SRC; the restore
      // barrier below returns it to the guest's final layout afterwards.
      mirror_.RecordEyeCopy(MirrorProcs(), copy_command_, eye, source[eye],
                            binding.final_layout, binding.width,
                            binding.height);
    }
  }

  // Phase 3: restore the eye images to their render-pass final layouts and
  // leave the swapchain images as color attachments for the compositor.
  std::array<VkImageMemoryBarrier, 4> finish{};
  for (int eye = 0; eye < 2; ++eye) {
    VkImageMemoryBarrier& restore_source = finish[eye];
    restore_source.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    restore_source.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    restore_source.dstAccessMask =
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    restore_source.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    restore_source.newLayout = eyes_[eye].final_layout;
    restore_source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restore_source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restore_source.image = source[eye];
    restore_source.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier& finish_destination = finish[2 + eye];
    finish_destination.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    finish_destination.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    finish_destination.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    finish_destination.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    finish_destination.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    finish_destination.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    finish_destination.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    finish_destination.image = destination[eye];
    finish_destination.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                           1};
  }
  vk_->CmdPipelineBarrier(copy_command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                          nullptr, static_cast<std::uint32_t>(finish.size()),
                          finish.data());

  if (vk_->EndCommandBuffer(copy_command_) != VK_SUCCESS) {
    return false;
  }
  if (vk_->ResetFences(vk_device_, 1, &copy_fence_) != VK_SUCCESS) {
    return false;
  }
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &copy_command_;
  // Same queue as the guest's frame submission: queue-order guarantees the
  // batched copies run after the eye render passes of this frame. One frame in
  // flight; the single fence wait bounds reuse of the copy command buffer and
  // of the mirror staging buffers.
  VkResult submit_result = VK_SUCCESS;
  {
    const ScopedStageTimer submit_timer(perf::Stage::kCopySubmit);
    submit_result = vk_->QueueSubmit(queue, 1, &submit, copy_fence_);
  }
  perf::ProcessCollector().Count(perf::Counter::kSubmits);
  if (submit_result != VK_SUCCESS) {
    return false;
  }
  VkResult fence_result = VK_SUCCESS;
  {
    const ScopedStageTimer fence_timer(perf::Stage::kCopyFenceWait);
    fence_result = vk_->WaitForFences(vk_device_, 1, &copy_fence_, VK_TRUE,
                                      2'000'000'000ULL);
  }
  perf::ProcessCollector().Count(perf::Counter::kFenceWaits);
  if (fence_result != VK_SUCCESS) {
    (void)vk_->DeviceWaitIdle(vk_device_);
    throw std::runtime_error("XR copy fence wait failed");
  }
  if (mirror_.enabled()) {
    const ScopedStageTimer publish_timer(perf::Stage::kMirrorPublish);
    mirror_.Publish(published_pose_.frame, eyes_[0].format, eyes_[0].width,
                    eyes_[0].height, perf::NowNs());
    perf::ProcessCollector().Count(perf::Counter::kMirrorPairs);
  }
  return true;
}

void OpenXrBackend::EnsureMirrorForExtent(std::uint32_t width,
                                          std::uint32_t height) {
  const std::string name = EnvString("MOCKTAIL_VR_MIRROR_SHM", "");
  if (name.empty()) {
    return;
  }
  {
    const bool was_enabled = mirror_.enabled();
    std::string error;
    if (!mirror_.Open(name, width, height, &error)) {
      Log("  [vr-backend] mirror unavailable: %s\n", error.c_str());
      return;
    }
    if (!was_enabled) Log("  [vr-backend] desktop mirror publishing to %s (%ux%u)\n",
        name.c_str(), width, height);
  }
  std::string error;
  if (!mirror_.EnsureStaging(MirrorProcs(), vk_device_, vk_physical_device_,
                             width, height, &error)) {
    Log("  [vr-backend] mirror staging unavailable: %s\n", error.c_str());
    mirror_.Close();
  }
}

MirrorVkProcs OpenXrBackend::MirrorProcs() const {
  MirrorVkProcs procs{};
  if (vk_ == nullptr) {
    return procs;
  }
  procs.CreateBuffer = vk_->CreateBuffer;
  procs.DestroyBuffer = vk_->DestroyBuffer;
  procs.GetBufferMemoryRequirements = vk_->GetBufferMemoryRequirements;
  procs.AllocateMemory = vk_->AllocateMemory;
  procs.FreeMemory = vk_->FreeMemory;
  procs.BindBufferMemory = vk_->BindBufferMemory;
  procs.MapMemory = vk_->MapMemory;
  procs.UnmapMemory = vk_->UnmapMemory;
  procs.CmdCopyImageToBuffer = vk_->CmdCopyImageToBuffer;
  procs.CmdPipelineBarrier = vk_->CmdPipelineBarrier;
  procs.GetPhysicalDeviceMemoryProperties =
      vk_->GetPhysicalDeviceMemoryProperties;
  return procs;
}

// DebugDeviceVR renders offscreen eyes but does not mirror them to the host
// window. Keep this copy on the guest GPU; no readback or image files in normal use.
void OpenXrBackend::RecordDesktopSwapchain(VkDevice device, VkSwapchainKHR swapchain,
    const VkSwapchainCreateInfoKHR* info, const VkImage* images, unsigned count) {
  std::lock_guard<std::mutex> lock(mutex_);
  desktop_swapchains_.erase(swapchain);
  if (!device || device != vk_device_ || !info || !images || !count ||
      !(info->imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
      info->imageArrayLayers != 1 || info->imageSharingMode != VK_SHARING_MODE_EXCLUSIVE)
    return;
  desktop_swapchains_[swapchain] = {info->imageExtent, info->imageFormat,
                                    {images, images + count}};
}

VkResult OpenXrBackend::MirrorDesktop(VkQueue queue, const VkPresentInfoKHR* info) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (EnvString("MOCKTAIL_VR_DESKTOP_MIRROR", "1") == "0" ||
      !vk_ || !info || info->swapchainCount != 1 ||
      !info->pSwapchains || !info->pImageIndices || !eyes_[0].valid ||
      !eyes_[0].image || (info->waitSemaphoreCount && !info->pWaitSemaphores) ||
      !queue_families_.count(queue) || queue_families_[queue] != vk_queue_family_)
    return VK_NOT_READY;
  const auto found = desktop_swapchains_.find(info->pSwapchains[0]);
  if (found == desktop_swapchains_.end() ||
      info->pImageIndices[0] >= found->second.images.size()) return VK_NOT_READY;
  const auto& target = found->second;
  VkFormatProperties source_properties{}, target_properties{};
  vk_->GetPhysicalDeviceFormatProperties(vk_physical_device_, eyes_[0].format, &source_properties);
  vk_->GetPhysicalDeviceFormatProperties(vk_physical_device_, target.format, &target_properties);
  if (!(source_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
      !(target_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) return VK_NOT_READY;
  std::string error;
  if (!EnsureCopyResources(vk_device_, vk_queue_family_, &error)) return VK_NOT_READY;
  if (vk_->ResetCommandBuffer(copy_command_, 0) != VK_SUCCESS) return VK_NOT_READY;
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vk_->BeginCommandBuffer(copy_command_, &begin) != VK_SUCCESS) return VK_NOT_READY;
  VkImageMemoryBarrier barriers[2]{};
  for (auto& b : barriers) {
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
  }
  barriers[0].image = eyes_[0].image;
  barriers[0].oldLayout = eyes_[0].final_layout;
  barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barriers[1].image = target.images[info->pImageIndices[0]];
  barriers[1].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vk_->CmdPipelineBarrier(copy_command_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
  const VkClearColorValue black{{0.f, 0.f, 0.f, 1.f}};
  vk_->CmdClearColorImage(copy_command_, barriers[1].image, barriers[1].newLayout,
      &black, 1, &barriers[1].subresourceRange);
  VkMemoryBarrier clear_barrier{};
  clear_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  clear_barrier.srcAccessMask = clear_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vk_->CmdPipelineBarrier(copy_command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &clear_barrier, 0, nullptr, 0, nullptr);
  const double scale = std::min(double(target.extent.width) / eyes_[0].width,
                                double(target.extent.height) / eyes_[0].height);
  const int width = std::max(1, int(eyes_[0].width * scale));
  const int height = std::max(1, int(eyes_[0].height * scale));
  const int x = (int(target.extent.width) - width) / 2;
  const int y = (int(target.extent.height) - height) / 2;
  VkImageBlit region{};
  region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.srcOffsets[1] = {static_cast<int>(eyes_[0].width), static_cast<int>(eyes_[0].height), 1};
  region.dstOffsets[0] = {x, y, 0};
  region.dstOffsets[1] = {x + width, y + height, 1};
  vk_->CmdBlitImage(copy_command_, barriers[0].image, barriers[0].newLayout,
      barriers[1].image, barriers[1].newLayout, 1, &region, VK_FILTER_NEAREST);
  for (auto& b : barriers) {
    std::swap(b.oldLayout, b.newLayout);
    b.srcAccessMask = b.dstAccessMask;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  }
  vk_->CmdPipelineBarrier(copy_command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
  if (vk_->EndCommandBuffer(copy_command_) != VK_SUCCESS ||
      vk_->ResetFences(vk_device_, 1, &copy_fence_) != VK_SUCCESS) return VK_NOT_READY;
  std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.waitSemaphoreCount = info->waitSemaphoreCount;
  submit.pWaitSemaphores = info->pWaitSemaphores;
  submit.pWaitDstStageMask = stages.data();
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &copy_command_;
  VkResult result = vk_->QueueSubmit(queue, 1, &submit, copy_fence_);
  if (result != VK_SUCCESS) return result;
  // Once submitted, never fall back to waiting on the consumed semaphores.
  result = vk_->WaitForFences(vk_device_, 1, &copy_fence_, VK_TRUE, UINT64_MAX);
  return result;
}

// Diagnostic only: read BOTH images on the guest queue while the XR image is
// still acquired. Never read a released image owned by the compositor.
bool OpenXrBackend::CaptureEyeEvidence(VkQueue queue, int eye) {
  const std::string directory = EnvString("MOCKTAIL_VR_XR_EVIDENCE_DIR", "");
  const auto interval = std::max<std::uint64_t>(1,
      EnvUnsigned("MOCKTAIL_VR_XR_EVIDENCE_INTERVAL", 20));
  if (directory.empty() || published_pose_.frame % interval != 0) return true;
  VkBuffer buffers[2] = {};
  VkDeviceMemory memory[2] = {};
  bool pending = false;
  auto cleanup = [&] {
    if (pending) vk_->DeviceWaitIdle(vk_device_);
    for (int i = 0; i < 2; ++i) {
      if (buffers[i]) vk_->DestroyBuffer(vk_device_, buffers[i], nullptr);
      if (memory[i]) vk_->FreeMemory(vk_device_, memory[i], nullptr);
    }
  };
  try {
    const auto& binding = eyes_[eye];
    const auto& slot = eye_swapchains_[eye];
    const VkDeviceSize size = VkDeviceSize(binding.width)*binding.height*4;
    const VkImage images[2] = {binding.image, slot.images[slot.acquired_index]};
    const VkFormat formats[2] = {binding.format, slot.format};
    const VkImageLayout layouts[2] = {binding.final_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkPhysicalDeviceMemoryProperties properties{};
    vk_->GetPhysicalDeviceMemoryProperties(vk_physical_device_, &properties);
    for (int i = 0; i < 2; ++i) {
      Require(formats[i] == VK_FORMAT_R8G8B8A8_UNORM ||
              formats[i] == VK_FORMAT_B8G8R8A8_UNORM,
              "evidence currently requires 8-bit UNORM images");
      VkBufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      info.size = size;
      info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      Require(vk_->CreateBuffer(vk_device_, &info, nullptr, &buffers[i]) == VK_SUCCESS,
              "evidence buffer creation failed");
      VkMemoryRequirements requirements{};
      vk_->GetBufferMemoryRequirements(vk_device_, buffers[i], &requirements);
      std::uint32_t type = UINT32_MAX;
      const auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      for (std::uint32_t t = 0; t < properties.memoryTypeCount; ++t)
        if ((requirements.memoryTypeBits & (1u << t)) &&
            (properties.memoryTypes[t].propertyFlags & flags) == flags) { type = t; break; }
      Require(type != UINT32_MAX, "no coherent evidence readback memory");
      VkMemoryAllocateInfo allocation{};
      allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      allocation.allocationSize = requirements.size;
      allocation.memoryTypeIndex = type;
      Require(vk_->AllocateMemory(vk_device_, &allocation, nullptr, &memory[i]) == VK_SUCCESS,
              "evidence memory allocation failed");
      Require(vk_->BindBufferMemory(vk_device_, buffers[i], memory[i], 0) == VK_SUCCESS,
              "evidence buffer binding failed");
    }
    Require(vk_->ResetCommandBuffer(copy_command_, 0) == VK_SUCCESS, "evidence reset failed");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Require(vk_->BeginCommandBuffer(copy_command_, &begin) == VK_SUCCESS, "evidence begin failed");
    for (int i = 0; i < 2; ++i) {
      VkImageMemoryBarrier barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.oldLayout = layouts[i];
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = images[i];
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vk_->CmdPipelineBarrier(copy_command_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {binding.width, binding.height, 1};
      vk_->CmdCopyImageToBuffer(copy_command_, images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buffers[i], 1, &region);
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      std::swap(barrier.oldLayout, barrier.newLayout);
      vk_->CmdPipelineBarrier(copy_command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    Require(vk_->EndCommandBuffer(copy_command_) == VK_SUCCESS, "evidence end failed");
    Require(vk_->ResetFences(vk_device_, 1, &copy_fence_) == VK_SUCCESS, "evidence fence reset failed");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &copy_command_;
    Require(vk_->QueueSubmit(queue, 1, &submit, copy_fence_) == VK_SUCCESS, "evidence submit failed");
    pending = true;
    Require(vk_->WaitForFences(vk_device_, 1, &copy_fence_, VK_TRUE, 2000000000ULL) == VK_SUCCESS,
            "evidence fence wait failed");
    pending = false;
    std::filesystem::create_directories(directory);
    const auto stem = std::filesystem::path(directory) /
        ("frame-" + std::to_string(published_pose_.frame) + "-eye" + std::to_string(eye));
    for (int i = 0; i < 2; ++i) {
      void* mapped = nullptr;
      Require(vk_->MapMemory(vk_device_, memory[i], 0, size, 0, &mapped) == VK_SUCCESS,
              "evidence map failed");
      const auto* bytes = static_cast<const unsigned char*>(mapped);
      std::vector<unsigned char> rgb(size/4*3);
      const bool bgra = formats[i] == VK_FORMAT_B8G8R8A8_UNORM;
      for (std::size_t pixel = 0; pixel < size/4; ++pixel) {
        rgb[pixel*3] = bytes[pixel*4 + (bgra ? 2 : 0)];
        rgb[pixel*3+1] = bytes[pixel*4+1];
        rgb[pixel*3+2] = bytes[pixel*4 + (bgra ? 0 : 2)];
      }
      vk_->UnmapMemory(vk_device_, memory[i]);
      std::ofstream file(stem.string() + (i ? "-destination.ppm" : "-source.ppm"), std::ios::binary);
      file << "P6\n" << binding.width << ' ' << binding.height << "\n255\n";
      file.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
      Require(file.good(), "evidence image write failed");
    }
    std::ofstream meta(stem.string() + ".json");
    meta << "{\"frame\":" << published_pose_.frame << ",\"eye\":" << eye
         << ",\"source\":\"" << images[0] << "\",\"destination\":\"" << images[1]
         << "\",\"owner\":\"" << binding.owner << "\",\"pose_applied_frame\":"
         << applied_poses_[binding.owner] << "}";
    Require(meta.good(), "evidence metadata write failed");
    cleanup();
    return true;
  } catch (const std::exception& error) {
    cleanup();
    Log("  [vr-backend] XR evidence failed: %s\n", error.what());
    return false;
  }
}

}  // namespace mocktail::vr

// Process symbols resolved by the Vulkan adapter through dlsym. They are
// inert while no backend is armed; the adapter falls back to its plain host
// loader path whenever a hook returns false.
extern "C" {

bool mocktail_vr_xr_create_vulkan_instance(
    const VkInstanceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkInstance* instance,
    VkResult* result) {
  auto* backend = mocktail::vr::ActiveVrBackend();
  return backend != nullptr &&
         backend->CreateVulkanInstance(create_info, allocator, instance,
                                       result);
}

bool mocktail_vr_xr_create_vulkan_device(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkDevice* device,
    VkResult* result) {
  auto* backend = mocktail::vr::ActiveVrBackend();
  return backend != nullptr &&
         backend->CreateVulkanDevice(physical_device, create_info, allocator,
                                     device, result);
}

void mocktail_vr_xr_note_instance_destroyed(VkInstance instance) {
  if (auto* backend = mocktail::vr::ActiveVrBackend()) backend->NoteInstanceDestroyed(instance);
}

void mocktail_vr_xr_note_device_destroyed(VkDevice device) {
  auto* backend = mocktail::vr::ActiveVrBackend();
  if (backend != nullptr) {
    backend->NoteDeviceDestroyed(device);
  }
}

void mocktail_vr_xr_note_queue(VkDevice device, VkQueue queue,
                               unsigned int family, unsigned int index) {
  auto* backend = mocktail::vr::ActiveVrBackend();
  if (backend != nullptr) {
    backend->NoteQueue(device, queue, family, index);
  }
}

bool mocktail_vr_xr_wants_resource_records() {
  auto* backend = mocktail::vr::ActiveVrBackend();
  return backend != nullptr && backend->WantsResourceRecords();
}

void mocktail_vr_xr_record_image(VkDevice device, VkImage image,
                                 const VkImageCreateInfo* info) {
  auto* backend = mocktail::vr::ActiveVrBackend();
  if (backend != nullptr) {
    backend->RecordImage(device, image, info);
  }
}

void mocktail_vr_xr_record_image_view(VkImageView view, VkImage image) {
  auto* backend = mocktail::vr::ActiveVrBackend();
  if (backend != nullptr) {
    backend->RecordImageView(view, image);
  }
}

void mocktail_vr_xr_record_framebuffer(VkFramebuffer framebuffer,
                                       const VkFramebufferCreateInfo* info) {
  auto* backend = mocktail::vr::ActiveVrBackend();
  if (backend != nullptr) {
    backend->RecordFramebuffer(framebuffer, info);
  }
}

void mocktail_vr_xr_record_render_pass(VkRenderPass render_pass,
                                       const VkRenderPassCreateInfo* info) {
  auto* backend = mocktail::vr::ActiveVrBackend();
  if (backend != nullptr) {
    backend->RecordRenderPass(render_pass, info);
  }
}

void mocktail_vr_xr_note_destroy_image(VkImage image) {
  auto* backend = mocktail::vr::ActiveVrBackend();
  if (backend != nullptr) {
    backend->NoteDestroyImage(image);
  }
}

void mocktail_vr_xr_note_destroy_image_view(VkImageView view) {
  auto* backend = mocktail::vr::ActiveVrBackend();
  if (backend != nullptr) {
    backend->NoteDestroyImageView(view);
  }
}

void mocktail_vr_xr_note_destroy_framebuffer(VkFramebuffer framebuffer) {
  auto* backend = mocktail::vr::ActiveVrBackend();
  if (backend != nullptr) {
    backend->NoteDestroyFramebuffer(framebuffer);
  }
}

void mocktail_vr_xr_note_render_pass_begin(VkCommandBuffer command_buffer,
                                           const VkRenderPassBeginInfo* info) {
  auto* backend = mocktail::vr::ActiveVrBackend();
  if (backend != nullptr) {
    backend->NoteRenderPassBegin(command_buffer, info);
  }
}

bool mocktail_vr_desktop_enabled() {
  return mocktail::vr::ActiveVrBackend() != nullptr;
}
void mocktail_vr_desktop_swapchain(VkDevice device, VkSwapchainKHR swapchain,
    const VkSwapchainCreateInfoKHR* info, const VkImage* images, unsigned count) {
  try {
    if (auto* backend = mocktail::vr::ActiveVrBackend())
      backend->RecordDesktopSwapchain(device, swapchain, info, images, count);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "  [vr-backend] desktop swapchain registration failed: %s\n", error.what());
  }
}
VkResult mocktail_vr_desktop_present(VkQueue queue, const VkPresentInfoKHR* info) {
  try {
    if (auto* backend = mocktail::vr::ActiveVrBackend()) return backend->MirrorDesktop(queue, info);
    return VK_NOT_READY;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "  [vr-backend] desktop mirror failed: %s\n", error.what());
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
}

void mocktail_vr_xr_note_present(VkQueue queue, VkDevice device) {
  auto* backend = mocktail::vr::ActiveVrBackend();
  if (backend != nullptr) {
    backend->NoteHostPresent(queue, device);
  }
}

}  // extern "C"

// Controller delivery ABI consumed by the input runtime through dlsym. These
// are defined only in the VR build; a non-VR build omits them and the consumer
// resolves nullptr, staying inert.
#include "mocktail/vr/xr_controller_abi.h"

extern "C" int32_t
mocktail_vr_controller_pop_delivery(MocktailVrControllerDelivery* out) {
  if (out == nullptr) {
    return 0;
  }
  auto* backend = mocktail::vr::ActiveVrBackend();
  if (backend == nullptr) {
    return 0;
  }
  // Exact native VR input owns Gamepad1 when the device bridge is active.
  // The JNI fallback must not inject a second copy into that same device.
  extern bool mocktail_vr_native_controller_input_active();
  if (mocktail_vr_native_controller_input_active()) return 0;
  mocktail::vr::ControllerDelivery delivery;
  if (!backend->TakeControllerDelivery(&delivery)) {
    return 0;
  }
  mocktail::vr::EncodeControllerDelivery(delivery, out);
  return 1;
}

extern "C" void mocktail_vr_controller_request_haptics(int32_t hand,
                                                       float amplitude,
                                                       uint64_t duration_ns,
                                                       float frequency_hz) {
  if (auto* backend = mocktail::vr::ActiveVrBackend()) {
    backend->RequestControllerHaptics(hand, amplitude, duration_ns,
                                      frequency_hz);
  }
}

extern "C" void mocktail_vr_controller_stop_haptics(int32_t hand) {
  if (auto* backend = mocktail::vr::ActiveVrBackend()) {
    backend->StopControllerHaptics(hand);
  }
}

extern "C" bool mocktail_vr_attach_egl(void *display, void *config,
                                       void *context, void *egl_get_proc,
                                       void *(*resolve)(const char *)) {
  if (auto *backend = mocktail::vr::ActiveVrBackend()) {
    const auto status = backend->AttachGlesContext(display, config, context,
                                                   egl_get_proc, resolve);
    if (!status.ok())
      std::fprintf(stderr, "  [vr-backend] %s\n", status.message().c_str());
    return status.ok();
  }
  return true;
}
extern "C" void mocktail_vr_detach_egl(void *context) {
  if (auto *backend = mocktail::vr::ActiveVrBackend())
    backend->DetachGlesContext(context);
}
extern "C" void mocktail_vr_release_egl() {
  mocktail::vr::GlesTransport::ReleaseCurrent();
}
extern "C" void *mocktail_vr_wrap_gles_proc(const char *name, void *raw) {
  if (auto *backend = mocktail::vr::ActiveVrBackend())
    return backend->WrapGlesProcAddress(name, raw);
  return raw;
}
extern "C" void mocktail_vr_gles_present() {
  if (auto *backend = mocktail::vr::ActiveVrBackend())
    backend->NoteGlesPresent();
}
