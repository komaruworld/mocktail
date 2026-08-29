#ifndef MOCKTAIL_GRAPHICS_SYSTEM_EGL_PROBE_H_
#define MOCKTAIL_GRAPHICS_SYSTEM_EGL_PROBE_H_

#include <string>

#include "mocktail/graphics/graphics_backend.h"

namespace mocktail {
namespace graphics {

// Options for probing the host system OpenGL/EGL stack.
//
// The probe creates a real, hidden SDL window and an OpenGL ES context on the
// host EGL driver Mocktail actually uses at runtime (SDL owns the EGL
// display/context), then validates the context and classifies acceleration by
// inspecting the GL_RENDERER/GL_VENDOR strings. This exercises the same driver
// and on-screen window path as the real presentation window, unlike a
// surfaceless pbuffer probe which can pass while the on-screen path fails, and
// it uses the same EGL library SDL loads rather than a separately resolved one.
struct SystemEglProbeOptions {
  // Requested OpenGL ES version. Defaults to 3.0.
  int gles_major = 3;
  int gles_minor = 0;
  bool allow_software_device = false;
};

// Probes the host system EGL/GLES stack through a real SDL window. No draw
// calls are issued. The result reports hardware vs software acceleration.
BackendCapability ProbeSystemEgl(const SystemEglProbeOptions& options);

}  // namespace graphics
}  // namespace mocktail

#endif  // MOCKTAIL_GRAPHICS_SYSTEM_EGL_PROBE_H_
