#include "window/window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>
#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "mocktail/platform/display_refresh_capabilities.h"
#include "mocktail/platform/sdl_application_metadata.h"
#include "mocktail/platform/sdl_event_converter.h"
#include "mocktail/platform/sdl_window_icon.h"
#include "window/input_pump_pacer.h"
#include "window/main_thread_command_gate.h"
#include "window/roblox_fullscreen_menu_request_gate.h"
#include "window/video_driver_policy.h"
#include "window/vulkan_present_progress_gate.h"
#include "window/vulkan_surface_recovery_gate.h"
#include "window/window_fullscreen_request_gate.h"
#include "window/window_fullscreen_state_sync.h"
#include "window/window_pointer_capture_owner.h"
#include "window/window_resize_readiness_gate.h"
#include "window/window_state_store.h"
#include "window/window_text_input_owner.h"

namespace mocktail {
namespace window {

// Local EGL declarations avoid conflicts with the stub headers.

using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLBoolean = unsigned int;
using EGLint = int;
using EGLenum = unsigned int;
using EGLNativeDisplayType = void*;
using EGLNativeWindowType = void*;
using __eglMustCastFP = void (*)();

static constexpr EGLint EGL_NONE_VAL = 0x3038;
static constexpr EGLint EGL_SURFACE_TYPE_VAL = 0x3033;
static constexpr EGLint EGL_WINDOW_BIT_VAL = 0x0004;
static constexpr EGLint EGL_RENDERABLE_TYPE_VAL = 0x3040;
static constexpr EGLint EGL_OPENGL_ES2_BIT_VAL = 0x0004;
static constexpr EGLint EGL_BLUE_SIZE_VAL = 0x3022;
static constexpr EGLint EGL_GREEN_SIZE_VAL = 0x3023;
static constexpr EGLint EGL_RED_SIZE_VAL = 0x3024;
static constexpr EGLint EGL_ALPHA_SIZE_VAL = 0x3021;
static constexpr EGLint EGL_DEPTH_SIZE_VAL = 0x3025;
static constexpr EGLint EGL_STENCIL_SIZE_VAL = 0x3026;
static constexpr EGLint EGL_CONTEXT_CLIENT_VERSION_VAL = 0x3098;
static constexpr EGLint EGL_PLATFORM_ANGLE_ANGLE_VAL = 0x3202;
static constexpr EGLint EGL_PLATFORM_ANGLE_TYPE_ANGLE_VAL = 0x3203;
static constexpr EGLint EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE_VAL = 0x3209;
static constexpr EGLint EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE_VAL =
    0x320a;
static constexpr EGLint EGL_PLATFORM_ANGLE_DEBUG_LAYERS_ENABLED_ANGLE_VAL =
    0x3451;
static constexpr EGLint EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE_VAL = 0x3450;
static constexpr EGLint EGL_PLATFORM_ANGLE_DEVICE_TYPE_SWIFTSHADER_ANGLE_VAL =
    0x3487;
static constexpr EGLint EGL_TRUE_VAL = 1;
static constexpr EGLint EGL_FALSE_VAL = 0;
static constexpr EGLBoolean EGL_TRUE_B = 1;
static constexpr EGLBoolean EGL_FALSE_B = 0;
static constexpr EGLDisplay EGL_NO_DISPLAY = nullptr;
static constexpr EGLContext EGL_NO_CONTEXT = nullptr;
static constexpr EGLSurface EGL_NO_SURFACE = nullptr;

using PFN_eglGetDisplay = EGLDisplay (*)(EGLNativeDisplayType);
using PFN_eglInitialize = EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*);
using PFN_eglBindAPI = EGLBoolean (*)(EGLenum);
using PFN_eglChooseConfig = EGLBoolean (*)(EGLDisplay, const EGLint*,
                                           EGLConfig*, EGLint, EGLint*);
using PFN_eglCreateWindowSurface = EGLSurface (*)(EGLDisplay, EGLConfig,
                                                  EGLNativeWindowType,
                                                  const EGLint*);
using PFN_eglCreatePbufferSurface = EGLSurface (*)(EGLDisplay, EGLConfig,
                                                   const EGLint*);
using PFN_eglCreateContext = EGLContext (*)(EGLDisplay, EGLConfig, EGLContext,
                                            const EGLint*);
using PFN_eglMakeCurrent = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface,
                                          EGLContext);
using PFN_eglSwapBuffers = EGLBoolean (*)(EGLDisplay, EGLSurface);
using PFN_eglGetError = EGLint (*)();
using PFN_eglDestroyContext = EGLBoolean (*)(EGLDisplay, EGLContext);
using PFN_eglDestroySurface = EGLBoolean (*)(EGLDisplay, EGLSurface);
using PFN_eglTerminate = EGLBoolean (*)(EGLDisplay);
using PFN_eglGetProcAddress = __eglMustCastFP (*)(const char*);

struct WindowState {
  SDL_Window* sdl_window = nullptr;
  EGLDisplay egl_display = nullptr;
  EGLConfig egl_config = nullptr;
  EGLSurface egl_surface = nullptr;
  EGLContext egl_context = nullptr;
  void* native_window = nullptr;
  int width = 1280;
  int height = 720;
  bool initialised = false;
  bool quit_requested = false;
  bool visible = false;
  bool software_window = false;
  bool direct_vulkan = false;
  bool input_test_sequence_queued = false;
  bool fullscreen_readiness_requested = false;
  bool fullscreen_readiness_return_requested = false;
  bool restored_fullscreen_sync_pending = false;
  int fullscreen_readiness_present_baseline = 0;
  bool state_persistence_active = false;
  bool state_persistence_dirty = false;
  uint64_t state_persistence_change_ticks_ns = 0;
  PersistedWindowState persisted_window;
  platform::DisplayRefreshCapabilities display_refresh;
  int resize_target_logical_width = 0;
  int resize_target_logical_height = 0;
};

static WindowState g_state;
static std::atomic<int> g_real_swap_count{0};
static std::atomic<uint64_t> g_first_present_ticks_ns{0};
static InputPumpPacer g_input_pump_pacer;
static PresentLifecycleGate g_present_lifecycle;
static VulkanPresentProgressGate g_vulkan_present_progress_gate;
static thread_local uint64_t g_vulkan_host_present_sequence = 0;
static VulkanSurfaceRecoveryGate g_vulkan_surface_recovery_gate;
static WindowSurfaceLifecycle g_window_surface_lifecycle;
static PlatformEventObserverGate g_platform_event_observer;
static MainThreadCommandGate g_pre_text_input_pump_gate;
static WindowFullscreenRequestGate g_fullscreen_request_gate;
static RobloxFullscreenMenuRequestGate g_fullscreen_menu_request_gate;
static WindowFullscreenStateSync g_fullscreen_state_sync;
static std::unique_ptr<WindowResizeReadinessGate> g_resize_readiness_gate;
static std::unique_ptr<SdlTextInputBackend> g_text_input_backend;
static std::unique_ptr<WindowTextInputOwner> g_text_input_owner;
static std::unique_ptr<SdlPointerCaptureBackend> g_pointer_capture_backend;
static std::unique_ptr<WindowPointerCaptureOwner> g_pointer_capture_owner;
static char g_preferred_egl_library[4096];
static char g_preferred_gles_library[4096];
static bool g_auto_angle_retry_attempted = false;
static std::filesystem::path g_window_state_path;

// Floors the live window at the size a restored geometry already has to clear.
void ApplyMinimumWindowSize(SDL_Window* window) {
  if (window == nullptr) {
    return;
  }
  if (!SDL_SetWindowMinimumSize(window, kMinimumWindowWidth,
                                kMinimumWindowHeight)) {
    std::fprintf(stderr, "  [window] SDL minimum size rejected: %s\n",
                 SDL_GetError());
  }
}

void ApplyWindowIcon(SDL_Window* window) {
  const Status status = platform::ApplySdlWindowIcon(window);
  if (!status.ok()) {
    std::fprintf(stderr, "  [window] SDL window icon unavailable: %s\n",
                 status.message().c_str());
  }
}

bool TraceFlagEnabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

bool WindowTraceEnabled() {
  static const bool enabled = TraceFlagEnabled("MOCKTAIL_WINDOW_TRACE") ||
                              TraceFlagEnabled("MOCKTAIL_TRACE_ALL") ||
                              TraceFlagEnabled("MOCKTAIL_FULL_TRACE");
  return enabled;
}

bool SdlEventTraceEnabled() {
  static const bool enabled = TraceFlagEnabled("MOCKTAIL_SDL_EVENT_TRACE") ||
                              TraceFlagEnabled("MOCKTAIL_TRACE_ALL") ||
                              TraceFlagEnabled("MOCKTAIL_FULL_TRACE");
  return enabled;
}

const char* GetEnvNonEmpty(const char* name) {
  if (name == nullptr) {
    return nullptr;
  }
  static std::mutex mutex;
  static std::unordered_map<const char*, const char*> cache;
  std::lock_guard<std::mutex> lock(mutex);
  const auto existing = cache.find(name);
  if (existing != cache.end()) {
    return existing->second;
  }
  const char* raw = std::getenv(name);
  const char* value = (raw != nullptr && raw[0] != '\0') ? raw : nullptr;
  cache.emplace(name, value);
  return value;
}

bool IsEnabledEnv(const char* name) {
  const char* value = GetEnvNonEmpty(name);
  return value != nullptr && std::strcmp(value, "0") != 0;
}

bool IsDisabledEnv(const char* name) {
  const char* value = GetEnvNonEmpty(name);
  return value != nullptr &&
         (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
          std::strcmp(value, "FALSE") == 0);
}

bool WindowStatePersistenceSuppressed() {
  return IsEnabledEnv("MOCKTAIL_RESIZE_READINESS") ||
         IsEnabledEnv("MOCKTAIL_FULLSCREEN_READINESS") ||
         IsEnabledEnv("MOCKTAIL_INPUT_READINESS") ||
         IsEnabledEnv("MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS");
}

bool IsWaylandVideoDriver() {
  const char* driver = SDL_GetCurrentVideoDriver();
  return driver != nullptr && std::strcmp(driver, "wayland") == 0;
}

bool SavedPositionIntersectsDisplay(const PersistedWindowState& state) {
  if (!state.has_position) {
    return false;
  }
  int display_count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&display_count);
  if (displays == nullptr || display_count <= 0) {
    SDL_free(displays);
    return false;
  }
  const SDL_Rect saved = {state.x, state.y, state.width, state.height};
  bool visible = false;
  for (int index = 0; index < display_count && !visible; ++index) {
    SDL_Rect bounds{};
    SDL_Rect intersection{};
    visible = SDL_GetDisplayBounds(displays[index], &bounds) &&
              SDL_GetRectIntersection(&saved, &bounds, &intersection) &&
              intersection.w >= 64 && intersection.h >= 64;
  }
  SDL_free(displays);
  return visible;
}

void ApplyRestoredWindowPosition() {
  if (g_state.sdl_window == nullptr || !g_state.persisted_window.has_position ||
      IsWaylandVideoDriver()) {
    return;
  }
  if (!SavedPositionIntersectsDisplay(g_state.persisted_window)) {
    std::fprintf(stderr,
                 "  [window-state] saved position is outside active "
                 "displays; using compositor placement\n");
    return;
  }
  if (!SDL_SetWindowPosition(g_state.sdl_window, g_state.persisted_window.x,
                             g_state.persisted_window.y)) {
    std::fprintf(stderr, "  [window-state] SDL position restore failed: %s\n",
                 SDL_GetError());
  }
}

