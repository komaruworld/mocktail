// When Mocktail has an SDL window, this transitional shim exposes that
// window's EGL display/surface/context to Android-side code. Deterministic
// fake handles require MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS=1.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

using EGLBoolean = uint32_t;
using EGLint = int32_t;
using EGLenum = uint32_t;
using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLNativeDisplayType = void*;
using EGLNativeWindowType = void*;
using __eglMustCastFP = void (*)();

namespace {

constexpr EGLBoolean kEglTrue = 1;
constexpr EGLBoolean kEglFalse = 0;
constexpr EGLint kEglSuccess = 0x3000;
constexpr EGLint kEglNotInitialized = 0x3001;
constexpr EGLint kEglBadDisplay = 0x3008;
constexpr EGLint kEglBadAttribute = 0x3004;
constexpr EGLint kEglConfigId = 0x3028;
constexpr EGLint kEglRedSize = 0x3024;
constexpr EGLint kEglGreenSize = 0x3023;
constexpr EGLint kEglBlueSize = 0x3022;
constexpr EGLint kEglAlphaSize = 0x3021;
constexpr EGLint kEglDepthSize = 0x3025;
constexpr EGLint kEglStencilSize = 0x3026;
constexpr EGLint kEglSurfaceType = 0x3033;
constexpr EGLint kEglWindowBit = 0x0004;
constexpr EGLint kEglPbufferBit = 0x0001;
constexpr EGLint kEglRenderableType = 0x3040;
constexpr EGLint kEglOpenGles2Bit = 0x0004;
constexpr EGLint kEglWidth = 0x3057;
constexpr EGLint kEglHeight = 0x3056;
constexpr EGLenum kEglOpenGlesApi = 0x30a0;

int g_display_handle;
int g_config_handle;
int g_context_handle;
int g_surface_handle;
std::atomic<int> g_swap_call_count{0};
thread_local EGLContext g_current_context = nullptr;
thread_local EGLSurface g_current_draw_surface = nullptr;
thread_local EGLSurface g_current_read_surface = nullptr;
thread_local EGLint g_last_error = kEglSuccess;

bool TraceEnabled() {
  return std::getenv("MOCKTAIL_EGL_TRACE") != nullptr ||
         std::getenv("MOCKTAIL_GL_TRACE") != nullptr ||
         std::getenv("MOCKTAIL_TRACE_ALL") != nullptr ||
         std::getenv("MOCKTAIL_FULL_TRACE") != nullptr;
}

bool TestGraphicsStubsEnabled() {
  const char* value = std::getenv("MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS");
  return value != nullptr && std::strcmp(value, "1") == 0;
}

template <typename Fn>
Fn ResolveDefault(const char* name) {
  void* symbol = dlsym(RTLD_DEFAULT, name);
  return reinterpret_cast<Fn>(symbol);
}

EGLDisplay FallbackDisplay() {
  return &g_display_handle;
}

EGLConfig FallbackConfig() {
  return &g_config_handle;
}

EGLContext FallbackContext() {
  return &g_context_handle;
}

EGLSurface FallbackSurface() {
  return &g_surface_handle;
}

EGLDisplay WindowDisplay() {
  using Fn = void* (*)();
  Fn fn = ResolveDefault<Fn>("mocktail_egl_display");
  return fn != nullptr ? fn() : nullptr;
}

EGLConfig WindowConfig() {
  using Fn = void* (*)();
  Fn fn = ResolveDefault<Fn>("mocktail_egl_config");
  return fn != nullptr ? fn() : nullptr;
}

EGLContext WindowContext() {
  using Fn = void* (*)();
  Fn fn = ResolveDefault<Fn>("mocktail_egl_context");
  return fn != nullptr ? fn() : nullptr;
}

EGLSurface WindowSurface() {
  using Fn = void* (*)();
  Fn fn = ResolveDefault<Fn>("mocktail_egl_surface");
  return fn != nullptr ? fn() : nullptr;
}

bool MakeWindowCurrent() {
  using Fn = bool (*)();
  Fn fn = ResolveDefault<Fn>("mocktail_window_make_current");
  return fn != nullptr && fn();
}

bool ReleaseWindowCurrent() {
  using Fn = bool (*)();
  Fn fn = ResolveDefault<Fn>("mocktail_window_release_current");
  return fn != nullptr && fn();
}

bool SwapWindowBuffers() {
  using Fn = bool (*)();
  Fn fn = ResolveDefault<Fn>("mocktail_window_swap_buffers");
  return fn != nullptr && fn();
}

void* WindowGlProcAddress(const char* name) {
  using Fn = void* (*)(const char*);
  Fn fn = ResolveDefault<Fn>("mocktail_gl_proc_address");
  return fn != nullptr ? fn(name) : nullptr;
}

int WindowWidth() {
  using Fn = int (*)();
  Fn fn = ResolveDefault<Fn>("mocktail_window_width");
  return fn != nullptr ? fn() : 1280;
}

int WindowHeight() {
  using Fn = int (*)();
  Fn fn = ResolveDefault<Fn>("mocktail_window_height");
  return fn != nullptr ? fn() : 720;
}

EGLDisplay ActiveDisplay() {
  EGLDisplay display = WindowDisplay();
  return display != nullptr
             ? display
             : (TestGraphicsStubsEnabled() ? FallbackDisplay() : nullptr);
}

EGLConfig ActiveConfig() {
  EGLConfig config = WindowConfig();
  return config != nullptr
             ? config
             : (TestGraphicsStubsEnabled() ? FallbackConfig() : nullptr);
}

EGLContext ActiveContext() {
  EGLContext context = WindowContext();
  return context != nullptr
             ? context
             : (TestGraphicsStubsEnabled() ? FallbackContext() : nullptr);
}

EGLSurface ActiveSurface() {
  EGLSurface surface = WindowSurface();
  return surface != nullptr
             ? surface
             : (TestGraphicsStubsEnabled() ? FallbackSurface() : nullptr);
}

bool IsActiveDisplay(EGLDisplay display) {
  return display != nullptr && display == ActiveDisplay();
}

EGLint ConfigAttribValue(EGLint attribute, bool* found) {
  *found = true;
  switch (attribute) {
    case kEglConfigId:
      return 1;
    case kEglRedSize:
    case kEglGreenSize:
    case kEglBlueSize:
    case kEglAlphaSize:
      return 8;
    case kEglDepthSize:
      return 24;
    case kEglStencilSize:
      return 8;
    case kEglSurfaceType:
      return kEglWindowBit | kEglPbufferBit;
    case kEglRenderableType:
      return kEglOpenGles2Bit;
    default:
      *found = false;
      return 0;
  }
}

void SetError(EGLint error) {
  g_last_error = error;
}

}  // namespace

