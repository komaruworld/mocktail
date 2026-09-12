#include "mocktail/vr/openxr_probe.h"

#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace mocktail::vr {
namespace {

std::string Version(XrVersion version) {
  return std::to_string(XR_VERSION_MAJOR(version)) + "." +
         std::to_string(XR_VERSION_MINOR(version)) + "." +
         std::to_string(XR_VERSION_PATCH(version));
}

struct InstanceScope {
  XrInstance instance = XR_NULL_HANDLE;
  ~InstanceScope() {
    if (instance != XR_NULL_HANDLE) (void)xrDestroyInstance(instance);
  }
};

void Failure(ProbeReport* report, XrInstance instance, const char* operation,
             XrResult result) {
  report->status = ProbeStatus::kError;
  if (result == XR_ERROR_RUNTIME_UNAVAILABLE) {
    report->status = ProbeStatus::kRuntimeUnavailable;
  } else if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
    report->status = ProbeStatus::kHeadsetUnavailable;
  } else if (result == XR_ERROR_FORM_FACTOR_UNSUPPORTED ||
             result == XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED) {
    report->status = ProbeStatus::kIncompatibleRuntime;
  }
  char name[XR_MAX_RESULT_STRING_SIZE] = {};
  if (instance != XR_NULL_HANDLE)
    (void)xrResultToString(instance, result, name);
  report->message = std::string(operation) + " failed: " +
                    (name[0] ? std::string(name) : std::to_string(result));
  if (report->status == ProbeStatus::kRuntimeUnavailable) {
    report->message += ". Start an OpenXR runtime such as WiVRn or Monado.";
  } else if (report->status == ProbeStatus::kHeadsetUnavailable) {
    report->message +=
        ". Connect a headset, or use a simulated runtime for development.";
  }
}

// Counts can change between the two enumeration calls. Retry a bounded number
// of times and never consume an output array after XR_ERROR_SIZE_INSUFFICIENT.
template <typename T, typename Enumerate>
XrResult EnumerateProperties(XrStructureType type, Enumerate enumerate,
                             std::vector<T>* values) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    std::uint32_t count = 0;
    XrResult result = enumerate(0, &count, nullptr);
    if (XR_FAILED(result)) return result;
    if (count > 4096) return XR_ERROR_LIMIT_REACHED;
    values->assign(count, T{});
    for (auto& value : *values) value.type = type;
    if (count == 0) return XR_SUCCESS;
    result = enumerate(count, &count, values->data());
    if (result == XR_ERROR_SIZE_INSUFFICIENT) continue;
    if (XR_FAILED(result)) return result;
    if (count > values->size()) return XR_ERROR_RUNTIME_FAILURE;
    values->resize(count);
    return XR_SUCCESS;
  }
  return XR_ERROR_SIZE_INSUFFICIENT;
}

}  // namespace

