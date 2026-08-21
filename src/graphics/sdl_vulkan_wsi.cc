#include "mocktail/graphics/sdl_vulkan_wsi.h"

#include <SDL3/SDL.h>

#include <string>
#include <utility>

namespace mocktail {
namespace graphics {
namespace {

Status SdlVulkanError(const char* operation) {
  std::string detail = operation != nullptr ? operation : "SDL Vulkan call";
  detail += " failed: ";
  const char* error = SDL_GetError();
  detail += error != nullptr && error[0] != '\0' ? error : "unknown SDL error";
  return Status::Error(StatusCode::kPlatformError, std::move(detail));
}

}  // namespace

SdlVulkanWsi::~SdlVulkanWsi() { Shutdown(); }

Status SdlVulkanWsi::Initialize(const platform::NativeWindowDescriptor& window,
                                const std::string& vulkan_loader_path) {
  if (initialized_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "SDL Vulkan WSI is already initialized");
  }
  if (window.surface_api != platform::WindowSurfaceApi::kDirectVulkan ||
      window.backend_window == nullptr) {
    return Status::Error(
        StatusCode::kInvalidArgument,
        "SDL Vulkan WSI requires a real SDL_WINDOW_VULKAN window");
  }

  const char* loader =
      vulkan_loader_path.empty() ? nullptr : vulkan_loader_path.c_str();
  if (!SDL_Vulkan_LoadLibrary(loader)) {
    return SdlVulkanError("SDL_Vulkan_LoadLibrary");
  }

  Uint32 extension_count = 0;
  const char* const* extensions =
      SDL_Vulkan_GetInstanceExtensions(&extension_count);
  if (extensions == nullptr || extension_count == 0) {
    SDL_Vulkan_UnloadLibrary();
    return SdlVulkanError("SDL_Vulkan_GetInstanceExtensions");
  }
  if (SDL_Vulkan_GetVkGetInstanceProcAddr() == nullptr) {
    SDL_Vulkan_UnloadLibrary();
    return SdlVulkanError("SDL_Vulkan_GetVkGetInstanceProcAddr");
  }

  required_instance_extensions_.clear();
  required_instance_extensions_.reserve(extension_count);
  for (Uint32 i = 0; i < extension_count; ++i) {
    if (extensions[i] == nullptr || extensions[i][0] == '\0') {
      SDL_Vulkan_UnloadLibrary();
      required_instance_extensions_.clear();
      return Status::Error(StatusCode::kPlatformError,
                           "SDL returned an empty Vulkan WSI extension");
    }
    required_instance_extensions_.emplace_back(extensions[i]);
  }

  sdl_window_ = window.backend_window;
  initialized_ = true;
  return Status::Ok();
}

void SdlVulkanWsi::Shutdown() {
  if (initialized_) {
    SDL_Vulkan_UnloadLibrary();
  }
  sdl_window_ = nullptr;
  required_instance_extensions_.clear();
  initialized_ = false;
}

SDL_FunctionPointer SdlVulkanWsi::GetInstanceProcAddress() const {
  return initialized_ ? SDL_Vulkan_GetVkGetInstanceProcAddr() : nullptr;
}

BackendCapability SdlVulkanWsi::DescribeCapability() const {
  if (!initialized_) {
    return {GraphicsBackendKind::kDirectVulkan, CapabilityState::kUnavailable,
            HardwareAcceleration::kUnknown,
            "SDL Vulkan loader and WSI are not initialized"};
  }

  std::string detail =
      "host Vulkan loader and SDL WSI extensions are loadable: ";
  for (size_t i = 0; i < required_instance_extensions_.size(); ++i) {
    if (i != 0) {
      detail += ",";
    }
    detail += required_instance_extensions_[i];
  }
  detail += "; physical device and surface presentation are not validated";
  return {GraphicsBackendKind::kDirectVulkan, CapabilityState::kLoadable,
          HardwareAcceleration::kUnknown, std::move(detail)};
}

Status SdlVulkanWsi::CreateSurface(VkInstance instance,
                                   const VkAllocationCallbacks* allocator,
                                   VkSurfaceKHR* surface) const {
  if (!initialized_ || sdl_window_ == nullptr) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "SDL Vulkan WSI is not initialized");
  }
  if (instance == nullptr || surface == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "Vulkan instance and surface output are required");
  }
  if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(sdl_window_), instance,
                                allocator, surface)) {
    return SdlVulkanError("SDL_Vulkan_CreateSurface");
  }
  return Status::Ok();
}

void SdlVulkanWsi::DestroySurface(
    VkInstance instance, VkSurfaceKHR surface,
    const VkAllocationCallbacks* allocator) const {
  if (!initialized_ || instance == nullptr || surface == nullptr) {
    return;
  }
  SDL_Vulkan_DestroySurface(instance, surface, allocator);
}

}  // namespace graphics
}  // namespace mocktail