void CaptureWindowState() {
  if (!g_state.state_persistence_active || g_state.sdl_window == nullptr) {
    return;
  }
  const SDL_WindowFlags flags = SDL_GetWindowFlags(g_state.sdl_window);
  g_state.persisted_window.fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
  g_state.persisted_window.maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
  if (g_state.persisted_window.fullscreen ||
      g_state.persisted_window.maximized) {
    return;
  }
  int width = 0;
  int height = 0;
  if (SDL_GetWindowSize(g_state.sdl_window, &width, &height) && width >= 160 &&
      height >= 120) {
    g_state.persisted_window.width = width;
    g_state.persisted_window.height = height;
  }
  if (!IsWaylandVideoDriver()) {
    int x = 0;
    int y = 0;
    if (SDL_GetWindowPosition(g_state.sdl_window, &x, &y)) {
      g_state.persisted_window.x = x;
      g_state.persisted_window.y = y;
      g_state.persisted_window.has_position = true;
    }
  }
}

bool PersistWindowState() {
  if (!g_state.state_persistence_active || g_window_state_path.empty()) {
    return true;
  }
  CaptureWindowState();
  const Status status =
      StoreWindowState(g_window_state_path, g_state.persisted_window);
  if (!status.ok()) {
    std::fprintf(stderr, "  [window-state] save failed: %s\n",
                 status.message().c_str());
    return false;
  }
  g_state.state_persistence_dirty = false;
  return true;
}

void MarkWindowStateDirty(bool flush_immediately) {
  if (!g_state.state_persistence_active) {
    return;
  }
  g_state.state_persistence_dirty = true;
  g_state.state_persistence_change_ticks_ns = SDL_GetTicksNS();
  if (flush_immediately) {
    (void)PersistWindowState();
  }
}

void MaybePersistWindowState() {
  constexpr uint64_t kSaveQuietPeriodNs = 250000000ULL;
  if (!g_state.state_persistence_dirty ||
      SDL_GetTicksNS() - g_state.state_persistence_change_ticks_ns <
          kSaveQuietPeriodNs) {
    return;
  }
  (void)PersistWindowState();
}

void MaybeSynchronizeRestoredFullscreenState() {
  if (!g_state.restored_fullscreen_sync_pending || !HasPresentedFrame()) {
    return;
  }
  const bool fullscreen =
      g_state.sdl_window != nullptr &&
      (SDL_GetWindowFlags(g_state.sdl_window) & SDL_WINDOW_FULLSCREEN) != 0;
  if (g_fullscreen_state_sync.Notify(fullscreen)) {
    g_state.restored_fullscreen_sync_pending = false;
  }
}

bool StringEquals(const char* value, const char* expected) {
  return value != nullptr && expected != nullptr &&
         std::strcmp(value, expected) == 0;
}

bool ParsePositiveInt(const char* name, int fallback, int* value) {
  if (value == nullptr) {
    return false;
  }
  const char* text = GetEnvNonEmpty(name);
  if (text == nullptr) {
    *value = fallback;
    return fallback > 0;
  }
  char* end = nullptr;
  errno = 0;
  const long parsed = std::strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || parsed <= 0 ||
      parsed > 16384) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

bool IsAutoGraphicsBackend(const char* backend) {
  return backend == nullptr || StringEquals(backend, "auto");
}

bool StartsWith(const char* value, const char* prefix) {
  if (value == nullptr || prefix == nullptr) {
    return false;
  }
  return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

bool FileExists(const char* path) {
  return path != nullptr && path[0] != '\0' && access(path, R_OK) == 0;
}

bool JoinExistingLibraryPath(const char* dir, const char* soname, char* out,
                             size_t out_size) {
  if (dir == nullptr || dir[0] == '\0' || soname == nullptr ||
      soname[0] == '\0' || out == nullptr || out_size == 0) {
    return false;
  }
  int written = std::snprintf(out, out_size, "%s/%s", dir, soname);
  if (written <= 0 || static_cast<size_t>(written) >= out_size) {
    return false;
  }
  return FileExists(out);
}

bool ShouldUseAngleBackend() {
  const char* backend = GetEnvNonEmpty("MOCKTAIL_GRAPHICS_BACKEND");
  return StartsWith(backend, "angle") ||
         GetEnvNonEmpty("MOCKTAIL_ANGLE_LIB_DIR") != nullptr;
}

bool ShouldUseNativeVulkanBackend() {
  const char* backend = GetEnvNonEmpty("MOCKTAIL_GRAPHICS_BACKEND");
  return StringEquals(backend, "vulkan") ||
         StringEquals(backend, "native-vulkan") ||
         StringEquals(backend, "direct-vulkan");
}

bool ShouldUseAngleVulkanBackend() {
  const char* backend = GetEnvNonEmpty("MOCKTAIL_GRAPHICS_BACKEND");
  return StringEquals(backend, "angle-vulkan") ||
         StringEquals(backend, "angle-swiftshader");
}

bool ShouldUseAngleSwiftShader() {
  return StringEquals(GetEnvNonEmpty("MOCKTAIL_GRAPHICS_BACKEND"),
                      "angle-swiftshader");
}

bool HasWaylandSession() {
  return GetEnvNonEmpty("WAYLAND_DISPLAY") != nullptr &&
         GetEnvNonEmpty("XDG_RUNTIME_DIR") != nullptr;
}

VideoDriverChoice ResolveConfiguredVideoDriverChoice() {
  VideoDriverPolicyInput input;
  input.has_explicit_sdl_driver =
      GetEnvNonEmpty("SDL_VIDEODRIVER") != nullptr ||
      GetEnvNonEmpty("SDL_VIDEO_DRIVER") != nullptr;
  input.force_wayland = IsEnabledEnv("MOCKTAIL_FORCE_WAYLAND");
  input.force_x11 = IsEnabledEnv("MOCKTAIL_FORCE_X11") ||
                    IsEnabledEnv("MOCKTAIL_ANGLE_FORCE_X11");
  input.prefer_wayland =
      !IsDisabledEnv("MOCKTAIL_PREFER_WAYLAND") &&
      !StringEquals(GetEnvNonEmpty("MOCKTAIL_FORCE_WAYLAND"), "0");
  input.has_wayland_session = HasWaylandSession();
  input.has_x11_display = GetEnvNonEmpty("DISPLAY") != nullptr;
  input.uses_direct_vulkan = ShouldUseNativeVulkanBackend();
  input.has_nvidia_kernel_driver =
      access("/proc/driver/nvidia/version", R_OK) == 0;
  return ResolveVideoDriverChoice(input);
}

bool ShouldShowWindowImmediately() {
  if (IsEnabledEnv("MOCKTAIL_HIDE_WINDOW_UNTIL_FIRST_SWAP")) {
    return false;
  }
  return IsEnabledEnv("MOCKTAIL_DEBUG_SHOW_WINDOW_BEFORE_FRAME");
}

const char* FindAngleLibrary(const char* explicit_env, const char* soname,
                             char* out, size_t out_size) {
  const char* explicit_path = GetEnvNonEmpty(explicit_env);
  if (explicit_path != nullptr) {
    return explicit_path;
  }

  const char* angle_dir = GetEnvNonEmpty("MOCKTAIL_ANGLE_LIB_DIR");
  if (JoinExistingLibraryPath(angle_dir, soname, out, out_size)) {
    return out;
  }

  if (!ShouldUseAngleBackend()) {
    return nullptr;
  }

  static const char* kAngleDirs[] = {
      "/usr/lib/chromium",
      "/usr/lib/chromium-browser",
      "/usr/lib/electron42",
      "/usr/lib/electron41",
      "/usr/lib/electron40",
      "/usr/lib/electron39",
      "/usr/lib/cef",
      "/opt/google/chrome",
      "/opt/google/chrome-beta",
      "/opt/google/chrome-unstable",
  };
  for (const char* dir : kAngleDirs) {
    if (JoinExistingLibraryPath(dir, soname, out, out_size)) {
      return out;
    }
  }
  return nullptr;
}

const char* FindInstalledAngleLibrary(const char* soname, char* out,
                                      size_t out_size) {
  static const char* kAngleDirs[] = {
      "/usr/lib/chromium",
      "/usr/lib/chromium-browser",
      "/usr/lib/electron42",
      "/usr/lib/electron41",
      "/usr/lib/electron40",
      "/usr/lib/electron39",
      "/usr/lib/cef",
      "/opt/google/chrome",
      "/opt/google/chrome-beta",
      "/opt/google/chrome-unstable",
  };
  for (const char* dir : kAngleDirs) {
    if (JoinExistingLibraryPath(dir, soname, out, out_size)) {
      return out;
    }
  }
  return nullptr;
}

bool HasInstalledAnglePair() {
  char egl_path[4096];
  char gles_path[4096];
  return FindInstalledAngleLibrary("libEGL.so", egl_path, sizeof(egl_path)) !=
             nullptr &&
         FindInstalledAngleLibrary("libGLESv2.so", gles_path,
                                   sizeof(gles_path)) != nullptr;
}

SDL_EGLAttrib* SDLCALL AnglePlatformAttributes(void* /*userdata*/) {
  SDL_EGLAttrib* attrs =
      static_cast<SDL_EGLAttrib*>(SDL_malloc(sizeof(SDL_EGLAttrib) * 7));
  if (attrs == nullptr) {
    return nullptr;
  }
  int i = 0;
  attrs[i++] = EGL_PLATFORM_ANGLE_TYPE_ANGLE_VAL;
  attrs[i++] = EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE_VAL;
  attrs[i++] = EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE_VAL;
  attrs[i++] = ShouldUseAngleSwiftShader()
                   ? EGL_PLATFORM_ANGLE_DEVICE_TYPE_SWIFTSHADER_ANGLE_VAL
                   : EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE_VAL;
  if (IsEnabledEnv("MOCKTAIL_ANGLE_DEBUG_LAYERS")) {
    attrs[i++] = EGL_PLATFORM_ANGLE_DEBUG_LAYERS_ENABLED_ANGLE_VAL;
    attrs[i++] = EGL_TRUE_VAL;
  }
  attrs[i++] = EGL_NONE_VAL;
  return attrs;
}

void ConfigureGraphicsBackendBeforeSDL() {
  g_preferred_egl_library[0] = '\0';
  g_preferred_gles_library[0] = '\0';

  SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");

  if (ShouldUseAngleVulkanBackend() || ShouldUseNativeVulkanBackend()) {
    // Mesa's implicit device selector can crash in the Android Vulkan path;
    // its supported opt-out is NODEVICE_SELECT=1.
    setenv("NODEVICE_SELECT", "1", 0);
    setenv("DISABLE_LAYER_MESA_ANTI_LAG", "1", 0);
  }

  const VideoDriverChoice video_driver_choice =
      ResolveConfiguredVideoDriverChoice();
  const char* video_driver = VideoDriverChoiceName(video_driver_choice);
  if (video_driver != nullptr) {
    // Preserve any non-empty user override.
    setenv("SDL_VIDEODRIVER", video_driver, 1);
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, video_driver);
  }
  if (video_driver_choice == VideoDriverChoice::kNvidiaDirectVulkanX11) {
    fprintf(stderr,
            "  [window] NVIDIA direct Vulkan on Wayland session: using "
            "X11/XWayland WSI; set SDL_VIDEODRIVER=wayland to override\n");
  }

  const char* egl_library = FindAngleLibrary(
      "MOCKTAIL_EGL_LIBRARY", "libEGL.so", g_preferred_egl_library,
      sizeof(g_preferred_egl_library));
  const char* gles_library = FindAngleLibrary(
      "MOCKTAIL_GLES_LIBRARY", "libGLESv2.so", g_preferred_gles_library,
      sizeof(g_preferred_gles_library));

  if (egl_library != nullptr) {
    setenv("MOCKTAIL_EGL_LIBRARY", egl_library, 0);
    SDL_SetHint(SDL_HINT_EGL_LIBRARY, egl_library);
  }
  if (gles_library != nullptr) {
    setenv("MOCKTAIL_GLES_LIBRARY", gles_library, 0);
    SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, gles_library);
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
  }

  if (ShouldUseAngleVulkanBackend()) {
    setenv("ANGLE_DEFAULT_PLATFORM", "vulkan", 0);
  }

  if (ShouldUseAngleBackend() || egl_library != nullptr ||
      gles_library != nullptr || WindowTraceEnabled()) {
    fprintf(stderr, "  [window] graphics backend=%s video=%s egl=%s gles=%s\n",
            GetEnvNonEmpty("MOCKTAIL_GRAPHICS_BACKEND")
                ? GetEnvNonEmpty("MOCKTAIL_GRAPHICS_BACKEND")
                : "system",
            GetEnvNonEmpty("SDL_VIDEODRIVER")
                ? GetEnvNonEmpty("SDL_VIDEODRIVER")
                : "(SDL default)",
            egl_library ? egl_library : "(system)",
            gles_library ? gles_library : "(system)");
  }
}

