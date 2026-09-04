#ifndef MOCKTAIL_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_
#define MOCKTAIL_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/runtime_config.h"

namespace mocktail {
namespace runtime {

// Readable ICD manifest for `vendor` that this build can actually load, or an
// empty path. Directories are searched in order; a manifest built for another
// architecture is never selected.
std::string SelectVulkanIcdManifest(
    const std::vector<std::filesystem::path>& directories,
    std::string_view vendor);

// Publishes the resolved graphics backend before the managed payload updater
// starts. OpenGL is a strict system EGL/GLES path; it never silently retries
// through ANGLE/Vulkan or accepts a window without a real graphics context.
bool ApplyGraphicsLaunchPolicy(const RuntimeConfig& config,
                               std::string* error = nullptr);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_
