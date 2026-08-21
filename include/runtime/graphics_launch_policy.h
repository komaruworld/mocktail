#ifndef MOCKTAIL_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_
#define MOCKTAIL_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_

#include <string>

#include "runtime/runtime_config.h"

namespace mocktail {
namespace runtime {

// Publishes the resolved graphics backend before the managed payload updater
// starts. OpenGL is a strict system EGL/GLES path; it never silently retries
// through ANGLE/Vulkan or accepts a window without a real graphics context.
bool ApplyGraphicsLaunchPolicy(const RuntimeConfig& config,
                               std::string* error = nullptr);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_GRAPHICS_LAUNCH_POLICY_H_