void ConfigureGraphicsBackendAfterSDLInit() {
  const char* gles_library = GetEnvNonEmpty("MOCKTAIL_GLES_LIBRARY");
  if (gles_library != nullptr) {
    if (!SDL_GL_LoadLibrary(gles_library)) {
      fprintf(stderr, "  [window] SDL_GL_LoadLibrary(%s) failed: %s\n",
              gles_library, SDL_GetError());
    } else if (WindowTraceEnabled() || ShouldUseAngleBackend()) {
      fprintf(stderr, "  [window] SDL_GL_LoadLibrary(%s) succeeded\n",
              gles_library);
    }
  }

  if (ShouldUseAngleVulkanBackend()) {
    SDL_GL_SetAttribute(SDL_GL_EGL_PLATFORM, EGL_PLATFORM_ANGLE_ANGLE_VAL);
    SDL_EGL_SetAttributeCallbacks(AnglePlatformAttributes, nullptr, nullptr,
                                  nullptr);
    fprintf(stderr,
            "  [window] ANGLE Vulkan EGL platform attributes enabled\n");
  }
}

void ShowWindowAccordingToStartupMode() {
  if (ShouldShowWindowImmediately()) {
    SDL_ShowWindow(g_state.sdl_window);
    SDL_RaiseWindow(g_state.sdl_window);
    g_state.visible = true;
  } else {
    fprintf(stderr, "  [window] hidden until first Roblox frame\n");
  }
}

void* QueryNativeWindowHandle() {
  SDL_PropertiesID props = SDL_GetWindowProperties(g_state.sdl_window);
  unsigned long xid = static_cast<unsigned long>(
      SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  if (xid != 0) {
    return reinterpret_cast<void*>(xid);
  }

  void* wl_egl_win = SDL_GetPointerProperty(
      props, SDL_PROP_WINDOW_WAYLAND_EGL_WINDOW_POINTER, nullptr);
  if (wl_egl_win != nullptr) {
    return wl_egl_win;
  }

  // Direct Vulkan uses SDL_Window as its stable native identity.
  return g_state.direct_vulkan ? g_state.sdl_window : nullptr;
}

void ResolveNativeWindowHandle() {
  g_state.native_window = QueryNativeWindowHandle();
  if (g_state.native_window == nullptr) {
    fprintf(stderr, "  [window] WARNING: no X11 XID or Wayland window found\n");
    return;
  }
  fprintf(stderr, "  [window] native window=%p\n", g_state.native_window);
}

bool ActivateWindowEventLifecycles() {
  g_state.display_refresh =
      platform::QuerySdlDisplayRefreshCapabilities(g_state.sdl_window);
  if (g_state.display_refresh.valid()) {
    fprintf(stderr, "  [window] display refresh current=%.3f Hz modes=%zu\n",
            g_state.display_refresh.current_hz,
            g_state.display_refresh.supported_hz.size());
  } else {
    fprintf(stderr, "  [window] display refresh unavailable\n");
  }
  int pixel_width = 0;
  int pixel_height = 0;
  SDL_GetWindowSizeInPixels(g_state.sdl_window, &pixel_width, &pixel_height);
  if (pixel_width <= 0 || pixel_height <= 0 ||
      g_state.native_window == nullptr) {
    fprintf(stderr,
            "  [window] cannot activate typed surface lifecycle "
            "(native=%p pixels=%dx%d)\n",
            g_state.native_window, pixel_width, pixel_height);
    return false;
  }
  const Status surface_status = g_window_surface_lifecycle.Activate(
      reinterpret_cast<uintptr_t>(g_state.native_window),
      static_cast<uint32_t>(pixel_width), static_cast<uint32_t>(pixel_height));
  if (!surface_status.ok()) {
    fprintf(stderr, "  [window] typed surface lifecycle failed: %s\n",
            surface_status.message().c_str());
    return false;
  }
  if (!g_platform_event_observer.Activate()) {
    fprintf(stderr, "  [window] platform event observer is already active\n");
    g_window_surface_lifecycle.Deactivate();
    return false;
  }
  WindowResizeReadinessConfig resize_config;
  resize_config.enabled =
      StringEquals(GetEnvNonEmpty("MOCKTAIL_RESIZE_READINESS"), "1");
  if (resize_config.enabled) {
    g_resize_readiness_gate = std::make_unique<WindowResizeReadinessGate>();
    int logical_width = 0;
    int logical_height = 0;
    if (!SDL_GetWindowSize(g_state.sdl_window, &logical_width,
                           &logical_height) ||
        logical_width <= 0 || logical_height <= 0) {
      fprintf(stderr,
              "  [resize] cannot determine initial SDL logical extent\n");
      g_platform_event_observer.Deactivate();
      g_window_surface_lifecycle.Deactivate();
      g_resize_readiness_gate.reset();
      return false;
    }
    const int fallback_width =
        logical_width >= 960 ? logical_width - 160 : logical_width + 160;
    const int fallback_height =
        logical_height >= 630 ? logical_height - 90 : logical_height + 90;
    int target_logical_width = 0;
    int target_logical_height = 0;
    if (!ParsePositiveInt("MOCKTAIL_RESIZE_WIDTH", fallback_width,
                          &target_logical_width) ||
        !ParsePositiveInt("MOCKTAIL_RESIZE_HEIGHT", fallback_height,
                          &target_logical_height)) {
      fprintf(
          stderr,
          "  [resize] invalid logical target; dimensions must be 1..16384\n");
      g_platform_event_observer.Deactivate();
      g_window_surface_lifecycle.Deactivate();
      g_resize_readiness_gate.reset();
      return false;
    }
    const uint64_t target_pixel_width =
        (static_cast<uint64_t>(target_logical_width) * pixel_width +
         static_cast<uint64_t>(logical_width) / 2) /
        static_cast<uint64_t>(logical_width);
    const uint64_t target_pixel_height =
        (static_cast<uint64_t>(target_logical_height) * pixel_height +
         static_cast<uint64_t>(logical_height) / 2) /
        static_cast<uint64_t>(logical_height);
    if (target_pixel_width > UINT32_MAX || target_pixel_height > UINT32_MAX) {
      fprintf(stderr, "  [resize] scaled target exceeds surface limits\n");
      g_platform_event_observer.Deactivate();
      g_window_surface_lifecycle.Deactivate();
      g_resize_readiness_gate.reset();
      return false;
    }
    resize_config.target_width = static_cast<uint32_t>(target_pixel_width);
    resize_config.target_height = static_cast<uint32_t>(target_pixel_height);
    const Status resize_status = g_resize_readiness_gate->Activate(
        resize_config, g_window_surface_lifecycle.Snapshot());
    if (!resize_status.ok()) {
      fprintf(stderr, "  [resize] readiness activation failed: %s\n",
              resize_status.message().c_str());
      g_platform_event_observer.Deactivate();
      g_window_surface_lifecycle.Deactivate();
      g_resize_readiness_gate.reset();
      return false;
    }
    fprintf(
        stderr,
        "  [resize] armed real compositor resize logical=%dx%d pixels=%ux%u\n",
        target_logical_width, target_logical_height, resize_config.target_width,
        resize_config.target_height);
    g_state.resize_target_logical_width = target_logical_width;
    g_state.resize_target_logical_height = target_logical_height;
  }
  g_state.width = pixel_width;
  g_state.height = pixel_height;
  g_pre_text_input_pump_gate.Activate();
  g_text_input_backend =
      std::make_unique<SdlTextInputBackend>(g_state.sdl_window);
  g_text_input_owner =
      std::make_unique<WindowTextInputOwner>(g_text_input_backend.get());
  g_pointer_capture_backend =
      std::make_unique<SdlPointerCaptureBackend>(g_state.sdl_window);
  g_pointer_capture_owner = std::make_unique<WindowPointerCaptureOwner>(
      g_pointer_capture_backend.get());
  fprintf(stderr,
          "  [input] SDL event pump target=%llu Hz raw-motion, no smoothing\n",
          static_cast<unsigned long long>(kProductionInputPumpHz));
  return true;
}

bool CreateSoftwareWaitingWindow(int width, int height, const char* title) {
  if (!StringEquals(GetEnvNonEmpty("MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS"),
                    "1")) {
    fprintf(stderr,
            "  [window] real EGL window setup failed; software waiting "
            "window is test-only and production is fail-closed\n");
    SDL_Quit();
    return false;
  }
  fprintf(stderr,
          "  [window] TEST-ONLY graphics stubs explicitly enabled; creating "
          "a non-rendering waiting window\n");
  SDL_WindowFlags window_flags = 0;
  if (!IsEnabledEnv("MOCKTAIL_DISABLE_HIGH_DPI")) {
    window_flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
  }
  if (g_state.state_persistence_active && g_state.persisted_window.fullscreen) {
    window_flags |= SDL_WINDOW_FULLSCREEN;
  } else if (g_state.state_persistence_active &&
             g_state.persisted_window.maximized) {
    window_flags |= SDL_WINDOW_MAXIMIZED;
  }
  if (!ShouldShowWindowImmediately()) {
    window_flags |= SDL_WINDOW_HIDDEN;
  }

  g_state.sdl_window = SDL_CreateWindow(title ? title : "Mocktail — Roblox",
                                        width, height, window_flags);
  if (g_state.sdl_window == nullptr) {
    fprintf(stderr, "  [window] SDL fallback window failed: %s\n",
            SDL_GetError());
    return false;
  }
  ApplyWindowIcon(g_state.sdl_window);
  ApplyMinimumWindowSize(g_state.sdl_window);
  ApplyRestoredWindowPosition();

  g_state.software_window = true;
  fprintf(stderr, "  [window] software SDL waiting window created (%dx%d)\n",
          width, height);
  ResolveNativeWindowHandle();
  fprintf(stderr,
          "  [window] software fallback waiting hidden; no Roblox frame "
          "presented yet\n");
  g_state.initialised = true;
  fprintf(stderr,
          "  [window] window initialisation complete (software fallback; "
          "EGL handles unavailable)\n");
  return true;
}

bool RetryWithAutoAngleFallback(int width, int height, const char* title,
                                const char* failed_step) {
  const bool disable_auto =
      IsEnabledEnv("MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK");
  const char* backend = GetEnvNonEmpty("MOCKTAIL_GRAPHICS_BACKEND");
  const bool explicit_backend = backend != nullptr &&
                                !StringEquals(backend, "system") &&
                                !IsAutoGraphicsBackend(backend);
  const bool has_angle_pair = HasInstalledAnglePair();
  if (g_auto_angle_retry_attempted || disable_auto || explicit_backend ||
      !has_angle_pair) {
    if (WindowTraceEnabled()) {
      fprintf(stderr,
              "  [window] auto ANGLE fallback skipped "
              "(attempted=%d disable=%d explicit_backend=%d has_angle=%d)\n",
              g_auto_angle_retry_attempted ? 1 : 0, disable_auto ? 1 : 0,
              explicit_backend ? 1 : 0, has_angle_pair ? 1 : 0);
    }
    return false;
  }
  g_auto_angle_retry_attempted = true;
  fprintf(stderr, "  [window] %s failed; retrying with ANGLE Vulkan backend\n",
          failed_step ? failed_step : "EGL window setup");
  SDL_Quit();
  setenv("MOCKTAIL_GRAPHICS_BACKEND", "angle-vulkan", 1);
  return Init(width, height, title);
}

Status ConfigureWindowStatePersistence(const std::filesystem::path& path) {
  if (g_state.initialised) {
    return Status::Error(
        StatusCode::kFailedPrecondition,
        "window state persistence must be configured before Init");
  }
  if (!path.is_absolute()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "window state path must be absolute");
  }
  g_window_state_path = path.lexically_normal();
  return Status::Ok();
}

