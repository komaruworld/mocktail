#include <SDL3/SDL_video.h>
#include <arpa/inet.h>
#include <asm/prctl.h>
#include <dlfcn.h>
#include <elf.h>
#include <execinfo.h>
#include <fcntl.h>
#include <jni.h>
#include <link.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "compat/bionic_abi_exports.h"
#include "compat/bionic_prctl_runtime.h"
#include "compat/bionic_pthread_create_runtime.h"
#include "compat/bionic_socket_runtime.h"
#include "compat/build_profile.h"
#include "compat/elf_build_id.h"
#include "compat/host_abi_experiment.h"
#include "compat/host_abi_profile.h"
#include "compat/host_allocator_bridge.h"
#include "jnivm/jnivm.h"
#include "legacy/bionic_runtime_wrappers.h"
#include "legacy/engine_startup_types.h"
#include "legacy/legacy_runtime.h"
#include "legacy/memory_inspection.h"
#include "legacy/rbxm_diagnostics.h"
#include "legacy/runtime_adapters.h"
#include "legacy/runtime_environment.h"
#include "legacy/runtime_paths.h"
#include "legacy/symbol_resolver.h"
#include "libc_shim/libc_shim.h"
#include "linker/linker.h"
#include "mocktail/graphics/bionic_egl_bridge.h"
#include "mocktail/graphics/system_egl_probe.h"
#include "runtime/discord_rpc.h"
#include "runtime/environment.h"
#include "runtime/jnivm_platform_web_callbacks.h"
#include "runtime/owned_pthread.h"
#include "runtime/platform_cache_migration.h"
#include "runtime/roblox_app_lifecycle.h"
#include "runtime/roblox_capability_resolver.h"
#include "runtime/roblox_experience_composition.h"
#include "runtime/roblox_game_session_native_adapter.h"
#include "runtime/roblox_platform_web_symbols.h"
#include "runtime/roblox_text_input_jni_bridge.h"
#include "runtime/roblox_window_input_runtime.h"
#include "runtime/runtime_config.h"
#include "runtime/runtime_paths.h"
#include "services/client_settings_service.h"
#include "services/http_client.h"
#include "window/window.h"
#include "window/window_game_surface_bridge.h"

#ifdef MOCKTAIL_USE_BIONIC_LINKER
#include <mcpelauncher/linker.h>
#endif

#ifndef MOCKTAIL_DEFAULT_COMPATIBILITY_MANIFEST
#define MOCKTAIL_DEFAULT_COMPATIBILITY_MANIFEST \
  "config/roblox_compatibility.json"
#endif

#include "legacy/legacy_runtime_core.h"
#include "legacy/legacy_runtime_impl.h"
#include "legacy/stage6_patches.h"