extern "C" {

EGLDisplay eglGetDisplay(EGLNativeDisplayType) {
  EGLDisplay display = ActiveDisplay();
  SetError(display != nullptr ? kEglSuccess : kEglNotInitialized);
  return display;
}

EGLBoolean eglInitialize(EGLDisplay display, EGLint* major, EGLint* minor) {
  if (!IsActiveDisplay(display)) {
    SetError(kEglBadDisplay);
    return kEglFalse;
  }
  if (major != nullptr) {
    *major = 1;
  }
  if (minor != nullptr) {
    *minor = 5;
  }
  SetError(kEglSuccess);
  return kEglTrue;
}

EGLBoolean eglBindAPI(EGLenum api) {
  if (api != kEglOpenGlesApi) {
    SetError(kEglBadAttribute);
    return kEglFalse;
  }
  SetError(kEglSuccess);
  return kEglTrue;
}

EGLBoolean eglChooseConfig(EGLDisplay display, const EGLint*,
                           EGLConfig* configs, EGLint config_size,
                           EGLint* num_config) {
  if (!IsActiveDisplay(display)) {
    SetError(kEglBadDisplay);
    return kEglFalse;
  }
  if (num_config != nullptr) {
    *num_config = 1;
  }
  if (configs != nullptr && config_size > 0) {
    EGLConfig config = ActiveConfig();
    if (config == nullptr) {
      SetError(kEglNotInitialized);
      return kEglFalse;
    }
    configs[0] = config;
  }
  SetError(kEglSuccess);
  return kEglTrue;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay display, EGLConfig config,
                              EGLint attribute, EGLint* value) {
  if (!IsActiveDisplay(display) || config == nullptr ||
      config != ActiveConfig() || value == nullptr) {
    SetError(kEglBadDisplay);
    return kEglFalse;
  }
  bool found = false;
  EGLint attr_value = ConfigAttribValue(attribute, &found);
  if (!found) {
    SetError(kEglBadAttribute);
    return kEglFalse;
  }
  *value = attr_value;
  SetError(kEglSuccess);
  return kEglTrue;
}

EGLSurface eglCreateWindowSurface(EGLDisplay display, EGLConfig config,
                                  EGLNativeWindowType native_window,
                                  const EGLint*) {
  if (!IsActiveDisplay(display) || config == nullptr ||
      config != ActiveConfig()) {
    SetError(kEglBadDisplay);
    return nullptr;
  }
  EGLSurface surface = ActiveSurface();
  if (surface == nullptr) {
    SetError(kEglNotInitialized);
    return nullptr;
  }
  if (TraceEnabled()) {
    fprintf(stderr,
            "[eglstub] eglCreateWindowSurface display=%p config=%p "
            "native_window=%p -> surface=%p\n",
            display, config, native_window, surface);
  }
  SetError(kEglSuccess);
  return surface;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay display, EGLConfig config,
                                   const EGLint*) {
  if (!IsActiveDisplay(display) || config == nullptr ||
      config != ActiveConfig()) {
    SetError(kEglBadDisplay);
    return nullptr;
  }
  if (!TestGraphicsStubsEnabled()) {
    SetError(kEglNotInitialized);
    return nullptr;
  }
  SetError(kEglSuccess);
  return FallbackSurface();
}

EGLContext eglCreateContext(EGLDisplay display, EGLConfig config,
                            EGLContext,
                            const EGLint*) {
  if (!IsActiveDisplay(display) || config == nullptr ||
      config != ActiveConfig()) {
    SetError(kEglBadDisplay);
    return nullptr;
  }
  EGLContext context = ActiveContext();
  if (context == nullptr) {
    SetError(kEglNotInitialized);
    return nullptr;
  }
  SetError(kEglSuccess);
  return context;
}

EGLBoolean eglMakeCurrent(EGLDisplay display, EGLSurface draw,
                          EGLSurface read, EGLContext context) {
  if (!IsActiveDisplay(display)) {
    SetError(kEglBadDisplay);
    return kEglFalse;
  }
  if (TraceEnabled()) {
    fprintf(stderr,
            "[eglstub] eglMakeCurrent display=%p draw=%p read=%p context=%p "
            "window_context=%p\n",
            display, draw, read, context, WindowContext());
  }
  if (context == nullptr && draw == nullptr && read == nullptr) {
    if (WindowContext() != nullptr && !ReleaseWindowCurrent()) {
      SetError(kEglNotInitialized);
      return kEglFalse;
    }
    g_current_context = nullptr;
    g_current_draw_surface = nullptr;
    g_current_read_surface = nullptr;
    SetError(kEglSuccess);
    return kEglTrue;
  }

  EGLContext active_context = ActiveContext();
  EGLSurface active_surface = ActiveSurface();
  if (active_context == nullptr || active_surface == nullptr ||
      context != active_context || draw != active_surface ||
      read != active_surface) {
    SetError(kEglNotInitialized);
    return kEglFalse;
  }
  if (WindowContext() != nullptr) {
    if (!MakeWindowCurrent()) {
      SetError(kEglNotInitialized);
      return kEglFalse;
    }
  }
  g_current_context = context;
  g_current_draw_surface = draw;
  g_current_read_surface = read;
  SetError(kEglSuccess);
  return kEglTrue;
}

EGLContext eglGetCurrentContext() {
  return g_current_context;
}

EGLSurface eglGetCurrentSurface(EGLint readdraw) {
  return readdraw == 0x3055 ? g_current_read_surface : g_current_draw_surface;
}

EGLDisplay eglGetCurrentDisplay() {
  return g_current_context != nullptr ? ActiveDisplay() : nullptr;
}

EGLBoolean eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
  if (!IsActiveDisplay(display)) {
    SetError(kEglBadDisplay);
    return kEglFalse;
  }
  const EGLSurface window_surface = WindowSurface();
  const int call_index = g_swap_call_count.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool surface_matches_window = window_surface != nullptr &&
                                      surface == window_surface;
  if (TraceEnabled() && (call_index <= 20 || call_index % 60 == 0)) {
    fprintf(stderr,
            "[eglstub] eglSwapBuffers #%d display=%p surface=%p window_surface=%p "
            "matches_window=%d\n",
            call_index, display, surface, window_surface,
            surface_matches_window ? 1 : 0);
  }
  if (!surface_matches_window) {
    SetError(kEglNotInitialized);
    return kEglFalse;
  }
  const bool swapped = SwapWindowBuffers();
  SetError(swapped ? kEglSuccess : kEglNotInitialized);
  return swapped ? kEglTrue : kEglFalse;
}