static void ApplySwapInterval() {
  // Present pacing for the host GL window. Adaptive vsync (-1) prevents the
  // classic "frame missed vblank -> drop to half refresh" stutter that plagues
  // OpenGL games on Linux; fall back to hard vsync (1). Honour MOCKTAIL_VSYNC
  // (auto/on/off) from the graphics.vsync config option.
  const char* vsync = GetEnvNonEmpty("MOCKTAIL_VSYNC");
  int interval = 1;
  bool try_adaptive = true;
  if (vsync != nullptr) {
    if (StringEquals(vsync, "off")) {
      interval = 0;
      try_adaptive = false;
    } else if (StringEquals(vsync, "on")) {
      interval = 1;
      try_adaptive = false;
    }
  }
  if (try_adaptive) {
    if (SDL_GL_SetSwapInterval(-1) == 0) {
      if (WindowTraceEnabled()) {
        fprintf(stderr, "  [window] swap interval -> adaptive vsync (-1)\n");
      }
      return;
    }
    if (WindowTraceEnabled()) {
      fprintf(stderr,
              "  [window] adaptive vsync unsupported (%s); using hard vsync\n",
              SDL_GetError());
    }
  }
  if (SDL_GL_SetSwapInterval(interval) != 0) {
    fprintf(stderr, "  [window] SDL_GL_SetSwapInterval(%d) failed: %s\n",
            interval, SDL_GetError());
  } else if (WindowTraceEnabled()) {
    fprintf(stderr, "  [window] swap interval -> %d\n", interval);
  }
}

bool Init(int width, int height, const char* title) {
  if (g_state.initialised) {
    return true;
  }

  const Status metadata_status = platform::ConfigureSdlApplicationMetadata();
  if (!metadata_status.ok()) {
    std::fprintf(stderr, "  [window] SDL application metadata failed: %s\n",
                 metadata_status.message().c_str());
    return false;
  }

  // The Vulkan overlay renders preedit text; the platform IME owns candidates.
  SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");

  g_state.state_persistence_active =
      !g_window_state_path.empty() && !WindowStatePersistenceSuppressed();
  g_state.state_persistence_dirty = false;
  g_state.state_persistence_change_ticks_ns = 0;
  g_state.persisted_window = {};
  g_state.persisted_window.width = width;
  g_state.persisted_window.height = height;
  g_state.restored_fullscreen_sync_pending = false;
  if (g_state.state_persistence_active) {
    const WindowStateLoadResult restored = LoadWindowState(g_window_state_path);
    if (!restored) {
      std::fprintf(stderr, "  [window-state] restore ignored: %s\n",
                   restored.status.message().c_str());
    } else if (restored.found) {
      g_state.persisted_window = restored.state;
      width = restored.state.width;
      height = restored.state.height;
      g_state.restored_fullscreen_sync_pending = true;
      std::fprintf(stderr,
                   "  [window-state] restored logical=%dx%d fullscreen=%d "
                   "maximized=%d position=%s\n",
                   width, height, restored.state.fullscreen ? 1 : 0,
                   restored.state.maximized ? 1 : 0,
                   restored.state.has_position ? "saved" : "compositor");
    }
  }
  g_state.width = width;
  g_state.height = height;
  g_state.input_test_sequence_queued = false;
  g_state.fullscreen_readiness_requested = false;
  g_state.fullscreen_readiness_return_requested = false;
  g_state.fullscreen_readiness_present_baseline = 0;
  g_fullscreen_request_gate.Reset();
  g_fullscreen_menu_request_gate.Reset();
  g_real_swap_count.store(0, std::memory_order_relaxed);

  ConfigureGraphicsBackendBeforeSDL();

  if (StringEquals(GetEnvNonEmpty("MOCKTAIL_GRAPHICS_BACKEND"),
                   "direct-vulkan")) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      fprintf(stderr, "  [window] SDL_Init failed: %s\n", SDL_GetError());
      return false;
    }
    SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    if (!IsEnabledEnv("MOCKTAIL_DISABLE_HIGH_DPI")) {
      window_flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
    if (g_state.state_persistence_active &&
        g_state.persisted_window.fullscreen) {
      window_flags |= SDL_WINDOW_FULLSCREEN;
    } else if (g_state.state_persistence_active &&
               g_state.persisted_window.maximized) {
      window_flags |= SDL_WINDOW_MAXIMIZED;
    }
    if (!ShouldShowWindowImmediately()) {
      window_flags |= SDL_WINDOW_HIDDEN;
    }
    g_state.sdl_window = SDL_CreateWindow(title ? title : "Mocktail — Roblox",
                                          width, height, window_flags);
    if (g_state.sdl_window == nullptr) {
      fprintf(stderr, "  [window] SDL Vulkan window failed: %s\n",
              SDL_GetError());
      SDL_Quit();
      return false;
    }
    ApplyWindowIcon(g_state.sdl_window);
    ApplyMinimumWindowSize(g_state.sdl_window);
    ApplyRestoredWindowPosition();
    g_state.direct_vulkan = true;
    g_state.initialised = true;
    ResolveNativeWindowHandle();
    // Direct Vulkan needs a non-null opaque Android handle even without an
    // EGL-specific Wayland handle.
    if (g_state.native_window == nullptr) {
      g_state.native_window = g_state.sdl_window;
    }
    // Wayland reports undefined currentExtent until the window is mapped.
    if (!SDL_ShowWindow(g_state.sdl_window)) {
      fprintf(stderr, "  [window] SDL_ShowWindow failed: %s\n", SDL_GetError());
      SDL_DestroyWindow(g_state.sdl_window);
      g_state.sdl_window = nullptr;
      SDL_Quit();
      g_state.direct_vulkan = false;
      g_state.initialised = false;
      return false;
    }
    if (!SDL_SyncWindow(g_state.sdl_window)) {
      fprintf(stderr, "  [window] SDL_SyncWindow failed: %s\n", SDL_GetError());
      SDL_DestroyWindow(g_state.sdl_window);
      g_state.sdl_window = nullptr;
      SDL_Quit();
      g_state.direct_vulkan = false;
      g_state.initialised = false;
      return false;
    }
    int pixel_width = 0;
    int pixel_height = 0;
    SDL_GetWindowSizeInPixels(g_state.sdl_window, &pixel_width, &pixel_height);
    g_state.visible = true;
    fprintf(stderr,
            "  [window] direct Vulkan window mapped for WSI configure "
            "(%dx%d pixels)\n",
            pixel_width, pixel_height);
    fprintf(stderr, "  [window] SDL3 direct Vulkan window created (%dx%d)\n",
            width, height);
    if (!g_present_lifecycle.Activate()) {
      fprintf(stderr, "  [window] host present lifecycle is already active\n");
      g_present_lifecycle.Deactivate();
      SDL_DestroyWindow(g_state.sdl_window);
      g_state.sdl_window = nullptr;
      SDL_Quit();
      g_state.native_window = nullptr;
      g_state.direct_vulkan = false;
      g_state.initialised = false;
      return false;
    }
    if (!g_vulkan_surface_recovery_gate.Activate()) {
      fprintf(stderr,
              "  [window] direct Vulkan surface recovery is already active\n");
      g_vulkan_surface_recovery_gate.Deactivate();
      g_present_lifecycle.Deactivate();
      SDL_DestroyWindow(g_state.sdl_window);
      g_state.sdl_window = nullptr;
      SDL_Quit();
      g_state.native_window = nullptr;
      g_state.direct_vulkan = false;
      g_state.initialised = false;
      return false;
    }
    if (!g_vulkan_present_progress_gate.Activate()) {
      fprintf(stderr,
              "  [window] Vulkan present progress gate is already active\n");
      g_vulkan_present_progress_gate.Deactivate();
      g_vulkan_surface_recovery_gate.Deactivate();
      g_present_lifecycle.Deactivate();
      SDL_DestroyWindow(g_state.sdl_window);
      g_state.sdl_window = nullptr;
      SDL_Quit();
      g_state.native_window = nullptr;
      g_state.direct_vulkan = false;
      g_state.initialised = false;
      return false;
    }
    if (!ActivateWindowEventLifecycles()) {
      g_vulkan_present_progress_gate.Deactivate();
      g_vulkan_surface_recovery_gate.Deactivate();
      g_present_lifecycle.Deactivate();
      SDL_DestroyWindow(g_state.sdl_window);
      g_state.sdl_window = nullptr;
      SDL_Quit();
      g_state.native_window = nullptr;
      g_state.direct_vulkan = false;
      g_state.initialised = false;
      return false;
    }
    return true;
  }

  if (WindowTraceEnabled()) {
    fprintf(stderr, "  [window] Init requested width=%d height=%d title=%s\n",
            width, height, title ? title : "(null)");
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "  [window] SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }
  if (WindowTraceEnabled()) {
    fprintf(stderr, "  [window] SDL_Init(SDL_INIT_VIDEO) succeeded\n");
  }

  ConfigureGraphicsBackendAfterSDLInit();

  // SDL may recheck this hint during context creation.
  SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");

  // Requested GLES version comes from graphics.gles_version (exported as
  // MOCKTAIL_SYSTEM_GLES_VERSION). Defaults to 3.0 when unset.
  int gles_major = 3;
  int gles_minor = 0;
  if (const char* system_gles_version =
          GetEnvNonEmpty("MOCKTAIL_SYSTEM_GLES_VERSION")) {
    if (StringEquals(system_gles_version, "31") ||
        StringEquals(system_gles_version, "3.1")) {
      gles_minor = 1;
    } else if (StringEquals(system_gles_version, "32") ||
               StringEquals(system_gles_version, "3.2")) {
      gles_minor = 2;
    }
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, gles_major);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, gles_minor);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  SDL_WindowFlags window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
  if (!IsEnabledEnv("MOCKTAIL_DISABLE_HIGH_DPI")) {
    window_flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
  }
  if (g_state.state_persistence_active && g_state.persisted_window.fullscreen) {
    window_flags |= SDL_WINDOW_FULLSCREEN;
  } else if (g_state.state_persistence_active &&
             g_state.persisted_window.maximized) {
    window_flags |= SDL_WINDOW_MAXIMIZED;
  }
  if (!ShouldShowWindowImmediately()) {
    window_flags |= SDL_WINDOW_HIDDEN;
  }

  g_state.sdl_window = SDL_CreateWindow(title ? title : "Mocktail — Roblox",
                                        width, height, window_flags);
  if (!g_state.sdl_window) {
    fprintf(stderr, "  [window] SDL_CreateWindow failed: %s\n", SDL_GetError());
    if (RetryWithAutoAngleFallback(width, height, title, "SDL_CreateWindow")) {
      return true;
    }
    if (IsDisabledEnv("MOCKTAIL_SOFTWARE_WINDOW_FALLBACK")) {
      SDL_Quit();
      return false;
    }
    return CreateSoftwareWaitingWindow(width, height, title);
  }
  ApplyWindowIcon(g_state.sdl_window);
  ApplyMinimumWindowSize(g_state.sdl_window);
  ApplyRestoredWindowPosition();
  fprintf(stderr, "  [window] SDL3 window created (%dx%d)\n", width, height);
  ShowWindowAccordingToStartupMode();

  SDL_GLContext sdl_context = SDL_GL_CreateContext(g_state.sdl_window);
  if (!sdl_context && IsEnabledEnv("MOCKTAIL_ALLOW_GLES2_RESEARCH")) {
    // Current Roblox clients reject GLES2; this path supports older payloads.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    sdl_context = SDL_GL_CreateContext(g_state.sdl_window);
  }
  if (!sdl_context) {
    fprintf(stderr,
            "  [window] OpenGL ES 3.0 context required; "
            "SDL_GL_CreateContext failed: %s\n",
            SDL_GetError());
    SDL_DestroyWindow(g_state.sdl_window);
    g_state.sdl_window = nullptr;
    if (RetryWithAutoAngleFallback(width, height, title,
                                   "SDL_GL_CreateContext")) {
      return true;
    }
    if (IsDisabledEnv("MOCKTAIL_SOFTWARE_WINDOW_FALLBACK")) {
      SDL_Quit();
      return false;
    }
    return CreateSoftwareWaitingWindow(width, height, title);
  }

  if (!SDL_GL_MakeCurrent(g_state.sdl_window, sdl_context)) {
    fprintf(stderr, "  [window] SDL_GL_MakeCurrent failed: %s\n",
            SDL_GetError());
    SDL_GL_DestroyContext(sdl_context);
    SDL_DestroyWindow(g_state.sdl_window);
    g_state.sdl_window = nullptr;
    if (RetryWithAutoAngleFallback(width, height, title,
                                   "SDL_GL_MakeCurrent")) {
      return true;
    }
    if (IsDisabledEnv("MOCKTAIL_SOFTWARE_WINDOW_FALLBACK")) {
      SDL_Quit();
      return false;
    }
    return CreateSoftwareWaitingWindow(width, height, title);
  }
  if (WindowTraceEnabled()) {
    fprintf(stderr, "  [window] SDL_GL_MakeCurrent succeeded (context=%p)\n",
            static_cast<void*>(sdl_context));
  }
  int context_major = 0;
  int context_minor = 0;
  const bool context_version_available =
      SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &context_major) &&
      SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &context_minor);
  if (context_version_available) {
    fprintf(stderr, "  [window] OpenGL ES context version=%d.%d\n",
            context_major, context_minor);
  } else {
    fprintf(stderr, "  [window] OpenGL ES context version unavailable: %s\n",
            SDL_GetError());
  }
  if ((!context_version_available || context_major < 3) &&
      !IsEnabledEnv("MOCKTAIL_ALLOW_GLES2_RESEARCH")) {
    fprintf(stderr,
            "  [window] refusing unsupported OpenGL ES context; version "
            "3.0 or newer is required\n");
    SDL_GL_MakeCurrent(g_state.sdl_window, nullptr);
    SDL_GL_DestroyContext(sdl_context);
    SDL_DestroyWindow(g_state.sdl_window);
    g_state.sdl_window = nullptr;
    if (RetryWithAutoAngleFallback(width, height, title,
                                   "OpenGL ES version check")) {
      return true;
    }
    if (IsDisabledEnv("MOCKTAIL_SOFTWARE_WINDOW_FALLBACK")) {
      SDL_Quit();
      return false;
    }
    return CreateSoftwareWaitingWindow(width, height, title);
  }

  g_state.egl_display = SDL_EGL_GetCurrentDisplay();
  g_state.egl_config = SDL_EGL_GetCurrentConfig();
  g_state.egl_surface = SDL_EGL_GetWindowSurface(g_state.sdl_window);
  g_state.egl_context = static_cast<EGLContext>(sdl_context);

  if (!g_state.egl_display || !g_state.egl_surface || !g_state.egl_context) {
    fprintf(stderr,
            "  [window] SDL_EGL handles query failed (display=%p, surface=%p, "
            "context=%p)\n",
            g_state.egl_display, g_state.egl_surface, g_state.egl_context);
    SDL_GL_DestroyContext(sdl_context);
    SDL_DestroyWindow(g_state.sdl_window);
    g_state.sdl_window = nullptr;
    g_state.egl_display = nullptr;
    g_state.egl_config = nullptr;
    g_state.egl_surface = nullptr;
    g_state.egl_context = nullptr;
    if (RetryWithAutoAngleFallback(width, height, title,
                                   "SDL_EGL handle query")) {
      return true;
    }
    if (IsDisabledEnv("MOCKTAIL_SOFTWARE_WINDOW_FALLBACK")) {
      SDL_Quit();
      return false;
    }
    return CreateSoftwareWaitingWindow(width, height, title);
  }
  fprintf(stderr, "  [window] EGL context and surface initialized via SDL3\n");
  if (WindowTraceEnabled()) {
    fprintf(
        stderr,
        "  [window] EGL handles display=%p config=%p surface=%p context=%p\n",
        g_state.egl_display, g_state.egl_config, g_state.egl_surface,
        g_state.egl_context);
  }

  ApplySwapInterval();

  ResolveNativeWindowHandle();

  // The render thread binds the context after the main thread releases it.
  SDL_GL_MakeCurrent(g_state.sdl_window, nullptr);
  if (WindowTraceEnabled()) {
    fprintf(stderr, "  [window] SDL_GL_MakeCurrent(nullptr) on main thread\n");
  }

  g_state.initialised = true;
  if (!g_present_lifecycle.Activate()) {
    fprintf(stderr, "  [window] host present lifecycle is already active\n");
    Shutdown();
    return false;
  }
  if (!ActivateWindowEventLifecycles()) {
    Shutdown();
    return false;
  }
  fprintf(stderr,
          "  [window] window initialisation complete (context NOT yet "
          "current — call MakeCurrentOnThread from engine thread)\n");
  return true;
}

