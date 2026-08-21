#include "runtime/graphics_launch_policy.h"

#include <cstdlib>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

constexpr char kVulkanClientSettingsOverrides[] =
    R"({"FStringGraphicsTextureManager2DenyPattern2":".*","FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})";

bool SetValue(const char* name, const std::string& value, std::string* error) {
  if (setenv(name, value.c_str(), 1) == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string("cannot publish resolved graphics setting: ") + name;
  }
  return false;
}

bool SetDefault(const char* name, const std::string& value,
                std::string* error) {
  const char* current = std::getenv(name);
  if (current != nullptr && current[0] != '\0') {
    return true;
  }
  return SetValue(name, value, error);
}

bool IsStrictOpenGlName(const std::string& name) {
  return name == "opengl" || name == "gles";
}

}  // namespace

bool ApplyGraphicsLaunchPolicy(const RuntimeConfig& config,
                               std::string* error) {
  if (config.graphics_backend() == GraphicsBackend::kUnknown) {
    if (error != nullptr) {
      *error = "cannot apply an unknown graphics backend";
    }
    return false;
  }

  const bool direct_vulkan =
      config.graphics_backend() == GraphicsBackend::kVulkan;
  if (!SetValue("MOCKTAIL_GRAPHICS_BACKEND",
                config.graphics_backend_name(), error) ||
      !SetValue("MOCKTAIL_PRELOAD_VULKAN_SHIM",
                direct_vulkan ? "1" : "0", error) ||
      !SetDefault("MOCKTAIL_REQUIRE_REAL_GRAPHICS", "1", error)) {
    return false;
  }

  if (IsStrictOpenGlName(config.graphics_backend_name()) &&
      (!SetValue("MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK", "1", error) ||
       !SetValue("MOCKTAIL_SOFTWARE_WINDOW_FALLBACK", "0", error))) {
    return false;
  }

  if (direct_vulkan &&
      !SetDefault("MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON",
                  kVulkanClientSettingsOverrides, error)) {
    return false;
  }
  return true;
}

}  // namespace runtime
}  // namespace mocktail