EGLBoolean eglSwapInterval(EGLDisplay display, EGLint) {
  if (!IsActiveDisplay(display) || g_current_context == nullptr) {
    SetError(kEglBadDisplay);
    return kEglFalse;
  }
  SetError(kEglSuccess);
  return kEglTrue;
}

EGLBoolean eglQuerySurface(EGLDisplay display, EGLSurface surface,
                           EGLint attribute, EGLint* value) {
  if (!IsActiveDisplay(display) || surface == nullptr ||
      surface != ActiveSurface() || value == nullptr) {
    SetError(kEglBadDisplay);
    return kEglFalse;
  }
  if (attribute == kEglWidth) {
    *value = WindowWidth();
  } else if (attribute == kEglHeight) {
    *value = WindowHeight();
  } else {
    SetError(kEglBadAttribute);
    return kEglFalse;
  }
  SetError(kEglSuccess);
  return kEglTrue;
}

const char* eglQueryString(EGLDisplay display, EGLint name) {
  if (!IsActiveDisplay(display)) {
    SetError(kEglBadDisplay);
    return nullptr;
  }
  SetError(kEglSuccess);
  switch (name) {
    case 0x3053:
      return "1.5 Mocktail";
    case 0x3054:
      return "Mocktail";
    case 0x3055:
      return "EGL_KHR_create_context EGL_KHR_surfaceless_context";
    case 0x308d:
      return "OpenGL_ES";
    default:
      return "";
  }
}

