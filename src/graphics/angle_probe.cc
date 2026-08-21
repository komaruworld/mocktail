#include "mocktail/graphics/angle_probe.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <dlfcn.h>

#include <array>
#include <initializer_list>
#include <string>
#include <utility>

namespace mocktail {
namespace graphics {
namespace {

class SharedLibrary final {
 public:
  explicit SharedLibrary(const std::string& path)
      : handle_(dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)) {}

  ~SharedLibrary() {
    if (handle_ != nullptr) {
      dlclose(handle_);
    }
  }

  SharedLibrary(const SharedLibrary&) = delete;
  SharedLibrary& operator=(const SharedLibrary&) = delete;

  bool valid() const { return handle_ != nullptr; }
  void* Find(const char* name) const {
    return handle_ != nullptr ? dlsym(handle_, name) : nullptr;
  }

 private:
  void* handle_ = nullptr;
};

BackendCapability Unavailable(std::string detail) {
  return {GraphicsBackendKind::kAngleVulkan, CapabilityState::kUnavailable,
          HardwareAcceleration::kUnknown, std::move(detail)};
}

std::string DlErrorFor(const char* library_kind, const std::string& path) {
  std::string detail = "failed to load ";
  detail += library_kind;
  detail += " library '";
  detail += path;
  detail += "': ";
  const char* error = dlerror();
  detail += error != nullptr ? error : "unknown dynamic loader error";
  return detail;
}

bool HasSymbols(const SharedLibrary& library,
                const std::initializer_list<const char*>& symbols,
                std::string* missing_symbol) {
  for (const char* symbol : symbols) {
    if (library.Find(symbol) == nullptr) {
      if (missing_symbol != nullptr) {
        *missing_symbol = symbol;
      }
      return false;
    }
  }
  return true;
}

}  // namespace

BackendCapability ProbeAngleVulkan(const AngleProbeOptions& options) {
  if (options.egl_library_path.empty() || options.gles_library_path.empty()) {
    return Unavailable(
        "pinned ANGLE EGL and GLES library paths are both required");
  }

  dlerror();
  SharedLibrary egl(options.egl_library_path);
  if (!egl.valid()) {
    return Unavailable(DlErrorFor("EGL", options.egl_library_path));
  }
  dlerror();
  SharedLibrary gles(options.gles_library_path);
  if (!gles.valid()) {
    return Unavailable(DlErrorFor("GLES", options.gles_library_path));
  }

  std::string missing;
  if (!HasSymbols(
          egl,
          {"eglGetProcAddress", "eglInitialize", "eglTerminate",
           "eglQueryString", "eglChooseConfig", "eglCreateContext",
           "eglCreateWindowSurface", "eglMakeCurrent", "eglSwapBuffers"},
          &missing)) {
    return Unavailable("ANGLE EGL library is missing required symbol " +
                       missing);
  }
  if (!HasSymbols(
          gles,
          {"glGetString", "glGetIntegerv", "glCreateShader", "glDrawElements"},
          &missing)) {
    return Unavailable("ANGLE GLES library is missing required symbol " +
                       missing);
  }

  auto get_proc_address =
      reinterpret_cast<PFNEGLGETPROCADDRESSPROC>(egl.Find("eglGetProcAddress"));
  auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
      egl.Find("eglGetPlatformDisplayEXT"));
  if (get_platform_display == nullptr && get_proc_address != nullptr) {
    get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        get_proc_address("eglGetPlatformDisplayEXT"));
  }
  if (get_platform_display == nullptr) {
    return Unavailable(
        "ANGLE EGL library does not expose eglGetPlatformDisplayEXT");
  }

  auto initialize =
      reinterpret_cast<PFNEGLINITIALIZEPROC>(egl.Find("eglInitialize"));
  auto terminate =
      reinterpret_cast<PFNEGLTERMINATEPROC>(egl.Find("eglTerminate"));
  auto query_string =
      reinterpret_cast<PFNEGLQUERYSTRINGPROC>(egl.Find("eglQueryString"));

  const EGLint device_type =
      options.allow_software_device
          ? EGL_PLATFORM_ANGLE_DEVICE_TYPE_SWIFTSHADER_ANGLE
          : EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE;
  const std::array<EGLint, 7> attributes = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE,
      EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE,
      EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE,
      device_type,
      EGL_PLATFORM_ANGLE_DEBUG_LAYERS_ENABLED_ANGLE,
      EGL_FALSE,
      EGL_NONE,
  };

  EGLDisplay display = get_platform_display(
      EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attributes.data());
  if (display == EGL_NO_DISPLAY) {
    return Unavailable("ANGLE failed to create a Vulkan EGLDisplay");
  }

  EGLint major = 0;
  EGLint minor = 0;
  if (initialize(display, &major, &minor) != EGL_TRUE) {
    terminate(display);
    return Unavailable("ANGLE Vulkan EGLDisplay failed eglInitialize");
  }

  const char* vendor = query_string(display, EGL_VENDOR);
  const char* version = query_string(display, EGL_VERSION);
  std::string detail = "validated ANGLE Vulkan EGL ";
  detail += std::to_string(major);
  detail += ".";
  detail += std::to_string(minor);
  if (vendor != nullptr) {
    detail += ", vendor=";
    detail += vendor;
  }
  if (version != nullptr) {
    detail += ", version=";
    detail += version;
  }
  terminate(display);

  return {GraphicsBackendKind::kAngleVulkan, CapabilityState::kReady,
          options.allow_software_device ? HardwareAcceleration::kSoftware
                                        : HardwareAcceleration::kHardware,
          std::move(detail)};
}

}  // namespace graphics
}  // namespace mocktail
