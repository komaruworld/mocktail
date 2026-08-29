// Host system OpenGL/EGL capability probe for the system-egl (OpenGL) backend.
// Validates the real on-screen EGL path by asking SDL to create a hidden window
// and an OpenGL ES context on the host driver Mocktail uses at runtime, then
// classifies acceleration by inspecting the GL_RENDERER/GL_VENDOR strings.

#include "mocktail/graphics/system_egl_probe.h"

#include <SDL3/SDL.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

namespace mocktail {
namespace graphics {
namespace {

constexpr std::uint32_t kGlRenderer = 0x1F01;  // GL_RENDERER
constexpr std::uint32_t kGlVendor = 0x1F00;    // GL_VENDOR

BackendCapability Unavailable(std::string detail) {
  return {GraphicsBackendKind::kSystemEgl, CapabilityState::kUnavailable,
          HardwareAcceleration::kUnknown, std::move(detail)};
}

bool IsSoftwareRenderer(const char* renderer, const char* vendor) {
  auto lower_contains = [](const char* source, const char* token) {
    if (source == nullptr) {
      return false;
    }
    std::string lowered;
    lowered.reserve(std::strlen(source) + 1);
    for (const char* p = source; *p != '\0'; ++p) {
      lowered.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
    }
    return std::strstr(lowered.c_str(), token) != nullptr;
  };
  return lower_contains(renderer, "llvmpipe") ||
         lower_contains(renderer, "swiftshader") ||
         lower_contains(renderer, "softpipe") ||
         lower_contains(renderer, "software") ||
         lower_contains(vendor, "software") ||
         lower_contains(renderer, "lavapipe");
}

}  // namespace

BackendCapability ProbeSystemEgl(const SystemEglProbeOptions& options) {
  // SDL_Init is idempotent; do not call SDL_Quit here so the probe is safe to
  // call while the runtime's own SDL window is still alive.
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    return Unavailable(std::string("SDL init failed: ") + SDL_GetError());
  }

  SDL_Window* window = SDL_CreateWindow(
      "mocktail-system-egl-probe", 64, 64,
      SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  if (window == nullptr) {
    return Unavailable(std::string("SDL window creation failed: ") +
                       SDL_GetError());
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, options.gles_major);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, options.gles_minor);

  SDL_GLContext context = SDL_GL_CreateContext(window);
  if (context == nullptr) {
    const std::string detail =
        std::string("SDL GL context failed: ") + SDL_GetError();
    SDL_DestroyWindow(window);
    return Unavailable(detail);
  }

  using GlGetStringFn = const char* (*)(std::uint32_t);
  GlGetStringFn gl_get_string =
      reinterpret_cast<GlGetStringFn>(SDL_GL_GetProcAddress("glGetString"));
  const char* renderer_raw =
      gl_get_string != nullptr ? gl_get_string(kGlRenderer) : nullptr;
  const char* vendor_raw =
      gl_get_string != nullptr ? gl_get_string(kGlVendor) : nullptr;
  const std::string renderer = renderer_raw != nullptr ? renderer_raw : "";
  const std::string vendor = vendor_raw != nullptr ? vendor_raw : "";

  const bool software = IsSoftwareRenderer(renderer.c_str(), vendor.c_str());

  SDL_GL_DestroyContext(context);
  SDL_DestroyWindow(window);

  if (software && !options.allow_software_device) {
    std::string detail = "system EGL resolved to a software rasterizer";
    if (!renderer.empty()) {
      detail += " (renderer=";
      detail += renderer;
      detail += ")";
    }
    return {GraphicsBackendKind::kSystemEgl, CapabilityState::kLoadable,
            HardwareAcceleration::kSoftware, std::move(detail)};
  }

  std::string detail = "validated system EGL (real window) ";
  detail += std::to_string(options.gles_major);
  detail += ".";
  detail += std::to_string(options.gles_minor);
  if (!vendor.empty()) {
    detail += ", vendor=";
    detail += vendor;
  }
  if (!renderer.empty()) {
    detail += ", renderer=";
    detail += renderer;
  }
  detail += software ? ", software" : ", hardware";

  return {GraphicsBackendKind::kSystemEgl, CapabilityState::kReady,
          software ? HardwareAcceleration::kSoftware
                   : HardwareAcceleration::kHardware,
          std::move(detail)};
}

}  // namespace graphics
}  // namespace mocktail
