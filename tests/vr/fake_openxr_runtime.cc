// Test-only runtime for exercising the real Khronos loader without hardware.
// Implements discovery only; it is neither a compositor nor an XR emulator.
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

PFN_xrVoidFunction MocktailTestPreviewFunction(const char* name);

namespace {

bool Scenario(const char* expected) {
  const char* value = std::getenv("MOCKTAIL_TEST_XR_SCENARIO");
  return value && std::strcmp(value, expected) == 0;
}

void Trace(const char* event) {
  const char* path = std::getenv("MOCKTAIL_TEST_XR_TRACE");
  if (path) {
    if (FILE* file = std::fopen(path, "a")) {
      std::fprintf(file, "%s\n", event);
      std::fclose(file);
    }
  }
}

XrResult XRAPI_CALL EnumerateExtensions(const char*, std::uint32_t capacity,
                                        std::uint32_t* count,
                                        XrExtensionProperties* properties) {
  *count = Scenario("no_vulkan") ? 0 : 1;
  if (capacity == 0 || *count == 0) return XR_SUCCESS;
  if (properties[0].type != XR_TYPE_EXTENSION_PROPERTIES)
    return XR_ERROR_VALIDATION_FAILURE;
  std::strcpy(properties[0].extensionName,
              XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
  properties[0].extensionVersion = XR_KHR_vulkan_enable2_SPEC_VERSION;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL CreateInstance(const XrInstanceCreateInfo* info,
                                   XrInstance* instance) {
  if (Scenario("create_failure")) return XR_ERROR_INITIALIZATION_FAILED;
  if (info->type != XR_TYPE_INSTANCE_CREATE_INFO || info->next != nullptr)
    return XR_ERROR_VALIDATION_FAILURE;
  static int handle;
  *instance = reinterpret_cast<XrInstance>(&handle);
  Trace("create");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL DestroyInstance(XrInstance) {
  Trace("destroy");
  return XR_SUCCESS;
}

XrResult XRAPI_CALL GetInstanceProperties(XrInstance,
                                          XrInstanceProperties* properties) {
  if (Scenario("api_error")) return XR_ERROR_RUNTIME_FAILURE;
  std::strcpy(properties->runtimeName, "Mocktail discovery fixture");
  properties->runtimeVersion = XR_MAKE_VERSION(1, 0, 42);
  return XR_SUCCESS;
}

XrResult XRAPI_CALL GetSystem(XrInstance, const XrSystemGetInfo* info,
                              XrSystemId* system) {
  if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
    return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
  if (Scenario("no_headset")) return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
  *system = 1;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL GetSystemProperties(XrInstance, XrSystemId,
                                        XrSystemProperties* properties) {
  properties->systemId = 1;
  std::strcpy(properties->systemName, "Simulated test HMD");
  properties->graphicsProperties = {4096, 4096, 16};
  properties->trackingProperties = {XR_TRUE, XR_TRUE};
  return XR_SUCCESS;
}

XrResult XRAPI_CALL EnumerateViews(XrInstance, XrSystemId,
                                   XrViewConfigurationType type,
                                   std::uint32_t capacity, std::uint32_t* count,
                                   XrViewConfigurationView* views) {
  if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
  *count = Scenario("bad_stereo") ? 1 : 2;
  if (capacity == 0) return XR_SUCCESS;
  static bool grew = false;
  if (Scenario("enumeration_changed") && !grew) {
    grew = true;
    return XR_ERROR_SIZE_INSUFFICIENT;
  }
  if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
  for (std::uint32_t eye = 0; eye < *count; ++eye) {
    if (views[eye].type != XR_TYPE_VIEW_CONFIGURATION_VIEW || views[eye].next)
      return XR_ERROR_VALIDATION_FAILURE;
    views[eye].recommendedImageRectWidth = 1440;
    views[eye].recommendedImageRectHeight = 1600;
    views[eye].maxImageRectWidth = 4096;
    views[eye].maxImageRectHeight = 4096;
    views[eye].recommendedSwapchainSampleCount = 1;
    views[eye].maxSwapchainSampleCount = 4;
  }
  return XR_SUCCESS;
}

XrResult XRAPI_CALL GetGraphicsRequirements(
    XrInstance, XrSystemId, XrGraphicsRequirementsVulkan2KHR* requirements) {
  if (Scenario("graphics_error")) return XR_ERROR_RUNTIME_FAILURE;
  if (requirements->type != XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR ||
      requirements->next)
    return XR_ERROR_VALIDATION_FAILURE;
  requirements->minApiVersionSupported = XR_MAKE_VERSION(1, 1, 0);
  requirements->maxApiVersionSupported = XR_MAKE_VERSION(1, 3, 0);
  return XR_SUCCESS;
}

XrResult XRAPI_CALL ResultToString(XrInstance, XrResult result, char* buffer) {
  std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "fixture_result_%d", result);
  return XR_SUCCESS;
}

XrResult XRAPI_CALL GetProcAddr(XrInstance, const char* name,
                                PFN_xrVoidFunction* function) {
  *function = nullptr;
#define MOCKTAIL_TEST_XR_PROC(xr_name, implementation)                \
  if (std::strcmp(name, #xr_name) == 0) {                             \
    *function = reinterpret_cast<PFN_xrVoidFunction>(implementation); \
    return XR_SUCCESS;                                                \
  }
  MOCKTAIL_TEST_XR_PROC(xrGetInstanceProcAddr, GetProcAddr)
  MOCKTAIL_TEST_XR_PROC(xrEnumerateInstanceExtensionProperties,
                        EnumerateExtensions)
  MOCKTAIL_TEST_XR_PROC(xrCreateInstance, CreateInstance)
  MOCKTAIL_TEST_XR_PROC(xrDestroyInstance, DestroyInstance)
  MOCKTAIL_TEST_XR_PROC(xrGetInstanceProperties, GetInstanceProperties)
  MOCKTAIL_TEST_XR_PROC(xrGetSystem, GetSystem)
  MOCKTAIL_TEST_XR_PROC(xrGetSystemProperties, GetSystemProperties)
  MOCKTAIL_TEST_XR_PROC(xrEnumerateViewConfigurationViews, EnumerateViews)
  MOCKTAIL_TEST_XR_PROC(xrGetVulkanGraphicsRequirements2KHR,
                        GetGraphicsRequirements)
  MOCKTAIL_TEST_XR_PROC(xrResultToString, ResultToString)
#undef MOCKTAIL_TEST_XR_PROC
  *function = MocktailTestPreviewFunction(name);
  if (*function) return XR_SUCCESS;
  return XR_ERROR_FUNCTION_UNSUPPORTED;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) XrResult XRAPI_CALL
xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loader,
                                  XrNegotiateRuntimeRequest* request) {
  if (loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
      request->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
      loader->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
      loader->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION)
    return XR_ERROR_INITIALIZATION_FAILED;
  request->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
  request->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 42);
  request->getInstanceProcAddr = GetProcAddr;
  return XR_SUCCESS;
}
