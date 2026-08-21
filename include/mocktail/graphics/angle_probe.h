#ifndef MOCKTAIL_GRAPHICS_ANGLE_PROBE_H_
#define MOCKTAIL_GRAPHICS_ANGLE_PROBE_H_

#include <string>

#include "mocktail/graphics/graphics_backend.h"

namespace mocktail {
namespace graphics {

struct AngleProbeOptions {
  // Production callers provide paths from one pinned ANGLE distribution.
  // This probe intentionally does not scan browser installation directories.
  std::string egl_library_path;
  std::string gles_library_path;
  bool allow_software_device = false;
};

// Loads a pinned ANGLE pair, verifies the required EGL/GLES symbols, and
// initializes an EGL_PLATFORM_ANGLE Vulkan display. No context is fabricated.
BackendCapability ProbeAngleVulkan(const AngleProbeOptions& options);

}  // namespace graphics
}  // namespace mocktail

#endif  // MOCKTAIL_GRAPHICS_ANGLE_PROBE_H_