bool MakeCurrentOnThread() {
  if (!g_state.initialised || !g_state.egl_context) {
    if (WindowTraceEnabled()) {
      fprintf(stderr,
              "  [window] MakeCurrentOnThread skipped (initialised=%d "
              "context=%p)\n",
              g_state.initialised ? 1 : 0, g_state.egl_context);
    }
    return false;
  }
  if (!SDL_GL_MakeCurrent(g_state.sdl_window,
                          static_cast<SDL_GLContext>(g_state.egl_context))) {
    fprintf(stderr, "  [window] MakeCurrentOnThread failed: %s\n",
            SDL_GetError());
    return false;
  }
  fprintf(stderr, "  [window] EGL context now current on engine thread\n");
  return true;
}

bool ReleaseCurrentOnThread() {
  if (!g_state.initialised || !g_state.sdl_window) {
    return false;
  }
  if (g_state.egl_context == nullptr) {
    return true;
  }
  if (!SDL_GL_MakeCurrent(g_state.sdl_window, nullptr)) {
    fprintf(stderr, "  [window] ReleaseCurrentOnThread failed: %s\n",
            SDL_GetError());
    return false;
  }
  if (WindowTraceEnabled()) {
    fprintf(stderr, "  [window] EGL context released on current thread\n");
  }
  return true;
}

bool IsInitialised() { return g_state.initialised; }
bool HasPresentedFrame() { return g_real_swap_count.load() > 0; }

void RecordFirstPresentTime(int present_count) {
  if (present_count != 1) {
    return;
  }
  g_first_present_ticks_ns.store(SDL_GetTicksNS(), std::memory_order_release);
}
int GetWidth() { return g_state.width; }
int GetHeight() { return g_state.height; }

void* GetEGLDisplay() { return g_state.egl_display; }
void* GetEGLSurface() { return g_state.egl_surface; }
void* GetEGLContext() { return g_state.egl_context; }
void* GetEGLConfig() { return g_state.egl_config; }
void* GetNativeWindow() { return g_state.native_window; }
void* GetBackendWindow() { return g_state.sdl_window; }
bool UsesDirectVulkan() { return g_state.direct_vulkan; }
bool PollWindowSurfaceEvent(WindowSurfaceEvent* event) {
  return g_window_surface_lifecycle.Poll(event);
}
WindowSurfaceSnapshot GetWindowSurfaceSnapshot() {
  return g_window_surface_lifecycle.Snapshot();
}
Status RecordResizeReadinessSurfaceCommit(const WindowSurfaceEvent& event) {
  if (g_resize_readiness_gate == nullptr) {
    return Status::Ok();
  }
  const WindowResizeReadinessState before =
      g_resize_readiness_gate->Snapshot().state;
  const Status status =
      g_resize_readiness_gate->RecordCommittedSurfaceEvent(event);
  const WindowResizeReadinessSnapshot after =
      g_resize_readiness_gate->Snapshot();
  if (status.ok() && before == WindowResizeReadinessState::kResizeRequested &&
      after.state == WindowResizeReadinessState::kSurfaceCommitted) {
    fprintf(
        stderr,
        "  [resize] typed JNI surface commit generation=%llu pixels=%ux%u\n",
        static_cast<unsigned long long>(event.surface.generation),
        event.surface.width, event.surface.height);
  }
  return status;
}

Status StopResizeReadiness() {
  if (g_resize_readiness_gate == nullptr) {
    return Status::Ok();
  }
  const Status status = g_resize_readiness_gate->RecordStopped();
  if (status.ok() && g_resize_readiness_gate->Snapshot().state ==
                         WindowResizeReadinessState::kStopped) {
    fprintf(stderr, "  [resize] readiness completed: Stopped\n");
  }
  return status;
}

Status ResizeReadinessCompletionStatus() {
  return g_resize_readiness_gate == nullptr
             ? Status::Ok()
             : g_resize_readiness_gate->CompletionStatus();
}
WindowViewportSnapshot GetWindowViewportSnapshot() {
  WindowViewportSnapshot snapshot;
  if (g_state.sdl_window == nullptr) {
    return snapshot;
  }
  SDL_GetWindowSize(g_state.sdl_window, &snapshot.logical_width,
                    &snapshot.logical_height);
  SDL_GetWindowSizeInPixels(g_state.sdl_window, &snapshot.pixel_width,
                            &snapshot.pixel_height);
  return snapshot;
}

platform::DisplayRefreshCapabilities GetDisplayRefreshCapabilities() {
  platform::DisplayRefreshCapabilities caps = g_state.display_refresh;
  if (!UnthrottledPresentationRequested() || !caps.valid()) {
    return caps;
  }
  constexpr float kUnthrottledEngineHz = 240.0f;
  if (caps.current_hz >= kUnthrottledEngineHz) {
    return caps;
  }
  static bool logged = false;
  if (!logged) {
    logged = true;
    std::fprintf(stderr,
                 "  [window] advertising %.2f Hz to engine (display %.2f Hz, "
                 "unthrottled)\n",
                 static_cast<double>(kUnthrottledEngineHz),
                 static_cast<double>(caps.current_hz));
  }
  caps.supported_hz.push_back(kUnthrottledEngineHz);
  return platform::NormalizeDisplayRefreshCapabilities(
      kUnthrottledEngineHz, std::move(caps.supported_hz));
}
bool SetPlatformEventObserver(PlatformEventObserver observer, void* context) {
  return g_platform_event_observer.Register(observer, context);
}
void ClearPlatformEventObserver() { g_platform_event_observer.Clear(); }

bool SetMouseLockQueryCallback(MouseLockQueryCallback callback, void* context) {
  return g_pointer_capture_owner != nullptr &&
         g_pointer_capture_owner->RegisterQuery(callback, context);
}

void ClearMouseLockQueryCallback() {
  if (g_pointer_capture_owner != nullptr) {
    g_pointer_capture_owner->ClearQuery();
  }
}
bool SetPreTextInputPumpCallback(PreTextInputPumpCallback callback,
                                 void* context) {
  return g_pre_text_input_pump_gate.Register(callback, context);
}
void ClearPreTextInputPumpCallback() { g_pre_text_input_pump_gate.Clear(); }
void SetWindowTextInputOwnerEnabled(bool enabled) {
  if (g_text_input_owner != nullptr) {
    g_text_input_owner->SetEnabled(enabled);
  }
}
bool RequestShowTextInput(uint64_t generation, const TextInputArea& area,
                          const TextInputOptions& options) {
  return g_text_input_owner != nullptr &&
         g_text_input_owner->RequestShowTextInput(generation, area, options);
}
bool RequestHideTextInput(uint64_t generation) {
  return g_text_input_owner != nullptr &&
         g_text_input_owner->RequestHideTextInput(generation);
}
void* GetGLProcAddress(const char* name) {
  if (!g_state.initialised || name == nullptr || name[0] == '\0') {
    return nullptr;
  }
  return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
}