ProbeReport ProbeOpenXr() {
  ProbeReport report;
  report.sdk_version = Version(XR_CURRENT_API_VERSION);
  std::vector<XrExtensionProperties> extensions;
  XrResult result = EnumerateProperties<XrExtensionProperties>(
      XR_TYPE_EXTENSION_PROPERTIES,
      [](std::uint32_t capacity, std::uint32_t* count,
         XrExtensionProperties* data) {
        return xrEnumerateInstanceExtensionProperties(nullptr, capacity, count,
                                                      data);
      },
      &extensions);
  if (XR_FAILED(result)) {
    Failure(&report, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
            result);
    return report;
  }
  for (const auto& extension : extensions) {
    report.extensions.emplace_back(extension.extensionName);
  }
  const bool vulkan =
      std::find(report.extensions.begin(), report.extensions.end(),
                XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) !=
      report.extensions.end();
  const char* enabled_extension = XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
  XrInstanceCreateInfo create_info{};
  create_info.type = XR_TYPE_INSTANCE_CREATE_INFO;
  std::strcpy(create_info.applicationInfo.applicationName, "Mocktail VR probe");
  std::strcpy(create_info.applicationInfo.engineName, "Mocktail");
  // Use the latest SDK while requiring only the core API this probe actually
  // uses, so runtimes exposing OpenXR 1.0 can also be diagnosed.
  create_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create_info.enabledExtensionCount = vulkan ? 1 : 0;
  create_info.enabledExtensionNames = vulkan ? &enabled_extension : nullptr;
  InstanceScope scope;
  result = xrCreateInstance(&create_info, &scope.instance);
  if (XR_FAILED(result)) {
    Failure(&report, XR_NULL_HANDLE, "xrCreateInstance", result);
    return report;
  }
  XrInstanceProperties properties{};
  properties.type = XR_TYPE_INSTANCE_PROPERTIES;
  result = xrGetInstanceProperties(scope.instance, &properties);
  if (XR_FAILED(result)) {
    Failure(&report, scope.instance, "xrGetInstanceProperties", result);
    return report;
  }
  report.runtime_name = properties.runtimeName;
  report.runtime_version = Version(properties.runtimeVersion);

  XrSystemGetInfo system_info{};
  system_info.type = XR_TYPE_SYSTEM_GET_INFO;
  system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  result = xrGetSystem(scope.instance, &system_info, &system);
  if (XR_FAILED(result)) {
    Failure(&report, scope.instance, "xrGetSystem", result);
    return report;
  }
  XrSystemProperties system_properties{};
  system_properties.type = XR_TYPE_SYSTEM_PROPERTIES;
  result = xrGetSystemProperties(scope.instance, system, &system_properties);
  if (XR_FAILED(result)) {
    Failure(&report, scope.instance, "xrGetSystemProperties", result);
    return report;
  }
  report.system_name = system_properties.systemName;
  report.orientation_tracking =
      system_properties.trackingProperties.orientationTracking == XR_TRUE;
  report.position_tracking =
      system_properties.trackingProperties.positionTracking == XR_TRUE;
  if (!vulkan) {
    report.status = ProbeStatus::kIncompatibleRuntime;
    report.message = "The runtime does not expose XR_KHR_vulkan_enable2.";
    return report;
  }

  std::vector<XrViewConfigurationView> views;
  result = EnumerateProperties<XrViewConfigurationView>(
      XR_TYPE_VIEW_CONFIGURATION_VIEW,
      [&](std::uint32_t capacity, std::uint32_t* count,
          XrViewConfigurationView* data) {
        return xrEnumerateViewConfigurationViews(
            scope.instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            capacity, count, data);
      },
      &views);
  if (XR_FAILED(result)) {
    Failure(&report, scope.instance, "xrEnumerateViewConfigurationViews",
            result);
    return report;
  }
  for (const auto& view : views) {
    report.views.push_back({view.recommendedImageRectWidth,
                            view.recommendedImageRectHeight,
                            view.recommendedSwapchainSampleCount});
  }
  if (views.size() != 2 || std::any_of(report.views.begin(), report.views.end(),
                                       [](const ViewConfiguration& view) {
                                         return !view.width || !view.height ||
                                                !view.samples;
                                       })) {
    report.status = ProbeStatus::kIncompatibleRuntime;
    report.message = "The runtime did not provide two usable stereo views.";
    return report;
  }

  PFN_xrVoidFunction function = nullptr;
  result = xrGetInstanceProcAddr(
      scope.instance, "xrGetVulkanGraphicsRequirements2KHR", &function);
  if (XR_FAILED(result) || function == nullptr) {
    Failure(&report, scope.instance,
            "xrGetVulkanGraphicsRequirements2KHR lookup",
            XR_FAILED(result) ? result : XR_ERROR_FUNCTION_UNSUPPORTED);
    return report;
  }
  XrGraphicsRequirementsVulkan2KHR requirements{};
  requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR;
  result = reinterpret_cast<PFN_xrGetVulkanGraphicsRequirements2KHR>(function)(
      scope.instance, system, &requirements);
  if (XR_FAILED(result)) {
    Failure(&report, scope.instance, "xrGetVulkanGraphicsRequirements2KHR",
            result);
    return report;
  }
  report.minimum_vulkan_version = Version(requirements.minApiVersionSupported);
  report.maximum_vulkan_version = Version(requirements.maxApiVersionSupported);
  report.status = ProbeStatus::kReady;
  report.message =
      "OpenXR HMD and Vulkan requirements discovered successfully.";
  return report;
}

}  // namespace mocktail::vr
