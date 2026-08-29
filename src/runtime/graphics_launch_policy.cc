#include "runtime/graphics_launch_policy.h"

#include "runtime/frame_rate_policy.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

constexpr char kVulkanClientSettingsOverrides[] =
    R"({"FStringGraphicsTextureManager2DenyPattern2":".*","FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})";

constexpr const char* kIcdDirectories[] = {
    "/usr/share/vulkan/icd.d",
    "/etc/vulkan/icd.d",
    "/usr/local/share/vulkan/icd.d",
};

struct HostGpus {
  bool intel = false;
  bool nvidia = false;
  bool amd = false;
};

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

bool EnvIsOff(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr &&
         (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0 ||
          std::strcmp(value, "igpu") == 0);
}

bool UnthrottledPresentation(const RuntimeConfig& config) {
  if (config.vsync_mode() == "off" || config.vsync_mode() == "0") {
    return true;
  }
  return config.frame_rate().mode == FrameRateLimitMode::kUnlimited;
}

bool ParsePciVendor(const std::string& raw, unsigned int* vendor) {
  if (vendor == nullptr || raw.empty()) {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(raw.c_str(), &end, 16);
  if (end == raw.c_str() || parsed > 0xffffUL) {
    return false;
  }
  *vendor = static_cast<unsigned int>(parsed);
  return true;
}

HostGpus DetectHostGpus() {
  HostGpus gpus;
  for (int index = 0; index < 16; ++index) {
    const std::string path = "/sys/class/drm/card" + std::to_string(index) +
                             "/device/vendor";
    std::ifstream input(path);
    std::string raw;
    unsigned int vendor = 0;
    if (!(input >> raw) || !ParsePciVendor(raw, &vendor)) {
      continue;
    }
    if (vendor == 0x8086) {
      gpus.intel = true;
    } else if (vendor == 0x10de) {
      gpus.nvidia = true;
    } else if (vendor == 0x1002) {
      gpus.amd = true;
    }
  }
  return gpus;
}

std::string FindIcdFile(const char* filename_needle) {
  if (filename_needle == nullptr || filename_needle[0] == '\0') {
    return {};
  }
  for (const char* directory : kIcdDirectories) {
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
      if (!iterator->is_regular_file(error)) {
        continue;
      }
      const std::string name = iterator->path().filename().string();
      if (name.find(".json") == std::string::npos ||
          name.find(filename_needle) == std::string::npos) {
        continue;
      }
      const std::string path = iterator->path().string();
      if (access(path.c_str(), R_OK) == 0) {
        return path;
      }
    }
  }
  return {};
}

std::string SelectHardwareIcd(const HostGpus& gpus) {
  const bool prefer_discrete = !EnvIsOff("DRI_PRIME") &&
                               !EnvIsOff("__NV_PRIME_RENDER_OFFLOAD");
  if (prefer_discrete && gpus.nvidia) {
    std::string nvidia = FindIcdFile("nvidia_icd");
    if (nvidia.empty()) {
      nvidia = FindIcdFile("nouveau_icd");
    }
    if (!nvidia.empty()) {
      return nvidia;
    }
  }
  if (prefer_discrete && gpus.amd) {
    const std::string amd = FindIcdFile("radeon_icd");
    if (!amd.empty()) {
      return amd;
    }
  }
  if (gpus.intel) {
    std::string intel = FindIcdFile("intel_icd");
    if (intel.empty()) {
      intel = FindIcdFile("intel_hasvk_icd");
    }
    return intel;
  }
  if (gpus.nvidia) {
    std::string nvidia = FindIcdFile("nvidia_icd");
    if (nvidia.empty()) {
      nvidia = FindIcdFile("nouveau_icd");
    }
    return nvidia;
  }
  if (gpus.amd) {
    return FindIcdFile("radeon_icd");
  }
  return {};
}

// Match Mesa ANV: 75% of RAM when the machine has more than 4GiB, else 50%.
// Forcing 50 on an 8GiB UHD 620 iGPU advertises a smaller Vk heap than ANV
// would, which increases BO eviction inside GEM_EXECBUFFER2.
const char* AnvSysMemLimitPercent() {
  std::ifstream input("/proc/meminfo");
  std::string key;
  unsigned long kb = 0;
  std::string unit;
  while (input >> key >> kb >> unit) {
    if (key == "MemTotal:") {
      return kb > 4UL * 1024UL * 1024UL ? "75" : "50";
    }
  }
  return "50";
}

bool ApplyVulkanIcdPolicy(const HostGpus& gpus, std::string* error) {
  // Drop software/emulation ICDs even when the user already pinned a driver
  // list. Old loaders ignore this variable.
  if (!SetDefault("VK_LOADER_DRIVERS_DISABLE",
                  "lvp_icd:dzn_icd:virtio_icd", error)) {
    return false;
  }
  const char* existing_files = std::getenv("VK_DRIVER_FILES");
  const char* existing_icds = std::getenv("VK_ICD_FILENAMES");
  if ((existing_files != nullptr && existing_files[0] != '\0') ||
      (existing_icds != nullptr && existing_icds[0] != '\0')) {
    return true;
  }
  const std::string icd = SelectHardwareIcd(gpus);
  if (icd.empty()) {
    return true;
  }
  std::fprintf(stderr, "  [runtime] vulkan ICD=%s\n", icd.c_str());
  return SetDefault("VK_DRIVER_FILES", icd, error) &&
         SetDefault("VK_ICD_FILENAMES", icd, error);
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

  if (config.graphics_backend() == GraphicsBackend::kSystem &&
      (!SetValue("MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK", "1", error) ||
       !SetValue("MOCKTAIL_SOFTWARE_WINDOW_FALLBACK", "0", error))) {
    return false;
  }

  if (direct_vulkan) {
    const char* wsi_mode =
        UnthrottledPresentation(config) ? "immediate" : "mailbox";
    const HostGpus gpus = DetectHostGpus();
    if (!SetDefault("MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON",
                    kVulkanClientSettingsOverrides, error) ||
        !SetDefault("ANV_SYS_MEM_LIMIT", AnvSysMemLimitPercent(), error) ||
        !SetDefault("MESA_VK_WSI_PRESENT_MODE", wsi_mode, error) ||
        // Move GEM_EXECBUFFER2 off the application thread onto Mesa's submit
        // worker so the render thread is not stuck in i915 ioctl.
        !SetDefault("MESA_VK_ENABLE_SUBMIT_THREAD", "1", error) ||
        !ApplyVulkanIcdPolicy(gpus, error)) {
      return false;
    }
    // Low FRM only on Intel-only machines. Hybrid NVIDIA/AMD laptops should
    // keep the desktop quality default on the discrete GPU.
    if (gpus.intel && !gpus.nvidia && !gpus.amd &&
        !SetDefault("MOCKTAIL_GRAPHICS_QUALITY", "1", error)) {
      return false;
    }
  }
  return true;
}

}  // namespace runtime
}  // namespace mocktail