namespace mocktail::legacy::internal {

int RunLegacy(const runtime::CommandLineOptions& options,
              RuntimeDependencies dependencies) {
  const bool user_overrode_start_lua_app_dm =
      HasEnvValue("MOCKTAIL_START_LUA_APP_DM");
  const bool user_overrode_start_lua_step =
      HasEnvValue("MOCKTAIL_STEP_START_LUA_APP_DM");
  const bool user_overrode_start_app_step =
      HasEnvValue("MOCKTAIL_STEP_START_APP_WITH_PARAMS");
  const bool user_overrode_call_start_app =
      HasEnvValue("MOCKTAIL_CALL_REAL_APP_BRIDGE_START");
  const bool user_overrode_call_update_surface =
      HasEnvValue("MOCKTAIL_CALL_REAL_APP_BRIDGE_UPDATE_SURFACE");
  const bool user_overrode_main_thread_message_pump =
      HasEnvValue("MOCKTAIL_MAIN_THREAD_MESSAGE_PUMP");

  std::cout << "======================================================\n"
            << "  Mocktail Roblox Compatibility Layer\n"
            << "======================================================\n"
            << std::flush;

  ApplyRuntimeDefaults();
  const mocktail::runtime::ProcessEnvironment process_environment;
  const mocktail::runtime::RuntimeConfig runtime_config =
      mocktail::runtime::RuntimeConfig::FromEnvironment(process_environment);
  mocktail::runtime::DiscordRpcSession discord_rpc(
      runtime_config.discord_rpc());
  if (runtime_config.discord_rpc().enabled) {
    std::string discord_rpc_detail;
    if (discord_rpc.Start(&discord_rpc_detail)) {
      std::cout << "  [discord-rpc] enabled; browsing activity queued\n";
    } else {
      std::cerr << "  [discord-rpc] unavailable: " << discord_rpc_detail
                << "; continuing without Rich Presence\n";
    }
  }
  const mocktail::runtime::InputCapabilityConfig& input_capabilities =
      runtime_config.input_capabilities();
  if (!runtime_config.frame_rate().valid()) {
    std::cerr << "[FATAL] Invalid MOCKTAIL_FRAME_RATE_LIMIT; expected "
                 "display, 30, 60, 120, 144, 240, or unlimited\n";
    return EXIT_FAILURE;
  }
  if (!runtime_config.theme_mode_valid()) {
    std::cerr << "[FATAL] Invalid MOCKTAIL_THEME; expected system, light, or "
                 "dark\n";
    return EXIT_FAILURE;
  }
  setenv("MOCKTAIL_TOUCH_ENABLED_INTERNAL",
         input_capabilities.touch_enabled ? "1" : "0", 1);
  setenv("MOCKTAIL_MOUSE_ENABLED_INTERNAL",
         input_capabilities.mouse_enabled ? "1" : "0", 1);
  setenv("MOCKTAIL_KEYBOARD_ENABLED_INTERNAL",
         input_capabilities.keyboard_enabled ? "1" : "0", 1);
  if (runtime_config.has_unsafe_detached_thread_overrides()) {
    std::cerr << "[FATAL] Unsupported detached legacy thread overrides:\n";
    for (const std::string& name :
         runtime_config.unsafe_detached_thread_overrides()) {
      std::cerr << "  - " << name << '\n';
    }
    std::cerr << "  Supported runtime requires synchronous or owned worker "
                 "execution.\n";
    return EXIT_FAILURE;
  }
  const runtime::ScopedRobloxCredentialBinding credential_binding(
      dependencies.jni_vm().get(), dependencies.roblox_credential());
  if (!credential_binding.bound()) {
    std::cerr << "[FATAL] Typed Pseudo-JVM credential provider is missing\n";
    return EXIT_FAILURE;
  }
  const std::string library_path =
      runtime_config.roblox_library_path().string();

  const mocktail::compat::BuildIdResult build_id_result =
      mocktail::compat::ReadElfBuildId(library_path);
  if (!build_id_result) {
    std::cerr << "[FATAL] Cannot identify Roblox library '" << library_path
              << "': " << build_id_result.error << '\n';
    return EXIT_FAILURE;
  }

  const char* manifest_override =
      std::getenv("MOCKTAIL_COMPATIBILITY_MANIFEST");
  const std::string compatibility_manifest =
      manifest_override != nullptr && manifest_override[0] != '\0'
          ? manifest_override
          : MOCKTAIL_DEFAULT_COMPATIBILITY_MANIFEST;
  const mocktail::compat::ProfileLookupResult profile_result =
      mocktail::compat::FindBuildProfile(compatibility_manifest,
                                         build_id_result.build_id);
  if (!profile_result) {
    std::cerr << "[FATAL] Cannot read Roblox compatibility profile: "
              << profile_result.error << '\n';
    return EXIT_FAILURE;
  }
  if (!profile_result.profile.has_value()) {
    std::cerr << "[FATAL] Unsupported Roblox Build ID "
              << build_id_result.build_id << ".\n"
              << "  Add and validate a profile in " << compatibility_manifest
              << " before starting native code.\n";
    return EXIT_FAILURE;
  }

  const mocktail::compat::BuildProfile& build_profile = *profile_result.profile;
  const mocktail::compat::HostAbiProfile* host_abi_profile =
      mocktail::compat::FindHostAbiProfile(build_profile.elf_build_id);
  const bool experiment_allowed =
      build_profile.default_allowed || options.allow_unverified_build;
  SetLegacyBinaryPatchesAllowed(build_profile.allow_legacy_binary_patches);
  const bool allow_host_abi_bridges =
      build_profile.allow_host_abi_bridges && experiment_allowed &&
      host_abi_profile != nullptr && host_abi_profile->bridge_entry_count > 0;
  const bool allow_host_constructor_replay =
      build_profile.allow_host_constructor_replay && allow_host_abi_bridges &&
      host_abi_profile->init_array_offset != 0 &&
      host_abi_profile->HasValidConstructorRanges();
  g_allow_host_abi_bridges.store(allow_host_abi_bridges,
                                 std::memory_order_release);
  g_allow_host_constructor_replay.store(allow_host_constructor_replay,
                                        std::memory_order_release);
  g_active_host_abi_profile.store(host_abi_profile, std::memory_order_release);
  g_host_abi_install_attempted = false;
  g_host_abi_install_result = {};
  SetEnvDefault("MOCKTAIL_ROBLOX_VERSION", build_profile.version_name.c_str());
  const std::string roblox_version_code =
      std::to_string(build_profile.version_code);
  SetEnvDefault("MOCKTAIL_ROBLOX_VERSION_CODE", roblox_version_code.c_str());
  const std::string default_user_agent =
      "Roblox/" + build_profile.version_name + " (Linux; Android 33; Mocktail)";
  SetEnvDefault("MOCKTAIL_USER_AGENT", default_user_agent.c_str());
  mocktail::compat::SetLegacyBionicDiagnosticsEnabled(
      build_profile.allow_legacy_binary_patches);
  std::cout << "  [compat] Roblox " << build_profile.version_name
            << " Build ID " << build_profile.elf_build_id << " ("
            << mocktail::compat::BuildStatusName(build_profile.status) << ")\n";
  std::cout << "  [compat] legacy binary patches: "
            << (build_profile.allow_legacy_binary_patches ? "enabled"
                                                          : "disabled")
            << '\n';
  std::cout << "  [compat] Build-ID host ABI profile: "
            << (allow_host_abi_bridges ? "allowed" : "denied") << '\n';
  std::cout << "  [compat] Build-ID constructor replay: "
            << (allow_host_constructor_replay ? "allowed" : "denied") << '\n'
            << std::flush;
  if (!build_profile.default_allowed && !options.allow_unverified_build) {
    std::cerr << "[FATAL] This Roblox build is not enabled for normal runs: "
              << build_profile.reason << '\n'
              << "  Use --allow-unverified-build only for an explicit "
                 "compatibility check.\n";
    return EXIT_FAILURE;
  }

  ApplyAuthStartupDefaults(
      !dependencies.roblox_credential().empty(), user_overrode_start_lua_app_dm,
      user_overrode_start_lua_step, user_overrode_start_app_step,
      user_overrode_call_start_app);
  EnableFullTraceIfRequested();
  if (TraceAllEnabled()) {
    std::cout << "  [trace] full tracing enabled\n" << std::flush;
  }

  // Roblox internal threads trigger SI_KERNEL traps from CET shadow-stack
  // return mismatches. Unsupported kernels ignore this request.
  {
    long r = syscall(SYS_arch_prctl, ARCH_SHSTK_DISABLE, ARCH_SHSTK_SHSTK);
    if (r == 0) {
      std::cout << "  [cet] shadow-stack (SHSTK) disabled\n";
    }
  }

  if (build_profile.allow_legacy_binary_patches) {
    InstallHeadlessSegvHandler();
  } else {
    std::cout << "  [compat] signal-recovery handler disabled for this Build "
                 "ID\n";
  }
  const bool is_headless = runtime_config.headless();
  if (!HasEnvValue("MOCKTAIL_APP_BRIDGE_HEADLESS_INIT_PARAMS")) {
    setenv("MOCKTAIL_APP_BRIDGE_HEADLESS_INIT_PARAMS", is_headless ? "1" : "0",
           1);
  }
  PrintStage(1, "Initialising Bionic linker + Pseudo-JVM");

#ifdef MOCKTAIL_USE_BIONIC_LINKER
  ::linker::init();
  std::cout << "  mcpelauncher Bionic linker initialised.\n";
#endif

  auto jni_vm = dependencies.jni_vm();
  if (jni_vm == nullptr) {
    std::cerr << "[FATAL] Typed Pseudo-JVM dependency is missing\n";
    return EXIT_FAILURE;
  }
  std::cout << "  Pseudo-JVM instance created successfully.\n";

  // FindClass needs these descriptors before JNI_OnLoad.
  PrintStage(2, "Registering Android SDK JNI classes");

  auto context_class = jni_vm->RegisterClass("android/content/Context");
  auto activity_class =
      jni_vm->RegisterClass("com/roblox/client/RobloxActivity");
  auto settings_class = jni_vm->RegisterClass("rbx/JNIRobloxSettings");

  settings_class->RegisterMethod(
      "nativeInitClientSettings", "()V", [](JNIEnv* /*env*/, jobject /*obj*/) {
        std::cout << "  [JNI callback] nativeInitClientSettings invoked\n";
      });

  auto native_gl_interface_class =
      jni_vm->RegisterClass("com/roblox/engine/jni/NativeGLInterface");
  g_native_gl_class_for_main_thread =
      reinterpret_cast<jclass>(native_gl_interface_class.get());
  g_vm_for_main_thread_pump = jni_vm.get();
  jni_vm->RegisterClass("com/roblox/engine/jni/NativeInputInterface");
  jni_vm->RegisterClass("com/roblox/engine/jni/NativeSettingsInterface");
  jni_vm->RegisterClass("com/roblox/engine/jni/NativeAppBridgeInterface");
  jni_vm->RegisterClass("com/roblox/engine/jni/NativeGLJavaInterface");
  jni_vm->RegisterClass("com/roblox/engine/jni/EngineJavaCallback2");
  jni_vm->RegisterClass(
      "com/roblox/engine/jni/OnAppBridgeNotificationListener");
  jni_vm->RegisterClass("com/roblox/client/flags/FlagJniInterface");
  jni_vm->RegisterClass("com/roblox/client/flags/NativeFlagsInitResult");
  jni_vm->RegisterClass("com/roblox/engine/jni/model/DeviceStaticParams");
  jni_vm->RegisterClass("com/roblox/engine/jni/model/DeviceParams");
  jni_vm->RegisterClass("com/roblox/engine/jni/model/PlatformParams");
  jni_vm->RegisterClass("com/roblox/engine/jni/model/NativeTextBoxInfo");
  jni_vm->RegisterClass("com/roblox/engine/jni/autovalue/InitParams");
  jni_vm->RegisterClass("com/roblox/engine/jni/autovalue/StartAppParams");
  jni_vm->RegisterClass("com/roblox/client/startup/MainGameActivity");
  jni_vm->RegisterClass("com/roblox/client/startup/NativeHelper");
  jni_vm->RegisterClass(
      "com/roblox/universalapp/activitylifecyclecallbacks/"
      "JNIActivityLifecycleCallbacks");
  jni_vm->RegisterClass("com/roblox/universalapp/messagebus/MessageBus");
  jni_vm->RegisterClass("com/roblox/universalapp/messagebus/Connection");
  jni_vm->RegisterClass(
      "com/roblox/universalapp/systemtheme/SystemThemeProtocol");
  jni_vm->RegisterClass("com/roblox/universalapp/cookie/JNICookieManager");
  jni_vm->RegisterClass("com/roblox/universalapp/cookie/JNICookieProtocol");
  jni_vm->RegisterClass(
      "com/roblox/universalapp/cookie/JNICookieProtocol$OnSetCookieHandler");
  jni_vm->RegisterClass("com/roblox/client/JNIAAssetManagerSetup");
  jni_vm->RegisterClass("org/fmod/FMOD");
  jni_vm->RegisterClass("android/content/res/AssetManager");
  jni_vm->RegisterClass("android/media/AudioManager");
  jni_vm->RegisterClass(
      "com/roblox/protocols/systemdialog/PlatformSystemDialogHandler");
  jni_vm->RegisterClass(
      "com/roblox/protocols/systemdialogplatforminterface/generated/"
      "SystemDialogRequest");
  jni_vm->RegisterClass(
      "com/roblox/protocols/systemdialogplatforminterface/generated/"
      "ISystemDialogCallback");
  jni_vm->RegisterClass("com/google/androidgamesdk/GameActivity");
  jni_vm->RegisterClass(
      "com/google/androidgamesdk/gametextinput/InputConnection");
  jni_vm->RegisterClass("com/google/androidgamesdk/gametextinput/State");
  jni_vm->RegisterClass("com/roblox/client/LocalStorageManager");
  jni_vm->RegisterClass(
      "com/roblox/protocols/localstorageplatforminterface/generated/"
      "ILocalStorageHandlerCore");
  jni_vm->RegisterClass(
      "com/roblox/protocols/localstorageplatforminterface/generated/"
      "ILocalStorageHandlerCore$CppProxy");
  jni_vm->RegisterClass(
      "com/roblox/protocols/localstorageplatforminterface/generated/"
      "IPlatformLocalStorageHandler");
  jni_vm->RegisterClass(
      "com/roblox/protocols/localstorageplatforminterface/generated/"
      "IPlatformLocalStorageHandler$CppProxy");
  jni_vm->RegisterClass("java/util/HashSet");
  jni_vm->RegisterClass("java/util/Iterator");
  jni_vm->RegisterClass("android/os/LocaleList");
  jni_vm->RegisterClass("java/util/Locale");
  jni_vm->RegisterClass("androidx/core/graphics/Insets");
  jni_vm->RegisterClass("androidx/core/view/WindowInsetsCompat$Type");
  jni_vm->RegisterClass("android/view/MotionEvent");
  jni_vm->RegisterClass("android/view/KeyEvent");
  jni_vm->RegisterClass("android/view/Surface");
  jni_vm->RegisterClass("java/lang/String");
  jni_vm->RegisterClass("java/lang/Object");

  std::cout << "  Registered " << jni_vm->GetClassCount() << " JNI class(es)\n";

  // Create SDL/EGL before loading Android stubs so SDL binds the host graphics
  // backend. The window stays hidden until the first real frame.
  bool window_initialised = false;
  if (!is_headless) {
    const int win_w = runtime_config.window().width;
    const int win_h = runtime_config.window().height;
    const char* win_title = runtime_config.window().title.c_str();
    std::cout << "  [window] Creating " << win_w << "x" << win_h
              << " window...\n"
              << std::flush;
    window_initialised = mocktail::window::Init(win_w, win_h, win_title);
    if (!window_initialised) {
      if (IsEnabled("MOCKTAIL_SOBER_MODE")) {
        std::cerr << "  [window] FATAL: Sober-style startup requires a "
                  << "working SDL video device. Check DISPLAY/WAYLAND_DISPLAY "
                  << "and SDL3.\n"
                  << std::flush;
        return EXIT_FAILURE;
      }
      std::cerr
          << "  [window] WARNING: window init failed, continuing headless\n"
          << std::flush;
    } else {
      std::cout << "  [window] Window ready; waiting for Roblox frames\n"
                << std::flush;
    }
  }
  const SDL_SystemTheme system_theme =
      window_initialised ? SDL_GetSystemTheme() : SDL_SYSTEM_THEME_UNKNOWN;
  const bool system_dark_theme = system_theme == SDL_SYSTEM_THEME_DARK;
  const bool dark_theme =
      runtime_config.theme_mode() == "dark" ||
      (runtime_config.theme_mode() == "system" && system_dark_theme);
  setenv("MOCKTAIL_RESOLVED_THEME_INTERNAL", dark_theme ? "Dark" : "Light", 1);
  const char* app_storage_file =
      std::getenv("MOCKTAIL_APP_STORAGE_FILE_INTERNAL");
  if (app_storage_file != nullptr) {
    std::string theme_error;
    if (!mocktail::runtime::ApplyRobloxThemeCacheOverride(
            app_storage_file, dependencies.account_identity().user_id,
            dark_theme, &theme_error)) {
      std::cerr << "[FATAL] Roblox theme cache override failed: " << theme_error
                << '\n';
      return EXIT_FAILURE;
    }
    std::cout << "  [theme] Roblox local theme="
              << (dark_theme ? "dark" : "light") << '\n'
              << std::flush;
  }
  const bool has_window = window_initialised;
  if (!has_window) {
    std::cout << "  [window] GUI-bound startup steps disabled (no SDL window)\n"
              << std::flush;
  } else {
    if (!HasEnvValue("MOCKTAIL_APP_LIFECYCLE_ACTIVE") &&
        !HasEnvValue("MOCKTAIL_STEP_APP_LIFECYCLE_ACTIVE")) {
      setenv("MOCKTAIL_APP_LIFECYCLE_ACTIVE", "1", 1);
      std::cout << "  [window] windowed startup: auto-enabled "
                << "AppLifecycleNativeAdapter.setActive\n"
                << std::flush;
    }
    if (!user_overrode_main_thread_message_pump) {
      setenv("MOCKTAIL_MAIN_THREAD_MESSAGE_PUMP", "1", 1);
      std::cout << "  [window] windowed startup: auto-enabled main-thread "
                << "message pump\n"
                << std::flush;
    }
    if (!user_overrode_call_update_surface) {
      std::cout << "  [window] windowed startup: leaving real "
                   "UpdateSurfaceAppWithPlatformParams opt-in\n"
                << std::flush;
    }
    if (!user_overrode_call_start_app &&
        IsEnabled("MOCKTAIL_STEP_START_APP_WITH_PARAMS")) {
      setenv("MOCKTAIL_CALL_REAL_APP_BRIDGE_START", "1", 1);
      std::cout << "  [window] windowed startup: auto-enabled real "
                << "StartAppWithParams when auth allows it\n"
                << std::flush;
    }
  }

  PrintStage(3, "Building Bionic symbol table from stubs");

  libc_shim::Install();

  // Android unwinders must see mcpelauncher-linker's soinfo list. Stage the
  // exact per-SONAME exports before any synthetic library snapshots the legacy
  // map, and keep the global compatibility map consistent for unresolved ELF
  // imports discovered below.
  void* bionic_dl_iterate_phdr = linker::BionicDlIteratePhdrAddress();
  linker::RegisterSyntheticSymbol("libc.so", "dl_iterate_phdr",
                                  bionic_dl_iterate_phdr);
  linker::RegisterSyntheticSymbol("libdl.so", "dl_iterate_phdr",
                                  bionic_dl_iterate_phdr);
  linker::RegisterSymbol("dl_iterate_phdr", bionic_dl_iterate_phdr);

  // SDL owns the host window and surface. Android libroblox must instead see
  // Mocktail's libEGL adapter, which reuses that surface. Do not resolve this
  // by SONAME: Chromium ANGLE also calls itself libEGL.so and would otherwise
  // win once the SDL backend has loaded it.
  mocktail::graphics::BionicEglBridge bionic_egl_bridge;
  if (has_window) {
    if (!bionic_egl_bridge.Load()) {
      std::cerr << "  [graphics] exact Bionic EGL adapter unavailable: "
                << bionic_egl_bridge.error() << '\n';
      if (IsEnabled("MOCKTAIL_REQUIRE_REAL_GRAPHICS")) {
        std::cerr << "[FATAL] Windowed mode requires Mocktail's exact "
                     "Bionic EGL adapter.\n";
        return EXIT_FAILURE;
      }
    } else {
      for (const auto& [name, address] : bionic_egl_bridge.exports()) {
        linker::RegisterSyntheticSymbol("libEGL.so", name, address);
        linker::RegisterSymbol(name, address);
      }
      std::cout << "  [graphics] Bionic libEGL adapter loaded from "
                << bionic_egl_bridge.library_path() << " ("
                << bionic_egl_bridge.exports().size() << " exports)\n";
    }
  }

  std::vector<const char*> stub_names = {
      "libc.so",         "libdl.so",  "libm.so",        "libz.so",
      "libandroid.so",   "liblog.so", "libmediandk.so", "libOpenSLES.so",
      "libOpenMAXAL.so", "libEGL.so", "libGLESv2.so",
  };
  if (IsEnabled("MOCKTAIL_PRELOAD_VULKAN_SHIM")) {
    stub_names.push_back("libvulkan.so");
  }

  std::vector<void*> stub_handles;
  void* bionic_vulkan_adapter_handle = nullptr;
  for (const char* name : stub_names) {
    void* h = nullptr;
    bool exact_adapter = false;
    if (std::strcmp(name, "libEGL.so") == 0 && bionic_egl_bridge.IsLoaded()) {
      h = bionic_egl_bridge.handle();
      exact_adapter = true;
    } else if (std::strcmp(name, "libvulkan.so") == 0) {
      const std::string vulkan_adapter = RuntimeVulkanAdapterPath();
      if (!vulkan_adapter.empty()) {
        h = ::dlopen(vulkan_adapter.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        exact_adapter = h != nullptr;
      }
    }
    if (h == nullptr) {
      h = ::dlopen(name, RTLD_LAZY | RTLD_GLOBAL | RTLD_NOLOAD);
      if (!h) h = ::dlopen(name, RTLD_LAZY | RTLD_GLOBAL);
    }
    if (h) {
      std::cout << "  [stubs] Preloaded " << name;
      if (exact_adapter) {
        std::cout << " via exact Mocktail adapter";
      }
      std::cout << '\n';
      stub_handles.push_back(h);
      if (std::strcmp(name, "libvulkan.so") == 0) {
        bionic_vulkan_adapter_handle = h;
      }
    } else {
      std::cerr << "  [warn]  Could not preload " << name << ": " << ::dlerror()
                << '\n';
    }
  }
  void* real_gles_handle = nullptr;
  if (has_window && !IsEnabled("MOCKTAIL_GLES_FORCE_STUB") &&
      !IsEnabled("MOCKTAIL_GLES_NOOP_DRAW_CALLS")) {
    real_gles_handle = OpenRealGlesLibrary();
  }

  // Prefer Bionic shims over glibc for guest layouts such as pthread and FILE.
  static const char* kSymbolsToRegister[] = {
      // Symbols exported by the Android compatibility stubs.
      "AAsset_close",
      "AAsset_getBuffer",
      "AAsset_getLength",
      "AAsset_getLength64",
      "AAsset_getRemainingLength",
      "AAsset_getRemainingLength64",
      "AAssetManager_fromJava",
      "AAssetManager_open",
      "AAsset_openFileDescriptor",
      "AAsset_openFileDescriptor64",
      "AAsset_read",
      "AAsset_seek",
      "AAsset_seek64",
      "AConfiguration_delete",
      "AConfiguration_fromAssetManager",
      "AConfiguration_getCountry",
      "AConfiguration_getLanguage",
      "AConfiguration_getNavHidden",
      "AConfiguration_getScreenHeightDp",
      "AConfiguration_getScreenSize",
      "AConfiguration_getScreenWidthDp",
      "AConfiguration_new",
      "alCreateEngine",
      "ALooper_acquire",
      "ALooper_addFd",
      "ALooper_forThread",
      "ALooper_pollOnce",
      "ALooper_prepare",
      "ALooper_release",
      "ALooper_removeFd",
      "AMediaCodec_configure",
      "AMediaCodec_createDecoderByType",
      "AMediaCodec_createEncoderByType",
      "AMediaCodec_delete",
      "AMediaCodec_dequeueInputBuffer",
      "AMediaCodec_dequeueOutputBuffer",
      "AMediaCodec_flush",
      "AMediaCodec_getInputBuffer",
      "AMediaCodec_getOutputBuffer",
      "AMediaCodec_getOutputFormat",
      "AMediaCodec_queueInputBuffer",
      "AMediaCodec_releaseOutputBuffer",
      "AMediaCodec_start",
      "AMediaCodec_stop",
      "AMediaFormat_delete",
      "AMediaFormat_getBuffer",
      "AMediaFormat_getInt32",
      "AMEDIAFORMAT_KEY_BIT_RATE",
      "AMEDIAFORMAT_KEY_CHANNEL_COUNT",
      "AMEDIAFORMAT_KEY_COLOR_FORMAT",
      "AMEDIAFORMAT_KEY_FRAME_RATE",
      "AMEDIAFORMAT_KEY_HEIGHT",
      "AMEDIAFORMAT_KEY_I_FRAME_INTERVAL",
      "AMEDIAFORMAT_KEY_MIME",
      "AMEDIAFORMAT_KEY_SAMPLE_RATE",
      "AMEDIAFORMAT_KEY_STRIDE",
      "AMEDIAFORMAT_KEY_WIDTH",
      "AMediaFormat_new",
      "AMediaFormat_setBuffer",
      "AMediaFormat_setFloat",
      "AMediaFormat_setInt32",
      "AMediaFormat_setString",
      "AMediaFormat_toString",
      "ANativeWindow_acquire",
      "ANativeWindow_fromSurface",
      "ANativeWindow_getHeight",
      "ANativeWindow_getWidth",
      "ANativeWindow_release",
      "eglBindAPI",
      "eglChooseConfig",
      "eglCreateContext",
      "eglCreatePbufferSurface",
      "eglCreateWindowSurface",
      "eglDestroyContext",
      "eglDestroySurface",
      "eglGetConfigAttrib",
      "eglGetCurrentContext",
      "eglGetDisplay",
      "eglGetError",
      "eglGetProcAddress",
      "eglInitialize",
      "eglMakeCurrent",
      "eglQueryString",
      "eglQuerySurface",
      "eglSwapBuffers",
      "eglSwapInterval",
      "eglTerminate",
      "glActiveTexture",
      "glAttachShader",
      "glBindAttribLocation",
      "glBindBuffer",
      "glBindFramebuffer",
      "glBindRenderbuffer",
      "glBindTexture",
      "glBlendFunc",
      "glBlendFuncSeparate",
      "glBufferData",
      "glBufferSubData",
      "glCheckFramebufferStatus",
      "glClear",
      "glClearColor",
      "glClearDepthf",
      "glClearStencil",
      "glColorMask",
      "glCompileShader",
      "glCompressedTexImage2D",
      "glCompressedTexSubImage2D",
      "glCopyTexSubImage2D",
      "glCreateProgram",
      "glCreateShader",
      "glCullFace",
      "glDeleteBuffers",
      "glDeleteFramebuffers",
      "glDeleteProgram",
      "glDeleteRenderbuffers",
      "glDeleteShader",
      "glDeleteTextures",
      "glDepthFunc",
      "glDepthMask",
      "glDisable",
      "glDisableVertexAttribArray",
      "glDrawArrays",
      "glDrawElements",
      "glEnable",
      "glEnableVertexAttribArray",
      "glFramebufferRenderbuffer",
      "glFramebufferTexture2D",
      "glGenBuffers",
      "glGenFramebuffers",
      "glGenRenderbuffers",
      "glGenTextures",
      "glGenerateMipmap",
      "glGetActiveUniform",
      "glGetError",
      "glGetIntegerv",
      "glGetProgramInfoLog",
      "glGetProgramiv",
      "glGetShaderInfoLog",
      "glGetShaderiv",
      "glGetString",
      "glGetUniformLocation",
      "glLinkProgram",
      "glPixelStorei",
      "glPolygonOffset",
      "glReadPixels",
      "glReleaseShaderCompiler",
      "glRenderbufferStorage",
      "glScissor",
      "glShaderSource",
      "glStencilFunc",
      "glStencilMask",
      "glStencilOp",
      "glTexImage2D",
      "glTexParameterf",
      "glTexParameterfv",
      "glTexParameteri",
      "glTexSubImage2D",
      "glUniform1i",
      "glUseProgram",
      "glVertexAttribPointer",
      "glViewport",
      "vkCreateAndroidSurfaceKHR",
      "vkCreateInstance",
      "vkDestroySurfaceKHR",
      "vkEnumerateInstanceExtensionProperties",
      "vkEnumerateInstanceLayerProperties",
      "vkGetDeviceProcAddr",
      "vkGetInstanceProcAddr",
      "__android_log_assert",
      "__android_log_buf_write",
      "__android_log_print",
      "__android_log_write",
      "__assert",
      "__assert2",
      "__ctype_get_mb_cur_max",
      "__errno",
      "__FD_CLR_chk",
      "__FD_ISSET_chk",
      "__FD_SET_chk",
      "fread",
      "__fread_chk",
      "fwrite",
      "__fwrite_chk",
      "fflush",
      "open",
      "fopen",
      "access",
      "stat",
      "lstat",
      "statvfs",
      "statfs",
      "mkdir",
      "opendir",
      "rename",
      "unlink",
      "rmdir",
      "realpath",
      "readlink",
      "__gnu_strerror_r",
      "__open_2",
      "__poll_chk",
      "pthread_attr_destroy",
      "pthread_attr_getstack",
      "pthread_attr_init",
      "pthread_attr_setdetachstate",
      "pthread_attr_setschedparam",
      "pthread_attr_setstacksize",
      "pthread_condattr_destroy",
      "pthread_condattr_init",
      "pthread_condattr_setclock",
      "pthread_cond_broadcast",
      "pthread_cond_destroy",
      "pthread_cond_init",
      "pthread_cond_signal",
      "pthread_cond_timedwait",
      "pthread_cond_wait",
      "pthread_create",
      "pthread_getattr_np",
      "pthread_mutexattr_destroy",
      "pthread_mutexattr_init",
      "pthread_mutexattr_settype",
      "pthread_mutex_destroy",
      "pthread_mutex_init",
      "pthread_mutex_lock",
      "pthread_mutex_trylock",
      "pthread_mutex_unlock",
      "pthread_rwlock_destroy",
      "pthread_rwlock_init",
      "pthread_rwlock_rdlock",
      "pthread_rwlock_unlock",
      "pthread_rwlock_wrlock",
      "__read_chk",
      "__readlink_chk",
      "__sendto_chk",
      "__sF",
      // stdio stream pointers — Bionic clients access these as FILE* globals
      "stdin",
      "stdout",
      "stderr",
      "slCreateEngine",
      "SL_IID_ANDROIDCONFIGURATION",
      "SL_IID_ANDROIDSIMPLEBUFFERQUEUE",
      "SL_IID_BUFFERQUEUE",
      "SL_IID_ENGINE",
      "SL_IID_PLAY",
      "SL_IID_RECORD",
      "SL_IID_VOLUME",
      "mocktail_recover_stack_chk_fail",
      "__stack_chk_fail",
      "__stack_chk_guard",
      "__strchr_chk",
      "__strlen_chk",
      "__strncpy_chk2",
      "sysconf",
      "__system_property_get",
      "__write_chk",

      // pthread functions whose host ABI is already compatible.
      "pthread_once",
      "pthread_self",
      "pthread_equal",
      "pthread_join",
      "pthread_detach",
      "pthread_kill",
      "pthread_exit",
      "pthread_getschedparam",
      "pthread_setschedparam",
      "pthread_key_create",
      "pthread_key_delete",
      "pthread_getspecific",
      "pthread_setspecific",
      "pthread_sigmask",
      "pthread_setname_np",

      // Standard C library functions resolved from host libc.
      "strcmp",
      "strncmp",
      "strcpy",
      "strncpy",
      "strlen",
      "strcat",
      "strncat",
      "strchr",
      "strrchr",
      "strstr",
      "strtol",
      "strtoul",
      "strtod",
      "strtof",
      "atoi",
      "atof",
      "atol",
      "memcpy",
      "memmove",
      "memset",
      "memcmp",
      "memchr",
      "malloc",
      "calloc",
      "realloc",
      "free",
      "abort",
      "exit",
      "getenv",
      "setenv",
      "putenv",
      "sprintf",
      "snprintf",
      "sscanf",
      "printf",
      "fprintf",
      "vprintf",
      "vfprintf",
      "vsprintf",
      "vsnprintf",
      "fclose",
      "feof",
      "ferror",
      "fgets",
      "fgetc",
      "fputc",
      "fputs",
      "fseek",
      "ftell",
      "rewind",
      "close",
      "read",
      "write",
      "lseek",
      "lseek64",
      "pread",
      "pwrite",
      "pread64",
      "pwrite64",
      "getcwd",
      "chdir",
      "dup",
      "dup2",
      "pipe",
      "socket",
      "connect",
      "bind",
      "listen",
      "accept",
      "setsockopt",
      "getsockopt",
      "send",
      "recv",
      "sendto",
      "recvfrom",
      "getaddrinfo",
      "freeaddrinfo",
      "getnameinfo",
      "inet_ntop",
      "inet_pton",
      "htons",
      "htonl",
      "ntohs",
      "ntohl",
      "clock_gettime",
      "clock_getres",
      "gettimeofday",
      "nanosleep",
      "usleep",
      "sleep",
      "time",
      "localtime",
      "gmtime",
      "mktime",
      "strftime",
      "mmap",
      "munmap",
      "mprotect",
      "msync",
      "mlock",
      "munlock",
      "madvise",
      "sigaction",
      "signal",
      "raise",
      "kill",
      "getpid",
      "getuid",
      "geteuid",
      "getgid",
      "getegid",
      "waitpid",
      "fork",
      "execve",
      "dlopen",
      "dlclose",
      "dlsym",
      "dlerror",
      "prctl",
      "ioctl",
      "fcntl",
      "isatty",
      "isalpha",
      "isdigit",
      "isspace",
      "isupper",
      "islower",
      "toupper",
      "tolower",
      "rand",
      "srand",
      "rand_r",
      "qsort",
      "bsearch",
      "abs",
      "labs",
      "llabs",
      "ceil",
      "floor",
      "round",
      "fabs",
      "pow",
      "sqrt",
      "log",
      "log2",
      "log10",
      "exp",
      "sin",
      "cos",
      "tan",
      "asin",
      "acos",
      "atan",
      "atan2",
      "strerror",
      "perror",
      "readdir",
      "closedir",
      "pthread_barrier_init",
      "pthread_barrier_destroy",
      "pthread_barrier_wait",
      "pthread_spin_init",
      "pthread_spin_destroy",
      "pthread_spin_lock",
      "pthread_spin_trylock",
      "pthread_spin_unlock",
  };

  int registered = 0;
  int total_symbols = 0;
  int gl_symbol_count = 0;
  int gl_from_window = 0;
  int gl_from_real_gles = 0;
  int gl_from_stub = 0;
  int gl_from_host = 0;
  int gl_unresolved = 0;
  for (const char* sym : kSymbolsToRegister) {
    ++total_symbols;
    auto result =
        ResolveSymbolForBionic(sym, has_window, real_gles_handle, stub_handles);
    void* addr = result.address;
    const bool is_gl_symbol = IsGlSymbol(sym);
    if (addr == nullptr && is_gl_symbol) {
      ++gl_unresolved;
    }
    if (is_gl_symbol) {
      ++gl_symbol_count;
      switch (result.source) {
        case SymbolResolveSource::kWindow:
          ++gl_from_window;
          break;
        case SymbolResolveSource::kRealGles:
          ++gl_from_real_gles;
          break;
        case SymbolResolveSource::kStub:
          ++gl_from_stub;
          break;
        case SymbolResolveSource::kHost:
          ++gl_from_host;
          break;
        case SymbolResolveSource::kMissing:
        default:
          break;
      }
    }

    if (addr) {
      linker::RegisterSymbol(sym, addr);
      ++registered;
    }
  }
  if (bionic_vulkan_adapter_handle != nullptr) {
    static constexpr const char* kVulkanAdapterExports[] = {
        "vkCreateAndroidSurfaceKHR",
        "vkCreateDevice",
        "vkCreateInstance",
        "vkCreateSwapchainKHR",
        "vkDestroyDevice",
        "vkDestroyInstance",
        "vkDestroySurfaceKHR",
        "vkDestroySwapchainKHR",
        "vkEnumerateInstanceExtensionProperties",
        "vkEnumerateInstanceLayerProperties",
        "vkGetDeviceProcAddr",
        "vkGetDeviceQueue",
        "vkGetDeviceQueue2",
        "vkGetInstanceProcAddr",
        "vkQueuePresentKHR",
    };
    size_t vulkan_exports = 0;
    for (const char* name : kVulkanAdapterExports) {
      void* address = ::dlsym(bionic_vulkan_adapter_handle, name);
      if (address == nullptr ||
          !StubOwnsSymbolAddress(bionic_vulkan_adapter_handle, address)) {
        continue;
      }
      linker::RegisterSyntheticSymbol("libvulkan.so", name, address);
      linker::RegisterSyntheticSymbol("libvulkan.so.1", name, address);
      linker::RegisterSymbol(name, address);
      ++vulkan_exports;
    }
    std::cout << "  [vulkan] registered exact Android loader adapter exports: "
              << vulkan_exports << '\n';
  }
  RegisterBionicDnsWrappers();
  RegisterBionicPathWrappers();
  linker::RegisterSymbol("prctl",
                         reinterpret_cast<void*>(mocktail_bionic_prctl));
  linker::RegisterSymbol("setsockopt",
                         reinterpret_cast<void*>(mocktail_bionic_setsockopt));
  linker::RegisterSymbol("sendmsg",
                         reinterpret_cast<void*>(mocktail_bionic_sendmsg));
  RegisterBionicMemoryWrappers();
  linker::RegisterSymbol(
      "pthread_condattr_init",
      reinterpret_cast<void*>(mocktail_pthread_condattr_init));
  linker::RegisterSymbol(
      "pthread_condattr_destroy",
      reinterpret_cast<void*>(mocktail_pthread_condattr_destroy));
  linker::RegisterSymbol(
      "pthread_condattr_setclock",
      reinterpret_cast<void*>(mocktail_pthread_condattr_setclock));
  linker::RegisterSymbol("pthread_cond_init",
                         reinterpret_cast<void*>(mocktail_pthread_cond_init));
  linker::RegisterSymbol(
      "pthread_cond_destroy",
      reinterpret_cast<void*>(mocktail_pthread_cond_destroy));
  linker::RegisterSymbol("pthread_cond_signal",
                         reinterpret_cast<void*>(mocktail_pthread_cond_signal));
  linker::RegisterSymbol(
      "pthread_cond_broadcast",
      reinterpret_cast<void*>(mocktail_pthread_cond_broadcast));
  linker::RegisterSymbol("pthread_cond_wait",
                         reinterpret_cast<void*>(mocktail_pthread_cond_wait));
  linker::RegisterSymbol(
      "pthread_cond_timedwait",
      reinterpret_cast<void*>(mocktail_pthread_cond_timedwait));
  linker::RegisterSymbol(
      "pthread_mutexattr_init",
      reinterpret_cast<void*>(mocktail_pthread_mutexattr_init));
  linker::RegisterSymbol(
      "pthread_mutexattr_destroy",
      reinterpret_cast<void*>(mocktail_pthread_mutexattr_destroy));
  linker::RegisterSymbol(
      "pthread_mutexattr_settype",
      reinterpret_cast<void*>(mocktail_pthread_mutexattr_settype));
  linker::RegisterSymbol("pthread_mutex_init",
                         reinterpret_cast<void*>(mocktail_pthread_mutex_init));
  linker::RegisterSymbol(
      "pthread_mutex_destroy",
      reinterpret_cast<void*>(mocktail_pthread_mutex_destroy));
  linker::RegisterSymbol("pthread_mutex_lock",
                         reinterpret_cast<void*>(mocktail_pthread_mutex_lock));
  linker::RegisterSymbol(
      "pthread_mutex_trylock",
      reinterpret_cast<void*>(mocktail_pthread_mutex_trylock));
  linker::RegisterSymbol(
      "pthread_mutex_unlock",
      reinterpret_cast<void*>(mocktail_pthread_mutex_unlock));
  linker::RegisterSymbol("pthread_once",
                         reinterpret_cast<void*>(mocktail_pthread_once));
  linker::RegisterSymbol("pthread_spin_init",
                         reinterpret_cast<void*>(mocktail_pthread_spin_init));
  linker::RegisterSymbol(
      "pthread_spin_destroy",
      reinterpret_cast<void*>(mocktail_pthread_spin_destroy));
  linker::RegisterSymbol("pthread_spin_lock",
                         reinterpret_cast<void*>(mocktail_pthread_spin_lock));
  linker::RegisterSymbol(
      "pthread_spin_trylock",
      reinterpret_cast<void*>(mocktail_pthread_spin_trylock));
  linker::RegisterSymbol("pthread_spin_unlock",
                         reinterpret_cast<void*>(mocktail_pthread_spin_unlock));
  linker::RegisterSymbol(
      "pthread_barrier_init",
      reinterpret_cast<void*>(mocktail_pthread_barrier_init));
  linker::RegisterSymbol(
      "pthread_barrier_destroy",
      reinterpret_cast<void*>(mocktail_pthread_barrier_destroy));
  linker::RegisterSymbol(
      "pthread_barrier_wait",
      reinterpret_cast<void*>(mocktail_pthread_barrier_wait));
  linker::RegisterSymbol("pthread_create", reinterpret_cast<void*>(
                                               mocktail_bionic_pthread_create));
  linker::RegisterSymbol("abort", reinterpret_cast<void*>(mocktail_abort));
  linker::RegisterSymbol(
      "__stack_chk_fail",
      reinterpret_cast<void*>(mocktail_recover_stack_chk_fail));
  (void)registered;
  std::cout << "  [linker] Registered " << registered << " / " << total_symbols
            << " known symbols.\n";
  std::cout << "  [linker] GL symbol resolution: total=" << gl_symbol_count
            << ", window=" << gl_from_window
            << ", real_gles=" << gl_from_real_gles << ", stub=" << gl_from_stub
            << ", host=" << gl_from_host << ", unresolved=" << gl_unresolved
            << '\n';
  if (IsEnabled("MOCKTAIL_REQUIRE_REAL_GRAPHICS") && has_window &&
      !IsEnabled("MOCKTAIL_GLES_FORCE_STUB") &&
      !IsEnabled("MOCKTAIL_GLES_NOOP_DRAW_CALLS")) {
    if (gl_from_stub > 0 || gl_unresolved > 0 ||
        (gl_from_window == 0 && gl_from_real_gles == 0 && gl_from_host == 0)) {
      std::cerr
          << "[FATAL] Mocktail cannot guarantee real GL symbols for "
             "windowed mode.\n"
          << "  Set MOCKTAIL_GLES_FORCE_STUB=1 or "
             "MOCKTAIL_GLES_NOOP_DRAW_CALLS=1 to continue with stubs.\n"
             "  Or clear MOCKTAIL_REQUIRE_REAL_GRAPHICS and inspect "
             "logs above.\n"
          << "  Consider setting MOCKTAIL_GRAPHICS_BACKEND=angle-vulkan and "
             "MOCKTAIL_GLES_LIBRARY=<path-to-real-libGLESv2.so>.\n";
      return EXIT_FAILURE;
    }
  }

  // Register imports before loading synthetic SONAMEs; load_library snapshots
  // g_bionic_symbols.
  {
    int fd = ::open(library_path.c_str(), O_RDONLY);
    if (fd >= 0) {
      struct stat st;
      if (::fstat(fd, &st) == 0) {
        void* map = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ,
                           MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
          auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(map);
          auto* shdr = reinterpret_cast<Elf64_Shdr*>(
              reinterpret_cast<char*>(map) + ehdr->e_shoff);
          Elf64_Shdr* dynsym_hdr = nullptr;
          Elf64_Shdr* dynstr_hdr = nullptr;
          for (int si = 0; si < ehdr->e_shnum; ++si) {
            if (shdr[si].sh_type == SHT_DYNSYM)
              dynsym_hdr = &shdr[si];
            else if (shdr[si].sh_type == SHT_STRTAB && si != ehdr->e_shstrndx)
              dynstr_hdr = &shdr[si];
          }
          if (dynsym_hdr && dynstr_hdr) {
            auto* syms = reinterpret_cast<Elf64_Sym*>(
                reinterpret_cast<char*>(map) + dynsym_hdr->sh_offset);
            const char* strtab =
                reinterpret_cast<const char*>(map) + dynstr_hdr->sh_offset;
            size_t num_syms = dynsym_hdr->sh_size / sizeof(Elf64_Sym);
            int auto_registered = 0;
            for (size_t si = 0; si < num_syms; ++si) {
              if (syms[si].st_shndx != SHN_UNDEF) continue;
              const char* name = strtab + syms[si].st_name;
              if (!name || name[0] == '\0') continue;
              if (linker::GetBionicSymbols().count(name)) continue;
              auto result = ResolveSymbolForBionic(
                  name, has_window, real_gles_handle, stub_handles);
              if (result.address != nullptr) {
                linker::RegisterSymbol(name, result.address);
                ++auto_registered;
              }
            }
            std::cout << "  [linker] Auto-registered " << auto_registered
                      << " additional ELF symbols from " << library_path
                      << '\n';
          }
          ::munmap(map, static_cast<size_t>(st.st_size));
        }
      }
      ::close(fd);
    }
  }

  // Load synthetic libraries only after the import map is complete.
  linker::RegisterBionicPthreadKeyRuntimeForLibc();
  linker::RegisterBionicSysconfRuntimeForLibc();
  linker::RegisterBionicSignalRuntimeForLibc();
  linker::RegisterBionicStdioRuntimeForLibc();
  linker::RegisterBionicDynamicLoaderForLibdl();
  std::cout << "  [linker] Registered typed Bionic pthread/sysconf/signal/"
               "stdio ABI for libc.so\n";
  std::cout << "  [linker] dl_iterate_phdr routed to Bionic linker before "
               "SONAME snapshots\n"
            << std::flush;
  for (const char* name : stub_names) {
    linker::LoadLibrary(name, name);
  }
  if (bionic_vulkan_adapter_handle != nullptr) {
    linker::LoadLibrary("libvulkan.so.1", "libvulkan.so.1");
  }
  PreloadPthreadSymbols();

  std::cout << "  Library path: " << library_path << '\n';
  linker::LibraryHandle roblox_handle =
      linker::LoadLibrary(library_path, "libroblox");

  if (roblox_handle == nullptr) {
    std::cerr << "\n[FATAL] Could not load '" << library_path << "'.\n"
              << "  Extract lib/x86_64/libroblox.so from a Roblox APK and\n"
              << "  place it at the path above (or set ROBLOX_LIB_PATH).\n";
    return EXIT_FAILURE;
  }

  const linker::ProgramHeaderValidation program_headers =
      linker::ValidateBionicProgramHeaders(roblox_handle);
  if (!program_headers) {
    std::cerr << "\n[FATAL] Bionic program-header visibility failed: "
              << program_headers.error << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "  [linker] Bionic unwind metadata validated for libroblox\n"
            << std::flush;

  // The pre-constructor hook owns installation; repeating arena setup here is
  // not idempotent.
  if (HostAbiExperimentRequested() &&
      (!g_host_abi_install_attempted || !g_host_abi_install_result)) {
    std::cerr << "\n[FATAL] Host ABI profile did not install before "
                 "libroblox constructors: "
              << (g_host_abi_install_result.error != nullptr
                      ? g_host_abi_install_result.error
                      : "linker pre-constructor hook was not observed")
              << '\n';
    return EXIT_FAILURE;
  }

  PrintStage(5, "Invoking JNI_OnLoad in libroblox.so");

  auto* jni_onload = reinterpret_cast<JniOnLoadFn>(
      linker::ResolveSymbol(roblox_handle, "JNI_OnLoad"));

  if (jni_onload == nullptr) {
    std::cerr << "\n[FATAL] Symbol 'JNI_OnLoad' not found in libroblox.so.\n";
    return EXIT_FAILURE;
  }

  const mocktail::compat::HostAbiProfile* stage5_host_profile =
      g_active_host_abi_profile.load(std::memory_order_acquire);
  if (g_host_abi_install_attempted &&
      g_host_abi_install_result.uses_native_mimalloc &&
      stage5_host_profile != nullptr) {
    const mocktail::compat::NativePreJniBootstrapStatus bootstrap_status =
        mocktail::compat::InitializeNativePreJniRegistry(
            g_libroblox_base_static, *stage5_host_profile);
    if (bootstrap_status ==
        mocktail::compat::NativePreJniBootstrapStatus::kInitialized) {
      std::cout << "  [compat] native pre-JNI registry initialized\n"
                << std::flush;
    } else if (bootstrap_status ==
               mocktail::compat::NativePreJniBootstrapStatus::kFailed) {
      std::cerr << "\n[FATAL] native pre-JNI registry initialization failed\n";
      return EXIT_FAILURE;
    }
  }

  JavaVM* raw_vm = jni_vm->GetJavaVM();
  PublishCurrentJniEnv(jni_vm->GetJNIEnv());
  int jni_timeout_ms = GetEnvInt("MOCKTAIL_JNI_ONLOAD_TIMEOUT_MS", 3000);
  bool soft_timeout_enabled = IsEnabled("MOCKTAIL_JNI_ONLOAD_SOFT_TIMEOUT");
  bool soft_timeout_forced =
      IsEnabled("MOCKTAIL_JNI_ONLOAD_SOFT_TIMEOUT_FORCE");
  g_stage6_jni_env = reinterpret_cast<uintptr_t>(jni_vm->GetJNIEnv());
  if (soft_timeout_enabled && !soft_timeout_forced) {
    std::cerr << "  [jvm] JNI_OnLoad soft timeout disabled in safe mode (set "
              << "MOCKTAIL_JNI_ONLOAD_SOFT_TIMEOUT_FORCE=1 to enable).\n"
              << std::flush;
  }
  g_jni_onload_soft_timeout = soft_timeout_enabled && soft_timeout_forced;

  jint jni_version = JNI_ERR;
  if (jni_timeout_ms > 0 && g_jni_onload_soft_timeout != 0) {
    std::cout << "  [jvm] JNI_OnLoad soft timeout watcher armed: "
              << jni_timeout_ms << "ms\n"
              << std::flush;
    JniOnLoadAsyncContext async_context{jni_onload, JNI_ERR, jni_vm.get()};
    mocktail::runtime::OwnedPthread onload_thread;
    const int create_result =
        onload_thread.Start(&RunJniOnLoadWorker, &async_context, 0);
    if (create_result != 0) {
      std::cout << "  [jvm] JNI_OnLoad worker thread creation failed: "
                << create_result << "\n"
                << std::flush;
      jni_version = jni_onload(raw_vm, nullptr);
    } else {
      const mocktail::runtime::OwnedPthreadWaitResult wait_result =
          onload_thread.WaitFor(jni_timeout_ms, 10);
      if (wait_result.joined()) {
        jni_version = async_context.result;
      } else {
        std::cerr << "  [jvm] JNI_OnLoad soft timeout exceeded; cancelling "
                     "without allowing runtime unwind\n"
                  << std::flush;
        const int cancel_join_grace_ms = std::clamp(
            GetEnvInt("MOCKTAIL_JNI_ONLOAD_CANCEL_JOIN_GRACE_MS", 5000), 100,
            60000);
        const mocktail::runtime::OwnedPthreadCancelResult cancel_result =
            onload_thread.CancelAndJoinFor(cancel_join_grace_ms, 10);
        std::cerr << "[FATAL] JNI_OnLoad did not complete before its timeout; "
                  << "cancel=" << cancel_result.cancel_error << " join="
                  << mocktail::runtime::OwnedPthreadWaitStatusName(
                         cancel_result.wait.status)
                  << " error=" << cancel_result.wait.platform_error
                  << "; terminating without RAII unwind\n"
                  << std::flush;
        std::_Exit(EXIT_FAILURE);
      }
    }
  } else {
    if (jni_timeout_ms > 0) {
      g_jni_onload_in_progress = 1;
      g_jni_onload_timings_printed = 0;
      std::cout << "  [jvm] JNI_OnLoad timeout watchdog armed: "
                << jni_timeout_ms << "ms\n"
                << std::flush;
      struct sigaction action;
      action.sa_sigaction = JniOnLoadTimeoutAlarm;
      action.sa_flags = SA_SIGINFO | SA_RESETHAND;
      sigemptyset(&action.sa_mask);
      sigaction(SIGALRM, &action, nullptr);
      alarm((jni_timeout_ms + 999) / 1000);
    }

    jni_version = jni_onload(raw_vm, nullptr);

    if (jni_timeout_ms > 0) {
      g_jni_onload_in_progress = 0;
      g_jni_onload_jmp_armed = 0;
      alarm(0);
    }
  }

  std::cout << "  JNI_OnLoad returned. Requested JNI version: 0x" << std::hex
            << std::setw(8) << std::setfill('0') << jni_version << std::dec
            << '\n';

  // libroblox owns this tagged-pointer segment table.  Keep it intact by
  // default; replacing it with the pseudo-JNI table makes native Roblox
  // scheduler objects resolve to Java object stubs with the wrong layout.
  {
    uintptr_t base_addr = g_libroblox_base;
#ifdef MOCKTAIL_USE_BIONIC_LINKER
    const uintptr_t loader_base =
        static_cast<uintptr_t>(::linker::get_library_base(roblox_handle));
    if (loader_base != 0) {
      base_addr = loader_base;
    }
#endif
    if (base_addr == 0) {
      Dl_info symbol_info{};
      if (::dladdr(reinterpret_cast<void*>(jni_onload), &symbol_info) != 0) {
        base_addr = reinterpret_cast<uintptr_t>(symbol_info.dli_fbase);
      }
    }
    if (base_addr == 0) {
      std::cerr << "[FATAL] Bionic linker did not report libroblox base\n";
      return EXIT_FAILURE;
    }
    g_libroblox_base = base_addr;
    g_mocktail_abort_libroblox_base = base_addr;
    if (build_profile.allow_legacy_binary_patches) {
      PatchRobloxSmallAllocator(base_addr);
      PatchRobloxAllocatorObject(base_addr);
      PatchRobloxServiceHostBuilder(base_addr);
      PatchRobloxJniReferenceHighTagMask(base_addr);
      RestoreConstructorEmutlsHelpers(base_addr);
      RestoreKnownRobloxEmutlsKeys(base_addr);
      PatchRobloxEmutlsGetBridge(base_addr);
      if (IsEnabled("MOCKTAIL_PATCH_ROBLOX_SEGMENT_TABLE")) {
        void** segment_table_ptr = (void**)(base_addr + 0x75a2a40);
        void** expanded_segment_table = ExpandedSegmentTable();
        uintptr_t addr = (uintptr_t)segment_table_ptr;
        uintptr_t page = addr & ~0xFFF;
        if (::mprotect((void*)page, 4096, PROT_READ | PROT_WRITE) == 0) {
          std::cout
              << "  [patch] Overwriting libroblox segment table pointer at "
              << segment_table_ptr << " (current value: " << *segment_table_ptr
              << ") with " << (void*)expanded_segment_table << "\n"
              << std::flush;
          *segment_table_ptr = expanded_segment_table;
        } else {
          std::cerr
              << "  [patch] Failed to make segment table pointer writeable: "
              << std::strerror(errno) << "\n"
              << std::flush;
        }
      } else {
        auto** segment_table_ptr = (void**)(base_addr + 0x75a2a40);
        std::cout << "  [patch] preserving libroblox segment table pointer at "
                  << segment_table_ptr << " value=" << *segment_table_ptr
                  << '\n'
                  << std::flush;
      }
    } else {
      std::cout << "  [compat] skipped all fixed-offset post-JNI patches\n"
                << std::flush;
    }
  }

  // Restore the pseudo-JNI table if JNI_OnLoad replaced it.
  {
    JNIEnv* main_env = jni_vm->GetJNIEnv();
    if (main_env) {
      std::cerr << "  [jvm] restoring env->functions after JNI_OnLoad"
                << " (was " << (void*)main_env->functions << ")\n"
                << std::flush;
      jni_vm->RestoreFunctions();
      std::cerr << "  [jvm] env->functions now " << (void*)main_env->functions
                << "\n"
                << std::flush;
    }
  }
  std::unique_ptr<mocktail::runtime::RobloxGameSessionRuntime>
      game_session_runtime;
  std::unique_ptr<mocktail::runtime::RobloxGameSessionSymbols>
      experience_game_symbols;
  std::shared_ptr<mocktail::runtime::RobloxExperienceComposition>
      experience_composition;
  std::shared_ptr<ExperienceLifecycleTarget> experience_lifecycle_target;
  std::shared_ptr<mocktail::runtime::RobloxWindowInputRuntime>
      window_input_runtime;
  std::unique_ptr<mocktail::runtime::RobloxTextInputJniBridge>
      text_input_bridge;
  mocktail::window::ScopedPresentObserver game_present_observer;
  if (!IsDisabled("MOCKTAIL_START_ENGINE")) {
    PrintStage(6, is_headless ? "Invoking headless Roblox startup entry points"
                              : "Invoking Roblox startup entry points");

    const mocktail::runtime::RobloxSymbolLookup capability_lookup(
        &ResolveRobloxCapabilitySymbol, &roblox_handle);
    const mocktail::runtime::RobloxCapabilityResolution capability_resolution =
        mocktail::runtime::ResolveRobloxCapabilities(
            capability_lookup,
            mocktail::runtime::RobloxCapabilityRequirements{has_window});
    if (!capability_resolution.ok()) {
      std::cerr << "\n[FATAL] Roblox startup capability resolution failed: "
                << mocktail::runtime::RobloxCapabilityResolutionStatusName(
                       capability_resolution.status())
                << '\n';
      for (const std::string& symbol_name :
           capability_resolution.missing_required_symbols()) {
        std::cerr << "  - " << symbol_name << '\n';
      }
      return EXIT_FAILURE;
    }

    const mocktail::runtime::RobloxCapabilities& roblox_capabilities =
        *capability_resolution.capabilities();
    const mocktail::runtime::RobloxStartupSymbols& startup_symbols =
        roblox_capabilities.startup;
    if (has_window) {
      const mocktail::runtime::RobloxGameSessionResolution game_resolution =
          mocktail::runtime::ResolveRobloxGameSessionSymbols(capability_lookup);
      if (!game_resolution.ok()) {
        std::cerr << "\n[FATAL] GAME lifecycle symbol resolution failed: "
                  << mocktail::runtime::RobloxGameSessionResolutionStatusName(
                         game_resolution.status())
                  << '\n';
        for (const std::string& symbol_name :
             game_resolution.missing_required_symbols()) {
          std::cerr << "  - " << symbol_name << '\n';
        }
        return EXIT_FAILURE;
      }
      if (IsEnabled("MOCKTAIL_START_GAME_WITH_PARAM")) {
        mocktail::runtime::JniEnvironmentProvider environment{
            raw_vm, jni_vm.get(), &RestoreGameSessionJniEnvironment};
        mocktail::runtime::RobloxGameSurfaceJniConfig surface_config;
        surface_config.asset_folder_path = DefaultAssetPath();
        surface_config.is_touch_device = input_capabilities.touch_enabled;
        surface_config.is_mouse_device = input_capabilities.mouse_enabled;
        surface_config.is_keyboard_device = input_capabilities.keyboard_enabled;
        game_session_runtime =
            std::make_unique<mocktail::runtime::RobloxGameSessionRuntime>(
                environment, *game_resolution.symbols(),
                std::move(surface_config));
        std::cout << "  [game-session] eager typed GAME symbols ready\n"
                  << std::flush;
      } else {
        experience_game_symbols =
            std::make_unique<mocktail::runtime::RobloxGameSessionSymbols>(
                *game_resolution.symbols());
        std::cout << "  [experience] dynamic GAME symbols ready\n"
                  << std::flush;
      }
    }

    NativeGameGlobalInitFn native_global_init =
        startup_symbols.game_global_init;
    NativeNoArgFn native_update_adapter_init =
        startup_symbols.update_adapter_init;
    NativeAppBridgeObjectParamsFn native_init_with_params =
        startup_symbols.app_bridge_v2_init_with_params;
    NativeNoArgFn native_start_lua_app_dm =
        startup_symbols.app_bridge_start_lua_app_dm;
    NativeAppBridgeObjectParamsFn native_start_app_with_params =
        startup_symbols.app_bridge_v2_start_app_with_params;
    NativeUpdateSurfaceAppFn native_update_surface_app =
        startup_symbols.app_bridge_v2_update_surface_app_with_platform_params;
    NativeNoArgFn native_call_messages_from_main_thread =
        startup_symbols.call_messages_from_main_thread;

    auto* native_init_client_settings =
        reinterpret_cast<NativeInitClientSettingsFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeGLInterface_"
            "nativeInitClientSettings"));
    auto* native_init_client_settings_signed =
        reinterpret_cast<NativeInitClientSettingsSignedFn>(
            linker::ResolveSymbol(
                roblox_handle,
                "Java_com_roblox_engine_jni_NativeGLInterface_"
                "nativeInitClientSettingsSigned"));
    auto* native_init_client_settings_cached =
        reinterpret_cast<NativeInitClientSettingsCachedFn>(
            linker::ResolveSymbol(
                roblox_handle,
                "Java_com_roblox_engine_jni_NativeGLInterface_"
                "nativeInitClientSettingsCached"));
    auto* native_init_client_settings_cached_compressed =
        reinterpret_cast<NativeInitClientSettingsCachedCompressedFn>(
            linker::ResolveSymbol(
                roblox_handle,
                "Java_com_roblox_engine_jni_NativeGLInterface_"
                "nativeInitClientSettingsCachedCompressed"));
    auto* native_post_client_settings =
        reinterpret_cast<NativePostClientSettingsFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeGLInterface_"
            "nativePostClientSettingsLoadedInitialization3"));
    auto* native_initialize_native_flags =
        reinterpret_cast<NativeInitializeNativeFlagsFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_client_flags_FlagJniInterface_"
            "nativeInitializeNativeFlags"));
    auto* native_app_bridge_app_start =
        reinterpret_cast<NativeAppBridgeAppStartFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeAppBridgeInterface_"
            "nativeAppBridgeAppStart__Ljava_lang_String_2Ljava_lang_String_2Z"
            "Ljava_lang_String_2Ljava_lang_String_2Ljava_lang_String_2"));
    auto* native_set_is_first_install =
        reinterpret_cast<NativeSetIsFirstInstallFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeAppBridgeInterface_"
            "setIsFirstInstall"));
    auto* native_set_base_url =
        reinterpret_cast<NativeSetBaseUrlFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeSetBaseUrl"));
    auto* native_set_device_info =
        reinterpret_cast<NativeObjectInitFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeSetDeviceInfo"));
    auto* native_base_url_protocol_init =
        reinterpret_cast<NativeObjectInitFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_universalapp_linking_JNIBaseUrlProtocol_init"));
    auto* native_set_roblox_channel =
        reinterpret_cast<NativeSetStringParamFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeSetRobloxChannel"));
    auto* native_override_channel_platform_name =
        reinterpret_cast<NativeSetStringParamFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeOverrideChannelPlatformName"));
    auto* native_set_roblox_version =
        reinterpret_cast<NativeSetStringParamFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeSetRobloxVersion"));
    auto* native_set_exception_reason_filename =
        reinterpret_cast<NativeSetStringParamFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeSetExceptionReasonFilename"));
    auto* native_set_base_data_directories =
        reinterpret_cast<NativeSetTwoStringParamsFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeSetBaseDataDirectories"));
    auto* native_set_cache_directory =
        reinterpret_cast<NativeSetStringParamFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeSetCacheDirectory"));
    auto* native_set_files_directory =
        reinterpret_cast<NativeSetStringParamFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeSetFilesDirectory"));
    auto* native_set_external_directory =
        reinterpret_cast<NativeSetStringParamFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeSetExternalDirectory"));
    auto* native_set_preferences_file =
        reinterpret_cast<NativeSetStringParamFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeSetPreferencesFile"));
    auto* native_set_default_app_policy_file =
        reinterpret_cast<NativeSetStringParamFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeSetDefaultAppPolicyFile"));
    auto* native_set_http_client_proxy =
        reinterpret_cast<NativeSetHttpClientProxyFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeSetHttpClientProxy"));
    auto* native_init_fast_log =
        reinterpret_cast<NativeNoArgFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeInitFastLog"));
    auto* native_set_multiple_cookies =
        reinterpret_cast<NativeSetTwoStringParamsFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeSetMultipleCookies"));
    auto* native_get_cookies_for_domain =
        reinterpret_cast<jnivm::RobloxCookieGetter>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeGetCookiesForDomain"));
    jni_vm->SetRobloxCookieGetter(native_get_cookies_for_domain);
    auto* native_cookie_manager_set_cookie =
        reinterpret_cast<NativeSetTwoStringParamsFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_universalapp_cookie_JNICookieManager_setCookie"));
    auto* native_set_platform_headers_with_idfa =
        reinterpret_cast<NativeSetThreeStringParamsFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeSetPlatformHeadersWithIdfa"));
    auto* native_set_user_id =
        reinterpret_cast<NativeSetStringParamFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_"
            "nativeSetUserId"));
    auto* native_init_asset_manager =
        reinterpret_cast<NativeObjectInitFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_client_JNIAAssetManagerSetup_initNative"));
    auto* native_init_storage_manager =
        reinterpret_cast<NativeInitStorageManagerFn>(
            linker::ResolveSymbol(roblox_handle,
                                  "Java_com_roblox_client_LocalStorageManager_"
                                  "initStorageManagerNativeV3"));
    auto* native_local_storage_set_platform_impl =
        reinterpret_cast<NativeSetPlatformImplFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_protocols_localstorageplatforminterface_"
            "generated_ILocalStorageHandlerCore_setPlatformImpl"));
    auto* native_set_init_params =
        reinterpret_cast<NativeAppBridgeSetInitParamsFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_client_startup_MainGameActivity_"
            "nativeAppBridgeSetInitParams"));
    auto* native_retry_init = reinterpret_cast<NativeNoArgFn>(
        linker::ResolveSymbol(roblox_handle,
                              "Java_com_roblox_client_startup_MainGameActivity_"
                              "nativeRetryInit"));
    auto* native_update_screen_orientation =
        reinterpret_cast<NativeUpdateScreenOrientationFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeInputInterface_"
            "nativeUpdateScreenOrientation"));
    auto* native_update_app_ui_sizes =
        reinterpret_cast<NativeUpdateAppUiSizesFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeGLInterface_updateAppUISizes"));
    auto* native_on_fragment_start = reinterpret_cast<NativeNoArgFn>(
        linker::ResolveSymbol(roblox_handle,
                              "Java_com_roblox_engine_jni_NativeGLInterface_"
                              "nativeOnFragmentStart"));
    auto* native_pass_current_display_refresh_rate =
        reinterpret_cast<NativePassCurrentDisplayRefreshRateFn>(
            linker::ResolveSymbol(
                roblox_handle,
                "Java_com_roblox_engine_jni_NativeGLInterface_"
                "nativePassCurrentDisplayRefreshRate"));
    auto* native_pass_supported_refresh_rates =
        reinterpret_cast<NativePassSupportedRefreshRatesFn>(
            linker::ResolveSymbol(
                roblox_handle,
                "Java_com_roblox_engine_jni_NativeGLInterface_"
                "nativePassSupportedRefreshRates"));
    auto* native_set_task_scheduler_background_mode =
        reinterpret_cast<NativeSetTaskSchedulerBackgroundModeFn>(
            linker::ResolveSymbol(
                roblox_handle,
                "Java_com_roblox_engine_jni_NativeGLInterface_"
                "setTaskSchedulerBackgroundMode"));
    auto* native_send_app_ready = reinterpret_cast<NativeSendAppReadyFn>(
        linker::ResolveSymbol(roblox_handle,
                              "Java_com_roblox_engine_jni_NativeGLInterface_"
                              "nativeAppBridgeV2SendAppEventOnAppReady"));
    auto* native_send_game_loaded = reinterpret_cast<NativeSendGameLoadedFn>(
        linker::ResolveSymbol(roblox_handle,
                              "Java_com_roblox_engine_jni_NativeGLInterface_"
                              "nativeAppBridgeV2SendAppEventOnGameLoaded"));
    auto* native_set_asset_path = reinterpret_cast<NativeSetStringParamFn>(
        linker::ResolveSymbol(roblox_handle,
                              "Java_com_roblox_client_startup_MainGameActivity_"
                              "nativeSetAssetPath"));
    auto resolve_activity_lifecycle =
        [&](const char* method_name) -> NativeActivityLifecycleStringFn {
      std::string symbol =
          "Java_com_roblox_universalapp_activitylifecyclecallbacks_"
          "JNIActivityLifecycleCallbacks_";
      symbol += method_name;
      return reinterpret_cast<NativeActivityLifecycleStringFn>(
          linker::ResolveSymbol(roblox_handle, symbol.c_str()));
    };
    NativeActivityLifecycleCallbacks activity_lifecycle_callbacks = {
        resolve_activity_lifecycle("nativeOnPreCreated"),
        resolve_activity_lifecycle("nativeOnCreated"),
        resolve_activity_lifecycle("nativeOnPostCreated"),
        resolve_activity_lifecycle("nativeOnPreStarted"),
        resolve_activity_lifecycle("nativeOnStarted"),
        resolve_activity_lifecycle("nativeOnPostStarted"),
        resolve_activity_lifecycle("nativeOnPreResumed"),
        resolve_activity_lifecycle("nativeOnResumed"),
        resolve_activity_lifecycle("nativeOnPostResumed"),
    };
    auto* native_game_activity_init = reinterpret_cast<
        NativeGameActivityInitFn>(linker::ResolveSymbol(
        roblox_handle,
        "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode"));
    auto* native_app_lifecycle_set_active =
        reinterpret_cast<NativeNoArgFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_universalapp_applifecyclenativeadapter_"
            "JNIAppLifecycleNativeAdapter_setActive"));
    g_native_call_messages_from_main_thread =
        native_call_messages_from_main_thread;
    std::cout << "  [engine] Stage6 symbol resolve done" << std::endl;
    std::cout
        << "    native_init_with_params="
        << reinterpret_cast<const void*>(native_init_with_params) << '\n'
        << "    native_init_client_settings="
        << reinterpret_cast<const void*>(native_init_client_settings) << '\n'
        << "    native_init_client_settings_signed="
        << reinterpret_cast<const void*>(native_init_client_settings_signed)
        << '\n'
        << "    native_init_client_settings_cached="
        << reinterpret_cast<const void*>(native_init_client_settings_cached)
        << '\n'
        << "    native_init_client_settings_cached_compressed="
        << reinterpret_cast<const void*>(
               native_init_client_settings_cached_compressed)
        << '\n'
        << "    native_initialize_native_flags="
        << reinterpret_cast<const void*>(native_initialize_native_flags) << '\n'
        << "    native_app_bridge_app_start="
        << reinterpret_cast<const void*>(native_app_bridge_app_start) << '\n'
        << "    native_set_is_first_install="
        << reinterpret_cast<const void*>(native_set_is_first_install) << '\n'
        << "    native_update_adapter_init="
        << reinterpret_cast<const void*>(native_update_adapter_init) << '\n'
        << "    native_update_app_ui_sizes="
        << reinterpret_cast<const void*>(native_update_app_ui_sizes) << '\n'
        << "    native_update_surface_app="
        << reinterpret_cast<const void*>(native_update_surface_app) << '\n'
        << "    native_start_app_with_params="
        << reinterpret_cast<const void*>(native_start_app_with_params) << '\n'
        << "    native_send_app_ready="
        << reinterpret_cast<const void*>(native_send_app_ready) << '\n'
        << "    native_send_game_loaded="
        << reinterpret_cast<const void*>(native_send_game_loaded) << '\n'
        << "    native_start_lua_app_dm="
        << reinterpret_cast<const void*>(native_start_lua_app_dm) << '\n'
        << "    native_call_messages_from_main_thread="
        << reinterpret_cast<const void*>(native_call_messages_from_main_thread)
        << '\n'
        << "    native_base_url_protocol_init="
        << reinterpret_cast<const void*>(native_base_url_protocol_init) << '\n'
        << "    native_set_device_info="
        << reinterpret_cast<const void*>(native_set_device_info) << '\n'
        << "    native_override_channel_platform_name="
        << reinterpret_cast<const void*>(native_override_channel_platform_name)
        << '\n'
        << "    native_set_roblox_version="
        << reinterpret_cast<const void*>(native_set_roblox_version) << '\n'
        << "    native_set_http_client_proxy="
        << reinterpret_cast<const void*>(native_set_http_client_proxy) << '\n'
        << "    native_init_fast_log="
        << reinterpret_cast<const void*>(native_init_fast_log) << '\n'
        << "    native_set_multiple_cookies="
        << reinterpret_cast<const void*>(native_set_multiple_cookies) << '\n'
        << "    native_cookie_manager_set_cookie="
        << reinterpret_cast<const void*>(native_cookie_manager_set_cookie)
        << '\n'
        << "    native_set_platform_headers_with_idfa="
        << reinterpret_cast<const void*>(native_set_platform_headers_with_idfa)
        << '\n'
        << "    native_local_storage_set_platform_impl="
        << reinterpret_cast<const void*>(native_local_storage_set_platform_impl)
        << '\n'
        << "    native_retry_init="
        << reinterpret_cast<const void*>(native_retry_init) << '\n'
        << "    native_set_asset_path="
        << reinterpret_cast<const void*>(native_set_asset_path) << '\n'
        << "    activity_on_pre_created="
        << reinterpret_cast<const void*>(
               activity_lifecycle_callbacks.on_pre_created)
        << '\n'
        << "    activity_on_created="
        << reinterpret_cast<const void*>(
               activity_lifecycle_callbacks.on_created)
        << '\n'
        << "    activity_on_post_created="
        << reinterpret_cast<const void*>(
               activity_lifecycle_callbacks.on_post_created)
        << '\n'
        << "    activity_on_pre_started="
        << reinterpret_cast<const void*>(
               activity_lifecycle_callbacks.on_pre_started)
        << '\n'
        << "    activity_on_started="
        << reinterpret_cast<const void*>(
               activity_lifecycle_callbacks.on_started)
        << '\n'
        << "    activity_on_post_started="
        << reinterpret_cast<const void*>(
               activity_lifecycle_callbacks.on_post_started)
        << '\n'
        << "    activity_on_pre_resumed="
        << reinterpret_cast<const void*>(
               activity_lifecycle_callbacks.on_pre_resumed)
        << '\n'
        << "    activity_on_resumed="
        << reinterpret_cast<const void*>(
               activity_lifecycle_callbacks.on_resumed)
        << '\n'
        << "    activity_on_post_resumed="
        << reinterpret_cast<const void*>(
               activity_lifecycle_callbacks.on_post_resumed)
        << '\n'
        << "    native_game_activity_init="
        << reinterpret_cast<const void*>(native_game_activity_init) << '\n'
        << "    native_app_lifecycle_set_active="
        << reinterpret_cast<const void*>(native_app_lifecycle_set_active)
        << '\n'
        << "    native_on_fragment_start="
        << reinterpret_cast<const void*>(native_on_fragment_start) << '\n'
        << "    native_pass_supported_refresh_rates="
        << reinterpret_cast<const void*>(native_pass_supported_refresh_rates)
        << '\n'
        << "    native_pass_current_display_refresh_rate="
        << reinterpret_cast<const void*>(
               native_pass_current_display_refresh_rate)
        << '\n'
        << std::flush;
    void* should_display_opengl_unsupported_message =
        linker::ResolveSymbol(roblox_handle,
                              "Java_com_roblox_engine_jni_NativeGLInterface_"
                              "shouldDisplayOpenGLUnsupportedMessage");
    void* message_bus_publish_response_raw = linker::ResolveSymbol(
        roblox_handle,
        "Java_com_roblox_universalapp_messagebus_MessageBus_"
        "publishProtocolMethodResponseRaw");

    if ((native_init_client_settings == nullptr &&
         native_init_client_settings_signed == nullptr &&
         native_init_client_settings_cached == nullptr &&
         native_init_client_settings_cached_compressed == nullptr) ||
        native_post_client_settings == nullptr ||
        native_set_base_url == nullptr || native_set_device_info == nullptr ||
        native_set_roblox_channel == nullptr ||
        native_set_exception_reason_filename == nullptr ||
        native_set_base_data_directories == nullptr ||
        native_set_cache_directory == nullptr ||
        native_set_files_directory == nullptr ||
        native_set_external_directory == nullptr ||
        native_set_preferences_file == nullptr ||
        native_set_default_app_policy_file == nullptr ||
        native_set_http_client_proxy == nullptr ||
        native_init_fast_log == nullptr ||
        native_set_multiple_cookies == nullptr ||
        native_set_platform_headers_with_idfa == nullptr ||
        native_init_storage_manager == nullptr ||
        native_set_init_params == nullptr || native_set_asset_path == nullptr ||
        activity_lifecycle_callbacks.on_created == nullptr ||
        activity_lifecycle_callbacks.on_started == nullptr ||
        activity_lifecycle_callbacks.on_resumed == nullptr ||
        message_bus_publish_response_raw == nullptr) {
      std::cerr << "\n[FATAL] One or more NativeGL startup symbols were not "
                << "found.\n";
      return EXIT_FAILURE;
    }

    if (build_profile.allow_legacy_binary_patches) {
      PatchNullSurfaceAppCrash(
          reinterpret_cast<void*>(native_start_app_with_params));
      PatchFontTableClassifierCrash(g_libroblox_base);
      PatchPostClientSettingsTelemetryCrash(g_libroblox_base);
      PatchRobloxStackCheckBranches(g_libroblox_base);
      PatchStage6StackCheckExceptionLandings(g_libroblox_base);
      PatchEmutlsZeroInitializerMemset(g_libroblox_base);
      PatchStage6ProtectedLockCmpxchgLoop(g_libroblox_base);
      PatchStage6UnalignedStackMovaps(g_libroblox_base);
      PatchStage6MessageBusSelfReferenceCallback(g_libroblox_base);
      PatchStage6MessagePumpReverseCopy(g_libroblox_base);
      PatchStage6GlUnsupportedMessageHelpers(g_libroblox_base);
      PatchStage6PlatformHeadersLookupReturnEmptyEntry(g_libroblox_base);
      PatchStage6StartGameAssetAtIndexClamp(g_libroblox_base);
      PatchStage6StartLuaOptionalStringLookups(g_libroblox_base);
      PatchStage6StartLuaNullAppStateGuard(g_libroblox_base);
      PatchStage6StartLuaForceLoggedInBranch(g_libroblox_base);
      PatchStage6StartLuaForceAltSetupBranch(g_libroblox_base);
      PatchStage6StartLuaGateForceDeep(g_libroblox_base);
      PatchStage6StartLuaDmForceSameThread(g_libroblox_base);
      PatchStage6StartLuaGateTrace(g_libroblox_base);
      PatchStage6StartLuaSingleSurfaceEntrySetup(g_libroblox_base);
      PatchStage6StartLuaAppDMGlobalLoadSetup(g_libroblox_base);
      PatchStage6StartLuaDeepEntryTrace(g_libroblox_base);
      PatchStage6StartLuaUserDidLoginStateLoadRecovery(g_libroblox_base);
      PatchStage6StartLuaRefcountReleaseTrace(g_libroblox_base);
      PatchStage6AppBridgeV2OwnerInitTrace(g_libroblox_base);
      PatchStage6AsyncAppBridgeHashAllocationFallback(g_libroblox_base);
      PatchStage6AppBridgeVectorAllocationFallback(g_libroblox_base);
      PatchStage6StartAppParamsAllocationFallback(g_libroblox_base);
      PatchStage6StartAppInstanceArgTrace(g_libroblox_base);
      PatchStage6DataModelPatchHelperReturnTrace(g_libroblox_base);
      PatchStage6DataModelPatchTerminalTrace(g_libroblox_base);
      PatchStage6RbxmNameSlotApplyRepair(g_libroblox_base);
      PatchStage6DataModelPatchLoadStepTrace(g_libroblox_base);
      PatchStage6AsyncAppBridgeXmlDeserializeError(g_libroblox_base);
      PatchStage6SystemDialogFormatHelperReturnFalse(g_libroblox_base);
      PatchStage6PlatformHeaderParseStackFailLanding(g_libroblox_base);
      InstallStage6AsyncAppBridgeXmlNameStringFallbacks(g_libroblox_base);
      PatchStage6AsyncAppBridgeOptionalContextFlag(g_libroblox_base);
      PatchNativeUpdateScreenOrientationSetupTrace(g_libroblox_base);
      PatchStage6EnableDmNotificationMonitorFlag(g_libroblox_base);
      PatchStage6EnableDmNotificationMonitorTrace(g_libroblox_base);
      InstallStage6StartLuaRegistryFallback(g_libroblox_base);
      InstallStage6RbxmFileManagerCacheRegistryFallback(g_libroblox_base);
      InstallStage6RbxmFileManagerFeatureRegistryFallback(g_libroblox_base);
      InstallStage6RbxmReflectionDescriptorFallback(g_libroblox_base);
      InstallStage6RbxmCoreClassRegistryFallback(g_libroblox_base);
      InstallSkippedRobloxHeadlessSingletonFallback(g_libroblox_base);
      InstallStage6SystemDialogSingletonGuardFallback(g_libroblox_base);
      InstallStage6SystemDialogDependencySingletonGuardFallback(
          g_libroblox_base);
      InstallStage6SystemDialogDescriptorFallbacks(g_libroblox_base);
      InstallStage6IxpDescriptorFallbacks(g_libroblox_base);
      InstallStage6DataModelPatchAnalyticsDescriptorFallbacks(g_libroblox_base);
      InstallStage6DataModelPatchTelemetryDescriptorFallbacks(g_libroblox_base);
      ForceStage6DeferRbxmSignatureCheckToPostTtiFlag(
          "after-datamodel-patch-telemetry-descriptor");
      InstallRobloxUrlStringFallbacks(g_libroblox_base);
      PatchStage6OpenGLUnsupportedMessageCounter(g_libroblox_base);
      PatchStage6GlHelperStateSlot(g_libroblox_base);
      PatchStage6GlQueueTrace(g_libroblox_base);
      PatchStage6GlPollReturn(g_libroblox_base);
      PatchStage6GlInfiniteWait(g_libroblox_base);
      PatchStage6GlWaitReturn(g_libroblox_base);
      PatchStage6GlTimedWaitReturnFalse(g_libroblox_base);
      PatchStage6GlConditionWaitWrapperReturnSuccess(g_libroblox_base);
      PatchStage6GlQueuePopReturnEmpty(g_libroblox_base);
      PatchStage6GlQueueTransferReturnFalse(g_libroblox_base);
      PatchStage6GlQueueCallbackTailReturnEmpty(g_libroblox_base);
      PatchStage6GlQueueDrainReturnFalse(g_libroblox_base);
      PatchStage6FmodRetryCount(g_libroblox_base);
      PatchStage6FmodInitTrace(g_libroblox_base);
      PatchStage6FmodCreateGroupTrace(g_libroblox_base);
      PatchStage6FmodNativeAudioDeviceGroupFailureLog(g_libroblox_base);
      PatchStage6FmodNativeAudioDeviceChangedNoOp(g_libroblox_base);
      PatchStage6FmodErrorTrace(g_libroblox_base);
      PatchStage6RslReleaseCountPanic(g_libroblox_base);
      PatchStage6TextboxSyncNullString(g_libroblox_base);
      InitializeSystemDialogHandlerFallback(
          reinterpret_cast<void*>(native_start_app_with_params));
      PatchSystemDialogPlatformCalls(
          reinterpret_cast<void*>(native_start_app_with_params));
      PatchShouldDisplayOpenGLUnsupportedMessage(
          should_display_opengl_unsupported_message);
      if (is_headless) {
        PatchHeadlessUpdateAdapterInit(
            reinterpret_cast<void*>(native_start_app_with_params));
        PatchHeadlessNullIndexBufferWrite(
            reinterpret_cast<void*>(native_start_app_with_params));
        PatchHeadlessMessageBusJavaPublish(message_bus_publish_response_raw);
        PatchHeadlessNullUtf16CopyWrite(
            reinterpret_cast<void*>(native_start_app_with_params));
        PatchStartAppDebugTrap(
            reinterpret_cast<void*>(native_start_app_with_params));
      } else {
        std::cout << "  [engine] skipping headless compatibility patches\n"
                  << std::flush;
      }
    } else {
      std::cout << "  [compat] skipped fixed-offset Stage 6 patch set\n"
                << std::flush;
    }
    // Real startup calls run by default; signal recovery falls back to the
    // Mocktail implementations.
    const bool requested_set_asset_path =
        ShouldRunStartupStep("MOCKTAIL_STEP_SET_ASSET_PATH", true);
    const bool requested_init_with_params =
        ShouldRunStartupStep("MOCKTAIL_STEP_INIT_WITH_PARAMS", true);
    const bool requested_start_app_with_params =
        ShouldRunStartupStep("MOCKTAIL_STEP_START_APP_WITH_PARAMS", true);
    const bool run_set_asset_path = requested_set_asset_path;
    const bool call_real_set_asset_path =
        run_set_asset_path &&
        !IsDisabled("MOCKTAIL_CALL_REAL_NATIVE_SET_ASSET_PATH");
    const bool run_global_init =
        ShouldRunStartupStep("MOCKTAIL_STEP_GAME_GLOBAL_INIT",
                             !IsDisabled("MOCKTAIL_GAME_GLOBAL_INIT"));
    const bool run_init_client_settings =
        ShouldRunStartupStep("MOCKTAIL_STEP_INIT_CLIENT_SETTINGS",
                             !IsDisabled("MOCKTAIL_INIT_CLIENT_SETTINGS"));
    const bool run_post_client_settings =
        ShouldRunStartupStep("MOCKTAIL_STEP_POST_CLIENT_SETTINGS",
                             IsEnabled("MOCKTAIL_POST_CLIENT_SETTINGS"));
    const bool run_app_bridge_app_start =
        ShouldRunStartupStep("MOCKTAIL_STEP_APP_BRIDGE_APP_START",
                             IsEnabled("MOCKTAIL_APP_BRIDGE_APP_START")) &&
        native_app_bridge_app_start != nullptr;
    const bool run_init_with_params = requested_init_with_params;
    const bool call_real_init_with_params =
        run_init_with_params &&
        !IsDisabled("MOCKTAIL_CALL_REAL_APP_BRIDGE_INIT");
    const bool run_update_screen_orientation =
        ShouldRunStartupStep("MOCKTAIL_STEP_UPDATE_SCREEN_ORIENTATION",
                             IsEnabled("MOCKTAIL_UPDATE_SCREEN_ORIENTATION")) &&
        native_update_screen_orientation != nullptr;
    const bool run_update_surface_app =
        ShouldRunStartupStep("MOCKTAIL_STEP_UPDATE_SURFACE_APP",
                             IsEnabled("MOCKTAIL_UPDATE_SURFACE_APP"));
    const bool call_real_update_surface_app =
        run_update_surface_app &&
        IsEnabled("MOCKTAIL_CALL_REAL_APP_BRIDGE_UPDATE_SURFACE");
    const bool run_activity_lifecycle =
        ShouldRunStartupStep("MOCKTAIL_STEP_ACTIVITY_LIFECYCLE",
                             IsEnabled("MOCKTAIL_ACTIVITY_LIFECYCLE"));
    const bool run_game_activity_init = ShouldRunStartupStep(
        "MOCKTAIL_STEP_GAME_ACTIVITY_INIT",
        has_window || IsEnabled("MOCKTAIL_GAME_ACTIVITY_INIT"));
    const bool run_set_init_params = ShouldRunStartupStep(
        "MOCKTAIL_STEP_SET_INIT_PARAMS",
        IsEnabled("MOCKTAIL_SET_INIT_PARAMS") ||
            (run_game_activity_init && !run_init_with_params));
    const bool run_game_activity_surface =
        ShouldRunStartupStep("MOCKTAIL_STEP_GAME_ACTIVITY_SURFACE",
                             has_window && run_game_activity_init &&
                                 IsEnabled("MOCKTAIL_GAME_ACTIVITY_SURFACE"));
    const bool run_app_lifecycle_active = ShouldRunStartupStep(
        "MOCKTAIL_STEP_APP_LIFECYCLE_ACTIVE",
        has_window && IsEnabled("MOCKTAIL_APP_LIFECYCLE_ACTIVE"));
    const bool run_native_fragment_start =
        ShouldRunStartupStep(
            "MOCKTAIL_STEP_NATIVE_FRAGMENT_START",
            has_window && IsEnabled("MOCKTAIL_NATIVE_FRAGMENT_START")) &&
        native_on_fragment_start != nullptr;
    const bool run_display_refresh_rate =
        ShouldRunStartupStep(
            "MOCKTAIL_STEP_PASS_CURRENT_DISPLAY_REFRESH_RATE",
            has_window &&
                IsEnabled("MOCKTAIL_PASS_CURRENT_DISPLAY_REFRESH_RATE")) &&
        (native_pass_current_display_refresh_rate != nullptr ||
         native_pass_supported_refresh_rates != nullptr);
    const bool run_start_app_with_params = requested_start_app_with_params;
    // Keep the real call opt-in because it can block before rendering starts.
    const bool call_real_start_app_with_params =
        run_start_app_with_params &&
        IsEnabled("MOCKTAIL_CALL_REAL_APP_BRIDGE_START");
    const bool run_start_lua_app_dm =
        ShouldRunStartupStep("MOCKTAIL_STEP_START_LUA_APP_DM",
                             IsEnabled("MOCKTAIL_START_LUA_APP_DM"));
    const bool run_native_settings =
        ShouldRunStartupStep("MOCKTAIL_STEP_NATIVE_SETTINGS",
                             run_init_with_params || run_start_app_with_params);
    const bool run_prepare_jni = ShouldRunStartupStep(
        "MOCKTAIL_STEP_PREP_JNI",
        run_set_asset_path || run_global_init || run_init_client_settings ||
            run_post_client_settings || run_app_bridge_app_start ||
            run_native_settings || run_set_init_params ||
            run_init_with_params || run_update_screen_orientation ||
            run_activity_lifecycle || run_game_activity_init ||
            run_game_activity_surface || run_app_lifecycle_active ||
            run_native_fragment_start || run_display_refresh_rate ||
            run_update_surface_app || run_start_app_with_params ||
            run_start_lua_app_dm);

    PrintStepDecision("prepare JNI args", run_prepare_jni);
    PrintStepDecision("nativeSetAssetPath", run_set_asset_path);
    PrintStepDecision("nativeGameGlobalInit", run_global_init);
    PrintStepDecision("nativeInitClientSettings", run_init_client_settings);
    PrintStepDecision("nativePostClientSettingsLoadedInitialization3",
                      run_post_client_settings);
    PrintStepDecision("GameActivity.initializeNativeCode",
                      run_game_activity_init);
    PrintStepDecision("NativeSettings directories", run_native_settings);
    PrintStepDecision("nativeAppBridgeSetInitParams", run_set_init_params);
    PrintStepDecision("nativeAppBridgeV2InitWithParams", run_init_with_params);
    PrintStepDecision("nativeUpdateScreenOrientation",
                      run_update_screen_orientation);
    PrintStepDecision("activity lifecycle", run_activity_lifecycle);
    PrintStepDecision("nativeAppBridgeAppStart", run_app_bridge_app_start);
    PrintStepDecision("GameActivity surface callbacks",
                      run_game_activity_surface);
    PrintStepDecision("JNIAppLifecycleNativeAdapter.setActive",
                      run_app_lifecycle_active);
    PrintStepDecision("NativeGLInterface.nativeOnFragmentStart",
                      run_native_fragment_start);
    PrintStepDecision("NativeGLInterface.nativePassCurrentDisplayRefreshRate",
                      run_display_refresh_rate);
    PrintStepDecision("nativeAppBridgeStartLuaAppDM", run_start_lua_app_dm);
    PrintStepDecision("nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams",
                      run_update_surface_app);
    PrintStepDecision("nativeAppBridgeV2StartAppWithParams",
                      run_start_app_with_params);
    if (run_set_asset_path && !call_real_set_asset_path) {
      PrintNativeBypass("nativeSetAssetPath",
                        "MOCKTAIL_CALL_REAL_NATIVE_SET_ASSET_PATH");
    }
    if (run_init_with_params && !call_real_init_with_params) {
      PrintNativeBypass("nativeAppBridgeV2InitWithParams",
                        "MOCKTAIL_CALL_REAL_APP_BRIDGE_INIT");
    }
    if (run_update_surface_app && !call_real_update_surface_app) {
      PrintNativeBypass("nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams",
                        "MOCKTAIL_CALL_REAL_APP_BRIDGE_UPDATE_SURFACE");
    }
    if (run_start_app_with_params && !call_real_start_app_with_params) {
      PrintNativeBypass("nativeAppBridgeV2StartAppWithParams",
                        "MOCKTAIL_CALL_REAL_APP_BRIDGE_START");
    }
    std::cout << "  [engine] startup decisions resolved. "
              << "run_prepare_jni=" << (run_prepare_jni ? 1 : 0) << '\n'
              << "  run_init_with_params=" << (run_init_with_params ? 1 : 0)
              << " run_start_app_with_params="
              << (run_start_app_with_params ? 1 : 0) << '\n'
              << std::flush;

    if (experience_game_symbols != nullptr) {
      mocktail::runtime::RobloxExperienceMessageBusSymbols message_bus_symbols;
      message_bus_symbols.get_launch_id =
          reinterpret_cast<mocktail::runtime::GetExperienceLaunchIdFn>(
              linker::ResolveSymbol(roblox_handle,
                                    "Java_com_roblox_universalapp_experience_"
                                    "JNIExperienceProtocol_getLaunchId"));
      message_bus_symbols.subscribe_raw =
          reinterpret_cast<mocktail::runtime::SubscribeExperienceLaunchRawFn>(
              linker::ResolveSymbol(
                  roblox_handle,
                  "Java_com_roblox_universalapp_messagebus_MessageBus_"
                  "doSubscribeRaw"));
      message_bus_symbols.delete_connection =
          reinterpret_cast<mocktail::runtime::DeleteMessageBusConnectionFn>(
              linker::ResolveSymbol(
                  roblox_handle,
                  "Java_com_roblox_universalapp_messagebus_Connection_"
                  "deleteSharedPtr"));
      const mocktail::runtime::RobloxPlatformWebSymbols platform_web_symbols =
          mocktail::runtime::ResolveRobloxPlatformWebSymbols(
              roblox_handle, message_bus_symbols.subscribe_raw,
              message_bus_symbols.delete_connection);
      mocktail::runtime::RobloxExperienceJniFactory jni_factory{
          jni_vm.get(),
          &CreateExperienceRawCallback,
          &ClearExperienceRawCallback,
          &CreateMessageBusRequestHandler,
          &ClearMessageBusRequestHandler,
          &CreateBrowserServiceMemStorageCallback,
          &ClearBrowserServiceMemStorageCallback,
          &mocktail::runtime::SetJnivmPlatformWebCallbacks,
          &mocktail::runtime::ClearJnivmPlatformWebCallbacks};
      mocktail::runtime::RobloxFreshLaunchPresentBoundary present_boundary{
          &game_present_observer, &RegisterFreshGamePresentObserver,
          &ClearFreshGamePresentObserver};
      mocktail::runtime::RobloxGameSurfaceJniConfig surface_config;
      surface_config.asset_folder_path = DefaultAssetPath();
      surface_config.is_touch_device = input_capabilities.touch_enabled;
      surface_config.is_mouse_device = input_capabilities.mouse_enabled;
      surface_config.is_keyboard_device = input_capabilities.keyboard_enabled;
      mocktail::runtime::JniEnvironmentProvider environment{
          raw_vm, jni_vm.get(), &RestoreGameSessionJniEnvironment};
      experience_composition =
          std::make_shared<mocktail::runtime::RobloxExperienceComposition>(
              environment, message_bus_symbols, platform_web_symbols.web_view,
              platform_web_symbols.browser_service, *experience_game_symbols,
              jni_factory, present_boundary, std::move(surface_config),
              &dependencies.roblox_credential(),
              mocktail::runtime::RobloxExperienceSurfaceProvider{},
              discord_rpc.observer(),
              dependencies.clear_persisted_web_view_cookie());
      const mocktail::Status platform_protocol_status =
          experience_composition->InitializePlatformProtocols();
      if (!platform_protocol_status.ok()) {
        std::cerr << "[FATAL] Roblox platform protocols did not initialize: "
                  << platform_protocol_status.message() << '\n';
        return EXIT_FAILURE;
      }
      std::cout << "  [platform] WebView and BrowserService protocols "
                   "initialized before native bootstrap\n"
                << std::flush;
    }

    EngineStartupContext startup_context_value = {
        jni_vm.get(),
        raw_vm,
        dependencies.account_identity(),
        &dependencies.roblox_credential(),
        game_session_runtime.get(),
        run_prepare_jni,
        run_set_asset_path,
        call_real_set_asset_path,
        run_global_init,
        run_init_client_settings,
        run_post_client_settings,
        run_app_bridge_app_start,
        run_native_settings,
        run_set_init_params,
        run_init_with_params,
        call_real_init_with_params,
        run_update_screen_orientation,
        run_update_surface_app,
        call_real_update_surface_app,
        run_start_app_with_params,
        call_real_start_app_with_params,
        run_activity_lifecycle,
        run_game_activity_init,
        run_game_activity_surface,
        run_app_lifecycle_active,
        run_native_fragment_start,
        run_display_refresh_rate,
        run_start_lua_app_dm,
        native_global_init,
        native_init_client_settings,
        native_init_client_settings_signed,
        native_init_client_settings_cached,
        native_init_client_settings_cached_compressed,
        native_post_client_settings,
        native_initialize_native_flags,
        native_app_bridge_app_start,
        native_set_is_first_install,
        native_set_base_url,
        native_set_device_info,
        native_base_url_protocol_init,
        native_set_roblox_channel,
        native_override_channel_platform_name,
        native_set_roblox_version,
        native_set_exception_reason_filename,
        native_set_base_data_directories,
        native_set_cache_directory,
        native_set_files_directory,
        native_set_external_directory,
        native_set_preferences_file,
        native_set_default_app_policy_file,
        native_set_http_client_proxy,
        native_init_fast_log,
        native_set_multiple_cookies,
        native_cookie_manager_set_cookie,
        native_set_platform_headers_with_idfa,
        native_set_user_id,
        native_init_asset_manager,
        native_init_storage_manager,
        native_local_storage_set_platform_impl,
        native_set_init_params,
        native_retry_init,
        native_init_with_params,
        native_update_adapter_init,
        native_update_screen_orientation,
        native_update_app_ui_sizes,
        native_set_task_scheduler_background_mode,
        native_update_surface_app,
        native_start_app_with_params,
        native_send_app_ready,
        native_send_game_loaded,
        native_set_asset_path,
        activity_lifecycle_callbacks,
        native_game_activity_init,
        native_app_lifecycle_set_active,
        native_on_fragment_start,
        native_pass_supported_refresh_rates,
        native_pass_current_display_refresh_rate,
        native_start_lua_app_dm};
    if (game_session_runtime != nullptr) {
      if (!game_present_observer.Register(
              &mocktail::runtime::RobloxGameSessionRuntime::
                  SuccessfulPresentCallback,
              game_session_runtime.get())) {
        std::cerr << "[FATAL] Could not register the GAME host present "
                     "observer\n";
        return EXIT_FAILURE;
      }
      std::cout << "  [game-session] host present observer registered\n"
                << std::flush;
    }
    if (IsEnabled("MOCKTAIL_ENGINE_DETACH")) {
      std::cerr << "[FATAL] Detached startup workers are unsupported because "
                   "their runtime ownership cannot be proven\n";
      return EXIT_FAILURE;
    }
    EngineStartupContext* startup_context = &startup_context_value;

    if (IsEnabled("MOCKTAIL_ENGINE_INLINE")) {
      std::cout << "  [engine] starting inline startup path\n" << std::flush;
      EngineStartupThread(startup_context);
    } else {
      std::cout << "  [engine] starting pthread startup path\n" << std::flush;
      std::cout << "  [engine] creating startup thread\n" << std::flush;
      mocktail::runtime::OwnedPthread startup_thread;
      const int create_result = startup_thread.Start(
          &EngineStartupThread, startup_context, GetEngineStackSize());
      std::cout << "  [engine] startup thread create result=" << create_result
                << '\n'
                << std::flush;
      if (create_result != 0) {
        std::cerr << "\n[FATAL] Could not create engine startup thread: "
                  << create_result << '\n';
        return EXIT_FAILURE;
      }

      const int startup_timeout_ms =
          GetEnvInt("MOCKTAIL_STARTUP_THREAD_TIMEOUT_MS", 0);
      if (startup_timeout_ms <= 0) {
        std::cout << "  [engine] joining startup thread\n" << std::flush;
      } else {
        std::cout << "  [engine] joining startup thread with timeout "
                  << startup_timeout_ms << "ms\n"
                  << std::flush;
      }

      const mocktail::runtime::OwnedPthreadWaitResult wait_result =
          startup_thread.WaitFor(
              startup_timeout_ms > 0 ? startup_timeout_ms : -1, 10,
              &PumpStartupOwnerThread);
      if (!wait_result.joined()) {
        if (wait_result.status ==
            mocktail::runtime::OwnedPthreadWaitStatus::kTimedOut) {
          std::cerr << "  [engine] startup thread timed out after "
                    << startup_timeout_ms
                    << "ms, cancelling and requiring a physical join\n"
                    << std::flush;
          for (int dump_index = 0; dump_index < 3; ++dump_index) {
            startup_thread.Signal(SIGUSR1);
            usleep(100 * 1000);
          }
        } else {
          std::cerr << "  [engine] startup thread join failed: "
                    << wait_result.platform_error << '\n'
                    << std::flush;
        }

        const int cancel_join_grace_ms =
            std::clamp(GetEnvInt("MOCKTAIL_STARTUP_CANCEL_JOIN_GRACE_MS", 5000),
                       100, 60000);
        const mocktail::runtime::OwnedPthreadCancelResult cancel_result =
            startup_thread.CancelAndJoinFor(cancel_join_grace_ms, 10,
                                            &PumpStartupOwnerThread);
        if (cancel_result.cancel_error != 0) {
          std::cerr << "  [engine] startup thread cancel failed: "
                    << cancel_result.cancel_error << '\n'
                    << std::flush;
        }
        std::cerr << "[FATAL] Startup worker did not complete normally; "
                  << cancel_join_grace_ms << "ms cancel/join grace result: "
                  << mocktail::runtime::OwnedPthreadWaitStatusName(
                         cancel_result.wait.status)
                  << " error=" << cancel_result.wait.platform_error
                  << ". Native rollback is unproven even after a physical "
                     "join; terminating without RAII unwind\n"
                  << std::flush;
        std::_Exit(EXIT_FAILURE);
      }
    }
    if (game_session_runtime != nullptr &&
        !game_session_runtime->startup_status().ok()) {
      std::cerr << "[FATAL] Typed GAME lifecycle did not start: "
                << game_session_runtime->startup_status().message() << '\n';
      return EXIT_FAILURE;
    }
    if (experience_composition != nullptr &&
        dependencies.account_identity().user_id > 0) {
      const mocktail::window::WindowSurfaceSnapshot window_surface =
          mocktail::window::GetWindowSurfaceSnapshot();
      mocktail::runtime::RobloxLuaAppExperienceReadiness readiness;
      readiness.principal.kind =
          mocktail::runtime::GameSessionPrincipalKind::kAuthenticated;
      readiness.principal.generation = 1;
      readiness.principal.principal_id =
          std::to_string(dependencies.account_identity().user_id);
      readiness.principal.base_url = "https://www.roblox.com/";
      readiness.surface = {window_surface.generation,
                           window_surface.native_window, window_surface.width,
                           window_surface.height};
      readiness.username = dependencies.account_identity().username;
      const mocktail::Status experience_status =
          experience_composition->OnLuaAppReady(std::move(readiness));
      if (!experience_status.ok()) {
        std::cerr << "[FATAL] Dynamic ExperienceProtocol did not initialize: "
                  << experience_status.message() << '\n';
        return EXIT_FAILURE;
      }
      experience_lifecycle_target = std::make_shared<ExperienceLifecycleTarget>(
          ExperienceLifecycleTarget{experience_composition});
      jni_vm->SetRobloxExperienceLifecycleCallbacks(
          experience_lifecycle_target,
          jnivm::RobloxExperienceLifecycleCallbacks{&NotifyLuaAppDidReturn});
      std::cout << "  [experience] subscribed to dynamic launch requests\n"
                << std::flush;
    } else if (experience_composition != nullptr) {
      std::cout << "  [experience] dynamic launch subscription awaits an "
                   "authenticated identity\n"
                << std::flush;
    }
    if (mocktail::window::IsInitialised()) {
      mocktail::runtime::JniEnvironmentProvider input_environment{
          raw_vm, jni_vm.get(), &RestoreGameSessionJniEnvironment};
      window_input_runtime =
          std::make_shared<mocktail::runtime::RobloxWindowInputRuntime>(
              input_environment, roblox_capabilities.input);
      const mocktail::Status input_status = window_input_runtime->Initialize();
      if (!input_status.ok()) {
        std::cerr << "[FATAL] Typed production input did not initialize: "
                  << input_status.message() << '\n';
        return EXIT_FAILURE;
      }
      const mocktail::Status text_input_status =
          mocktail::runtime::RobloxTextInputJniBridge::Create(
              jni_vm.get(), window_input_runtime, &text_input_bridge);
      if (!text_input_status.ok()) {
        std::cerr << "[FATAL] Typed Roblox text input did not initialize: "
                  << text_input_status.message() << '\n';
        return EXIT_FAILURE;
      }
    }
    std::cout << "  [engine] legacy startup call sequence returned\n"
              << std::flush;
  }

  std::cout << "\n======================================================\n"
            << "  Bootstrap sequence returned; validating readiness.\n"
            << "======================================================\n";

  bool real_frame_presented = false;
  bool lifecycle_shutdown_completed = false;
  bool game_surface_events_completed = true;
  bool input_shutdown_completed = window_input_runtime == nullptr;
  bool resize_readiness_completed = true;
  if (mocktail::window::IsInitialised()) {
    std::cout << "  [main] entering SDL event loop (close the window to quit)\n"
              << std::flush;
    std::unique_ptr<mocktail::window::WindowGameSurfaceBridge> surface_bridge;
    if (game_session_runtime != nullptr) {
      surface_bridge =
          std::make_unique<mocktail::window::WindowGameSurfaceBridge>(
              mocktail::window::MakeWindowGameSurfaceEventSource(),
              mocktail::window::MakeWindowGameSurfaceConsumer(
                  game_session_runtime.get()),
              mocktail::window::MakeWindowResizeReadinessCommitObserver());
    } else if (experience_composition != nullptr &&
               experience_composition->subscribed()) {
      mocktail::window::WindowGameSurfaceConsumer experience_consumer{
          experience_composition.get(), &ExperienceSurfaceCreated,
          &ExperienceSurfaceChanged, &ExperienceSurfaceDestroyed};
      surface_bridge =
          std::make_unique<mocktail::window::WindowGameSurfaceBridge>(
              mocktail::window::MakeWindowGameSurfaceEventSource(),
              experience_consumer,
              mocktail::window::MakeWindowResizeReadinessCommitObserver());
    }
    const auto drain_game_surface_events = [&]() {
      if (surface_bridge == nullptr) {
        return true;
      }
      const mocktail::window::WindowGameSurfaceDrainResult result =
          surface_bridge->Drain();
      if (!result.ok()) {
        std::cerr << "[FATAL] Typed GAME surface bridge rejected an event: "
                  << result.status.message() << '\n';
        return false;
      }
      if (result.drained_events != 0) {
        const mocktail::window::WindowGameSurfaceBridgeSnapshot snapshot =
            surface_bridge->Snapshot();
        std::cout << "  [game-session] typed window surface events committed="
                  << result.drained_events
                  << " last_generation=" << snapshot.last_generation << '\n'
                  << std::flush;
      }
      return true;
    };
    int main_loop_ticks = 0;
    const bool fps_trace = IsEnabled("MOCKTAIL_TRACE_FPS");
    const bool trace_main_loop = IsEnabled("MOCKTAIL_TRACE_MAIN_LOOP");
    uint64_t fps_window_start_ns = fps_trace ? MonotonicNanos() : 0;
    uint64_t fps_samples = 0;
    uint64_t fps_tick_ns = 0;
    uint64_t fps_sdl_ns = 0;
    uint64_t fps_platform_ns = 0;
    uint64_t fps_engine_ns = 0;
    uint64_t fps_launch_ns = 0;
    uint64_t fps_pace_sleep_ns = 0;
    while (true) {
      const uint64_t fps_tick_start_ns = fps_trace ? MonotonicNanos() : 0;
      const bool keep_window_open = mocktail::window::PumpEvents();
      game_surface_events_completed = drain_game_surface_events();
      const uint64_t fps_after_sdl_ns = fps_trace ? MonotonicNanos() : 0;
      if (!game_surface_events_completed || !keep_window_open) {
        break;
      }
      ++main_loop_ticks;
      if (trace_main_loop &&
          (main_loop_ticks <= 10 || main_loop_ticks % 100 == 0)) {
        std::cerr << "  [main] SDL event loop tick #" << main_loop_ticks << '\n'
                  << std::flush;
      }
      RunPendingMainThreadTaskSchedulerForeground();
      if (g_pending_main_thread_start_lua_app_dm &&
          !g_pending_main_thread_start_lua_started &&
          g_vm_for_main_thread_pump && g_native_gl_class_for_main_thread &&
          MonotonicMillis() >= g_pending_main_thread_start_lua_due_ms) {
        g_pending_main_thread_start_lua_started = true;
        JNIEnv* env = AttachMainThreadJniEnv();
        if (env == nullptr) {
          continue;
        }
        if (IsEnabled("MOCKTAIL_TRACE_START_LUA_JNI")) {
          setenv("MOCKTAIL_JNI_TRACE", "1", 1);
        }
        std::cout << "  [main] delayed nativeAppBridgeStartLuaAppDM\n"
                  << std::flush;
        g_pending_main_thread_start_lua_app_dm(
            env, g_native_gl_class_for_main_thread);
        CallStartLuaDirectClosureIfRequested("main");
        std::cout << "  [main] delayed nativeAppBridgeStartLuaAppDM returned\n"
                  << std::flush;
        DumpStage6AppBridgeStaticState("after main-thread StartLuaAppDM");
      }
      if (experience_composition != nullptr) {
        const mocktail::Status platform_event_status =
            experience_composition->DrainPlatformEvents();
        if (!platform_event_status.ok()) {
          std::cerr << "[FATAL] Roblox platform event delivery failed: "
                    << platform_event_status.message() << '\n';
          game_surface_events_completed = false;
          break;
        }
      }
      const uint64_t fps_after_platform_ns = fps_trace ? MonotonicNanos() : 0;
      PumpRobloxMainThreadMessagesOnce();
      const uint64_t fps_after_engine_ns = fps_trace ? MonotonicNanos() : 0;
      if (experience_composition != nullptr &&
          experience_composition->subscribed()) {
        const mocktail::Status launch_status =
            experience_composition->DrainLaunchRequests();
        if (!launch_status.ok()) {
          std::cerr << "[FATAL] Dynamic experience launch failed: "
                    << launch_status.message() << '\n';
          game_surface_events_completed = false;
          break;
        }
      }
      const uint64_t fps_after_launch_ns = fps_trace ? MonotonicNanos() : 0;
      const uint64_t fps_pace_ns = mocktail::window::PaceInputPump();
      if (fps_trace) {
        const uint64_t fps_tick_end_ns = MonotonicNanos();
        ++fps_samples;
        fps_tick_ns += fps_tick_end_ns - fps_tick_start_ns;
        fps_sdl_ns += fps_after_sdl_ns - fps_tick_start_ns;
        fps_platform_ns += fps_after_platform_ns - fps_after_sdl_ns;
        fps_engine_ns += fps_after_engine_ns - fps_after_platform_ns;
        fps_launch_ns += fps_after_launch_ns - fps_after_engine_ns;
        fps_pace_sleep_ns += fps_pace_ns;
        if (fps_tick_end_ns - fps_window_start_ns >= 1000000000ULL &&
            fps_samples != 0) {
          std::fprintf(stderr,
                       "  [fps] main n=%llu tick=%llu us sdl=%llu us "
                       "drain=%llu us engine=%llu us launch=%llu us "
                       "pace_sleep=%llu us\n",
                       static_cast<unsigned long long>(fps_samples),
                       static_cast<unsigned long long>(fps_tick_ns /
                                                       fps_samples / 1000ULL),
                       static_cast<unsigned long long>(fps_sdl_ns /
                                                       fps_samples / 1000ULL),
                       static_cast<unsigned long long>(fps_platform_ns /
                                                       fps_samples / 1000ULL),
                       static_cast<unsigned long long>(fps_engine_ns /
                                                       fps_samples / 1000ULL),
                       static_cast<unsigned long long>(fps_launch_ns /
                                                       fps_samples / 1000ULL),
                       static_cast<unsigned long long>(fps_pace_sleep_ns /
                                                       fps_samples / 1000ULL));
          fps_window_start_ns = fps_tick_end_ns;
          fps_samples = 0;
          fps_tick_ns = 0;
          fps_sdl_ns = 0;
          fps_platform_ns = 0;
          fps_engine_ns = 0;
          fps_launch_ns = 0;
          fps_pace_sleep_ns = 0;
        }
      }
    }
    std::cout << "  [main] window closed, shutting down\n" << std::flush;
    real_frame_presented = mocktail::window::HasPresentedFrame();
    if (text_input_bridge != nullptr) {
      input_shutdown_completed = text_input_bridge->Shutdown().ok();
    }
    if (window_input_runtime != nullptr) {
      input_shutdown_completed =
          window_input_runtime->Shutdown().ok() && input_shutdown_completed;
    }
    bool experience_destroyed_app = false;
    if (experience_composition != nullptr) {
      jni_vm->ClearRobloxExperienceLifecycleCallbacks();
      experience_lifecycle_target.reset();
      const mocktail::runtime::GameSessionSnapshot snapshot =
          experience_composition->Snapshot();
      experience_destroyed_app = snapshot.game_running ||
                                 snapshot.game_paused ||
                                 snapshot.game_present_pending;
      const mocktail::Status experience_shutdown =
          experience_composition->Shutdown();
      if (experience_destroyed_app) {
        lifecycle_shutdown_completed = experience_shutdown.ok();
      }
      if (!experience_shutdown.ok()) {
        std::cerr << "  [main] ExperienceProtocol shutdown failed: "
                  << experience_shutdown.message() << '\n';
      }
    }
    if (game_session_runtime != nullptr) {
      const mocktail::runtime::GameSessionUpdateResult shutdown_result =
          game_session_runtime->Shutdown();
      lifecycle_shutdown_completed =
          shutdown_result.ok() &&
          shutdown_result.state ==
              mocktail::runtime::GameSessionState::kStopped;
      std::cout << "  [main] Roblox lifecycle shutdown: "
                << mocktail::runtime::GameSessionStateName(
                       shutdown_result.state)
                << " (" << shutdown_result.message << ")\n"
                << std::flush;
      game_present_observer.Reset();
    } else if (!experience_destroyed_app) {
      const mocktail::runtime::RobloxSymbolLookup lifecycle_lookup(
          &ResolveRobloxCapabilitySymbol, &roblox_handle);
      const mocktail::runtime::RobloxAppLifecycleResolution
          lifecycle_resolution =
              mocktail::runtime::ResolveRobloxAppLifecycleSymbols(
                  lifecycle_lookup);
      if (lifecycle_resolution.ok()) {
        JNIEnv* shutdown_env = AttachMainThreadJniEnv();
        mocktail::runtime::RobloxAppLifecycle lifecycle(
            *lifecycle_resolution.symbols());
        const mocktail::runtime::RobloxAppShutdownResult shutdown_result =
            lifecycle.Shutdown(shutdown_env, g_native_gl_class_for_main_thread);
        lifecycle_shutdown_completed = shutdown_result.ok();
        std::cout << "  [main] Roblox lifecycle shutdown: "
                  << mocktail::runtime::RobloxAppShutdownStatusName(
                         shutdown_result.status)
                  << " (" << shutdown_result.message << ")\n"
                  << std::flush;
      } else {
        std::cerr << "  [main] Roblox lifecycle resolution failed: "
                  << mocktail::runtime::RobloxAppLifecycleResolutionStatusName(
                         lifecycle_resolution.status())
                  << '\n'
                  << std::flush;
      }
    }
    const mocktail::Status resize_stop_status =
        lifecycle_shutdown_completed
            ? mocktail::window::StopResizeReadiness()
            : mocktail::window::ResizeReadinessCompletionStatus();
    const mocktail::Status resize_completion_status =
        mocktail::window::ResizeReadinessCompletionStatus();
    resize_readiness_completed =
        resize_stop_status.ok() && resize_completion_status.ok();
    if (!resize_readiness_completed) {
      const mocktail::Status& failure = resize_stop_status.ok()
                                            ? resize_completion_status
                                            : resize_stop_status;
      std::cerr << "[FATAL] Real resize readiness failed: " << failure.message()
                << '\n';
    }
    const mocktail::Status audio_shutdown_status =
        dependencies.ShutdownBeforePlatform();
    if (!audio_shutdown_status.ok()) {
      std::cerr << "  [main] audio shutdown blocked SDL teardown: "
                << audio_shutdown_status.message() << '\n';
      lifecycle_shutdown_completed = false;
    } else {
      mocktail::window::Shutdown();
    }
  } else {
    int keepalive_ms = GetEnvInt("MOCKTAIL_KEEPALIVE_MS", 0);
    if (keepalive_ms > 0) {
      std::cout << "  [main] headless keepalive: " << keepalive_ms << " ms\n"
                << std::flush;
      if (IsEnabled("MOCKTAIL_MAIN_THREAD_MESSAGE_PUMP") ||
          IsEnabled("MOCKTAIL_HEADLESS_KEEPALIVE_PUMP")) {
        int waited_ms = 0;
        constexpr int kHeadlessKeepalivePumpIntervalMs = 10;
        while (waited_ms < keepalive_ms) {
          RunPendingMainThreadTaskSchedulerForeground();
          PumpRobloxMainThreadMessagesOnce();
          const int chunk_ms = std::min(kHeadlessKeepalivePumpIntervalMs,
                                        keepalive_ms - waited_ms);
          usleep(static_cast<useconds_t>(chunk_ms) * 1000);
          waited_ms += chunk_ms;
        }
      } else {
        usleep(static_cast<useconds_t>(keepalive_ms) * 1000);
      }
      std::cout << "  [main] headless keepalive returned\n" << std::flush;
    } else if (IsEnabled("MOCKTAIL_KEEPALIVE")) {
      std::cout << "  [main] headless keepalive: forever\n" << std::flush;
      while (true) {
        pause();
      }
    }
  }

  if (!build_profile.default_allowed) {
    std::cerr
        << "[FATAL] Compatibility run returned, but Build ID "
        << build_profile.elf_build_id
        << " has not passed the readiness gates; refusing a success exit.\n";
    return EXIT_FAILURE;
  }
  if (is_headless) {
    std::cerr << "[FATAL] Headless LuaApp readiness evidence is not wired into "
                 "the supported runtime yet.\n";
    return EXIT_FAILURE;
  }
  if (!real_frame_presented) {
    std::cerr << "[FATAL] Windowed runtime exited without a real presented "
                 "frame.\n";
    return EXIT_FAILURE;
  }
  if (!lifecycle_shutdown_completed) {
    std::cerr << "[FATAL] Windowed runtime exited without completing the "
                 "Roblox lifecycle shutdown.\n";
    return EXIT_FAILURE;
  }
  if (!game_surface_events_completed) {
    std::cerr << "[FATAL] Windowed runtime rejected a typed GAME surface "
                 "lifecycle event.\n";
    return EXIT_FAILURE;
  }
  if (!input_shutdown_completed) {
    std::cerr << "[FATAL] Windowed runtime exited without completing typed "
                 "input shutdown.\n";
    return EXIT_FAILURE;
  }
  if (!resize_readiness_completed) {
    std::cerr << "[FATAL] Windowed runtime exited without completing the real "
                 "resize/rebind readiness sequence.\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

}  // namespace mocktail::legacy::internal