void ShowIfHidden() {
  if (!g_state.initialised || !g_state.sdl_window || g_state.visible ||
      IsDisabledEnv("MOCKTAIL_SHOW_WINDOW_ON_FIRST_SWAP")) {
    return;
  }
  SDL_ShowWindow(g_state.sdl_window);
  SDL_RaiseWindow(g_state.sdl_window);
  g_state.visible = true;
  fprintf(stderr, "  [window] shown on first Roblox frame\n");
}

extern "C" void mocktail_window_show_if_hidden() { ShowIfHidden(); }

extern "C" bool mocktail_window_swap_buffers() { return SwapBuffers(); }

extern "C" bool mocktail_window_make_current() { return MakeCurrentOnThread(); }

extern "C" bool mocktail_window_release_current() {
  return ReleaseCurrentOnThread();
}

extern "C" bool mocktail_window_has_presented_frame() {
  return HasPresentedFrame();
}

extern "C" int mocktail_window_width() { return GetWidth(); }

extern "C" int mocktail_window_height() { return GetHeight(); }

extern "C" void* mocktail_gl_proc_address(const char* name) {
  return GetGLProcAddress(name);
}

bool SwapBuffers() {
  auto present_scope = g_present_lifecycle.EnterPresent();
  if (!present_scope) {
    return false;
  }
  if (!g_state.initialised || !g_state.sdl_window) {
    if (WindowTraceEnabled()) {
      fprintf(
          stderr, "  [window] SwapBuffers skipped (initialised=%d window=%p)\n",
          g_state.initialised ? 1 : 0, static_cast<void*>(g_state.sdl_window));
    }
    return false;
  }
  if (g_state.egl_display == nullptr || g_state.egl_surface == nullptr ||
      g_state.egl_context == nullptr) {
    if (WindowTraceEnabled()) {
      fprintf(stderr,
              "  [window] SwapBuffers rejected: real EGL "
              "display/surface/context is unavailable\n");
    }
    return false;
  }

  if (!SDL_GL_SwapWindow(g_state.sdl_window)) {
    fprintf(stderr, "  [window] SDL_GL_SwapWindow failed: %s\n",
            SDL_GetError());
    return false;
  }

  const int swap_count =
      g_real_swap_count.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t now_ticks_ns = SDL_GetTicksNS();
  WindowResizeReadinessState resize_state_before =
      WindowResizeReadinessState::kDisabled;
  if (g_resize_readiness_gate != nullptr) {
    resize_state_before = g_resize_readiness_gate->Snapshot().state;
    const Status resize_status = g_resize_readiness_gate->RecordPresent(
        static_cast<uint64_t>(swap_count));
    if (!resize_status.ok()) {
      fprintf(stderr, "  [resize] present evidence rejected: %s\n",
              resize_status.message().c_str());
    } else if (resize_state_before ==
                   WindowResizeReadinessState::kSurfaceCommitted &&
               g_resize_readiness_gate->Snapshot().state ==
                   WindowResizeReadinessState::kPostRebindPresented) {
      fprintf(stderr,
              "  [resize] post-rebind host present backend=opengl frame=%d\n",
              swap_count);
    }
  }
  RecordFirstPresentTime(swap_count);
  present_scope.NotifyObserver(static_cast<uint64_t>(swap_count));
  ShowIfHidden();
  if (swap_count == 1) {
    fprintf(stderr, "  [window] first Roblox frame presented\n");
  }
  if (swap_count == 1 || swap_count % 240 == 0) {
    fprintf(stderr, "  [perf] present_sample frame=%d monotonic_ns=%llu\n",
            swap_count, static_cast<unsigned long long>(now_ticks_ns));
  }
  if (WindowTraceEnabled() && (swap_count <= 20 || swap_count % 60 == 0)) {
    fprintf(stderr,
            "  [window] SwapBuffers #%d window=%p surface=%p context=%p\n",
            swap_count, static_cast<void*>(g_state.sdl_window),
            g_state.egl_surface, g_state.egl_context);
  }
  return true;
}

void NoteVulkanPresent() {
  auto present_scope = g_present_lifecycle.EnterPresent();
  if (!present_scope) {
    return;
  }
  if (!g_state.initialised || !g_state.direct_vulkan ||
      g_state.sdl_window == nullptr) {
    return;
  }
  g_vulkan_surface_recovery_gate.NotifyPresent();
  const int present_count =
      g_real_swap_count.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t now_ticks_ns = SDL_GetTicksNS();
  g_vulkan_present_progress_gate.NotifyFramePresented(
      now_ticks_ns, static_cast<uint64_t>(present_count),
      SDL_GetCurrentThreadID());
  WindowResizeReadinessState resize_state_before =
      WindowResizeReadinessState::kDisabled;
  if (g_resize_readiness_gate != nullptr) {
    resize_state_before = g_resize_readiness_gate->Snapshot().state;
    const Status resize_status = g_resize_readiness_gate->RecordPresent(
        static_cast<uint64_t>(present_count));
    if (!resize_status.ok()) {
      fprintf(stderr, "  [resize] present evidence rejected: %s\n",
              resize_status.message().c_str());
    } else if (resize_state_before ==
                   WindowResizeReadinessState::kSurfaceCommitted &&
               g_resize_readiness_gate->Snapshot().state ==
                   WindowResizeReadinessState::kPostRebindPresented) {
      fprintf(stderr,
              "  [resize] post-rebind host present backend=vulkan frame=%d\n",
              present_count);
    }
  }
  RecordFirstPresentTime(present_count);
  present_scope.NotifyObserver(static_cast<uint64_t>(present_count));
  ShowIfHidden();
  if (present_count == 1) {
    fprintf(stderr, "  [window] first Roblox Vulkan frame presented\n");
  }
  if (present_count == 1 || present_count % 240 == 0) {
    fprintf(stderr, "  [perf] present_sample frame=%d monotonic_ns=%llu\n",
            present_count, static_cast<unsigned long long>(now_ticks_ns));
  }
  if (WindowTraceEnabled() &&
      (present_count <= 20 || present_count % 60 == 0)) {
    fprintf(stderr, "  [window] vkQueuePresentKHR #%d window=%p\n",
            present_count, static_cast<void*>(g_state.sdl_window));
  }
}

void NoteVulkanSurfaceOutOfDate() {
  g_vulkan_surface_recovery_gate.NotifyOutOfDate();
}

bool SetPresentObserver(PresentObserver observer, void* context) {
  return g_present_lifecycle.Register(observer, context);
}

void ClearPresentObserver() { g_present_lifecycle.Clear(); }

ScopedPresentObserver::~ScopedPresentObserver() { Reset(); }

bool ScopedPresentObserver::Register(PresentObserver observer, void* context) {
  if (registered_ || !SetPresentObserver(observer, context)) {
    return false;
  }
  registered_ = true;
  return true;
}

void ScopedPresentObserver::Reset() {
  if (!registered_) {
    return;
  }
  ClearPresentObserver();
  registered_ = false;
}

extern "C" void* mocktail_window_backend_window() { return GetBackendWindow(); }

extern "C" bool mocktail_window_uses_direct_vulkan() {
  return UsesDirectVulkan();
}

extern "C" void mocktail_window_note_vulkan_present() { NoteVulkanPresent(); }

extern "C" void mocktail_window_note_vulkan_host_present_begin() {
  g_vulkan_host_present_sequence =
      g_vulkan_present_progress_gate.NotifyHostPresentBegin(
          SDL_GetTicksNS(), SDL_GetCurrentThreadID());
}

extern "C" void mocktail_window_note_vulkan_host_present_end(
    std::int32_t /*result*/) {
  g_vulkan_present_progress_gate.NotifyHostPresentEnd(
      g_vulkan_host_present_sequence);
  g_vulkan_host_present_sequence = 0;
}

extern "C" std::uint64_t mocktail_window_note_vulkan_call_begin(
    const char* call_name) {
  return g_vulkan_present_progress_gate.NotifyCallBegin(
      call_name, SDL_GetTicksNS(), SDL_GetCurrentThreadID());
}

extern "C" void mocktail_window_note_vulkan_call_end(std::uint64_t sequence,
                                                     std::int32_t result) {
  g_vulkan_present_progress_gate.NotifyCallEnd(
      sequence, result, SDL_GetTicksNS(), SDL_GetCurrentThreadID());
}

extern "C" void mocktail_window_note_vulkan_surface_out_of_date() {
  NoteVulkanSurfaceOutOfDate();
}

bool ShouldIgnoreCloseRequest() {
  const char* explicit_ignore = std::getenv("MOCKTAIL_IGNORE_WINDOW_CLOSE");
  if (explicit_ignore && explicit_ignore[0] != '\0' &&
      std::strcmp(explicit_ignore, "0") != 0) {
    return true;
  }
  const char* keepalive_ms = std::getenv("MOCKTAIL_KEEPALIVE_MS");
  return keepalive_ms && keepalive_ms[0] != '\0' &&
         std::strcmp(keepalive_ms, "0") != 0;
}

void MaybeQueueInputReadinessSequence() {
  if (g_state.input_test_sequence_queued ||
      !StringEquals(GetEnvNonEmpty("MOCKTAIL_INPUT_TEST_CLICK"), "1") ||
      !HasPresentedFrame() || !g_platform_event_observer.HasObserver() ||
      g_state.sdl_window == nullptr) {
    return;
  }

  int logical_width = 0;
  int logical_height = 0;
  if (!SDL_GetWindowSize(g_state.sdl_window, &logical_width, &logical_height) ||
      logical_width <= 0 || logical_height <= 0) {
    return;
  }
  const float x = static_cast<float>(logical_width) * 0.5f;
  const float y = static_cast<float>(logical_height) * 0.5f;
  const SDL_WindowID window_id = SDL_GetWindowID(g_state.sdl_window);

  SDL_Event focus{};
  focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
  focus.window.windowID = window_id;
  // An opt-in readiness click may need a synthetic focus event.
  const bool needs_focus =
      (SDL_GetWindowFlags(g_state.sdl_window) & SDL_WINDOW_INPUT_FOCUS) == 0;

  SDL_Event motion{};
  motion.type = SDL_EVENT_MOUSE_MOTION;
  motion.motion.windowID = window_id;
  motion.motion.x = x;
  motion.motion.y = y;

  SDL_Event down{};
  down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  down.button.windowID = window_id;
  down.button.button = SDL_BUTTON_LEFT;
  down.button.down = true;
  down.button.clicks = 1;
  down.button.x = x;
  down.button.y = y;

  SDL_Event up = down;
  up.type = SDL_EVENT_MOUSE_BUTTON_UP;
  up.button.down = false;

  SDL_Event touch_down{};
  touch_down.type = SDL_EVENT_FINGER_DOWN;
  touch_down.tfinger.windowID = window_id;
  touch_down.tfinger.touchID = 1;
  touch_down.tfinger.fingerID = 1;
  touch_down.tfinger.x = 0.5f;
  touch_down.tfinger.y = 0.5f;
  touch_down.tfinger.pressure = 1.0f;

  SDL_Event touch_up = touch_down;
  touch_up.type = SDL_EVENT_FINGER_UP;
  touch_up.tfinger.pressure = 0.0f;

  g_state.input_test_sequence_queued = true;
  if ((needs_focus && !SDL_PushEvent(&focus)) || !SDL_PushEvent(&motion) ||
      !SDL_PushEvent(&down) || !SDL_PushEvent(&up) ||
      !SDL_PushEvent(&touch_down) || !SDL_PushEvent(&touch_up)) {
    fprintf(stderr,
            "  [input] failed to queue SDL readiness pointer sequence: %s\n",
            SDL_GetError());
    return;
  }
  fprintf(stderr, "  [input] SDL readiness pointer sequence queued\n");
}