EGLBoolean eglDestroyContext(EGLDisplay display, EGLContext context) {
  if (!IsActiveDisplay(display) || context == nullptr ||
      context != ActiveContext()) {
    SetError(kEglBadDisplay);
    return kEglFalse;
  }
  g_current_context = nullptr;
  SetError(kEglSuccess);
  return kEglTrue;
}

EGLBoolean eglDestroySurface(EGLDisplay display, EGLSurface surface) {
  if (!IsActiveDisplay(display) || surface == nullptr ||
      surface != ActiveSurface()) {
    SetError(kEglBadDisplay);
    return kEglFalse;
  }
  SetError(kEglSuccess);
  return kEglTrue;
}

EGLBoolean eglTerminate(EGLDisplay display) {
  if (!IsActiveDisplay(display)) {
    SetError(kEglBadDisplay);
    return kEglFalse;
  }
  g_current_context = nullptr;
  SetError(kEglSuccess);
  return kEglTrue;
}

EGLint eglGetError() {
  EGLint error = g_last_error;
  g_last_error = kEglSuccess;
  return error;
}

__eglMustCastFP eglGetProcAddress(const char* procname) {
  if (procname == nullptr) {
    return nullptr;
  }
#define EGL_PROC(name) \
  if (std::strcmp(procname, #name) == 0) return reinterpret_cast<__eglMustCastFP>(name)
  EGL_PROC(eglBindAPI);
  EGL_PROC(eglChooseConfig);
  EGL_PROC(eglCreateContext);
  EGL_PROC(eglCreatePbufferSurface);
  EGL_PROC(eglCreateWindowSurface);
  EGL_PROC(eglDestroyContext);
  EGL_PROC(eglDestroySurface);
  EGL_PROC(eglGetConfigAttrib);
  EGL_PROC(eglGetCurrentContext);
  EGL_PROC(eglGetCurrentDisplay);
  EGL_PROC(eglGetCurrentSurface);
  EGL_PROC(eglGetDisplay);
  EGL_PROC(eglGetError);
  EGL_PROC(eglInitialize);
  EGL_PROC(eglMakeCurrent);
  EGL_PROC(eglQueryString);
  EGL_PROC(eglQuerySurface);
  EGL_PROC(eglSwapBuffers);
  EGL_PROC(eglSwapInterval);
  EGL_PROC(eglTerminate);
#undef EGL_PROC
  void* gl_symbol = WindowGlProcAddress(procname);
  if (gl_symbol != nullptr) {
    return reinterpret_cast<__eglMustCastFP>(gl_symbol);
  }
  void* symbol = dlsym(RTLD_DEFAULT, procname);
  if (symbol != nullptr) {
    return reinterpret_cast<__eglMustCastFP>(symbol);
  }
  if (TraceEnabled()) {
    return nullptr;
  }
  return nullptr;
}

}  // extern "C"