void MaybeRequestResizeReadiness() {
  if (g_resize_readiness_gate == nullptr || g_state.sdl_window == nullptr) {
    return;
  }
  WindowResizeRequest request;
  if (!g_resize_readiness_gate->TakeResizeRequest(&request)) {
    return;
  }
  if (g_state.resize_target_logical_width <= 0 ||
      g_state.resize_target_logical_height <= 0 ||
      !SDL_SetWindowSize(g_state.sdl_window,
                         g_state.resize_target_logical_width,
                         g_state.resize_target_logical_height)) {
    fprintf(stderr, "  [resize] SDL_SetWindowSize failed: %s\n",
            SDL_GetError());
    return;
  }
  const WindowResizeReadinessSnapshot snapshot =
      g_resize_readiness_gate->Snapshot();
  fprintf(stderr,
          "  [resize] SDL compositor resize requested after present=%llu "
          "logical=%dx%d expected_pixels=%ux%u\n",
          static_cast<unsigned long long>(snapshot.first_present_serial),
          g_state.resize_target_logical_width,
          g_state.resize_target_logical_height, request.width, request.height);
}

bool FullscreenShortcutEnabled() {
  const char* value = GetEnvNonEmpty("MOCKTAIL_F11_FULLSCREEN");
  return value == nullptr || !StringEquals(value, "0");
}

bool RequestFullscreenState(bool fullscreen, const char* reason) {
  if (g_state.sdl_window == nullptr) {
    return false;
  }
  const bool current_fullscreen =
      (SDL_GetWindowFlags(g_state.sdl_window) & SDL_WINDOW_FULLSCREEN) != 0;
  if (current_fullscreen != fullscreen) {
    // Save the restore rectangle before SDL replaces it with monitor bounds.
    CaptureWindowState();
    if (!SDL_SetWindowFullscreen(g_state.sdl_window, fullscreen)) {
      fprintf(stderr, "  [fullscreen] SDL request failed: %s\n",
              SDL_GetError());
      return false;
    }
    fprintf(stderr, "  [fullscreen] %s requested state=%s\n",
            reason != nullptr ? reason : "toggle",
            fullscreen ? "fullscreen" : "windowed");
  }
  if (g_state.state_persistence_active) {
    g_state.persisted_window.fullscreen = fullscreen;
    g_state.state_persistence_dirty = true;
    g_state.state_persistence_change_ticks_ns = SDL_GetTicksNS();
    const Status store_status =
        StoreWindowState(g_window_state_path, g_state.persisted_window);
    if (!store_status.ok()) {
      std::fprintf(stderr, "  [window-state] fullscreen save failed: %s\n",
                   store_status.message().c_str());
    } else {
      g_state.state_persistence_dirty = false;
    }
  }
  if (!g_fullscreen_state_sync.Notify(fullscreen)) {
    fprintf(stderr,
            "  [fullscreen] Roblox settings state synchronization failed\n");
    return false;
  }
  return true;
}

bool RequestFullscreenToggle(const char* reason) {
  if (g_state.sdl_window == nullptr) {
    return false;
  }
  const bool fullscreen =
      (SDL_GetWindowFlags(g_state.sdl_window) & SDL_WINDOW_FULLSCREEN) != 0;
  return RequestFullscreenState(!fullscreen, reason);
}

void MaybeApplyAndroidFullscreenRequest() {
  bool fullscreen = false;
  if (!g_fullscreen_request_gate.Take(&fullscreen)) {
    return;
  }
  if (!RequestFullscreenState(fullscreen, "Roblox settings")) {
    fprintf(stderr, "  [fullscreen] rejected Android setWindowFlags request\n");
  }
}

void MaybeApplyRobloxFullscreenMenuRequest() {
  if (!g_fullscreen_menu_request_gate.Take(SDL_GetTicksNS())) {
    return;
  }
  if (!RequestFullscreenToggle("Roblox menu")) {
    fprintf(stderr, "  [fullscreen] rejected Roblox menu toggle request\n");
  }
}

bool HandleFullscreenShortcut(const SDL_Event& event) {
  if (!FullscreenShortcutEnabled() ||
      (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP) ||
      event.key.scancode != SDL_SCANCODE_F11) {
    return false;
  }
  if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
    (void)RequestFullscreenToggle("F11");
  }
  // The host owns both halves of this shortcut. Forwarding only the release
  // would leave Roblox's Android key state inconsistent.
  return true;
}

void MaybeRequestFullscreenReadiness() {
  const char* mode = GetEnvNonEmpty("MOCKTAIL_FULLSCREEN_READINESS");
  const bool single_transition = StringEquals(mode, "1");
  const bool round_trip = StringEquals(mode, "roundtrip");
  if ((!single_transition && !round_trip) || !HasPresentedFrame()) {
    return;
  }
  const int present_count = g_real_swap_count.load(std::memory_order_acquire);
  if (!g_state.fullscreen_readiness_requested) {
    g_state.fullscreen_readiness_requested = true;
    g_state.fullscreen_readiness_present_baseline = present_count;
    if (!RequestFullscreenToggle("readiness")) {
      g_state.quit_requested = true;
    }
    return;
  }
  constexpr int kRoundTripStablePresents = 60;
  const bool currently_fullscreen =
      g_state.sdl_window != nullptr &&
      (SDL_GetWindowFlags(g_state.sdl_window) & SDL_WINDOW_FULLSCREEN) != 0;
  if (!round_trip || g_state.fullscreen_readiness_return_requested ||
      !currently_fullscreen ||
      present_count - g_state.fullscreen_readiness_present_baseline <
          kRoundTripStablePresents) {
    return;
  }
  g_state.fullscreen_readiness_return_requested = true;
  if (!RequestFullscreenToggle("readiness-return")) {
    g_state.quit_requested = true;
  }
}

void MaybeRecoverVulkanSurface() {
  if (!g_state.direct_vulkan || !g_window_surface_lifecycle.active()) {
    return;
  }
  VulkanSurfaceRecoveryRequest request;
  if (!g_vulkan_surface_recovery_gate.TakeRebindRequest(SDL_GetTicksNS(),
                                                        &request)) {
    return;
  }
  const Status recreate_status = g_window_surface_lifecycle.Recreate();
  if (!recreate_status.ok()) {
    fprintf(stderr, "  [vulkan] surface recovery rejected: %s\n",
            recreate_status.message().c_str());
    g_state.quit_requested = true;
    return;
  }
  const WindowSurfaceSnapshot snapshot = g_window_surface_lifecycle.Snapshot();
  fprintf(stderr,
          "  [vulkan] swapchain out of date count=%llu coalesced=%llu; "
          "queued typed surface %s recreation generation=%llu pixels=%ux%u\n",
          static_cast<unsigned long long>(request.observed_error_count),
          static_cast<unsigned long long>(request.coalesced_error_count),
          request.retry ? "retry" : "recovery",
          static_cast<unsigned long long>(snapshot.generation), snapshot.width,
          snapshot.height);
}

void MaybeReportVulkanPresentStall() {
  if (!g_state.direct_vulkan || g_state.sdl_window == nullptr) {
    return;
  }
  const SDL_WindowFlags flags = SDL_GetWindowFlags(g_state.sdl_window);
  const bool window_can_present =
      (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0;
  VulkanPresentStallReport report;
  if (!g_vulkan_present_progress_gate.Poll(SDL_GetTicksNS(), window_can_present,
                                           &report)) {
    return;
  }
  const uint64_t now_ticks_ns = SDL_GetTicksNS();
  const uint64_t completed_ago_ms =
      report.last_completed_call_completed_ns != 0 &&
              now_ticks_ns >= report.last_completed_call_completed_ns
          ? (now_ticks_ns - report.last_completed_call_completed_ns) /
                1000000ULL
          : 0;
  const uint64_t completed_duration_ms =
      report.last_completed_call_completed_ns >=
                  report.last_completed_call_started_ns &&
              report.last_completed_call_started_ns != 0
          ? (report.last_completed_call_completed_ns -
             report.last_completed_call_started_ns) /
                1000000ULL
          : 0;
  const char* stage = "before_host_present";
  if (report.stage == VulkanPresentStallStage::kInsideVulkanCall) {
    stage = "inside_vulkan_call";
  } else if (report.stage == VulkanPresentStallStage::kInsideHostPresent) {
    stage = "inside_host_present";
  }
  fprintf(
      stderr,
      "  [vulkan] progress watchdog: stage=%s stalled_for=%llu ms "
      "frame=%llu host_presents=%llu render_tid=%llu primary_call=%s "
      "primary_id=%llu primary_tid=%llu last_snapshot=%s last_call=%s "
      "last_id=%llu last_tid=%llu last_result=%d last_duration=%llu ms "
      "completed_ago=%llu ms secondary_call=%s secondary_id=%llu "
      "secondary_tid=%llu secondary_for=%llu ms\n",
      stage, static_cast<unsigned long long>(report.elapsed_ns / 1000000ULL),
      static_cast<unsigned long long>(report.frame_count),
      static_cast<unsigned long long>(report.host_present_count),
      static_cast<unsigned long long>(report.last_present_thread_id),
      report.call_name != nullptr ? report.call_name : "none",
      static_cast<unsigned long long>(report.call_sequence),
      static_cast<unsigned long long>(report.thread_id),
      report.last_completed_call_snapshot_coherent ? "coherent" : "unavailable",
      report.last_completed_call_name != nullptr
          ? report.last_completed_call_name
          : "none",
      static_cast<unsigned long long>(report.last_completed_call_sequence),
      static_cast<unsigned long long>(report.last_completed_call_thread_id),
      report.last_completed_call_result,
      static_cast<unsigned long long>(completed_duration_ms),
      static_cast<unsigned long long>(completed_ago_ms),
      report.oldest_active_call_name != nullptr ? report.oldest_active_call_name
                                                : "none",
      static_cast<unsigned long long>(report.oldest_active_call_sequence),
      static_cast<unsigned long long>(report.oldest_active_call_thread_id),
      static_cast<unsigned long long>(report.oldest_active_call_elapsed_ns /
                                      1000000ULL));
}

bool PumpEvents() {
  MaybeSynchronizeRestoredFullscreenState();
  MaybeApplyRobloxFullscreenMenuRequest();
  MaybeApplyAndroidFullscreenRequest();
  MaybeRequestResizeReadiness();
  MaybeRequestFullscreenReadiness();
  MaybeQueueInputReadinessSequence();
  if (!g_pre_text_input_pump_gate.Invoke()) {
    fprintf(stderr, "  [input] main-thread text command drain failed\n");
  }
  if (g_text_input_owner != nullptr && !g_text_input_owner->Pump()) {
    fprintf(stderr, "  [input] SDL text-input state update failed\n");
  }
  if (g_pointer_capture_owner != nullptr &&
      !g_pointer_capture_owner->Pump(g_text_input_owner != nullptr &&
                                     g_text_input_owner->active())) {
    fprintf(stderr, "  [input] SDL pointer capture state update failed\n");
  }
  platform::PlatformEvent pending_motion{};
  bool has_pending_motion = false;

  auto flush_motion = [&]() {
    if (has_pending_motion) {
      g_platform_event_observer.Notify(pending_motion);
      has_pending_motion = false;
      pending_motion = {};
    }
  };

  SDL_Event event;
  // Pump + PeepEvents drains without SDL_WaitEventTimeoutNS. Unthrottled
  // ticks only ingest OS events every 4ms so the leader does not contend
  // with the render thread on wl_display thousands of times per second.
  constexpr uint64_t kUnthrottledPumpIntervalNs = 4000000ULL;
  static uint64_t last_os_pump_ns = 0;
  const uint64_t now_ns = SDL_GetTicksNS();
  if (!UnthrottledPresentationRequested() || last_os_pump_ns == 0 ||
      now_ns - last_os_pump_ns >= kUnthrottledPumpIntervalNs) {
    SDL_PumpEvents();
    last_os_pump_ns = now_ns;
  }
  while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST,
                        SDL_EVENT_LAST) > 0) {
    if (event.type != SDL_EVENT_MOUSE_MOTION) {
      flush_motion();
    }
    if (SdlEventTraceEnabled()) {
      fprintf(stderr, "  [window] SDL event type=%u\n", event.type);
    }
    const bool is_window_event = event.type >= SDL_EVENT_WINDOW_FIRST &&
                                 event.type <= SDL_EVENT_WINDOW_LAST;
    if (event.type == SDL_EVENT_QUIT) {
      if (!ShouldIgnoreCloseRequest()) {
        g_state.quit_requested = true;
      }
    }
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
      if (!ShouldIgnoreCloseRequest()) {
        g_state.quit_requested = true;
      }
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST &&
        g_text_input_owner != nullptr) {
      g_text_input_owner->OnFocusLost();
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST &&
        g_pointer_capture_owner != nullptr &&
        !g_pointer_capture_owner->OnFocusLost()) {
      fprintf(stderr, "  [input] failed to release pointer after focus loss\n");
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED &&
        g_text_input_owner != nullptr) {
      g_text_input_owner->OnFocusGained();
    }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED &&
        g_pointer_capture_owner != nullptr &&
        !g_pointer_capture_owner->OnFocusGained(
            g_text_input_owner != nullptr && g_text_input_owner->active())) {
      fprintf(stderr, "  [input] failed to restore pointer after focus gain\n");
    }

    const bool geometry_changed = event.type == SDL_EVENT_WINDOW_MOVED ||
                                  event.type == SDL_EVENT_WINDOW_RESIZED ||
                                  event.type == SDL_EVENT_WINDOW_MAXIMIZED ||
                                  event.type == SDL_EVENT_WINDOW_RESTORED;
    const bool fullscreen_changed =
        event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
        event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN;
    if (geometry_changed || fullscreen_changed) {
      CaptureWindowState();
      if (fullscreen_changed) {
        g_state.persisted_window.fullscreen =
            event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
      }
      MarkWindowStateDirty(fullscreen_changed);
    }

    const bool fullscreen_shortcut = HandleFullscreenShortcut(event);
    platform::PlatformEvent platform_event;
    const bool converted =
        platform::ConvertSdlEvent(g_state.sdl_window, event, &platform_event);
    if (is_window_event && g_window_surface_lifecycle.active()) {
      int pixel_width = 0;
      int pixel_height = 0;
      SDL_GetWindowSizeInPixels(g_state.sdl_window, &pixel_width,
                                &pixel_height);
      void* observed_native_window = QueryNativeWindowHandle();
      const Status surface_status = g_window_surface_lifecycle.Observe(
          reinterpret_cast<uintptr_t>(observed_native_window),
          pixel_width > 0 ? static_cast<uint32_t>(pixel_width) : 0,
          pixel_height > 0 ? static_cast<uint32_t>(pixel_height) : 0);
      if (!surface_status.ok()) {
        fprintf(stderr, "  [window] surface event rejected: %s\n",
                surface_status.message().c_str());
        g_state.quit_requested = true;
      } else {
        const bool fullscreen_transition =
            event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
            event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN;
        if (fullscreen_transition && pixel_width > 0 && pixel_height > 0) {
          const Status refresh_status = g_window_surface_lifecycle.Refresh();
          if (!refresh_status.ok()) {
            fprintf(stderr, "  [fullscreen] surface refresh rejected: %s\n",
                    refresh_status.message().c_str());
            g_state.quit_requested = true;
          } else {
            fprintf(stderr,
                    "  [fullscreen] SDL transition committed state=%s "
                    "pixels=%dx%d\n",
                    event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN
                        ? "fullscreen"
                        : "windowed",
                    pixel_width, pixel_height);
          }
        }
        g_state.native_window = observed_native_window;
        if (pixel_width > 0 && pixel_height > 0) {
          g_state.width = pixel_width;
          g_state.height = pixel_height;
        }
        if ((event.type == SDL_EVENT_WINDOW_RESIZED ||
             event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
             event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
             event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) &&
            g_text_input_owner != nullptr &&
            !g_text_input_owner->OnViewportChanged()) {
          fprintf(
              stderr,
              "  [input] SDL text-input area reapply failed after resize\n");
        }
      }
    }
    if (converted && !fullscreen_shortcut) {
      if (event.type == SDL_EVENT_MOUSE_MOTION) {
        if (!has_pending_motion) {
          pending_motion = platform_event;
          has_pending_motion = true;
        } else {
          auto* cur =
              std::get_if<platform::MouseMotionEvent>(&pending_motion.payload);
          const auto* nxt =
              std::get_if<platform::MouseMotionEvent>(&platform_event.payload);
          if (cur != nullptr && nxt != nullptr) {
            cur->delta_x += nxt->delta_x;
            cur->delta_y += nxt->delta_y;
            cur->x = nxt->x;
            cur->y = nxt->y;
            cur->buttons = nxt->buttons;
            pending_motion.timestamp_ns = platform_event.timestamp_ns;
          } else {
            flush_motion();
            pending_motion = platform_event;
            has_pending_motion = true;
          }
        }
      } else {
        g_platform_event_observer.Notify(platform_event);
      }
    }
    if (converted && !fullscreen_shortcut && event.type == SDL_EVENT_KEY_DOWN &&
        !event.key.repeat &&
        (event.key.scancode == SDL_SCANCODE_LSHIFT ||
         event.key.scancode == SDL_SCANCODE_RSHIFT) &&
        g_pointer_capture_owner != nullptr &&
        !g_pointer_capture_owner->OnShiftKeyPressed(
            g_text_input_owner != nullptr && g_text_input_owner->active())) {
      fprintf(stderr,
              "  [input] SDL shift-lock pointer capture update failed\n");
    }
    if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
         event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        event.button.button == SDL_BUTTON_RIGHT &&
        g_pointer_capture_owner != nullptr &&
        !g_pointer_capture_owner->OnRightButton(
            event.button.down,
            g_text_input_owner != nullptr && g_text_input_owner->active())) {
      fprintf(stderr,
              "  [input] SDL right-button pointer capture update failed\n");
    }
  }
  flush_motion();
  MaybePersistWindowState();
  MaybeRecoverVulkanSurface();
  MaybeReportVulkanPresentStall();
  static const char* auto_exit_value =
      GetEnvNonEmpty("MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS");
  if (!g_state.quit_requested && auto_exit_value != nullptr) {
    char* end = nullptr;
    const long delay_ms = std::strtol(auto_exit_value, &end, 10);
    const uint64_t first_present_ns =
        g_first_present_ticks_ns.load(std::memory_order_acquire);
    if (end != auto_exit_value && delay_ms > 0 && first_present_ns != 0) {
      const uint64_t elapsed_ns = SDL_GetTicksNS() - first_present_ns;
      if (elapsed_ns >= static_cast<uint64_t>(delay_ms) * 1000000ULL) {
        fprintf(stderr,
                "  [window] automatic exit requested %ld ms after first "
                "present\n",
                delay_ms);
        g_state.quit_requested = true;
      }
    }
  }
  return !g_state.quit_requested;
}

bool UnthrottledPresentationRequested() {
  static const bool unthrottled = [] {
    const char* vsync = GetEnvNonEmpty("MOCKTAIL_VSYNC");
    if (vsync != nullptr &&
        (std::strcmp(vsync, "off") == 0 || std::strcmp(vsync, "0") == 0)) {
      return true;
    }
    const char* frame_rate = GetEnvNonEmpty("MOCKTAIL_FRAME_RATE_LIMIT");
    return frame_rate != nullptr && std::strcmp(frame_rate, "unlimited") == 0;
  }();
  return unthrottled;
}

uint64_t PaceInputPump() {
  if (!g_state.initialised) {
    return 0;
  }
  if (UnthrottledPresentationRequested()) {
    return 0;
  }
  if (__builtin_expect(SDL_HasEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST), 0)) {
    return 0;
  }
  const uint64_t delay_ns =
      g_input_pump_pacer.DelayBeforeNextPump(SDL_GetTicksNS());
  if (delay_ns != 0) {
    SDL_DelayPrecise(delay_ns);
  }
  return delay_ns;
}

void Shutdown() {
  g_input_pump_pacer.Reset();
  g_pre_text_input_pump_gate.Deactivate();
  g_platform_event_observer.Deactivate();
  g_vulkan_present_progress_gate.Deactivate();
  g_vulkan_surface_recovery_gate.Deactivate();
  g_window_surface_lifecycle.Deactivate();
  g_present_lifecycle.Deactivate();
  if (!g_state.initialised) {
    g_fullscreen_request_gate.Reset();
    g_fullscreen_menu_request_gate.Reset();
    g_pointer_capture_owner.reset();
    g_pointer_capture_backend.reset();
    g_text_input_owner.reset();
    g_text_input_backend.reset();
    return;
  }
  if (WindowTraceEnabled()) {
    fprintf(stderr, "  [window] Shutdown window=%p context=%p surface=%p\n",
            static_cast<void*>(g_state.sdl_window), g_state.egl_context,
            g_state.egl_surface);
  }
  (void)PersistWindowState();
  if (g_text_input_owner != nullptr) {
    g_text_input_owner->Shutdown();
  }
  if (g_pointer_capture_owner != nullptr &&
      !g_pointer_capture_owner->Shutdown()) {
    fprintf(stderr, "  [input] SDL pointer capture shutdown failed\n");
  }
  g_pointer_capture_owner.reset();
  g_pointer_capture_backend.reset();
  g_text_input_owner.reset();
  g_text_input_backend.reset();
  if (!g_state.direct_vulkan) {
    SDL_GL_MakeCurrent(g_state.sdl_window, nullptr);
  }
  if (!g_state.direct_vulkan && g_state.egl_context) {
    SDL_GL_DestroyContext(static_cast<SDL_GLContext>(g_state.egl_context));
    g_state.egl_context = nullptr;
  }
  if (g_state.sdl_window) {
    SDL_DestroyWindow(g_state.sdl_window);
    g_state.sdl_window = nullptr;
  }
  SDL_Quit();
  g_state.initialised = false;
  g_state.native_window = nullptr;
  g_state.direct_vulkan = false;
  g_state.display_refresh = {};
  g_state.input_test_sequence_queued = false;
  g_state.resize_target_logical_width = 0;
  g_state.resize_target_logical_height = 0;
  g_state.fullscreen_readiness_requested = false;
  g_state.fullscreen_readiness_return_requested = false;
  g_state.restored_fullscreen_sync_pending = false;
  g_state.fullscreen_readiness_present_baseline = 0;
  g_state.state_persistence_active = false;
  g_state.state_persistence_dirty = false;
  g_state.state_persistence_change_ticks_ns = 0;
  g_state.persisted_window = {};
  g_fullscreen_request_gate.Reset();
  g_fullscreen_menu_request_gate.Reset();
  g_real_swap_count.store(0, std::memory_order_relaxed);
  g_first_present_ticks_ns.store(0, std::memory_order_relaxed);
  g_resize_readiness_gate.reset();
}

bool RequestFullscreenFromAndroidWindowFlags(int flags, int mask) {
  return g_fullscreen_request_gate.RequestFromAndroidFlags(flags, mask);
}

bool RequestFullscreenFromRobloxMenuLog(const char* tag, const char* message) {
  return g_fullscreen_menu_request_gate.RequestFromAndroidLog(tag, message,
                                                              SDL_GetTicksNS());
}

bool SetFullscreenStateSyncCallback(FullscreenStateSyncCallback callback,
                                    void* context) {
  return g_fullscreen_state_sync.Register(callback, context);
}

void ClearFullscreenStateSyncCallback() { g_fullscreen_state_sync.Clear(); }

}  // namespace window
}  // namespace mocktail
