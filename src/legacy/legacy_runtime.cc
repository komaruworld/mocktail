#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <iomanip>
#include <iostream>
#include <csignal>
#include <netdb.h>
#include <memory>
#include <mutex>
#include <new>
#include <poll.h>
#include <unordered_map>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <execinfo.h>
#include <ucontext.h>
#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <asm/prctl.h>
#include <sys/syscall.h>
#include <vector>

#include <jni.h>
#include <SDL3/SDL_video.h>

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
#include "legacy/legacy_runtime.h"
#include "libc_shim/libc_shim.h"
#include "linker/linker.h"
#include "mocktail/graphics/bionic_egl_bridge.h"
#include "runtime/environment.h"
#include "runtime/discord_rpc.h"
#include "runtime/jnivm_platform_web_callbacks.h"
#include "runtime/owned_pthread.h"
#include "runtime/platform_cache_migration.h"
#include "runtime/roblox_app_lifecycle.h"
#include "runtime/roblox_capability_resolver.h"
#include "runtime/roblox_platform_web_symbols.h"
#include "runtime/roblox_experience_composition.h"
#include "runtime/roblox_game_session_native_adapter.h"
#include "runtime/roblox_window_input_runtime.h"
#include "runtime/roblox_text_input_jni_bridge.h"
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

std::atomic<bool> g_allow_legacy_binary_patches{false};
std::atomic<bool> g_allow_host_abi_bridges{false};
std::atomic<bool> g_allow_host_constructor_replay{false};
std::atomic<const mocktail::compat::HostAbiProfile*>
    g_active_host_abi_profile{nullptr};

namespace {

void* ResolveRobloxCapabilitySymbol(void* context, const char* symbol_name) {
  if (context == nullptr || symbol_name == nullptr) {
    return nullptr;
  }
  auto* handle = static_cast<linker::LibraryHandle*>(context);
  return linker::ResolveSymbol(*handle, symbol_name);
}

void RestoreGameSessionJniEnvironment(void* context) {
  auto* vm = static_cast<jnivm::VM*>(context);
  if (vm != nullptr) {
    vm->RestoreFunctions();
  }
}

volatile uintptr_t g_stage6_jni_env = 0;
volatile uintptr_t g_libroblox_base = 0;
volatile uintptr_t g_game_activity_native_handle = 0;
jobject g_saved_game_activity = nullptr;
extern "C" void* mocktail_gameactivity_on_trim_memory_native;
volatile sig_atomic_t g_jni_onload_in_progress = 0;
volatile sig_atomic_t g_jni_onload_timings_printed = 0;
volatile sig_atomic_t g_jni_onload_soft_timeout = 0;
volatile sig_atomic_t g_jni_onload_jmp_armed = 0;
thread_local sigjmp_buf g_jni_onload_jmp_buf;
thread_local sigjmp_buf g_init_with_params_jmp_buf;
thread_local volatile sig_atomic_t g_init_with_params_recovery_in_progress = 0;
thread_local sigjmp_buf g_start_app_with_params_jmp_buf;
thread_local volatile sig_atomic_t g_start_app_with_params_recovery_in_progress = 0;
thread_local sigjmp_buf g_start_lua_app_dm_jmp_buf;
thread_local volatile sig_atomic_t g_start_lua_app_dm_recovery_in_progress = 0;
thread_local sigjmp_buf g_send_app_ready_jmp_buf;
thread_local volatile sig_atomic_t g_send_app_ready_recovery_in_progress = 0;
thread_local sigjmp_buf g_send_game_loaded_jmp_buf;
thread_local volatile sig_atomic_t g_send_game_loaded_recovery_in_progress = 0;
thread_local sigjmp_buf g_set_asset_path_jmp_buf;
thread_local volatile sig_atomic_t g_set_asset_path_recovery_in_progress = 0;
thread_local sigjmp_buf g_game_global_init_jmp_buf;
thread_local volatile sig_atomic_t g_game_global_init_recovery_in_progress = 0;
thread_local sigjmp_buf g_init_client_settings_jmp_buf;
thread_local volatile sig_atomic_t g_init_client_settings_recovery_in_progress = 0;
thread_local sigjmp_buf g_post_client_settings_jmp_buf;
thread_local volatile sig_atomic_t g_post_client_settings_recovery_in_progress = 0;
thread_local sigjmp_buf g_initialize_native_flags_jmp_buf;
thread_local volatile sig_atomic_t
    g_initialize_native_flags_recovery_in_progress = 0;
thread_local sigjmp_buf g_cookie_setter_jmp_buf;
thread_local volatile sig_atomic_t g_cookie_setter_recovery_in_progress = 0;
thread_local sigjmp_buf g_native_settings_jmp_buf;
thread_local volatile sig_atomic_t g_native_settings_recovery_in_progress = 0;
thread_local const char* g_native_settings_recovery_name = nullptr;
thread_local sigjmp_buf g_app_bridge_app_start_jmp_buf;
thread_local volatile sig_atomic_t g_app_bridge_app_start_recovery_in_progress =
    0;
thread_local sigjmp_buf g_game_activity_init_jmp_buf;
thread_local volatile sig_atomic_t g_game_activity_init_recovery_in_progress =
    0;
thread_local sigjmp_buf g_game_activity_surface_jmp_buf;
thread_local volatile sig_atomic_t g_game_activity_surface_recovery_in_progress = 0;
thread_local sigjmp_buf g_activity_lifecycle_jmp_buf;
thread_local volatile sig_atomic_t g_activity_lifecycle_recovery_in_progress = 0;
thread_local sigjmp_buf g_update_screen_orientation_jmp_buf;
thread_local volatile sig_atomic_t
    g_update_screen_orientation_recovery_in_progress = 0;
thread_local sigjmp_buf g_update_surface_app_jmp_buf;
thread_local volatile sig_atomic_t g_update_surface_app_recovery_in_progress = 0;
thread_local sigjmp_buf g_native_fragment_start_jmp_buf;
thread_local volatile sig_atomic_t
    g_native_fragment_start_recovery_in_progress = 0;
thread_local sigjmp_buf g_display_refresh_rate_jmp_buf;
thread_local volatile sig_atomic_t
    g_display_refresh_rate_recovery_in_progress = 0;
constexpr sig_atomic_t kStage6RecoveryInactive = 0;
constexpr sig_atomic_t kStage6RecoveryInline = 1;

void PrintBacktraceNoSig(const char* prefix);
void PrintContextBacktrace(ucontext_t* ucontext, const char* prefix);
static uintptr_t NullVtableStub();

using JniOnLoadFn = jint (*)(JavaVM*, void*);
using NativeGameGlobalInitFn = void (*)(JNIEnv*, jclass);
using NativeInitClientSettingsFn = jint (*)(JNIEnv*, jclass, jstring, jstring,
                                            jstring);
using NativeInitClientSettingsSignedFn = jint (*)(JNIEnv*, jclass, jstring,
                                                  jstring, jstring, jstring);
using NativeInitClientSettingsCachedFn = jint (*)(JNIEnv*, jclass, jstring,
                                                  jstring, jstring, jstring,
                                                  jlong);
using NativeInitClientSettingsCachedCompressedFn =
    jint (*)(JNIEnv*, jclass, jbyteArray, jstring, jstring, jstring, jlong,
             jboolean);
using NativePostClientSettingsFn = void (*)(JNIEnv*, jclass, jobject);
using NativeInitializeNativeFlagsFn = jobject (*)(JNIEnv*, jclass,
                                                  jobjectArray);
using NativeAppBridgeAppStartFn = void (*)(JNIEnv*, jclass, jstring, jstring,
                                           jboolean, jstring, jstring,
                                           jstring);
using NativeSetIsFirstInstallFn = void (*)(JNIEnv*, jclass, jboolean);
using NativeAppBridgeObjectParamsFn = void (*)(JNIEnv*, jclass, jobject);
using NativeAppBridgeSetInitParamsFn = void (*)(JNIEnv*, jclass, jobject);
using NativeUpdateSurfaceAppFn = void (*)(JNIEnv*, jclass, jobject, jobject);
using NativeUpdateAppUiSizesFn = void (*)(JNIEnv*, jclass, jint, jint, jint,
                                          jint, jint);
using NativeUpdateScreenOrientationFn = void (*)(JNIEnv*, jclass, jint);
using NativeSendAppReadyFn = void (*)(JNIEnv*, jclass, jstring, jstring,
                                      jstring, jstring);
using NativeSendGameLoadedFn = void (*)(JNIEnv*, jclass, jstring, jstring,
                                        jstring);
using NativePassSupportedRefreshRatesFn = void (*)(JNIEnv*, jclass,
                                                   jfloatArray);
using NativePassCurrentDisplayRefreshRateFn = void (*)(JNIEnv*, jclass, jfloat);
using NativeSetStringParamFn = void (*)(JNIEnv*, jclass, jstring);
using NativeSetBaseUrlFn = void (*)(JNIEnv*, jclass, jstring, jstring);
using NativeSetTaskSchedulerBackgroundModeFn = void (*)(JNIEnv*, jclass,
                                                        jboolean, jstring);
using NativeObjectInitFn = void (*)(JNIEnv*, jclass, jobject);
using NativeSetTwoStringParamsFn = void (*)(JNIEnv*, jclass, jstring, jstring);
using NativeSetThreeStringParamsFn = void (*)(JNIEnv*, jclass, jstring,
                                              jstring, jstring);
using NativeSetHttpClientProxyFn = void (*)(JNIEnv*, jclass, jstring, jlong);
using NativeInitStorageManagerFn = void (*)(JNIEnv*, jobject, jobject, jstring,
                                            jstring);
using NativeSetPlatformImplFn = jobject (*)(JNIEnv*, jclass, jobject);
using NativeActivityLifecycleStringFn = void (*)(JNIEnv*, jobject, jstring);
using NativeNoArgFn = void (*)(JNIEnv*, jclass);
using NativeDirectNoArgFn = void (*)();
using NativeGameActivityInitFn = jlong (*)(JNIEnv*, jobject, jstring, jstring,
                                           jstring, jobject, jbyteArray,
                                           jobject);
using GameActivityLifecycleFn = void (*)(JNIEnv*, jobject, jlong);
using GameActivitySurfaceCreatedFn = void (*)(JNIEnv*, jobject, jlong, jobject);
using GameActivitySurfaceChangedFn = void (*)(JNIEnv*, jobject, jlong, jobject,
                                              jint, jint, jint);

struct NativeActivityLifecycleCallbacks {
  NativeActivityLifecycleStringFn on_pre_created;
  NativeActivityLifecycleStringFn on_created;
  NativeActivityLifecycleStringFn on_post_created;
  NativeActivityLifecycleStringFn on_pre_started;
  NativeActivityLifecycleStringFn on_started;
  NativeActivityLifecycleStringFn on_post_started;
  NativeActivityLifecycleStringFn on_pre_resumed;
  NativeActivityLifecycleStringFn on_resumed;
  NativeActivityLifecycleStringFn on_post_resumed;
};

NativeNoArgFn g_native_call_messages_from_main_thread = nullptr;
NativeNoArgFn g_pending_main_thread_start_lua_app_dm = nullptr;
jclass g_native_gl_class_for_main_thread = nullptr;
jnivm::VM* g_vm_for_main_thread_pump = nullptr;
std::atomic<int> g_main_thread_message_pump_ready{0};
uint64_t g_pending_main_thread_start_lua_due_ms = 0;
bool g_pending_main_thread_start_lua_started = false;
NativeSetTaskSchedulerBackgroundModeFn
    g_pending_main_thread_task_scheduler_background_mode = nullptr;
jclass g_pending_main_thread_task_scheduler_class = nullptr;
std::atomic<int> g_pending_main_thread_task_scheduler_state{0};
std::atomic<int> g_pending_main_thread_task_scheduler_recovered{0};
constexpr int kMainThreadTaskSchedulerIdle = 0;
constexpr int kMainThreadTaskSchedulerPending = 1;
constexpr int kMainThreadTaskSchedulerRunning = 2;
constexpr int kMainThreadTaskSchedulerComplete = 3;
constexpr int kMainThreadTaskSchedulerTimedOut = 4;

void PublishCurrentJniEnv(JNIEnv* env);

struct JniOnLoadAsyncContext {
  JniOnLoadFn fn;
  jint result = JNI_ERR;
  jnivm::VM* vm;
};

void* RunJniOnLoadWorker(void* arg) {
  auto* context = static_cast<JniOnLoadAsyncContext*>(arg);
  if (context == nullptr || context->vm == nullptr) {
    return nullptr;
  }
  auto context_class = context->vm->RegisterClass("android/content/Context");
  auto activity_class =
      context->vm->RegisterClass("com/roblox/client/RobloxActivity");
  auto lifecycle_callbacks_class = context->vm->RegisterClass(
      "com/roblox/universalapp/activitylifecyclecallbacks/"
      "JNIActivityLifecycleCallbacks");
  auto settings_class = context->vm->RegisterClass("rbx/JNIRobloxSettings");
  settings_class->RegisterMethod(
      "nativeInitClientSettings", "()V",
      [](JNIEnv* /*env*/, jobject /*obj*/) {
        std::cout << "  [JNI callback] nativeInitClientSettings invoked\n";
      });
  static_cast<void>(context_class);
  static_cast<void>(activity_class);
  static_cast<void>(lifecycle_callbacks_class);
  PublishCurrentJniEnv(context->vm->GetJNIEnv());
  context->result = context->fn(context->vm->GetJavaVM(), nullptr);
  return nullptr;
}

struct EngineStartupContext {
  jnivm::VM* vm;
  JavaVM* java_vm;
  jnivm::RobloxAuthIdentity account_identity;
  const mocktail::runtime::SecureRobloxCredential* roblox_credential;
  mocktail::runtime::RobloxGameSessionRuntime* game_session_runtime;
  bool run_prepare_jni;
  bool run_set_asset_path;
  bool call_real_set_asset_path;
  bool run_global_init;
  bool run_init_client_settings;
  bool run_post_client_settings;
  bool run_app_bridge_app_start;
  bool run_native_settings;
  bool run_set_init_params;
  bool run_init_with_params;
  bool call_real_init_with_params;
  bool run_update_screen_orientation;
  bool run_update_surface_app;
  bool call_real_update_surface_app;
  bool run_start_app_with_params;
  bool call_real_start_app_with_params;
  bool run_activity_lifecycle;
  bool run_game_activity_init;
  bool run_game_activity_surface;
  bool run_app_lifecycle_active;
  bool run_native_fragment_start;
  bool run_display_refresh_rate;
  bool run_start_lua_app_dm;
  NativeGameGlobalInitFn native_global_init;
  NativeInitClientSettingsFn native_init_client_settings;
  NativeInitClientSettingsSignedFn native_init_client_settings_signed;
  NativeInitClientSettingsCachedFn native_init_client_settings_cached;
  NativeInitClientSettingsCachedCompressedFn
      native_init_client_settings_cached_compressed;
  NativePostClientSettingsFn native_post_client_settings;
  NativeInitializeNativeFlagsFn native_initialize_native_flags;
  NativeAppBridgeAppStartFn native_app_bridge_app_start;
  NativeSetIsFirstInstallFn native_set_is_first_install;
  NativeSetBaseUrlFn native_set_base_url;
  NativeObjectInitFn native_set_device_info;
  NativeObjectInitFn native_base_url_protocol_init;
  NativeSetStringParamFn native_set_roblox_channel;
  NativeSetStringParamFn native_override_channel_platform_name;
  NativeSetStringParamFn native_set_roblox_version;
  NativeSetStringParamFn native_set_exception_reason_filename;
  NativeSetTwoStringParamsFn native_set_base_data_directories;
  NativeSetStringParamFn native_set_cache_directory;
  NativeSetStringParamFn native_set_files_directory;
  NativeSetStringParamFn native_set_external_directory;
  NativeSetStringParamFn native_set_preferences_file;
  NativeSetStringParamFn native_set_default_app_policy_file;
  NativeSetHttpClientProxyFn native_set_http_client_proxy;
  NativeNoArgFn native_init_fast_log;
  NativeSetTwoStringParamsFn native_set_multiple_cookies;
  NativeSetTwoStringParamsFn native_cookie_manager_set_cookie;
  NativeSetThreeStringParamsFn native_set_platform_headers_with_idfa;
  NativeSetStringParamFn native_set_user_id;
  NativeObjectInitFn native_init_asset_manager;
  NativeInitStorageManagerFn native_init_storage_manager;
  NativeSetPlatformImplFn native_local_storage_set_platform_impl;
  NativeAppBridgeSetInitParamsFn native_set_init_params;
  NativeNoArgFn native_retry_init;
  NativeAppBridgeObjectParamsFn native_init_with_params;
  NativeNoArgFn native_update_adapter_init;
  NativeUpdateScreenOrientationFn native_update_screen_orientation;
  NativeUpdateAppUiSizesFn native_update_app_ui_sizes;
  NativeSetTaskSchedulerBackgroundModeFn
      native_set_task_scheduler_background_mode;
  NativeUpdateSurfaceAppFn native_update_surface_app;
  NativeAppBridgeObjectParamsFn native_start_app_with_params;
  NativeSendAppReadyFn native_send_app_ready;
  NativeSendGameLoadedFn native_send_game_loaded;
  NativeSetStringParamFn native_set_asset_path;
  NativeActivityLifecycleCallbacks activity_lifecycle_callbacks;
  NativeGameActivityInitFn native_game_activity_init;
  NativeNoArgFn native_app_lifecycle_set_active;
  NativeNoArgFn native_on_fragment_start;
  NativePassSupportedRefreshRatesFn native_pass_supported_refresh_rates;
  NativePassCurrentDisplayRefreshRateFn native_pass_current_display_refresh_rate;
  NativeNoArgFn native_start_lua_app_dm;
};


constexpr size_t kDefaultEngineStackSize = 1024ULL * 1024 * 1024;
constexpr size_t kMinEngineStackSize = 16ULL * 1024 * 1024;


bool IsEnabled(const char* name);
bool IsDisabled(const char* name);


bool IsLegacyBinaryCompatibilityToggle(const char* name) {
  if (name == nullptr) {
    return false;
  }
  static constexpr const char* kUnsafePrefixes[] = {
      "MOCKTAIL_PATCH_",
      "MOCKTAIL_RECOVER_",
      "MOCKTAIL_STAGE6_",
      "MOCKTAIL_TRACE_STAGE6_",
      "MOCKTAIL_DUMP_STAGE6_",
      "MOCKTAIL_INSTALL_STAGE6_",
      "MOCKTAIL_CALL_STAGE6_",
      "MOCKTAIL_SEED_STAGE6_",
      "MOCKTAIL_RESET_STAGE6_",
      "MOCKTAIL_LIBROBLOX_CTOR_",
      "MOCKTAIL_SKIP_LIBROBLOX_CTOR_",
      "MOCKTAIL_ALLOW_LIBROBLOX_CTOR_",
      "MOCKTAIL_QUARANTINE_LIBROBLOX_",
      "MOCKTAIL_EMUTLS_",
  };
  for (const char* prefix : kUnsafePrefixes) {
    if (std::strncmp(name, prefix, std::strlen(prefix)) == 0) {
      return true;
    }
  }
  static constexpr const char* kUnsafeExactNames[] = {
      "MOCKTAIL_HEADLESS_SIGSEGV_GUARDS",
      "MOCKTAIL_DEFER_RBXM_SIGNATURE_CHECK_TO_POST_TTI",
      "MOCKTAIL_KEEP_CONSTRUCTOR_EMUTLS_HELPERS_PATCHED",
      "MOCKTAIL_MAX_LIBROBLOX_CTORS",
      "MOCKTAIL_NO_RECOVER_START_APP",
      "MOCKTAIL_RESTORE_KNOWN_EMUTLS_KEYS",
      "MOCKTAIL_RUN_LIBROBLOX_CTORS",
      "MOCKTAIL_SKIP_CONSTRUCTOR_PATCH_OFFSETS",
  };
  for (const char* unsafe_name : kUnsafeExactNames) {
    if (std::strcmp(name, unsafe_name) == 0) {
      return true;
    }
  }
  return false;
}

const char* CachedGetenv(const char* name) {
  if (name == nullptr) {
    return nullptr;
  }
  // Launch flags are string literals and do not change after startup. Cache by
  // pointer identity so the main-thread pump does not walk environ + allocate
  // on every tick.
  static std::mutex mutex;
  static std::unordered_map<const char*, const char*> cache;
  std::lock_guard<std::mutex> lock(mutex);
  const auto existing = cache.find(name);
  if (existing != cache.end()) {
    return existing->second;
  }
  const char* value = std::getenv(name);
  cache.emplace(name, value);
  return value;
}

bool IsEnabled(const char* name) {
  if (!g_allow_legacy_binary_patches.load(std::memory_order_acquire) &&
      IsLegacyBinaryCompatibilityToggle(name)) {
    return false;
  }
  const char* value = CachedGetenv(name);
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

bool IsDisabled(const char* name) {
  if (!g_allow_legacy_binary_patches.load(std::memory_order_acquire) &&
      IsLegacyBinaryCompatibilityToggle(name)) {
    return true;
  }
  const char* value = CachedGetenv(name);
  return value != nullptr && std::strcmp(value, "0") == 0;
}

bool StartsWith(const char* value, const char* prefix) {
  if (value == nullptr || prefix == nullptr) {
    return false;
  }
  return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

bool IsGlSymbol(const char* name) {
  return StartsWith(name, "gl");
}

enum class SymbolResolveSource {
  kMissing = 0,
  kWindow = 1,
  kRealGles = 2,
  kStub = 3,
  kHost = 4,
};

struct SymbolResolveResult {
  void* address = nullptr;
  SymbolResolveSource source = SymbolResolveSource::kMissing;
};

// dlsym searches the object and its dependency chain, so a
// stub that transitively links SDL or another host library would
// satisfy EGL/GL/Vulkan lookups with unrelated host symbols
//
// So, we should only accept the symbol if its stub owns it.
bool StubOwnsSymbolAddress(void* handle, void* address) {
  if (handle == nullptr || address == nullptr) {
    return false;
  }

  Dl_info info = {};
  if (::dladdr(address, &info) == 0 || info.dli_fbase == nullptr) {
    return false;
  }

	link_map* map = nullptr;
  if (::dlinfo(handle, RTLD_DI_LINKMAP, &map) != 0 || map == nullptr) {
    return false;
  }

  return reinterpret_cast<void*>(map->l_addr) == info.dli_fbase;
}

SymbolResolveResult ResolveSymbolForBionic(const char* name, bool has_window,
                                          void* real_gles_handle,
                                          const std::vector<void*>& stub_handles) {
  const bool is_gl_symbol = IsGlSymbol(name);
  const bool prefer_real_gles =
      has_window && !IsEnabled("MOCKTAIL_GLES_FORCE_STUB") &&
      !IsEnabled("MOCKTAIL_GLES_NOOP_DRAW_CALLS");
  SymbolResolveResult result;

  if (is_gl_symbol && prefer_real_gles) {
    result.address = mocktail::window::GetGLProcAddress(name);
    if (result.address != nullptr) {
      result.source = SymbolResolveSource::kWindow;
      return result;
    }
  }

  if (is_gl_symbol && prefer_real_gles && real_gles_handle != nullptr) {
    result.address = ::dlsym(real_gles_handle, name);
    if (result.address != nullptr) {
      result.source = SymbolResolveSource::kRealGles;
      return result;
    }
  }

  for (void* h : stub_handles) {
    if (void *candidate = ::dlsym(h, name); candidate != nullptr && StubOwnsSymbolAddress(h, candidate)) {
      result.address = candidate;
      result.source = SymbolResolveSource::kStub;
      return result;
    }
  }

  result.address = ::dlsym(RTLD_DEFAULT, name);
  if (result.address != nullptr) {
    result.source = SymbolResolveSource::kHost;
  }
  return result;
}

bool TraceAllEnabled() {
  return IsEnabled("MOCKTAIL_TRACE_ALL") || IsEnabled("MOCKTAIL_FULL_TRACE");
}

bool VerboseOutputEnabled() {
  return IsEnabled("MOCKTAIL_VERBOSE") || TraceAllEnabled();
}

bool LibRobloxConstructorTraceEnabled() {
  return IsEnabled("MOCKTAIL_TRACE_LIBROBLOX_CONSTRUCTORS") ||
         TraceAllEnabled();
}

void EnableFullTraceIfRequested() {
  if (!TraceAllEnabled()) {
    return;
  }
  const char* kTraceEnvNames[] = {
      "MOCKTAIL_ENGINE_TRACE",
      "MOCKTAIL_JNI_TRACE",
      "MOCKTAIL_JNI_VM_TRACE",
      "MOCKTAIL_JNI_STRING_TRACE",
      "MOCKTAIL_DNS_TRACE",
      "MOCKTAIL_GL_TRACE",
      "MOCKTAIL_EGL_TRACE",
      "MOCKTAIL_WINDOW_TRACE",
      "MOCKTAIL_ANDROID_TRACE",
      "MOCKTAIL_ASSET_TRACE",
      "MOCKTAIL_TRACE_POST_CLIENT_SETTINGS_JNI",
      "MOCKTAIL_TRACE_START_LUA_JNI",
  };
  for (const char* name : kTraceEnvNames) {
    setenv(name, "1", 1);
  }
}

void SetEnvDefault(const char* name, const char* value) {
  if (name == nullptr || value == nullptr) {
    return;
  }
  const char* current = std::getenv(name);
  if (current == nullptr || current[0] == '\0') {
    setenv(name, value, 1);
  }
}

bool IsMocktailStubGlesPath(const char* path) {
  if (path == nullptr || path[0] == '\0') {
    return true;
  }
  std::string value(path);
  return value == "libGLESv2.so" ||
         value.find("/build/libGLESv2.so") != std::string::npos;
}

void* OpenRealGlesLibrary() {
  const char* env_library = std::getenv("MOCKTAIL_GLES_LIBRARY");
  const char* candidates[] = {
      env_library,
      "/usr/lib/libGLESv2.so.2",
      "/usr/lib64/libGLESv2.so.2",
      "libGLESv2.so.2",
      "/usr/lib/chromium/libGLESv2.so",
      "/usr/lib/chromium-browser/libGLESv2.so",
      "/usr/lib/electron42/libGLESv2.so",
      "/usr/lib/electron41/libGLESv2.so",
      "/usr/lib/electron40/libGLESv2.so",
      "/usr/lib/electron39/libGLESv2.so",
      "/usr/lib/cef/libGLESv2.so",
  };
  for (const char* candidate : candidates) {
    if (IsMocktailStubGlesPath(candidate)) {
      continue;
    }
    void* handle = ::dlopen(candidate, RTLD_LAZY | RTLD_LOCAL);
    if (handle != nullptr) {
      std::cout << "  [gles] Using real GLES from " << candidate << '\n';
      return handle;
    }
  }
  std::cerr << "  [gles] real libGLESv2 not found; using Mocktail GLES shim\n";
  return nullptr;
}

void ApplyRuntimeDefaults() {
  SetEnvDefault("MOCKTAIL_SOBER_MODE", "1");
  SetEnvDefault("MOCKTAIL_HEADLESS", "0");
  // Startup worker ownership is joined. Returning while it still references
  // JNI, runtime, or stack state is never supported.
  SetEnvDefault("MOCKTAIL_ENGINE_DETACH", "0");
  SetEnvDefault("MOCKTAIL_INIT_CLIENT_SETTINGS", "0");
  SetEnvDefault("MOCKTAIL_POST_CLIENT_SETTINGS", "0");
  SetEnvDefault("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1");
  // Current Roblox Android builds drive app startup through NativeGLInterface's
  // V2 app bridge path. The legacy NativeAppBridgeInterface entry point stays
  // available by opt-in, but running it in parallel can block V2 init.
  SetEnvDefault("MOCKTAIL_APP_BRIDGE_APP_START", "0");
  SetEnvDefault("MOCKTAIL_APP_BRIDGE_APP_START_THREAD", "0");
  SetEnvDefault("MOCKTAIL_START_LUA_APP_DM", "1");
  SetEnvDefault("MOCKTAIL_START_LUA_APP_DM_INLINE", "0");
  // The shipped APK drives these callbacks in a mostly synchronous ASMA flow.
  // Keep the background worker path opt-in so the default mirrors release
  // ordering and avoids racing heap-sensitive V2 startup state.
  SetEnvDefault("MOCKTAIL_START_LUA_APP_DM_THREAD", "0");
  SetEnvDefault("MOCKTAIL_START_LUA_APP_DM_AFTER_START_APP", "1");
  SetEnvDefault("MOCKTAIL_START_LUA_APP_DM_DELAY_MS", "500");
  SetEnvDefault("MOCKTAIL_APP_BRIDGE_HEADLESS_INIT_PARAMS", "0");
  SetEnvDefault("MOCKTAIL_PLACE_ID", "0");
  SetEnvDefault("MOCKTAIL_CALL_REAL_APP_BRIDGE_INIT", "1");
  SetEnvDefault("MOCKTAIL_CALL_REAL_APP_BRIDGE_INIT_THREAD", "0");
  SetEnvDefault("MOCKTAIL_APP_BRIDGE_INIT_THREAD_TIMEOUT_MS", "1500");
  SetEnvDefault("MOCKTAIL_CALL_REAL_APP_BRIDGE_START", "1");
  SetEnvDefault("MOCKTAIL_START_GAME_WITH_PARAM", "0");
  SetEnvDefault("MOCKTAIL_SET_APP_BRIDGE_NOTIFICATION_LISTENER", "1");
  // APK ASMA calls nativeAppBridgeV2StartAppWithParams synchronously before
  // driving post-start surface/Lua callbacks. Keep the worker path opt-in so
  // those follow-up callbacks cannot race native StartApp construction.
  SetEnvDefault("MOCKTAIL_CALL_REAL_APP_BRIDGE_START_THREAD", "0");
  SetEnvDefault("MOCKTAIL_ASMA_START_TASK_SCHEDULER_FOREGROUND", "1");
  SetEnvDefault("MOCKTAIL_TASK_SCHEDULER_FOREGROUND_ON_MAIN_THREAD", "1");
  SetEnvDefault("MOCKTAIL_NATIVE_FRAGMENT_START", "1");
  SetEnvDefault("MOCKTAIL_PASS_CURRENT_DISPLAY_REFRESH_RATE", "1");
  SetEnvDefault("MOCKTAIL_PASS_SUPPORTED_REFRESH_RATES", "1");
  SetEnvDefault("MOCKTAIL_PASS_ACTIVITY_TO_GAME_SURFACE_PARAMS", "0");
  SetEnvDefault("MOCKTAIL_SYNC_START_APP_WITH_GAME", "1");
  // APK ASMA publishes queued ready events after the Lua app startup path has
  // run. Keep this inline by default; the detached worker can outlive teardown.
  SetEnvDefault("MOCKTAIL_SEND_APP_READY", "1");
  SetEnvDefault("MOCKTAIL_SEND_APP_READY_THREAD", "0");
  SetEnvDefault("MOCKTAIL_SEND_GAME_LOADED", "0");
  SetEnvDefault("MOCKTAIL_SEND_GAME_LOADED_THREAD", "0");
  SetEnvDefault("MOCKTAIL_UPDATE_SCREEN_ORIENTATION", "0");
  SetEnvDefault("MOCKTAIL_STEP_UPDATE_SURFACE_APP", "0");
  SetEnvDefault("MOCKTAIL_CALL_REAL_APP_BRIDGE_UPDATE_SURFACE", "0");
  SetEnvDefault("MOCKTAIL_CALL_REAL_APP_BRIDGE_UPDATE_SURFACE_THREAD", "0");
  // Keep the ASMA/V2 NativeGL path as the default. GameActivity's native app
  // glue currently stalls V2 init on surface flags; leave it available opt-in.
  SetEnvDefault("MOCKTAIL_STEP_GAME_ACTIVITY_INIT", "0");
  SetEnvDefault("MOCKTAIL_STEP_GAME_ACTIVITY_SURFACE", "0");
  // GameActivity lifecycle callbacks can block on android_app_set_activity_state
  // in some Linux shims; keep them opt-in to avoid startup dead-ends.
  SetEnvDefault("MOCKTAIL_GAME_ACTIVITY_LIFECYCLE_CALLBACKS", "0");
  SetEnvDefault("MOCKTAIL_NATIVE_SET_USER_ID", "0");
  SetEnvDefault("MOCKTAIL_MAIN_THREAD_MESSAGE_PUMP", "0");
  SetEnvDefault("MOCKTAIL_IGNORE_WINDOW_CLOSE", "0");
  SetEnvDefault("MOCKTAIL_WIN_TITLE", "Roblox");
  SetEnvDefault("MOCKTAIL_GRAPHICS_BACKEND", "angle-vulkan");
  SetEnvDefault("MOCKTAIL_REQUIRE_REAL_GRAPHICS", "0");
}

bool IsHeadlessMode() {
  return IsEnabled("MOCKTAIL_HEADLESS");
}

bool ShouldRunStartupStep(const char* step_env, bool default_value) {
  if (IsEnabled(step_env)) {
    return true;
  }
  if (IsDisabled(step_env)) {
    return false;
  }
  return default_value;
}

int GetEnvInt(const char* name, int default_value) {
  const char* value = CachedGetenv(name);
  if (value == nullptr || value[0] == '\0') {
    return default_value;
  }
  char* end = nullptr;
  long parsed = std::strtol(value, &end, 10);
  if (end == value) {
    return default_value;
  }
  return static_cast<int>(parsed);
}


jlong GetEnvLong(const char* name, jlong default_value) {
  const char* value = CachedGetenv(name);
  if (value == nullptr || value[0] == '\0') {
    return default_value;
  }
  char* end = nullptr;
  long long parsed = std::strtoll(value, &end, 10);
  if (end == value) {
    return default_value;
  }
  return static_cast<jlong>(parsed);
}

uint64_t MonotonicMillis() {
  timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
         static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
}

uint64_t MonotonicNanos() {
  timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

JNIEnv* AttachMainThreadJniEnv() {
  if (g_vm_for_main_thread_pump == nullptr) {
    return nullptr;
  }
  JNIEnv* env = nullptr;
  JavaVM* java_vm = g_vm_for_main_thread_pump->GetJavaVM();
  if (java_vm != nullptr) {
    void* raw_env = nullptr;
    jint attach_result = java_vm->AttachCurrentThread(&raw_env, nullptr);
    if (attach_result == JNI_OK && raw_env != nullptr) {
      env = static_cast<JNIEnv*>(raw_env);
    } else if (IsEnabled("MOCKTAIL_TRACE_MAIN_THREAD_PUMP")) {
      std::cerr << "  [main] AttachCurrentThread failed in pump: "
                << attach_result << '\n'
                << std::flush;
    }
  }
  if (env == nullptr) {
    env = g_vm_for_main_thread_pump->GetJNIEnv();
  }
  g_vm_for_main_thread_pump->RestoreFunctions();
  PublishCurrentJniEnv(env);
  return env;
}

bool InvokeTaskSchedulerForeground(
    JNIEnv* env, jclass native_gl_class,
    NativeSetTaskSchedulerBackgroundModeFn native_set_task_scheduler_background_mode,
    const char* log_scope) {
  if (env == nullptr || native_gl_class == nullptr ||
      native_set_task_scheduler_background_mode == nullptr) {
    std::cerr << "  [" << log_scope
              << "] NativeGLInterface.setTaskSchedulerBackgroundMode skipped: "
              << "missing JNI state\n"
              << std::flush;
    return false;
  }

  jstring reason = env->NewStringUTF("ASMA.start");
  std::cout << "  [" << log_scope
            << "] NativeGLInterface.setTaskSchedulerBackgroundMode(false, "
            << "ASMA.start)\n"
            << std::flush;
  if (sigsetjmp(g_update_surface_app_jmp_buf, 0) == 0) {
    g_update_surface_app_recovery_in_progress = kStage6RecoveryInline;
    native_set_task_scheduler_background_mode(env, native_gl_class, JNI_FALSE,
                                              reason);
    g_update_surface_app_recovery_in_progress = kStage6RecoveryInactive;
    std::cout << "  [" << log_scope
              << "] NativeGLInterface.setTaskSchedulerBackgroundMode returned\n"
              << std::flush;
    return true;
  }

  g_update_surface_app_recovery_in_progress = kStage6RecoveryInactive;
  std::cerr << "  [" << log_scope
            << "] setTaskSchedulerBackgroundMode recovered from crash\n"
            << std::flush;
  return false;
}

void RunPendingMainThreadTaskSchedulerForeground() {
  int expected = kMainThreadTaskSchedulerPending;
  if (!g_pending_main_thread_task_scheduler_state.compare_exchange_strong(
          expected, kMainThreadTaskSchedulerRunning,
          std::memory_order_acq_rel)) {
    return;
  }

  JNIEnv* env = AttachMainThreadJniEnv();
  const bool invoked = InvokeTaskSchedulerForeground(
      env, g_pending_main_thread_task_scheduler_class,
      g_pending_main_thread_task_scheduler_background_mode, "main");
  g_pending_main_thread_task_scheduler_recovered.store(invoked ? 0 : 1,
                                                       std::memory_order_release);
  g_pending_main_thread_task_scheduler_state.store(
      kMainThreadTaskSchedulerComplete, std::memory_order_release);
}

bool RunTaskSchedulerForegroundOnMainThread(
    NativeSetTaskSchedulerBackgroundModeFn native_set_task_scheduler_background_mode,
    jclass native_gl_class) {
  if (IsDisabled("MOCKTAIL_TASK_SCHEDULER_FOREGROUND_ON_MAIN_THREAD") ||
      IsEnabled("MOCKTAIL_ENGINE_INLINE") || !mocktail::window::IsInitialised()) {
    return false;
  }
  if (native_set_task_scheduler_background_mode == nullptr ||
      native_gl_class == nullptr) {
    return false;
  }

  g_pending_main_thread_task_scheduler_background_mode =
      native_set_task_scheduler_background_mode;
  g_pending_main_thread_task_scheduler_class = native_gl_class;
  g_pending_main_thread_task_scheduler_recovered.store(
      0, std::memory_order_release);
  g_pending_main_thread_task_scheduler_state.store(
      kMainThreadTaskSchedulerPending, std::memory_order_release);
  std::cout << "  [engine] NativeGLInterface.setTaskSchedulerBackgroundMode "
            << "scheduled on main thread\n"
            << std::flush;

  const int timeout_ms = GetEnvInt(
      "MOCKTAIL_TASK_SCHEDULER_FOREGROUND_MAIN_THREAD_TIMEOUT_MS", 3000);
  const uint64_t start_ms = MonotonicMillis();
  while (true) {
    const int state = g_pending_main_thread_task_scheduler_state.load(
        std::memory_order_acquire);
    if (state == kMainThreadTaskSchedulerComplete) {
      const bool recovered =
          g_pending_main_thread_task_scheduler_recovered.load(
              std::memory_order_acquire) != 0;
      g_pending_main_thread_task_scheduler_state.store(
          kMainThreadTaskSchedulerIdle, std::memory_order_release);
      if (recovered) {
        std::cerr << "  [engine] main-thread "
                  << "setTaskSchedulerBackgroundMode recovered from crash\n"
                  << std::flush;
      } else {
        std::cout << "  [engine] main-thread "
                  << "setTaskSchedulerBackgroundMode returned\n"
                  << std::flush;
      }
      return true;
    }

    if (timeout_ms >= 0) {
      const uint64_t now_ms = MonotonicMillis();
      if (now_ms >= start_ms &&
          now_ms - start_ms >= static_cast<uint64_t>(timeout_ms)) {
        g_pending_main_thread_task_scheduler_state.store(
            kMainThreadTaskSchedulerTimedOut, std::memory_order_release);
        std::cerr << "  [engine] main-thread "
                  << "setTaskSchedulerBackgroundMode timed out after "
                  << timeout_ms << " ms\n"
                  << std::flush;
        return true;
      }
    }
    usleep(1000);
  }
}

void PumpRobloxMainThreadMessagesOnce() {
  static const bool force_early =
      IsEnabled("MOCKTAIL_FORCE_EARLY_MAIN_THREAD_MESSAGE_PUMP");
  static const bool pump_disabled =
      IsDisabled("MOCKTAIL_MAIN_THREAD_MESSAGE_PUMP");
  static const bool trace_pump =
      IsEnabled("MOCKTAIL_TRACE_MAIN_THREAD_PUMP");
  static const int pump_limit =
      GetEnvInt("MOCKTAIL_MAIN_THREAD_MESSAGE_PUMP_LIMIT", 0);
  if (g_main_thread_message_pump_ready.load() == 0 && !force_early) {
    static bool logged_not_ready = false;
    if (!logged_not_ready && trace_pump) {
      logged_not_ready = true;
      std::cerr << "  [main] nativeCallMessagesFromMainThread pump not ready\n"
                << std::flush;
    }
    return;
  }

  if (g_native_call_messages_from_main_thread == nullptr ||
      g_vm_for_main_thread_pump == nullptr ||
      g_native_gl_class_for_main_thread == nullptr || pump_disabled) {
    static bool logged_unavailable = false;
    if (!logged_unavailable && trace_pump) {
      logged_unavailable = true;
      std::cerr << "  [main] nativeCallMessagesFromMainThread pump unavailable"
                << " fn="
                << reinterpret_cast<const void*>(
                       g_native_call_messages_from_main_thread)
                << " vm=" << static_cast<void*>(g_vm_for_main_thread_pump)
                << " class="
                << reinterpret_cast<const void*>(
                       g_native_gl_class_for_main_thread)
                << " disabled=" << pump_disabled << '\n'
                << std::flush;
    }
    return;
  }

  JNIEnv* env = AttachMainThreadJniEnv();
  if (env == nullptr) {
    static bool logged_missing_env = false;
    if (!logged_missing_env && trace_pump) {
      logged_missing_env = true;
      std::cerr << "  [main] nativeCallMessagesFromMainThread pump has no JNIEnv\n"
                << std::flush;
    }
    return;
  }
  static int pump_count = 0;
  ++pump_count;
  if (pump_limit > 0 && pump_count > pump_limit) {
    return;
  }
  if (__builtin_expect(trace_pump, 0)) {
    if (pump_count <= 10 || pump_count % 100 == 0) {
      std::cerr << "  [main] nativeCallMessagesFromMainThread pump #"
                << pump_count << " env=" << static_cast<void*>(env) << '\n'
                << std::flush;
    }
  }
  g_native_call_messages_from_main_thread(
      env, g_native_gl_class_for_main_thread);
  if (__builtin_expect(trace_pump, 0)) {
    if (pump_count <= 10 || pump_count % 100 == 0) {
      std::cerr << "  [main] nativeCallMessagesFromMainThread returned #"
                << pump_count << '\n'
                << std::flush;
    }
  }
}

void PumpStartupOwnerThread(void* /*context*/) {
  RunPendingMainThreadTaskSchedulerForeground();
  PumpRobloxMainThreadMessagesOnce();
}

std::string GetEnvString(const char* name, const char* default_value) {
  const char* value = CachedGetenv(name);
  if (value == nullptr || value[0] == '\0') {
    return default_value ? default_value : "";
  }
  return value;
}

bool HasEnvValue(const char* name) {
  const char* value = CachedGetenv(name);
  return value != nullptr && value[0] != '\0';
}

struct BionicAddrInfo {
  int ai_flags;
  int ai_family;
  int ai_socktype;
  int ai_protocol;
  socklen_t ai_addrlen;
  char* ai_canonname;
  struct sockaddr* ai_addr;
  BionicAddrInfo* ai_next;
};

static_assert(sizeof(BionicAddrInfo) == 48,
              "unexpected x86_64 bionic addrinfo size");

bool DnsTraceEnabled() {
  return IsEnabled("MOCKTAIL_DNS_TRACE");
}

addrinfo HostHintsFromBionic(const BionicAddrInfo* hints) {
  addrinfo host_hints{};
  if (!hints) {
    return host_hints;
  }
  host_hints.ai_flags = hints->ai_flags;
  host_hints.ai_family = hints->ai_family;
  host_hints.ai_socktype = hints->ai_socktype;
  host_hints.ai_protocol = hints->ai_protocol;
  return host_hints;
}

BionicAddrInfo* BionicAddrInfoFromHost(const addrinfo* host) {
  BionicAddrInfo* head = nullptr;
  BionicAddrInfo* tail = nullptr;
  for (const addrinfo* current = host; current; current = current->ai_next) {
    auto* node =
        static_cast<BionicAddrInfo*>(std::calloc(1, sizeof(BionicAddrInfo)));
    if (!node) {
      break;
    }
    node->ai_flags = current->ai_flags;
    node->ai_family = current->ai_family;
    node->ai_socktype = current->ai_socktype;
    node->ai_protocol = current->ai_protocol;
    node->ai_addrlen = current->ai_addrlen;
    if (current->ai_canonname) {
      node->ai_canonname = ::strdup(current->ai_canonname);
    }
    if (current->ai_addr && current->ai_addrlen > 0) {
      node->ai_addr = static_cast<sockaddr*>(std::malloc(current->ai_addrlen));
      if (node->ai_addr) {
        std::memcpy(node->ai_addr, current->ai_addr, current->ai_addrlen);
      }
    }
    if (!head) {
      head = node;
    } else {
      tail->ai_next = node;
    }
    tail = node;
  }
  return head;
}

extern "C" int mocktail_getaddrinfo(const char* node, const char* service,
                                    const BionicAddrInfo* hints,
                                    BionicAddrInfo** result) {
  if (!result) {
    return EAI_FAIL;
  }
  *result = nullptr;
  addrinfo host_hints = HostHintsFromBionic(hints);
  addrinfo* host_result = nullptr;
  int rc = ::getaddrinfo(node, service, hints ? &host_hints : nullptr,
                         &host_result);
  if (DnsTraceEnabled()) {
    std::cout << "  [dns] getaddrinfo node=" << (node ? node : "(null)")
              << " service=" << (service ? service : "(null)")
              << " family=" << (hints ? hints->ai_family : 0)
              << " socktype=" << (hints ? hints->ai_socktype : 0)
              << " rc=" << rc
              << " message=" << (rc == 0 ? "ok" : ::gai_strerror(rc))
              << '\n';
  }
  if (rc != 0) {
    return rc;
  }
  *result = BionicAddrInfoFromHost(host_result);
  ::freeaddrinfo(host_result);
  return *result ? 0 : EAI_MEMORY;
}

extern "C" void mocktail_freeaddrinfo(BionicAddrInfo* info) {
  while (info) {
    BionicAddrInfo* next = info->ai_next;
    std::free(info->ai_canonname);
    std::free(info->ai_addr);
    std::free(info);
    info = next;
  }
}

extern "C" hostent* mocktail_gethostbyname(const char* name) {
  hostent* result = ::gethostbyname(name);
  if (DnsTraceEnabled()) {
    std::cout << "  [dns] gethostbyname name=" << (name ? name : "(null)")
              << " result=" << reinterpret_cast<void*>(result) << '\n';
  }
  return result;
}

extern "C" int mocktail_mprotect(void* addr, size_t len, int prot) {
  if (addr == nullptr || len == 0) {
    errno = EINVAL;
    return -1;
  }
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    errno = EINVAL;
    return -1;
  }
  uintptr_t start = reinterpret_cast<uintptr_t>(addr);
  uintptr_t page_mask = static_cast<uintptr_t>(page_size) - 1;
  uintptr_t page_start = start & ~page_mask;
  uintptr_t end = start + len;
  if (end < start) {
    errno = EINVAL;
    return -1;
  }
  uintptr_t page_end = (end + page_mask) & ~page_mask;
  if (page_end < page_start) {
    errno = EINVAL;
    return -1;
  }
  return ::mprotect(reinterpret_cast<void*>(page_start),
                    static_cast<size_t>(page_end - page_start), prot);
}

void RegisterBionicDnsWrappers() {
  linker::RegisterSymbol("getaddrinfo",
                         reinterpret_cast<void*>(mocktail_getaddrinfo));
  linker::RegisterSymbol("freeaddrinfo",
                         reinterpret_cast<void*>(mocktail_freeaddrinfo));
  linker::RegisterSymbol("gethostbyname",
                         reinterpret_cast<void*>(mocktail_gethostbyname));
}

void RegisterBionicPathWrappers() {
  linker::RegisterSymbol("open", reinterpret_cast<void*>(mocktail_open));
  linker::RegisterSymbol("__open_2", reinterpret_cast<void*>(mocktail___open_2));
  linker::RegisterSymbol("fopen", reinterpret_cast<void*>(mocktail_fopen));
  linker::RegisterSymbol("access", reinterpret_cast<void*>(mocktail_access));
  linker::RegisterSymbol("stat", reinterpret_cast<void*>(mocktail_stat));
  linker::RegisterSymbol("lstat", reinterpret_cast<void*>(mocktail_lstat));
  linker::RegisterSymbol("statvfs", reinterpret_cast<void*>(mocktail_statvfs));
  linker::RegisterSymbol("statfs", reinterpret_cast<void*>(mocktail_statfs));
  linker::RegisterSymbol("mkdir", reinterpret_cast<void*>(mocktail_mkdir));
  linker::RegisterSymbol("opendir", reinterpret_cast<void*>(mocktail_opendir));
  linker::RegisterSymbol("rename", reinterpret_cast<void*>(mocktail_rename));
  linker::RegisterSymbol("unlink", reinterpret_cast<void*>(mocktail_unlink));
  linker::RegisterSymbol("rmdir", reinterpret_cast<void*>(mocktail_rmdir));
  linker::RegisterSymbol("realpath", reinterpret_cast<void*>(mocktail_realpath));
  linker::RegisterSymbol("readlink", reinterpret_cast<void*>(mocktail_readlink));
  linker::RegisterSymbol("__readlink_chk",
                         reinterpret_cast<void*>(mocktail___readlink_chk));
}

mocktail::runtime::RuntimePaths CurrentRuntimePaths() {
  const mocktail::runtime::ProcessEnvironment environment;
  return mocktail::runtime::RuntimePaths::FromEnvironment(environment);
}

std::string SoberDataRoot() {
  return CurrentRuntimePaths().sober_data_root().string();
}

std::string SoberCacheRoot() {
  return CurrentRuntimePaths().sober_cache_root().string();
}

std::string MocktailCacheRoot() {
  return CurrentRuntimePaths().cache_root().string();
}

std::string MocktailConfigRoot() {
  return CurrentRuntimePaths().config_root().string();
}

std::string DefaultSoberAwarePath(const char* sober_path,
                                  const char* fallback_path) {
  return CurrentRuntimePaths()
      .DefaultSoberAwarePath(sober_path ? sober_path : "",
                             fallback_path ? fallback_path : "")
      .string();
}

std::string DefaultAssetPath() {
  return CurrentRuntimePaths().DefaultAssetPath().string();
}

bool RegisterFreshGamePresentObserver(
    void* context, mocktail::runtime::FreshLaunchPresentObserver observer,
    void* observer_context) {
  if (context == nullptr) {
    return false;
  }
  return static_cast<mocktail::window::ScopedPresentObserver*>(context)
      ->Register(observer, observer_context);
}

void ClearFreshGamePresentObserver(void* context) {
  if (context != nullptr) {
    static_cast<mocktail::window::ScopedPresentObserver*>(context)->Reset();
  }
}

jobject CreateExperienceRawCallback(
    void* context, std::shared_ptr<void> callback_context,
    void (*run)(void*, JNIEnv*, jstring)) {
  if (context == nullptr || run == nullptr) {
    return nullptr;
  }
  return static_cast<jnivm::VM*>(context)->CreateMessageBusRawCallback(
      std::move(callback_context), jnivm::MessageBusRawCallbacks{run});
}

void ClearExperienceRawCallback(void* context, jobject callback) {
  if (context != nullptr) {
    static_cast<jnivm::VM*>(context)->ClearMessageBusRawCallback(callback);
  }
}

jobject CreateMessageBusRequestHandler(void *context,
                                       std::shared_ptr<void> callback_context,
                                       std::string (*run)(void *, JNIEnv *,
                                                          jstring)) {
  if (context == nullptr || run == nullptr) {
    return nullptr;
  }
  return static_cast<jnivm::VM *>(context)->CreateMessageBusRequestHandler(
      std::move(callback_context),
      jnivm::MessageBusRequestHandlerCallbacks{run});
}

void ClearMessageBusRequestHandler(void *context, jobject handler) {
  if (context != nullptr) {
    static_cast<jnivm::VM *>(context)->ClearMessageBusRequestHandler(handler);
  }
}

jobject CreateBrowserServiceMemStorageCallback(
    void *context, std::shared_ptr<void> callback_context,
    void (*on_item_set)(void *, JNIEnv *, jstring)) {
  if (context == nullptr || on_item_set == nullptr) {
    return nullptr;
  }
  return static_cast<jnivm::VM *>(context)->CreateMemStorageCallback(
      std::move(callback_context),
      jnivm::MemStorageCallbackCallbacks{on_item_set});
}

void ClearBrowserServiceMemStorageCallback(void *context, jobject callback) {
  if (context != nullptr) {
    static_cast<jnivm::VM *>(context)->ClearMemStorageCallback(callback);
  }
}

struct ExperienceLifecycleTarget {
  std::weak_ptr<mocktail::runtime::RobloxExperienceComposition> composition;
};

void NotifyLuaAppDidReturn(void* context) {
  if (context == nullptr) {
    return;
  }
  const std::shared_ptr<mocktail::runtime::RobloxExperienceComposition>
      composition =
          static_cast<ExperienceLifecycleTarget*>(context)->composition.lock();
  if (composition != nullptr) {
    composition->NotifyLuaAppDidReturn();
  }
}

mocktail::runtime::GameSessionUpdateResult ExperienceSurfaceCreated(
    void* context, uint64_t generation) {
  return static_cast<mocktail::runtime::RobloxExperienceComposition*>(context)
      ->SurfaceCreated(generation);
}

mocktail::runtime::GameSessionUpdateResult ExperienceSurfaceChanged(
    void* context, mocktail::runtime::GameSurface surface) {
  return static_cast<mocktail::runtime::RobloxExperienceComposition*>(context)
      ->SurfaceChanged(std::move(surface));
}

mocktail::runtime::GameSessionUpdateResult ExperienceSurfaceDestroyed(
    void* context, uint64_t generation) {
  return static_cast<mocktail::runtime::RobloxExperienceComposition*>(context)
      ->SurfaceDestroyed(generation);
}

std::string GetEnvStringDefaultPath(const char* name,
                                    const std::string& default_value) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return default_value;
  }
  return value;
}

bool EnsureDirectory(const std::string& path) {
  return mocktail::runtime::RuntimePaths::EnsureDirectory(path);
}

void EnsureAndroidDirectory(const std::string& android_path) {
  std::string host_path = libc_shim::TranslatePath(android_path);
  if (EnsureDirectory(host_path) && host_path != android_path &&
      IsEnabled("MOCKTAIL_ENGINE_TRACE")) {
    std::cerr << "  [engine] directory " << android_path << " -> "
              << host_path << '\n';
  }
}

size_t GetEngineStackSize() {
  const char* value = std::getenv("MOCKTAIL_ENGINE_STACK_MB");
  if (value == nullptr || value[0] == '\0') {
    return kDefaultEngineStackSize;
  }

  char* end = nullptr;
  unsigned long long stack_mb = std::strtoull(value, &end, 10);
  if (end == value || stack_mb == 0) {
    return kDefaultEngineStackSize;
  }

  size_t stack_size = static_cast<size_t>(stack_mb) * 1024 * 1024;
  return stack_size < kMinEngineStackSize ? kMinEngineStackSize : stack_size;
}

void PrintStage(int stage, const char* description) {
  std::cout << "[stage " << stage << "] " << description << '\n'
            << std::flush;
}


bool IsUnsafeSoftTimeoutModule(void* rip) {
  Dl_info dlinfo;
  if (rip == nullptr || dladdr(rip, &dlinfo) == 0 || dlinfo.dli_fname == nullptr) {
    return true;
  }
  const char* module = dlinfo.dli_fname;
  return std::strstr(module, "libc.so") != nullptr ||
         std::strstr(module, "libpthread.so") != nullptr ||
         std::strstr(module, "ld-linux") != nullptr ||
         std::strstr(module, "libstdc++") != nullptr ||
         std::strstr(module, "libgcc_s") != nullptr;
}

void JniOnLoadTimeoutAlarm(int, siginfo_t* info, void* context) {
  static_cast<void>(info);
  auto* uc = static_cast<ucontext_t*>(context);
  auto rip = uc ? static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]) : 0;
  auto rsp = uc ? static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]) : 0;
  auto rbp = uc ? static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RBP]) : 0;
  auto rax = uc ? static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RAX]) : 0;
  char regs_msg[192];
  int len = std::snprintf(
      regs_msg, sizeof(regs_msg),
      "  [timeout] RIP=0x%016llx RSP=0x%016llx RBP=0x%016llx "
      "RAX=0x%016llx\n",
      static_cast<unsigned long long>(rip), static_cast<unsigned long long>(rsp),
      static_cast<unsigned long long>(rbp), static_cast<unsigned long long>(rax));
  write(2, regs_msg, static_cast<size_t>(len));

  if (g_jni_onload_in_progress == 0 || g_jni_onload_timings_printed != 0) {
    return;
  }
  g_jni_onload_timings_printed = 1;

  const char prefix[] =
      "  [timeout] JNI_OnLoad still in progress; stack snapshot:\n";
  write(2, prefix, sizeof(prefix) - 1);
  PrintContextBacktrace(uc, "    ");
  PrintBacktraceNoSig("    ");
  if (g_jni_onload_soft_timeout != 0 && g_jni_onload_jmp_armed != 0 &&
      !IsUnsafeSoftTimeoutModule(reinterpret_cast<void*>(rip))) {
    g_jni_onload_jmp_armed = 0;
    g_jni_onload_in_progress = 0;
    siglongjmp(g_jni_onload_jmp_buf, 1);
    return;
  }
  if (g_jni_onload_soft_timeout != 0 && g_jni_onload_jmp_armed != 0) {
    const char skip_msg[] =
        "  [timeout] soft timeout skipped for unsafe module; waiting\n";
    write(2, skip_msg, sizeof(skip_msg) - 1);
    return;
  }
  _exit(124);
}


void PublishCurrentJniEnv(JNIEnv* env) {
  using SetCurrentJniEnvFn = void (*)(void*);
  auto* set_current_jni_env = reinterpret_cast<SetCurrentJniEnvFn>(
      ::dlsym(RTLD_DEFAULT, "mocktail_set_current_jni_env"));
  if (set_current_jni_env) {
    set_current_jni_env(env);
  }
}

bool EngineTraceEnabled() {
  return IsEnabled("MOCKTAIL_ENGINE_TRACE");
}

void PreloadPthreadSymbols() {
  using PreloadPthreadSymbolsFn = void (*)();
  auto* preload_pthread_symbols = reinterpret_cast<PreloadPthreadSymbolsFn>(
      ::dlsym(RTLD_DEFAULT, "mocktail_preload_pthread_symbols"));
  if (preload_pthread_symbols) {
    preload_pthread_symbols();
  }
}

void EngineLog(const char* message) {
  if (EngineTraceEnabled()) {
    std::cerr << "  [engine] " << message << '\n';
  }
}

void EngineLogPtr(const char* name, const void* ptr) {
  if (EngineTraceEnabled()) {
    std::cerr << "  [engine] " << name << "=0x"
              << std::hex << reinterpret_cast<uintptr_t>(ptr) << std::dec
              << '\n';
  }
}

void PrintStepDecision(const char* name, bool enabled) {
  if (!VerboseOutputEnabled()) {
    return;
  }
  std::cout << "  [engine] " << (enabled ? "run  " : "skip ") << name << '\n'
            << std::flush;
}

void PrintNativeBypass(const char* name, const char* flag) {
  if (!VerboseOutputEnabled()) {
    return;
  }
  std::cout << "  [engine] bypass native " << name << " (set " << flag
            << "=1 to call Roblox entrypoint)\n"
            << std::flush;
}


// Free counterpart for host-owned allocations. Pointers without the Mocktail
// alloc header are ignored so native freelist walks do not touch uninit state
// after constructors are skipped on the host.


mocktail::compat::HostAbiExperimentResult g_host_abi_install_result;
bool g_host_abi_install_attempted = false;

bool HostAbiExperimentRequested() {
  return g_allow_host_abi_bridges.load(std::memory_order_acquire) &&
         !IsDisabled("MOCKTAIL_HOST_ABI_BRIDGES");
}

bool InstallActiveHostAbiExperiment(uintptr_t libroblox_base) {
  if (g_host_abi_install_attempted) {
    return static_cast<bool>(g_host_abi_install_result);
  }
  g_host_abi_install_attempted = true;
  if (!HostAbiExperimentRequested()) {
    std::cout << "  [compat] host ABI profile disabled by policy\n"
              << std::flush;
    return false;
  }

  const mocktail::compat::HostAbiProfile* profile =
      g_active_host_abi_profile.load(std::memory_order_acquire);
  if (libroblox_base == 0 || profile == nullptr) {
    std::cerr << "  [compat] host ABI install has no active profile/base\n"
              << std::flush;
    return false;
  }

  const mocktail::compat::HostAbiBridgeTargets targets{
      reinterpret_cast<void*>(&mocktail::compat::HostAllocate),
      reinterpret_cast<void*>(&mocktail::compat::HostReallocate),
      reinterpret_cast<void*>(&mocktail::compat::HostAlignedAllocate),
      reinterpret_cast<void*>(&mocktail::compat::HostFree),
      reinterpret_cast<void*>(&mocktail::compat::HostUsableSize),
      reinterpret_cast<void*>(&mocktail::compat::HostAllocatorObjectAllocate),
      reinterpret_cast<void*>(&NullVtableStub),
  };
  const char* allocator_bridge_override =
      std::getenv("MOCKTAIL_HOST_ALLOCATOR_BRIDGES");
  const mocktail::compat::HostAllocatorStrategy allocator_strategy =
      profile->ResolveAllocatorStrategy(
          allocator_bridge_override != nullptr,
          IsEnabled("MOCKTAIL_HOST_ALLOCATOR_BRIDGES"));
  const bool install_allocator_bridges =
      allocator_strategy ==
      mocktail::compat::HostAllocatorStrategy::kHostBridges;
  const mocktail::compat::HostAbiExperimentOptions options{
      install_allocator_bridges,
      install_allocator_bridges &&
          !IsDisabled("MOCKTAIL_HOST_ALLOCATOR_OBJECT"),
      install_allocator_bridges && !IsDisabled("MOCKTAIL_HOST_EMPTY_STRING"),
      install_allocator_bridges &&
          !IsDisabled("MOCKTAIL_HOST_ALLOC_ARENA_INIT"),
      install_allocator_bridges &&
          !IsDisabled("MOCKTAIL_HOST_JNI_SINGLETON_SEED"),
  };
  g_host_abi_install_result = mocktail::compat::InstallHostAbiExperiment(
      libroblox_base, *profile, targets, options);
  return static_cast<bool>(g_host_abi_install_result);
}

bool InitializeActiveHostAbiThread() {
  if (!HostAbiExperimentRequested()) {
    return true;
  }
  const mocktail::compat::HostAbiProfile* profile =
      g_active_host_abi_profile.load(std::memory_order_acquire);
  if (profile == nullptr || g_libroblox_base == 0) {
    return false;
  }
  return mocktail::compat::InitializeHostAbiThread(g_libroblox_base, *profile);
}


static uintptr_t NullVtableStub() { return 0; }


void PrintBacktraceNoSig(const char* prefix) { (void)prefix; }

void PrintContextBacktrace(ucontext_t* ucontext, const char* prefix) {
  (void)ucontext;
  (void)prefix;
}

bool EnvOffsetListContains(const char* name, uintptr_t offset) {
  const char* skip_list = std::getenv(name);
  if (skip_list == nullptr || *skip_list == '\0') {
    return false;
  }
  const char* cursor = skip_list;
  while (*cursor != '\0') {
    while (*cursor == ',' || std::isspace(static_cast<unsigned char>(*cursor))) {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }
    char* end = nullptr;
    errno = 0;
    unsigned long long parsed = std::strtoull(cursor, &end, 0);
    if (end == cursor) {
      break;
    }
    if (errno == 0 && static_cast<uintptr_t>(parsed) == offset) {
      return true;
    }
    cursor = end;
  }
  return false;
}

extern "C" int mocktail_recover_stack_chk_fail() {
  if (g_init_with_params_recovery_in_progress != 0) {
    g_init_with_params_recovery_in_progress = 0;
    siglongjmp(g_init_with_params_jmp_buf, 1);
  }
  if (g_start_app_with_params_recovery_in_progress != 0) {
    g_start_app_with_params_recovery_in_progress = 0;
    siglongjmp(g_start_app_with_params_jmp_buf, 1);
  }
  if (g_start_lua_app_dm_recovery_in_progress != 0) {
    g_start_lua_app_dm_recovery_in_progress = 0;
    siglongjmp(g_start_lua_app_dm_jmp_buf, 1);
  }
  if (g_game_global_init_recovery_in_progress != 0) {
    g_game_global_init_recovery_in_progress = 0;
    siglongjmp(g_game_global_init_jmp_buf, 1);
  }
  if (g_init_client_settings_recovery_in_progress != 0) {
    g_init_client_settings_recovery_in_progress = 0;
    siglongjmp(g_init_client_settings_jmp_buf, 1);
  }
  if (g_post_client_settings_recovery_in_progress != 0) {
    g_post_client_settings_recovery_in_progress = 0;
    siglongjmp(g_post_client_settings_jmp_buf, 1);
  }
  const char msg[] =
      "  [patch] stack check bridge had no active recovery target\n";
  write(2, msg, sizeof(msg) - 1);
  return 0;
}

bool EnvIndexRangeListContains(const char* name, size_t index) {
  const char* range_list = std::getenv(name);
  if (range_list == nullptr || *range_list == '\0') {
    return false;
  }

  const char* cursor = range_list;
  while (*cursor != '\0') {
    while (*cursor == ',' || std::isspace(static_cast<unsigned char>(*cursor))) {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }

    char* end = nullptr;
    unsigned long long start = std::strtoull(cursor, &end, 0);
    if (end == cursor) {
      break;
    }

    unsigned long long stop = start;
    cursor = end;
    if (*cursor == '-') {
      ++cursor;
      char* range_end = nullptr;
      stop = std::strtoull(cursor, &range_end, 0);
      if (range_end == cursor) {
        break;
      }
      cursor = range_end;
    }

    if (start <= index && index <= stop) {
      return true;
    }

    while (*cursor != '\0' && *cursor != ',') {
      ++cursor;
    }
  }

  return false;
}


bool LibRobloxConstructorOffsetSkipped(uintptr_t offset) {
  return EnvOffsetListContains("MOCKTAIL_SKIP_LIBROBLOX_CTOR_OFFSETS", offset);
}

bool LibRobloxConstructorIndexSkipped(size_t index) {
  return EnvIndexRangeListContains("MOCKTAIL_SKIP_LIBROBLOX_CTOR_INDEX_RANGES",
                                   index);
}

bool LibRobloxConstructorIndexAllowed(size_t index) {
  return EnvIndexRangeListContains("MOCKTAIL_ALLOW_LIBROBLOX_CTOR_INDEX_RANGES",
                                   index);
}


static size_t g_current_ctor_index = 0;
static std::vector<uintptr_t> g_original_ctors;
static uintptr_t g_libroblox_base_static = 0;
static size_t g_libroblox_ctor_start_index = 0;
static size_t g_libroblox_ctor_end_index = 0;
static size_t g_libroblox_ctor_executed_count = 0;
static size_t g_libroblox_ctor_skipped_range_count = 0;
static size_t g_libroblox_ctor_skipped_default_count = 0;
static size_t g_libroblox_ctor_skipped_index_count = 0;
static size_t g_libroblox_ctor_skipped_env_count = 0;



bool LibRobloxConstructorDefaultQuarantineEnabled() {
  // The Build-ID profile already supplies the final allowlisted range.
  // Applying the legacy "only index 0" quarantine on top would contradict it.
  if (g_allow_host_constructor_replay.load(std::memory_order_acquire)) {
    return false;
  }
  if (!IsEnabled("MOCKTAIL_RUN_LIBROBLOX_CTORS")) {
    return false;
  }
  if (IsDisabled("MOCKTAIL_QUARANTINE_LIBROBLOX_UNSAFE_CTORS")) {
    return false;
  }
  const char* policy = std::getenv("MOCKTAIL_LIBROBLOX_CTOR_POLICY");
  if (policy != nullptr && (std::strcmp(policy, "all") == 0 ||
                            std::strcmp(policy, "unsafe") == 0)) {
    return false;
  }
  return true;
}

bool LibRobloxConstructorDefaultQuarantineSkipped(size_t index) {
  if (!LibRobloxConstructorDefaultQuarantineEnabled()) {
    return false;
  }
  if (index == 0 || LibRobloxConstructorIndexAllowed(index)) {
    return false;
  }
  return true;
}

size_t GetCtorIndexEnv(const char* name, size_t default_value,
                       size_t max_value) {
  int value = GetEnvInt(name, -1);
  if (value < 0) {
    return default_value;
  }
  return std::min(static_cast<size_t>(value), max_value);
}

void MaybePrintLibRobloxConstructorSummary(size_t idx) {
  if (idx + 1 != g_original_ctors.size()) {
    return;
  }
  std::cout << "  [ctor] Summary: executed="
            << g_libroblox_ctor_executed_count
            << " skipped_by_range=" << g_libroblox_ctor_skipped_range_count
            << " skipped_by_default=" << g_libroblox_ctor_skipped_default_count
            << " skipped_by_index=" << g_libroblox_ctor_skipped_index_count
            << " skipped_by_env=" << g_libroblox_ctor_skipped_env_count
            << '\n'
            << std::flush;
}

extern "C" void MocktailConstructorWrapper() {
  if (g_current_ctor_index >= g_original_ctors.size()) {
    std::cerr << "  [ctor] Warning: constructor index " << g_current_ctor_index
              << " out of bounds!\n" << std::flush;
    return;
  }
  size_t idx = g_current_ctor_index++;
  uintptr_t orig = g_original_ctors[idx];
  uintptr_t offset = orig - g_libroblox_base_static;
  const bool trace_constructor = LibRobloxConstructorTraceEnabled();
  if (trace_constructor) {
    std::cout << "  [ctor] [" << idx << "/" << g_original_ctors.size()
              << "] offset 0x" << std::hex << offset << std::dec << '\n'
              << std::flush;
  }
  if (idx < g_libroblox_ctor_start_index ||
      idx >= g_libroblox_ctor_end_index) {
    ++g_libroblox_ctor_skipped_range_count;
    if (trace_constructor) {
      std::cout << "  [ctor] [" << idx << "] Skipped by ctor index range.\n"
                << std::flush;
    }
    MaybePrintLibRobloxConstructorSummary(idx);
    return;
  }
  const mocktail::compat::HostAbiProfile* active_host_profile =
      g_active_host_abi_profile.load(std::memory_order_acquire);
  const bool use_native_mimalloc =
      g_host_abi_install_attempted &&
      g_host_abi_install_result.uses_native_mimalloc;
  if (g_allow_host_constructor_replay.load(std::memory_order_acquire) &&
      active_host_profile != nullptr &&
      !(use_native_mimalloc
            ? active_host_profile->AllowsNativeMimallocConstructor(idx)
            : active_host_profile->AllowsConstructor(idx))) {
    ++g_libroblox_ctor_skipped_range_count;
    if (trace_constructor) {
      std::cout << "  [ctor] [" << idx << "] Skipped by typed "
                << (use_native_mimalloc ? "native Mimalloc" : "host bridge")
                << " run ranges.\n"
                << std::flush;
    }
    MaybePrintLibRobloxConstructorSummary(idx);
    return;
  }
  if (LibRobloxConstructorDefaultQuarantineSkipped(idx)) {
    ++g_libroblox_ctor_skipped_default_count;
    if (trace_constructor) {
      std::cout << "  [ctor] [" << idx
                << "] Skipped by default unsafe ctor quarantine.\n"
                << std::flush;
    }
    MaybePrintLibRobloxConstructorSummary(idx);
    return;
  }
  if (LibRobloxConstructorIndexSkipped(idx)) {
    ++g_libroblox_ctor_skipped_index_count;
    if (trace_constructor) {
      std::cout << "  [ctor] [" << idx << "] Skipped by ctor index env.\n"
                << std::flush;
    }
    MaybePrintLibRobloxConstructorSummary(idx);
    return;
  }
  if (LibRobloxConstructorOffsetSkipped(offset)) {
    ++g_libroblox_ctor_skipped_env_count;
    if (trace_constructor) {
      std::cout << "  [ctor] [" << idx << "] Skipped by env.\n"
                << std::flush;
    }
    MaybePrintLibRobloxConstructorSummary(idx);
    return;
  }
  if (trace_constructor) {
    std::cout << "  [ctor] [" << idx << "] Executing.\n" << std::flush;
  }
  void (*fn)() = reinterpret_cast<void(*)()>(orig);
  fn();
  if (use_native_mimalloc && active_host_profile != nullptr) {
    const mocktail::compat::NativeMimallocBootstrapStatus bootstrap_status =
        mocktail::compat::CompleteNativeMimallocConstructor(
            g_libroblox_base_static, *active_host_profile, idx);
    if (bootstrap_status ==
        mocktail::compat::NativeMimallocBootstrapStatus::kInitialized) {
      std::cout << "  [ctor] Native mimalloc thread state initialized after ["
                << idx << "]\n"
                << std::flush;
    } else if (bootstrap_status ==
               mocktail::compat::NativeMimallocBootstrapStatus::kFailed) {
      std::cerr << "  [ctor] Native mimalloc thread state initialization "
                   "failed after ["
                << idx << "]\n"
                << std::flush;
    }
  }
  ++g_libroblox_ctor_executed_count;
  if (trace_constructor) {
    std::cout << "  [ctor] [" << idx << "] Done.\n" << std::flush;
  }
  MaybePrintLibRobloxConstructorSummary(idx);
}

extern "C" void mocktail_before_soinfo_constructors(const char* realpath,
                                                    uintptr_t base) {
  if (realpath == nullptr || std::strstr(realpath, "libroblox.so") == nullptr ||
      base == 0) {
    return;
  }

  g_libroblox_base = base;
  g_mocktail_abort_libroblox_base = base;
  g_libroblox_base_static = base;

  const mocktail::compat::HostAbiProfile* host_abi =
      g_active_host_abi_profile.load(std::memory_order_acquire);

  // Host alloc bridges must be in place before any constructor executes.
  if (HostAbiExperimentRequested() && !InstallActiveHostAbiExperiment(base)) {
    std::cerr << "  [compat] host ABI install failed before constructors\n"
              << std::flush;
    return;
  }

  const bool force_run_ctors =
      (std::getenv("MOCKTAIL_SKIP_LIBROBLOX_CTORS") != nullptr &&
       std::strcmp(std::getenv("MOCKTAIL_SKIP_LIBROBLOX_CTORS"), "0") == 0) ||
      IsEnabled("MOCKTAIL_RUN_LIBROBLOX_CTORS");
  const bool want_light_wrap =
      IsEnabled("MOCKTAIL_WRAP_LIBROBLOX_CTORS") || force_run_ctors;
  const bool use_native_mimalloc =
      g_host_abi_install_attempted &&
      g_host_abi_install_result.uses_native_mimalloc;
  mocktail::compat::NativeThreadInitializer thread_initializer = nullptr;
  if (use_native_mimalloc && host_abi != nullptr &&
      host_abi->data_seeds.allocator_thread_initializer != 0) {
    thread_initializer =
        reinterpret_cast<mocktail::compat::NativeThreadInitializer>(
            base + host_abi->data_seeds.allocator_thread_initializer);
  }
  mocktail::compat::ConfigureBionicPthreadThreadInitializer(
      thread_initializer);
  libc_shim::GuestAllocator guest_allocator = nullptr;
  if (use_native_mimalloc && host_abi != nullptr &&
      host_abi->native_allocator.IsValid()) {
    guest_allocator = reinterpret_cast<libc_shim::GuestAllocator>(
        base + host_abi->native_allocator.allocate);
  }
  libc_shim::ConfigureGuestAllocator(guest_allocator);
  const bool has_selected_constructor_ranges =
      host_abi != nullptr &&
      (use_native_mimalloc
           ? host_abi->HasValidNativeMimallocConstructorRanges()
           : host_abi->HasValidConstructorRanges());
  const bool have_init_array =
      g_allow_host_constructor_replay.load(std::memory_order_acquire) &&
      HostAbiExperimentRequested() &&
      host_abi != nullptr && host_abi->init_array_offset != 0 &&
      has_selected_constructor_ranges;
  if (!(want_light_wrap && have_init_array)) {
    std::cout << "  [compat] leaving libroblox constructors untouched for "
                 "this Build-ID profile\n"
              << std::flush;
    return;
  }

  g_current_ctor_index = 0;
  g_libroblox_ctor_executed_count = 0;
  g_libroblox_ctor_skipped_range_count = 0;
  g_libroblox_ctor_skipped_default_count = 0;
  g_libroblox_ctor_skipped_index_count = 0;
  g_libroblox_ctor_skipped_env_count = 0;

  size_t kCtorCount = 0;
  uintptr_t init_array_offset = 0;
  if (have_init_array) {
    init_array_offset = host_abi->init_array_offset;
    kCtorCount = host_abi->init_array_count;
  }
  const size_t profile_ctor_start =
      have_init_array
          ? (use_native_mimalloc
                 ? host_abi->NativeMimallocConstructorRangeBegin()
                 : host_abi->ConstructorRangeBegin())
          : 0;
  const size_t profile_ctor_end =
      have_init_array
          ? (use_native_mimalloc
                 ? host_abi->NativeMimallocConstructorRangeEndExclusive()
                 : host_abi->ConstructorRangeEndExclusive())
          : kCtorCount;
  g_libroblox_ctor_start_index = std::max(
      profile_ctor_start,
      GetCtorIndexEnv("MOCKTAIL_LIBROBLOX_CTOR_START_INDEX",
                      profile_ctor_start, kCtorCount));
  g_libroblox_ctor_end_index = std::min(
      profile_ctor_end,
      GetCtorIndexEnv("MOCKTAIL_LIBROBLOX_CTOR_END_INDEX", profile_ctor_end,
                      kCtorCount));
  int max_ctors = GetEnvInt("MOCKTAIL_MAX_LIBROBLOX_CTORS", -1);
  if (max_ctors >= 0) {
    g_libroblox_ctor_end_index =
        std::min(g_libroblox_ctor_end_index,
                 g_libroblox_ctor_start_index +
                     static_cast<size_t>(max_ctors));
  }
  if (g_libroblox_ctor_end_index < g_libroblox_ctor_start_index) {
    g_libroblox_ctor_end_index = g_libroblox_ctor_start_index;
  }
  uintptr_t* init_array =
      reinterpret_cast<uintptr_t*>(base + init_array_offset);

  long ctor_page_size = sysconf(_SC_PAGESIZE);
  if (ctor_page_size > 0) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(init_array);
    uintptr_t page = addr & ~(static_cast<uintptr_t>(ctor_page_size) - 1);
    const size_t span =
        (kCtorCount * sizeof(uintptr_t)) + static_cast<size_t>(ctor_page_size);
    mprotect(reinterpret_cast<void*>(page), span, PROT_READ | PROT_WRITE);
  }

  g_original_ctors.clear();
  g_original_ctors.reserve(kCtorCount);
  for (size_t i = 0; i < kCtorCount; ++i) {
    g_original_ctors.push_back(init_array[i]);
    init_array[i] = reinterpret_cast<uintptr_t>(MocktailConstructorWrapper);
  }
  std::cout << "  [compat] wrapping .init_array @+0x" << std::hex
            << init_array_offset << std::dec << " count=" << kCtorCount
            << '\n'
            << std::flush;
  std::cout << "  [compat] Wrapped " << kCtorCount
            << " constructors for typed replay"
            << (LibRobloxConstructorTraceEnabled() ? " with trace" : "")
            << '\n'
            << std::flush;
  std::cout << "  [ctor] Active index range ["
            << g_libroblox_ctor_start_index << ", "
            << g_libroblox_ctor_end_index << ")\n"
            << std::flush;
  if (use_native_mimalloc) {
    std::cout << "  [ctor] Native mimalloc integration probe selected; "
                 "host allocator bridges are disabled\n"
              << std::flush;
  }
  if (LibRobloxConstructorDefaultQuarantineEnabled()) {
    std::cout
        << "  [ctor] Default unsafe ctor quarantine enabled: only index 0 "
           "runs unless MOCKTAIL_ALLOW_LIBROBLOX_CTOR_INDEX_RANGES allows more\n"
        << std::flush;
  } else {
    std::cout << "  [ctor] Default unsafe ctor quarantine disabled\n"
              << std::flush;
  }

}

extern "C" bool mocktail_should_skip_soinfo_constructors(const char* realpath) {
  if (realpath == nullptr ||
      std::strstr(realpath, "libroblox.so") == nullptr) {
    return false;
  }

  const bool typed_replay_allowed =
      g_allow_host_constructor_replay.load(std::memory_order_acquire) &&
      HostAbiExperimentRequested();
  if (!typed_replay_allowed) {
    std::cout << "  [compat] skipping libroblox constructors: no exact "
                 "Build-ID replay policy\n"
              << std::flush;
    return true;
  }

  // Explicit host ABI policy, independent of fixed-offset binary patches.
  // Native libroblox .init_array currently aborts in emutls growth on Linux
  // hosts (caller off≈0x28bae15 on 2.725.1142). Skip by default for
  // non-patched profiles so name-based JNI startup remains reachable.
  //
  // Controls:
  //   MOCKTAIL_SKIP_LIBROBLOX_CTORS=1  force skip
  //   MOCKTAIL_SKIP_LIBROBLOX_CTORS=0  force run
  //   MOCKTAIL_RUN_LIBROBLOX_CTORS=1   force run (legacy name)
  const char* skip_constructors = std::getenv("MOCKTAIL_SKIP_LIBROBLOX_CTORS");
  if (skip_constructors != nullptr) {
    if (std::strcmp(skip_constructors, "0") == 0) {
      std::cout << "  [compat] running libroblox static constructors "
                   "(MOCKTAIL_SKIP_LIBROBLOX_CTORS=0)\n"
                << std::flush;
      return false;
    }
    std::cout << "  [compat] skipping libroblox static constructors "
                 "(MOCKTAIL_SKIP_LIBROBLOX_CTORS)\n"
              << std::flush;
    return true;
  }

  const char* run_constructors = std::getenv("MOCKTAIL_RUN_LIBROBLOX_CTORS");
  if (run_constructors != nullptr && std::strcmp(run_constructors, "0") != 0) {
    std::cout << "  [compat] running libroblox static constructors "
                 "(MOCKTAIL_RUN_LIBROBLOX_CTORS)\n"
              << std::flush;
    return false;
  }

  if (typed_replay_allowed) {
    std::cout
        << "  [compat] skipping libroblox static constructors for host load "
           "(set MOCKTAIL_SKIP_LIBROBLOX_CTORS=0 to force native .init_array)\n"
        << std::flush;
    return true;
  }

  std::cout << "  [patch] skipping libroblox static constructors"
            << " (set MOCKTAIL_RUN_LIBROBLOX_CTORS=1 to enable)\n"
            << std::flush;
  return true;
}


std::string JStringToString(JNIEnv* env, jstring value) {
  if (!env || !value) {
    return {};
  }
  const char* chars = env->GetStringUTFChars(value, nullptr);
  if (!chars) {
    return {};
  }
  std::string result(chars);
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

void MocktailSetAssetPath(JNIEnv* env, jstring asset_path) {
  std::string path = JStringToString(env, asset_path);
  if (path.empty()) {
    path = DefaultAssetPath();
  }
  setenv("MOCKTAIL_ASSET_ROOT", path.c_str(), 1);
  std::cout << "  [engine] asset root set to " << path << '\n' << std::flush;
}

void MocktailAppBridgeInit(JNIEnv* env, jstring app_params) {
  std::string params = JStringToString(env, app_params);
  if (params.empty()) {
    params = "{}";
  }
  setenv("MOCKTAIL_APP_BRIDGE_PARAMS", params.c_str(), 1);
  setenv("MOCKTAIL_APP_BRIDGE_INIT", "1", 1);
  std::cout << "  [engine] app bridge params staged\n" << std::flush;
}

void MocktailAppBridgeStart(JNIEnv* env, jstring app_params) {
  std::string params = JStringToString(env, app_params);
  if (params.empty()) {
    params = "{}";
  }
  setenv("MOCKTAIL_APP_BRIDGE_START_PARAMS", params.c_str(), 1);
  setenv("MOCKTAIL_APP_STARTED", "1", 1);
  std::cout << "  [engine] app bridge marked started\n" << std::flush;
}

jstring NewStringFromEnvDefault(JNIEnv* env, const char* env_name,
                                const char* default_value) {
  const char* value = std::getenv(env_name);
  if (!value || value[0] == '\0') {
    value = default_value;
  }
  return env->NewStringUTF(value);
}

std::string MocktailCookiePath() {
  return MocktailConfigRoot() + "/cookie";
}

bool CookieHasAttribute(const std::string& cookie, const char* attribute) {
  std::string lower_cookie = cookie;
  std::string lower_attribute = attribute ? attribute : "";
  std::transform(lower_cookie.begin(), lower_cookie.end(),
                 lower_cookie.begin(), [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  std::transform(lower_attribute.begin(), lower_attribute.end(),
                 lower_attribute.begin(), [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return !lower_attribute.empty() &&
         lower_cookie.find(lower_attribute) != std::string::npos;
}

std::string CookieForJNICookieManager(std::string_view cookie_header,
                                      const std::string& domain) {
  if (cookie_header.empty()) {
    return {};
  }
  std::string cookie(cookie_header);
  if (!CookieHasAttribute(cookie, "domain=")) {
    if (!cookie.empty() && cookie.back() != ';') {
      cookie += "; ";
    } else if (!cookie.empty()) {
      cookie += ' ';
    }
    cookie += "Domain=";
    cookie += domain.empty() ? "roblox.com" : domain;
  }
  return cookie;
}

void ApplyAuthStartupDefaults(bool credential_available,
                              bool user_overrode_start_lua_app_dm,
                              bool user_overrode_start_lua_step,
                              bool user_overrode_start_app_step,
                              bool user_overrode_call_start_app) {
  if (credential_available) {
    std::cout << "  [auth] typed Roblox credential ready for native startup\n";
    return;
  }
  if (IsEnabled("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP")) {
    std::cout << "  [auth] no Roblox cookie found; continuing in no-cookie mode\n"
              << std::flush;
    return;
  }

  std::cerr << "  [auth] no Roblox cookie found; continuing in strict mode\n"
            << std::flush;

  if (!user_overrode_start_lua_app_dm) {
    setenv("MOCKTAIL_START_LUA_APP_DM", "0", 1);
  }
  if (!user_overrode_start_lua_step) {
    setenv("MOCKTAIL_STEP_START_LUA_APP_DM", "0", 1);
  }
  if (!user_overrode_start_app_step) {
    setenv("MOCKTAIL_STEP_START_APP_WITH_PARAMS", "0", 1);
  }
  if (!user_overrode_call_start_app) {
    setenv("MOCKTAIL_CALL_REAL_APP_BRIDGE_START", "0", 1);
  }
}

std::string ResolveClientSettingsJson() {
  mocktail::services::ClientSettingsOptions options;
  options.explicit_json =
      GetEnvString("MOCKTAIL_CLIENT_SETTINGS_JSON", "");
  options.explicit_file =
      GetEnvString("MOCKTAIL_CLIENT_SETTINGS_JSON_FILE", "");
  options.use_bundled = IsEnabled("MOCKTAIL_USE_BUNDLED_CLIENT_SETTINGS");
  options.sober_mode = IsEnabled("MOCKTAIL_SOBER_MODE");
  options.fetch = IsEnabled("MOCKTAIL_FETCH_CLIENT_SETTINGS");
  options.auto_update =
      !IsDisabled("MOCKTAIL_CLIENT_SETTINGS_AUTO_UPDATE");
  options.application =
      GetEnvString("MOCKTAIL_CLIENT_SETTINGS_APP", "GoogleAndroidApp");
  options.url = GetEnvString("MOCKTAIL_CLIENT_SETTINGS_URL", "");
  options.cache_file = GetEnvString(
      "MOCKTAIL_CLIENT_SETTINGS_CACHE_FILE",
      (MocktailCacheRoot() + "/clientsettings/" + options.application +
       ".json")
          .c_str());

  static mocktail::services::CurlHttpClient http_client;
  static mocktail::services::ClientSettingsService settings_service(
      http_client);
  const mocktail::services::ClientSettingsResult result =
      settings_service.Resolve(options);

  using mocktail::services::ClientSettingsSource;
  switch (result.source) {
    case ClientSettingsSource::kExplicitJson:
      break;
    case ClientSettingsSource::kExplicitFile:
      std::cout << "  [settings] using explicit client settings file "
                << options.explicit_file << " bytes=" << result.json.size()
                << '\n';
      break;
    case ClientSettingsSource::kBundledFile:
      std::cout << "  [settings] using bundled client settings "
                << options.bundled_file << " bytes=" << result.json.size()
                << '\n';
      break;
    case ClientSettingsSource::kSafeDefaults:
      std::cout << "  [settings] using safe inline client settings\n";
      break;
    case ClientSettingsSource::kDownloaded:
      std::cout << "  [settings] flags "
                << (result.cache_updated ? "updated" : "unchanged") << '\n';
      break;
    case ClientSettingsSource::kCache:
      if (!result.error.empty()) {
        std::cerr
            << "  [settings] CDN fetch failed; using cached flags if present: "
            << result.error << '\n';
      }
      std::cout << "  [settings] using cached flags\n";
      break;
    case ClientSettingsSource::kEmptyDefaults:
      if (!result.error.empty()) {
        std::cerr << "  [settings] CDN fetch failed: " << result.error << '\n';
      }
      std::cerr
          << "  [settings] no client settings available; using empty defaults\n";
      break;
  }
  std::cout << std::flush;
  std::cerr << std::flush;
  return result.json;
}

jstring NewClientSettingsString(JNIEnv* env) {
  std::string content = ResolveClientSettingsJson();
  return env->NewStringUTF(content.c_str());
}

void SetObjectField(JNIEnv* env, jobject object, const char* name,
                    const char* signature, jobject value) {
  if (!env || !object || !name || !signature) {
    return;
  }
  jclass object_class = env->GetObjectClass(object);
  jfieldID field_id = env->GetFieldID(object_class, name, signature);
  env->SetObjectField(object, field_id, value);
}

void SetStringField(JNIEnv* env, jobject object, const char* name,
                    const char* value) {
  SetObjectField(env, object, name, "Ljava/lang/String;",
                 env->NewStringUTF(value ? value : ""));
}

void SetJStringField(JNIEnv* env, jobject object, const char* name,
                    jstring value) {
  if (!env || !object || !name) {
    return;
  }
  jclass object_class = env->GetObjectClass(object);
  jfieldID field_id = env->GetFieldID(object_class, name, "Ljava/lang/String;");
  env->SetObjectField(object, field_id, value);
}

void SetIntField(JNIEnv* env, jobject object, const char* name, jint value) {
  if (!env || !object || !name) {
    return;
  }
  jclass object_class = env->GetObjectClass(object);
  jfieldID field_id = env->GetFieldID(object_class, name, "I");
  env->SetIntField(object, field_id, value);
}

void SetLongField(JNIEnv* env, jobject object, const char* name, jlong value) {
  if (!env || !object || !name) {
    return;
  }
  jclass object_class = env->GetObjectClass(object);
  jfieldID field_id = env->GetFieldID(object_class, name, "J");
  env->SetLongField(object, field_id, value);
}

void SetFloatField(JNIEnv* env, jobject object, const char* name,
                   jfloat value) {
  if (!env || !object || !name) {
    return;
  }
  jclass object_class = env->GetObjectClass(object);
  jfieldID field_id = env->GetFieldID(object_class, name, "F");
  env->SetFloatField(object, field_id, value);
}

void SetBooleanField(JNIEnv* env, jobject object, const char* name,
                     jboolean value) {
  if (!env || !object || !name) {
    return;
  }
  jclass object_class = env->GetObjectClass(object);
  jfieldID field_id = env->GetFieldID(object_class, name, "Z");
  env->SetBooleanField(object, field_id, value);
}

void SetRobloxServiceUrlFields(JNIEnv* env, jobject object) {
  if (!env || !object) {
    return;
  }
  constexpr const char* kBaseUrl = "https://www.roblox.com";
  constexpr const char* kApiUrl = "https://apis.roblox.com";
  constexpr const char* kClientSettingsUrl =
      "https://clientsettingscdn.roblox.com/v2/settings-compressed/"
      "application/AndroidApp.zst";
  constexpr const char* kClientSettingsBaseUrl =
      "https://clientsettingscdn.roblox.com";
  constexpr const char* kEcsv2ClientUrl =
      "https://ecsv2.roblox.com/client/pbe";
  constexpr const char* kEcsv2TimespentUrl =
      "https://ecsv2.roblox.com/timespent/pbe";
  constexpr const char* kTelemetryUrl =
      "https://apis.roblox.com/experience-signals-ingest/public/v1/"
      "events/single";

  SetStringField(env, object, "baseUrl", kBaseUrl);
  SetStringField(env, object, "baseURL", kBaseUrl);
  SetStringField(env, object, "wwwBaseUrl", kBaseUrl);
  SetStringField(env, object, "apiBaseUrl", kApiUrl);
  SetStringField(env, object, "apiGatewayUrl", kApiUrl);
  SetStringField(env, object, "clientSettingsUrl", kClientSettingsUrl);
  SetStringField(env, object, "settingsUrl", kClientSettingsUrl);
  SetStringField(env, object, "clientSettingsBaseUrl", kClientSettingsBaseUrl);
  SetStringField(env, object, "clientSettingsHost", kClientSettingsBaseUrl);
  SetStringField(env, object, "ecsv2Url", kEcsv2ClientUrl);
  SetStringField(env, object, "ecsUrl", kEcsv2ClientUrl);
  SetStringField(env, object, "eventStreamUrl", kEcsv2ClientUrl);
  SetStringField(env, object, "telemetryUrl", kTelemetryUrl);
  SetStringField(env, object, "telemetryEndpoint", kTelemetryUrl);
  SetStringField(env, object, "timeSpentUrl", kEcsv2TimespentUrl);
}

jobject NewObject(JNIEnv* env, const char* class_name) {
  if (!env || !class_name) {
    return nullptr;
  }
  jclass clazz = env->FindClass(class_name);
  return env->AllocObject(clazz);
}

void InstallNativeGlJavaImplementation(JNIEnv* env, jclass native_gl_java_class) {
  if (IsDisabled("MOCKTAIL_SET_NATIVE_GL_JAVA_IMPLEMENTATION")) {
    return;
  }
  if (env == nullptr || native_gl_java_class == nullptr) {
    std::cerr << "  [engine] NativeGLJavaInterface.setImplementation skipped: "
              << "missing JNI state\n"
              << std::flush;
    return;
  }

  jobject engine_java_callback =
      NewObject(env, "com/roblox/engine/jni/EngineJavaCallback2");
  if (engine_java_callback == nullptr) {
    std::cerr << "  [engine] NativeGLJavaInterface.setImplementation skipped: "
              << "EngineJavaCallback2 allocation failed\n"
              << std::flush;
    return;
  }

  jmethodID set_implementation = env->GetStaticMethodID(
      native_gl_java_class, "setImplementation",
      "(Lcom/roblox/engine/jni/EngineJavaCallback2;)V");
  if (set_implementation == nullptr) {
    std::cerr << "  [engine] NativeGLJavaInterface.setImplementation skipped: "
              << "method not found\n"
              << std::flush;
    return;
  }

  std::cout << "  [engine] NativeGLJavaInterface.setImplementation\n"
            << std::flush;
  env->CallStaticVoidMethod(native_gl_java_class, set_implementation,
                            engine_java_callback);
  std::cout << "  [engine] NativeGLJavaInterface.setImplementation returned\n"
            << std::flush;
}

jobject BuildPlatformParams(JNIEnv* env, jobject surface, bool is_headless);

jobject BuildDeviceParams(JNIEnv* env) {
  jobject params = NewObject(env, "com/roblox/engine/jni/model/DeviceParams");
  if (!params) {
    return nullptr;
  }

  const std::string device_name =
      GetEnvString("MOCKTAIL_DEVICE_NAME", "Mocktail Linux");
  const std::string manufacturer =
      GetEnvString("MOCKTAIL_DEVICE_MANUFACTURER", "Mocktail");
  const std::string device_sku =
      GetEnvString("MOCKTAIL_DEVICE_SKU", "mocktail-x86_64");
  const std::string soc_model =
      GetEnvString("MOCKTAIL_DEVICE_SOC_MODEL", "x86_64");
  SetStringField(env, params, "osVersion", "33");
  SetStringField(env, params, "deviceName", device_name.c_str());
  const std::string app_version =
      GetEnvString("MOCKTAIL_ROBLOX_VERSION", "unknown");
  SetStringField(env, params, "appVersion", app_version.c_str());
  SetStringField(env, params, "country", "US");
  SetStringField(env, params, "manufacturer", manufacturer.c_str());
  SetStringField(env, params, "displayResolution", "1280x720");
  SetStringField(env, params, "networkType", "wifi");
  SetStringField(env, params, "deviceSku", device_sku.c_str());
  SetStringField(env, params, "socModel", soc_model.c_str());
  SetStringField(env, params, "appBuildVariant", "googleProdRelease");
  SetStringField(env, params, "testDeviceName", device_name.c_str());

  const bool is_low_ram = !IsEnabled("MOCKTAIL_DISABLE_LOW_RAM_DEVICE");
  SetIntField(env, params, "deviceTotalMemoryMB", is_low_ram ? 2048 : 4096);
  SetIntField(env, params, "displayPhysicalWidthPixels", 1280);
  SetIntField(env, params, "displayPhysicalHeightPixels", 720);
  SetIntField(env, params, "memoryClass", is_low_ram ? 256 : 512);
  SetIntField(env, params, "largeMemoryClass", is_low_ram ? 512 : 1024);
  SetLongField(env, params, "lowMemoryKillerBackgroundAppThreshold",
               is_low_ram ? 256 : 0);
  SetLongField(env, params, "lowMemoryKillerForegroundAppThreshold",
               is_low_ram ? 512 : 0);
  SetBooleanField(env, params, "cpu64Bit", JNI_TRUE);
  SetBooleanField(env, params, "isChrome", JNI_FALSE);
  SetBooleanField(env, params, "isLowRamDevice",
                  is_low_ram ? JNI_TRUE : JNI_FALSE);
  return params;
}

jobject BuildAppBridgeInitParams(JNIEnv* env, jstring client_settings,
                                 jstring fast_flags, jstring app_params,
                                 jstring asset_path, bool is_headless) {
  jobject params =
      NewObject(env, "com/roblox/engine/jni/autovalue/InitParams");
  if (!params) {
    return nullptr;
  }

  jobject activity = NewObject(env, "com/roblox/client/RobloxActivity");
  jobject context = NewObject(env, "android/content/Context");
  jobject asset_manager = NewObject(env, "android/content/res/AssetManager");
  jobject resources = NewObject(env, "android/content/res/Resources");
  jobject class_loader = NewObject(env, "java/lang/ClassLoader");
  jobject package_manager = NewObject(env, "android/content/pm/PackageManager");
  jobject window = NewObject(env, "android/view/Window");
  jobject display = NewObject(env, "android/view/Display");
  jobject surface = NewObject(env, "android/view/Surface");
  jobject platform_params = BuildPlatformParams(env, surface, is_headless);
  jobject device_params = BuildDeviceParams(env);
  jobject surface_holder = NewObject(env, "android/view/SurfaceHolder");
  jobject view = NewObject(env, "android/view/View");

  SetObjectField(env, params, "platformParams",
                 "Lcom/roblox/engine/jni/model/PlatformParams;",
                 platform_params);
  jobject start_game_device_params =
      IsEnabled("MOCKTAIL_START_GAME_DEVICE_PARAMS_NON_NULL") ? device_params
                                                             : nullptr;
  SetObjectField(env, params, "deviceParams",
                 "Lcom/roblox/engine/jni/model/DeviceParams;",
                 start_game_device_params);
  SetStringField(env, params, "baseURL", "https://www.roblox.com");
  const std::string user_agent = GetEnvString(
      "MOCKTAIL_USER_AGENT", "Roblox/unknown (Linux; Android 33; Mocktail)");
  SetStringField(env, params, "userAgent", user_agent.c_str());
  SetBooleanField(env, params, "isTablet", JNI_FALSE);
  SetBooleanField(env, params, "isPotato", JNI_FALSE);
  SetBooleanField(env, params, "isVrDevice", JNI_FALSE);
  SetStringField(env, params, "buildVariant", "googleProdRelease");
  SetObjectField(env, params, "vrContext", "Landroid/app/Activity;",
                 activity);

  SetObjectField(env, params, "activity", "Lcom/roblox/client/RobloxActivity;",
                 activity);
  SetObjectField(env, params, "robloxActivity",
                 "Lcom/roblox/client/RobloxActivity;", activity);
  SetObjectField(env, params, "mainActivity",
                 "Lcom/roblox/client/RobloxActivity;", activity);
  SetObjectField(env, params, "context", "Landroid/content/Context;", context);
  SetObjectField(env, params, "applicationContext",
                 "Landroid/content/Context;", context);
  SetObjectField(env, params, "assetManager",
                 "Landroid/content/res/AssetManager;", asset_manager);
  SetObjectField(env, params, "assets",
                 "Landroid/content/res/AssetManager;", asset_manager);
  SetObjectField(env, params, "resources",
                 "Landroid/content/res/Resources;", resources);
  SetObjectField(env, params, "classLoader", "Ljava/lang/ClassLoader;",
                 class_loader);
  SetObjectField(env, params, "packageManager",
                 "Landroid/content/pm/PackageManager;", package_manager);
  SetObjectField(env, params, "window", "Landroid/view/Window;", window);
  SetObjectField(env, params, "display", "Landroid/view/Display;", display);
  SetObjectField(env, params, "surface", "Landroid/view/Surface;", surface);
  SetObjectField(env, params, "surfaceHolder", "Landroid/view/SurfaceHolder;",
                 surface_holder);
  SetObjectField(env, params, "view", "Landroid/view/View;", view);
  SetObjectField(env, params, "decorView", "Landroid/view/View;", view);
  SetObjectField(env, params, "rootView", "Landroid/view/View;", view);

  SetObjectField(env, params, "clientSettingsJson", "Ljava/lang/String;",
                 client_settings);
  SetObjectField(env, params, "clientSettings", "Ljava/lang/String;",
                 client_settings);
  SetObjectField(env, params, "fastFlagsJson", "Ljava/lang/String;",
                 fast_flags);
  SetObjectField(env, params, "fastFlags", "Ljava/lang/String;", fast_flags);
  SetObjectField(env, params, "appParamsJson", "Ljava/lang/String;",
                 app_params);
  SetObjectField(env, params, "appParams", "Ljava/lang/String;", app_params);
  SetObjectField(env, params, "launchParams", "Ljava/lang/String;",
                 app_params);
  SetObjectField(env, params, "assetPath", "Ljava/lang/String;", asset_path);

  SetStringField(env, params, "packageName", "com.roblox.client");
  SetStringField(env, params, "platformName", "Android");
  const std::string init_app_version =
      GetEnvString("MOCKTAIL_ROBLOX_VERSION", "unknown");
  SetStringField(env, params, "appVersion", init_app_version.c_str());
  const std::string init_device_name =
      GetEnvString("MOCKTAIL_DEVICE_NAME", "Mocktail Headless");
  SetStringField(env, params, "deviceName", init_device_name.c_str());
  SetStringField(env, params, "locale", "en_us");
  SetRobloxServiceUrlFields(env, params);

  SetIntField(env, params, "screenWidth", 1280);
  SetIntField(env, params, "screenHeight", 720);
  SetIntField(env, params, "densityDpi", 160);
  SetIntField(env, params, "sdkVersion", 33);
  jboolean headless = is_headless ? JNI_TRUE : JNI_FALSE;
  SetBooleanField(env, params, "headless", headless);
  SetBooleanField(env, params, "isHeadless", headless);
  SetBooleanField(env, params, "isFirstInstall", JNI_FALSE);
  SetBooleanField(env, params, "isLowMemoryDevice", JNI_FALSE);

  return params;
}

jobject BuildApplicationExitInfoList(JNIEnv* env) {
  jobject list = NewObject(env, "java/util/ArrayList");
  if (!list) {
    list = NewObject(env, "java/util/List");
  }
  return list;
}

jobject BuildPlatformParams(JNIEnv* env, jobject surface, bool is_headless) {
  jobject params =
      NewObject(env, "com/roblox/engine/jni/model/PlatformParams");
  if (!params) {
    return nullptr;
  }

  SetObjectField(env, params, "surface", "Landroid/view/Surface;", surface);
  SetStringField(env, params, "platform", "Android");
  const std::string platform_device_name =
      GetEnvString("MOCKTAIL_DEVICE_NAME", "Mocktail Headless");
  SetStringField(env, params, "deviceName", platform_device_name.c_str());
  SetStringField(env, params, "locale", "en_us");
  SetStringField(env, params, "assetFolderPath", DefaultAssetPath().c_str());
  SetIntField(env, params, "width", 1280);
  SetIntField(env, params, "height", 720);
  SetIntField(env, params, "screenWidth", 1280);
  SetIntField(env, params, "screenHeight", 720);
  SetIntField(env, params, "densityDpi", 160);
  SetIntField(env, params, "viewportWidthMm", 203);
  SetIntField(env, params, "viewportHeightMm", 114);
  SetFloatField(env, params, "dpiScale", 1.0f);
  SetBooleanField(
      env, params, "isKeyboardDevice",
      IsEnabled("MOCKTAIL_KEYBOARD_ENABLED_INTERNAL") ? JNI_TRUE : JNI_FALSE);
  SetBooleanField(
      env, params, "isMouseDevice",
      IsEnabled("MOCKTAIL_MOUSE_ENABLED_INTERNAL") ? JNI_TRUE : JNI_FALSE);
  SetBooleanField(
      env, params, "isTouchDevice",
      IsEnabled("MOCKTAIL_TOUCH_ENABLED_INTERNAL") ? JNI_TRUE : JNI_FALSE);
  SetBooleanField(env, params, "isLuaHomePageEnabled", JNI_TRUE);
  SetBooleanField(env, params, "isLuaGamesPageEnabled", JNI_TRUE);
  SetBooleanField(env, params, "isLuaChatEnabled", JNI_TRUE);
  SetBooleanField(env, params, "isTablet", JNI_FALSE);
  jboolean headless = is_headless ? JNI_TRUE : JNI_FALSE;
  SetBooleanField(env, params, "headless", headless);
  SetBooleanField(env, params, "isHeadless", headless);
  return params;
}

jobject BuildStartAppParams(JNIEnv* env, jstring app_params,
                           jobject platform_params, jobject surface,
                           bool is_headless, const char* launch_mode,
                           const jnivm::RobloxAuthIdentity& identity) {
  jobject params =
      NewObject(env, "com/roblox/engine/jni/autovalue/StartAppParams");
  if (!params) {
    return nullptr;
  }

  jobject activity = NewObject(env, "com/roblox/client/RobloxActivity");
  jobject context = NewObject(env, "android/content/Context");
  jobject asset_manager = NewObject(env, "android/content/res/AssetManager");
  jobject resources = NewObject(env, "android/content/res/Resources");
  jobject class_loader = NewObject(env, "java/lang/ClassLoader");
  jobject package_manager = NewObject(env, "android/content/pm/PackageManager");
  jobject window = NewObject(env, "android/view/Window");
  jobject display = NewObject(env, "android/view/Display");
  jobject surface_holder = NewObject(env, "android/view/SurfaceHolder");
  jobject view = NewObject(env, "android/view/View");

  SetObjectField(env, params, "surface", "Landroid/view/Surface;", surface);
  SetObjectField(env, params, "platformParams",
                 "Lcom/roblox/engine/jni/model/PlatformParams;",
                 platform_params);
  SetStringField(env, params, "appStarterPlace",
                 "rbxasset://places/Mobile.rbxl");
  SetStringField(env, params, "appStarterScript", "LuaAppStarterScript");
  SetLongField(env, params, "appUserId", identity.user_id);
  SetBooleanField(env, params, "isUnder13", JNI_FALSE);
  SetStringField(env, params, "username", identity.username.c_str());
  SetIntField(env, params, "membershipType",
              GetEnvInt("MOCKTAIL_ROBLOX_MEMBERSHIP_TYPE", 0));
  const char* theme = std::getenv("MOCKTAIL_RESOLVED_THEME_INTERNAL");
  SetStringField(env, params, "selectedTheme",
                 theme != nullptr ? theme : "Dark");
  SetObjectField(env, params, "vrContext", "Landroid/app/Activity;",
                 activity);

  SetObjectField(env, params, "appParams", "Ljava/lang/String;", app_params);
  SetObjectField(env, params, "appParamsJson", "Ljava/lang/String;",
                 app_params);
  SetObjectField(env, params, "launchParams", "Ljava/lang/String;",
                 app_params);
  SetObjectField(env, params, "activity", "Lcom/roblox/client/RobloxActivity;",
                 activity);
  SetObjectField(env, params, "robloxActivity",
                 "Lcom/roblox/client/RobloxActivity;", activity);
  SetObjectField(env, params, "mainActivity",
                 "Lcom/roblox/client/RobloxActivity;", activity);
  SetObjectField(env, params, "context", "Landroid/content/Context;", context);
  SetObjectField(env, params, "applicationContext",
                 "Landroid/content/Context;", context);
  SetObjectField(env, params, "assetManager",
                 "Landroid/content/res/AssetManager;", asset_manager);
  SetObjectField(env, params, "assets",
                 "Landroid/content/res/AssetManager;", asset_manager);
  SetObjectField(env, params, "resources",
                 "Landroid/content/res/Resources;", resources);
  SetObjectField(env, params, "classLoader", "Ljava/lang/ClassLoader;",
                 class_loader);
  SetObjectField(env, params, "packageManager",
                 "Landroid/content/pm/PackageManager;", package_manager);
  SetObjectField(env, params, "window", "Landroid/view/Window;", window);
  SetObjectField(env, params, "display", "Landroid/view/Display;", display);
  SetObjectField(env, params, "surfaceHolder", "Landroid/view/SurfaceHolder;",
                 surface_holder);
  SetObjectField(env, params, "view", "Landroid/view/View;", view);
  SetObjectField(env, params, "decorView", "Landroid/view/View;", view);
  SetObjectField(env, params, "rootView", "Landroid/view/View;", view);
  const char* mode = launch_mode;
  if (mode == nullptr || mode[0] == '\0') {
    mode = is_headless ? "headless" : "normal";
  }
  SetStringField(env, params, "launchMode", mode);
  SetStringField(env, params, "joinData", "{}");
  SetStringField(env, params, "packageName", "com.roblox.client");
  SetStringField(env, params, "platformName", "Android");
  const std::string start_app_version =
      GetEnvString("MOCKTAIL_ROBLOX_VERSION", "unknown");
  SetStringField(env, params, "appVersion", start_app_version.c_str());
  const std::string start_device_name =
      GetEnvString("MOCKTAIL_DEVICE_NAME", "Mocktail Linux");
  SetStringField(env, params, "deviceName", start_device_name.c_str());
  SetStringField(env, params, "locale", "en_us");
  SetRobloxServiceUrlFields(env, params);
  SetIntField(env, params, "width", 1280);
  SetIntField(env, params, "height", 720);
  SetIntField(env, params, "screenWidth", 1280);
  SetIntField(env, params, "screenHeight", 720);
  SetIntField(env, params, "densityDpi", 160);
  SetIntField(env, params, "sdkVersion", 33);
  SetIntField(env, params, "placeId", 0);
  jboolean headless = is_headless ? JNI_TRUE : JNI_FALSE;
  SetBooleanField(env, params, "headless", headless);
  SetBooleanField(env, params, "isHeadless", headless);
  SetBooleanField(env, params, "isFirstInstall", JNI_FALSE);
  SetBooleanField(env, params, "isLowMemoryDevice", JNI_FALSE);
  return params;
}

jobject BuildStartGameParams(JNIEnv* env, jobject platform_params,
                            jobject device_params, jobject surface,
                            jobject vr_context, const char* launch_data_env,
                            const jnivm::RobloxAuthIdentity& identity) {
  std::cout << "  [engine] BuildStartGameParams begin\n" << std::flush;
  if (!env) {
    std::cerr << "  [engine] BuildStartGameParams: env is null\n" << std::flush;
    return nullptr;
  }
  jclass start_game_params_class =
      env->FindClass("com/roblox/engine/jni/autovalue/StartGameParams");
  if (!start_game_params_class) {
    std::cerr << "  [engine] BuildStartGameParams: StartGameParams class not found\n"
              << std::flush;
    return nullptr;
  }
  const jlong place_id = GetEnvLong("MOCKTAIL_PLACE_ID", 0);
  const jlong join_target_user_id =
      GetEnvLong("MOCKTAIL_GAME_JOIN_USER_ID", 0);
  const jlong conversation_id =
      GetEnvLong("MOCKTAIL_GAME_CONVERSATION_ID", 0);
  const jlong referred_by_player_id =
      GetEnvLong("MOCKTAIL_REFERRED_BY_PLAYER_ID", 0);
  const jint join_request_type =
      GetEnvInt("MOCKTAIL_GAME_JOIN_REQUEST_TYPE", -1);
  const std::string access_code_value =
      GetEnvString("MOCKTAIL_GAME_ACCESS_CODE", "");
  const std::string link_code_value =
      GetEnvString("MOCKTAIL_GAME_LINK_CODE", "");
  const std::string reserved_server_access_code_value =
      GetEnvString("MOCKTAIL_GAME_RESERVED_SERVER_ACCESS_CODE", "");
  const std::string call_id_value = GetEnvString("MOCKTAIL_GAME_CALL_ID", "");
  const std::string event_id_value = GetEnvString("MOCKTAIL_GAME_EVENT_ID", "");
  const std::string join_attempt_id_value =
      GetEnvString("MOCKTAIL_GAME_JOIN_ATTEMPT_ID", "");
  const std::string join_attempt_origin_value =
      GetEnvString("MOCKTAIL_GAME_JOIN_ATTEMPT_ORIGIN", "");
  const std::string iso_context_value =
      GetEnvString("MOCKTAIL_GAME_ISO_CONTEXT", "");
  const std::string& username_value = identity.username;
  const std::string launch_data_value =
      GetEnvString(launch_data_env != nullptr ? launch_data_env
                                              : "MOCKTAIL_GAME_PARAMS_JSON",
                   "");
  const std::string game_id_value =
      GetEnvString("MOCKTAIL_GAME_ID", "");
  const std::string referral_page_value =
      GetEnvString("MOCKTAIL_REFERRAL_PAGE", "");
  const std::string game_join_context_value =
      GetEnvString("MOCKTAIL_GAME_JOIN_CONTEXT", "");
  if (EngineTraceEnabled()) {
    std::cout << "  [engine] StartGame params: place_id=" << place_id
              << " join_target_user_id=" << join_target_user_id
              << " game_id=" << game_id_value
              << " join_request_type=" << join_request_type << '\n'
              << std::flush;
  }

  const jstring access_code = env->NewStringUTF(access_code_value.c_str());
  const jstring link_code = env->NewStringUTF(link_code_value.c_str());
  const jstring reserved_server_access_code =
      env->NewStringUTF(reserved_server_access_code_value.c_str());
  const jstring call_id = env->NewStringUTF(call_id_value.c_str());
  const jstring event_id = env->NewStringUTF(event_id_value.c_str());
  const jstring join_attempt_id = env->NewStringUTF(join_attempt_id_value.c_str());
  const jstring join_attempt_origin =
      env->NewStringUTF(join_attempt_origin_value.c_str());
  const jstring iso_context = env->NewStringUTF(iso_context_value.c_str());
  jstring username = env->NewStringUTF(username_value.c_str());
  jstring launch_data = env->NewStringUTF(launch_data_value.c_str());
  jstring game_id = env->NewStringUTF(game_id_value.c_str());
  jstring referral_page = env->NewStringUTF(referral_page_value.c_str());
  jstring game_join_context = env->NewStringUTF(game_join_context_value.c_str());

  jobject params = NewObject(env, "com/roblox/engine/jni/autovalue/StartGameParams");
  if (!params) {
    return nullptr;
  }

  SetObjectField(env, params, "surface", "Landroid/view/Surface;", surface);
  SetObjectField(env, params, "platformParams",
                 "Lcom/roblox/engine/jni/model/PlatformParams;",
                 platform_params);
  SetObjectField(env, params, "deviceParams",
                 "Lcom/roblox/engine/jni/model/DeviceParams;",
                 nullptr);
  SetLongField(env, params, "placeId", place_id);
  SetLongField(env, params, "userId", join_target_user_id);
  SetJStringField(env, params, "accessCode", access_code);
  SetJStringField(env, params, "callId", call_id);
  SetJStringField(env, params, "linkCode", link_code);
  SetJStringField(env, params, "reservedServerAccessCode",
                  reserved_server_access_code);
  SetLongField(env, params, "conversationId", conversation_id);
  SetIntField(env, params, "joinRequestType", join_request_type);
  SetJStringField(env, params, "gameId", game_id);
  SetBooleanField(env, params, "isUnder13", JNI_FALSE);
  SetJStringField(env, params, "username", username);
  SetJStringField(env, params, "referralPage", referral_page);
  SetJStringField(env, params, "launchData", launch_data);
  SetJStringField(env, params, "gameJoinContext", game_join_context);
  SetJStringField(env, params, "eventId", event_id);
  SetJStringField(env, params, "joinAttemptId", join_attempt_id);
  SetJStringField(env, params, "joinAttemptOrigin", join_attempt_origin);
  SetJStringField(env, params, "isoContext", iso_context);
  SetLongField(env, params, "referredByPlayerId", referred_by_player_id);
  SetObjectField(env, params, "vrContext", "Landroid/app/Activity;", vr_context);

  return params;
}

jobject BuildMockSurface(JNIEnv* env) {
  jobject surface = NewObject(env, "android/view/Surface");
  if (!surface) {
    return nullptr;
  }
  SetIntField(env, surface, "width", 1280);
  SetIntField(env, surface, "height", 720);
  SetBooleanField(env, surface, "valid", JNI_TRUE);
  SetBooleanField(env, surface, "isValid", JNI_TRUE);
  return surface;
}

jobject BuildConfiguration(JNIEnv* env) {
  return jnivm::CreateAndroidConfiguration(env);
}

void ConfigureNativeSettings(JNIEnv* env, jclass settings_class,
                             const EngineStartupContext* context) {
  if (!env || !settings_class || !context) {
    return;
  }

  const std::string sober_data_root = SoberDataRoot();
  const std::string sober_cache_root = SoberCacheRoot();
  std::string data_dir = GetEnvStringDefaultPath(
      "MOCKTAIL_ANDROID_DATA_DIR",
      DefaultSoberAwarePath(sober_data_root.c_str(),
                            "/data/user/0/com.roblox.client"));
  std::string files_dir = GetEnvStringDefaultPath(
      "MOCKTAIL_ANDROID_FILES_DIR",
      DefaultSoberAwarePath((sober_data_root + "/files").c_str(),
                            "/data/user/0/com.roblox.client/files"));
  std::string settings_cache_dir = GetEnvStringDefaultPath(
      "MOCKTAIL_ANDROID_SETTINGS_CACHE_DIR",
      DefaultSoberAwarePath(sober_cache_root.c_str(),
                            "/data/user/0/com.roblox.client"));
  std::string cache_dir = GetEnvStringDefaultPath(
      "MOCKTAIL_ANDROID_CACHE_DIR",
      DefaultSoberAwarePath((sober_cache_root + "/cache").c_str(),
                            "/data/user/0/com.roblox.client/cache"));
  std::string external_base = GetEnvStringDefaultPath(
      "MOCKTAIL_ANDROID_EXTERNAL_BASE_DIR",
      DefaultSoberAwarePath((sober_data_root + "/sdcard/Android/data/"
                             "com.roblox.client")
                                .c_str(),
                            "/sdcard/Android/data/com.roblox.client"));
  std::string external_dir = GetEnvStringDefaultPath(
      "MOCKTAIL_ANDROID_EXTERNAL_DIR",
      DefaultSoberAwarePath((external_base + "/files").c_str(),
                            "/sdcard/Android/data/com.roblox.client/files"));
  std::string preferences_file = GetEnvString("MOCKTAIL_ANDROID_PREFERENCES_FILE",
                                              "prefs");
  std::string default_policy_file = GetEnvString(
      "MOCKTAIL_DEFAULT_APP_POLICY_FILE",
      "content/guac/defaultConfigs/GuacDefaultPolicy-GlobalDist.json");
  std::string base_url =
      GetEnvString("MOCKTAIL_BASE_URL", "https://www.roblox.com/");
  std::string api_url =
      GetEnvString("MOCKTAIL_API_URL", "https://api.roblox.com/");
  std::string roblox_channel =
      GetEnvString("MOCKTAIL_ROBLOX_CHANNEL", "production");
  std::string channel_platform_name =
      GetEnvString("MOCKTAIL_CHANNEL_PLATFORM_NAME", "GoogleAndroidApp");
  std::string roblox_version =
      GetEnvString("MOCKTAIL_ROBLOX_VERSION", "unknown");
  std::string exception_reason_filename =
      GetEnvString("MOCKTAIL_EXCEPTION_REASON_FILENAME",
                   "exception_reason.txt");
  std::string http_proxy_host =
      GetEnvString("MOCKTAIL_HTTP_PROXY_HOST", "");
  jlong http_proxy_port =
      static_cast<jlong>(GetEnvInt("MOCKTAIL_HTTP_PROXY_PORT", 0));
  std::string cookie_base_url =
      GetEnvString("MOCKTAIL_COOKIE_BASE_URL", "https://www.roblox.com/");
  std::string cookie_domain =
      GetEnvString("MOCKTAIL_COOKIE_DOMAIN", "roblox.com");
  const mocktail::runtime::SecureRobloxCredential* credential =
      context->roblox_credential;
  const std::string_view roblox_cookies =
      credential != nullptr ? credential->view() : std::string_view();
  std::string cookie_manager_cookies =
      CookieForJNICookieManager(roblox_cookies, cookie_domain);
  if (!roblox_cookies.empty()) {
    std::cout << "  [engine] typed credential prepared bytes="
              << roblox_cookies.size() << '\n'
              << std::flush;
  } else if (IsEnabled("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP")) {
    std::cout << "  [engine] no Roblox cookie found; proceeding without login\n"
              << std::flush;
  } else {
    std::cout << "  [engine] WARNING: no Roblox cookie found at "
              << MocktailCookiePath() << '\n'
              << std::flush;
  }
  std::string android_id =
      GetEnvString("MOCKTAIL_ANDROID_ID", "0000000000000000");
  std::string advertising_id =
      GetEnvString("MOCKTAIL_ADVERTISING_ID", "");
  const jnivm::RobloxAuthIdentity& account_identity =
      context->account_identity;
  std::string account_user_id = std::to_string(account_identity.user_id);

  EnsureAndroidDirectory(data_dir);
  EnsureAndroidDirectory(files_dir);
  EnsureAndroidDirectory(settings_cache_dir);
  EnsureAndroidDirectory(cache_dir);
  EnsureAndroidDirectory(external_base);
  EnsureAndroidDirectory(external_dir);
  EnsureAndroidDirectory(data_dir + "/rbx-storage");
  EnsureAndroidDirectory(data_dir + "/appData");
  EnsureAndroidDirectory(data_dir + "/appData/LocalStorage");
  EnsureAndroidDirectory(data_dir + "/appData/rbx-storage");
  EnsureAndroidDirectory(files_dir + "/rbx-storage");
  EnsureAndroidDirectory(files_dir + "/appData");
  EnsureAndroidDirectory(files_dir + "/appData/LocalStorage");
  EnsureAndroidDirectory(files_dir + "/appData/OTAPatchBackups");
  EnsureAndroidDirectory(files_dir + "/appData/rbx-storage");
  EnsureAndroidDirectory(cache_dir + "/ContentProvider_2");
  EnsureAndroidDirectory(cache_dir + "/rbx-storage");
  EnsureAndroidDirectory(cache_dir + "/sounds");

  jstring data_dir_string = env->NewStringUTF(data_dir.c_str());
  jstring files_dir_string = env->NewStringUTF(files_dir.c_str());
  jstring settings_cache_dir_string =
      env->NewStringUTF(settings_cache_dir.c_str());
  jstring external_base_string = env->NewStringUTF(external_base.c_str());
  jstring external_dir_string = env->NewStringUTF(external_dir.c_str());
  jstring preferences_file_string = env->NewStringUTF(preferences_file.c_str());
  jstring default_policy_file_string =
      env->NewStringUTF(default_policy_file.c_str());
  jstring base_url_string = env->NewStringUTF(base_url.c_str());
  jstring api_url_string = env->NewStringUTF(api_url.c_str());
  jstring roblox_channel_string = env->NewStringUTF(roblox_channel.c_str());
  jstring channel_platform_name_string =
      env->NewStringUTF(channel_platform_name.c_str());
  jstring roblox_version_string = env->NewStringUTF(roblox_version.c_str());
  jstring exception_reason_filename_string =
      env->NewStringUTF(exception_reason_filename.c_str());
  jstring http_proxy_host_string = env->NewStringUTF(http_proxy_host.c_str());
  jstring cookie_base_url_string = env->NewStringUTF(cookie_base_url.c_str());
  jstring cookie_domain_string = env->NewStringUTF(cookie_domain.c_str());
  jstring roblox_cookies_string = env->NewStringUTF(
      credential != nullptr ? credential->c_str() : "");
  jstring cookie_manager_cookies_string =
      env->NewStringUTF(cookie_manager_cookies.c_str());
  jstring android_id_string = env->NewStringUTF(android_id.c_str());
  jstring advertising_id_string = env->NewStringUTF(advertising_id.c_str());
  jstring googleplay_string = env->NewStringUTF("googleplay");
  jstring user_id_string = env->NewStringUTF(account_user_id.c_str());
  auto run_native_setting = [](const char* name, auto call) -> bool {
    if (sigsetjmp(g_native_settings_jmp_buf, 1) == 0) {
      g_native_settings_recovery_name = name;
      g_native_settings_recovery_in_progress = 1;
      call();
      g_native_settings_recovery_in_progress = 0;
      g_native_settings_recovery_name = nullptr;
      return true;
    }
    g_native_settings_recovery_in_progress = 0;
    std::cerr << "  [engine] NativeSettings " << name
              << " recovered from crash\n"
              << std::flush;
    g_native_settings_recovery_name = nullptr;
    return false;
  };

  if (context->native_set_http_client_proxy &&
      ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_HTTP_CLIENT_PROXY", false)) {
    std::cout << "  [engine] NativeSettings httpClientProxy host="
              << http_proxy_host << " port=" << http_proxy_port << '\n'
              << std::flush;
    run_native_setting("httpClientProxy", [&]() {
      context->native_set_http_client_proxy(env, settings_class,
                                            http_proxy_host_string,
                                            http_proxy_port);
    });
  }
  if (context->native_set_exception_reason_filename &&
      ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_EXCEPTION_REASON_FILENAME",
                           true)) {
    std::cout << "  [engine] NativeSettings exceptionReasonFilename="
              << exception_reason_filename << '\n'
              << std::flush;
    run_native_setting("exceptionReasonFilename", [&]() {
      context->native_set_exception_reason_filename(
          env, settings_class, exception_reason_filename_string);
    });
  }
  if (context->native_set_base_url &&
      ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_BASE_URL", true)) {
    std::cout << "  [engine] NativeSettings baseUrl=" << base_url
              << " apiUrl=" << api_url << '\n' << std::flush;
    run_native_setting("baseUrl", [&]() {
      context->native_set_base_url(env, settings_class, base_url_string,
                                   api_url_string);
    });
  }
  if (context->native_set_roblox_channel &&
      ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_ROBLOX_CHANNEL", true)) {
    std::cout << "  [engine] NativeSettings robloxChannel=" << roblox_channel
              << '\n'
              << std::flush;
    run_native_setting("robloxChannel", [&]() {
      context->native_set_roblox_channel(env, settings_class,
                                         roblox_channel_string);
    });
  }
  if (context->native_override_channel_platform_name &&
      ShouldRunStartupStep("MOCKTAIL_NATIVE_OVERRIDE_CHANNEL_PLATFORM_NAME",
                           true)) {
    std::cout << "  [engine] NativeSettings channelPlatformName="
              << channel_platform_name << '\n' << std::flush;
    run_native_setting("channelPlatformName", [&]() {
      context->native_override_channel_platform_name(
          env, settings_class, channel_platform_name_string);
    });
  }
  if (context->native_set_roblox_version &&
      ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_ROBLOX_VERSION", true)) {
    std::cout << "  [engine] NativeSettings robloxVersion=" << roblox_version
              << '\n' << std::flush;
    run_native_setting("robloxVersion", [&]() {
      context->native_set_roblox_version(env, settings_class,
                                         roblox_version_string);
    });
  }
  if (context->native_set_device_info &&
      ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_DEVICE_INFO", true)) {
    std::cout << "  [engine] NativeSettings deviceInfo\n" << std::flush;
    jobject device_params = BuildDeviceParams(env);
    run_native_setting("deviceInfo", [&]() {
      context->native_set_device_info(env, settings_class, device_params);
    });
  }

  if (ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_BASE_DATA_DIRS", true)) {
    run_native_setting("baseDataDirectories", [&]() {
      context->native_set_base_data_directories(env, settings_class,
                                                data_dir_string,
                                                external_base_string);
    });
  }
  if (ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_CACHE_DIR", true)) {
    run_native_setting("cacheDirectory", [&]() {
      context->native_set_cache_directory(env, settings_class,
                                          settings_cache_dir_string);
    });
  }
  if (ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_FILES_DIR", true)) {
    run_native_setting("filesDirectory", [&]() {
      context->native_set_files_directory(env, settings_class,
                                          files_dir_string);
    });
  }
  if (ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_EXTERNAL_DIR", true)) {
    run_native_setting("externalDirectory", [&]() {
      context->native_set_external_directory(env, settings_class,
                                             external_dir_string);
    });
  }
  if (ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_PREFERENCES_FILE", true)) {
    run_native_setting("preferencesFile", [&]() {
      context->native_set_preferences_file(env, settings_class,
                                           preferences_file_string);
    });
  }
  if (ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_DEFAULT_POLICY_FILE", false)) {
    run_native_setting("defaultPolicyFile", [&]() {
      context->native_set_default_app_policy_file(env, settings_class,
                                                  default_policy_file_string);
    });
  }
  if (context->native_init_fast_log &&
      ShouldRunStartupStep("MOCKTAIL_NATIVE_INIT_FAST_LOG", true)) {
    std::cout << "  [engine] NativeSettings initFastLog\n" << std::flush;
    run_native_setting("initFastLog", [&]() {
      context->native_init_fast_log(env, settings_class);
    });
  }

  jclass cookie_manager_class = nullptr;
  auto find_cookie_manager_class = [&]() -> jclass {
    if (cookie_manager_class == nullptr) {
      cookie_manager_class =
          env->FindClass("com/roblox/universalapp/cookie/JNICookieManager");
    }
    return cookie_manager_class;
  };
  if (context->native_cookie_manager_set_cookie &&
      !cookie_manager_cookies.empty() &&
      // This native path expects Roblox's full cookie backend singleton. In
      // Mocktail it currently reaches heap function pointers, so keep it opt-in.
      ShouldRunStartupStep("MOCKTAIL_JNI_COOKIE_MANAGER_SET_COOKIE", false)) {
    jclass cls = find_cookie_manager_class();
    if (cls != nullptr) {
      std::cout << "  [engine] JNICookieManager.setCookie domain="
                << cookie_domain << " bytes=" << cookie_manager_cookies.size()
                << '\n' << std::flush;
      if (IsEnabled("MOCKTAIL_UNSAFE_NATIVE_COOKIE_SETTER")) {
        if (sigsetjmp(g_cookie_setter_jmp_buf, 1) == 0) {
          g_cookie_setter_recovery_in_progress = 1;
          context->native_cookie_manager_set_cookie(
              env, cls, cookie_domain_string, cookie_manager_cookies_string);
          g_cookie_setter_recovery_in_progress = 0;
          std::cout << "  [engine] JNICookieManager.setCookie returned\n"
                    << std::flush;
        } else {
          g_cookie_setter_recovery_in_progress = 0;
          std::cerr << "  [engine] JNICookieManager.setCookie recovered; "
                    << "native cookie singleton is not ready\n"
                    << std::flush;
        }
      } else {
        std::cerr << "  [engine] JNICookieManager.setCookie blocked; set "
                  << "MOCKTAIL_UNSAFE_NATIVE_COOKIE_SETTER=1 to run the "
                  << "known-crashing native path\n"
                  << std::flush;
      }
    }
  } else if (!cookie_manager_cookies.empty() &&
             context->native_cookie_manager_set_cookie != nullptr) {
    std::cout << "  [engine] JNICookieManager.setCookie skipped; set "
              << "MOCKTAIL_JNI_COOKIE_MANAGER_SET_COOKIE=1 to force\n"
              << std::flush;
  } else if (!cookie_manager_cookies.empty() &&
             context->native_cookie_manager_set_cookie == nullptr) {
    std::cout << "  [engine] WARNING: JNICookieManager.setCookie unavailable\n"
              << std::flush;
  }

  if (context->native_set_multiple_cookies && !roblox_cookies.empty() &&
      ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_MULTIPLE_COOKIES", true)) {
    std::cout << "  [engine] NativeSettings multipleCookies base="
              << cookie_base_url << " bytes=" << roblox_cookies.size() << '\n'
              << std::flush;
    run_native_setting("multipleCookies", [&]() {
      context->native_set_multiple_cookies(env, settings_class,
                                           cookie_base_url_string,
                                           roblox_cookies_string);
    });
    std::cout << "  [engine] NativeSettings multipleCookies returned\n"
              << std::flush;
  }
  if (context->native_set_platform_headers_with_idfa &&
      ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_PLATFORM_HEADERS", false)) {
    std::cout << "  [engine] NativeSettings platformHeaders androidId="
              << android_id << '\n'
              << std::flush;
    run_native_setting("platformHeaders", [&]() {
      context->native_set_platform_headers_with_idfa(
          env, settings_class, android_id_string, googleplay_string,
          advertising_id_string);
    });
  }
  if (context->native_set_user_id &&
      ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_USER_ID", false)) {
    std::cout << "  [engine] NativeSettings userId=" << account_user_id << '\n'
              << std::flush;
    run_native_setting("userId", [&]() {
      context->native_set_user_id(env, settings_class, user_id_string);
    });
  }
  mocktail::runtime::SecurelyClearString(&cookie_manager_cookies);
}

void ConfigureLocalStorage(JNIEnv* env, const EngineStartupContext* context) {
  if (!env || !context) {
    return;
  }
  const std::string sober_data_root = SoberDataRoot();
  const std::string sober_cache_root = SoberCacheRoot();
  std::string data_dir = GetEnvStringDefaultPath(
      "MOCKTAIL_ANDROID_DATA_DIR",
      DefaultSoberAwarePath(sober_data_root.c_str(),
                            "/data/user/0/com.roblox.client"));
  std::string files_dir = GetEnvStringDefaultPath(
      "MOCKTAIL_ANDROID_FILES_DIR",
      DefaultSoberAwarePath((sober_data_root + "/files").c_str(),
                            "/data/user/0/com.roblox.client/files"));
  std::string cache_dir = GetEnvStringDefaultPath(
      "MOCKTAIL_ANDROID_CACHE_DIR",
      DefaultSoberAwarePath((sober_cache_root + "/cache").c_str(),
                            "/data/user/0/com.roblox.client/cache"));
  EnsureAndroidDirectory(files_dir + "/appData");
  EnsureAndroidDirectory(files_dir + "/appData/LocalStorage");
  EnsureAndroidDirectory(files_dir + "/appData/OTAPatchBackups");
  EnsureAndroidDirectory(files_dir + "/appData/rbx-storage");
  EnsureAndroidDirectory(data_dir + "/rbx-storage");
  EnsureAndroidDirectory(data_dir + "/shared_prefs");
  EnsureAndroidDirectory(cache_dir);
  EnsureAndroidDirectory(cache_dir + "/ContentProvider_2");
  EnsureAndroidDirectory(cache_dir + "/rbx-storage");
  EnsureAndroidDirectory(cache_dir + "/sounds");

  jobject asset_manager = nullptr;
  auto get_asset_manager = [&]() -> jobject {
    if (asset_manager == nullptr) {
      asset_manager = NewObject(env, "android/content/res/AssetManager");
    }
    return asset_manager;
  };

  if (context->native_init_asset_manager &&
      ShouldRunStartupStep("MOCKTAIL_JNI_ASSET_MANAGER_SETUP", true)) {
    jclass asset_manager_setup_class =
        env->FindClass("com/roblox/client/JNIAAssetManagerSetup");
    std::cout << "  [engine] JNIAAssetManagerSetup.initNative\n"
              << std::flush;
    context->native_init_asset_manager(env, asset_manager_setup_class,
                                       get_asset_manager());
    std::cout << "  [engine] JNIAAssetManagerSetup.initNative returned\n"
              << std::flush;
  }

  if (context->native_local_storage_set_platform_impl &&
      ShouldRunStartupStep("MOCKTAIL_LOCAL_STORAGE_SET_PLATFORM_IMPL", false)) {
    jobject platform_handler = NewObject(
        env,
        "com/roblox/protocols/localstorageplatforminterface/generated/"
        "IPlatformLocalStorageHandler");
    jobject shared_preferences =
        NewObject(env, "android/content/SharedPreferences");
    const std::string shared_prefs_file =
        data_dir + "/shared_prefs/LOCAL_STORAGE_SHARED_PREFS.xml";
    SetObjectField(env, platform_handler, "sharedPreferences",
                   "Landroid/content/SharedPreferences;", shared_preferences);
    SetStringField(env, platform_handler, "sharedPrefsFile",
                   shared_prefs_file.c_str());
    SetStringField(env, platform_handler, "dataDir", data_dir.c_str());

    jclass core_class = env->FindClass(
        "com/roblox/protocols/localstorageplatforminterface/generated/"
        "ILocalStorageHandlerCore");
    std::cout << "  [engine] LocalStorage setPlatformImpl sharedPrefs="
              << shared_prefs_file << '\n'
              << std::flush;
    jobject core = context->native_local_storage_set_platform_impl(
        env, core_class, platform_handler);
    std::cout << "  [engine] LocalStorage setPlatformImpl returned " << core
              << '\n'
              << std::flush;
  }

  if (!context->native_init_storage_manager ||
      !ShouldRunStartupStep("MOCKTAIL_LOCAL_STORAGE_INIT_STORAGE_MANAGER",
                            true)) {
    return;
  }

  jobject local_storage_manager =
      NewObject(env, "com/roblox/client/LocalStorageManager");
  jstring files_dir_string = env->NewStringUTF(files_dir.c_str());
  jstring cache_dir_string = env->NewStringUTF(cache_dir.c_str());
  std::cout << "  [engine] LocalStorageManager.initStorageManagerNativeV3 files="
            << files_dir << " cache=" << cache_dir << '\n'
            << std::flush;
  context->native_init_storage_manager(env, local_storage_manager,
                                       get_asset_manager(), files_dir_string,
                                       cache_dir_string);
  std::cout << "  [engine] LocalStorageManager.initStorageManagerNativeV3 returned\n"
            << std::flush;
}


// Resolve the exact Android Vulkan loader adapter shipped next to this
// runtime binary. Never resolve it by SONAME: an unversioned host
// libvulkan.so on the search path (for example distribution or Nix wrappers)
// would win, silently drop VK_KHR_android_surface support, and break
// direct-Vulkan mode with "Unable to create Vulkan instance".
static std::string RuntimeVulkanAdapterPath() {
  const char* override_dir = std::getenv("MOCKTAIL_RUNTIME_LIBRARY_DIR");
  std::string directory;
  if (override_dir != nullptr && override_dir[0] != '\0') {
    directory = override_dir;
  } else {
    char executable_path[4097] = {};
    const ssize_t length =
        ::readlink("/proc/self/exe", executable_path,
                   sizeof(executable_path) - 1U);
    if (length <= 0) return {};
    executable_path[length] = '\0';
    std::string path(executable_path);
    const std::string::size_type separator = path.find_last_of('/');
    if (separator == std::string::npos) return {};
    directory = separator == 0 ? "/" : path.substr(0, separator);
  }
  if (directory.empty()) return {};
  return directory + "/libvulkan.so";
}


void* EngineStartupThread(void* arg) {
  auto* context = static_cast<EngineStartupContext*>(arg);
  if (context == nullptr) {
    std::cerr << "  [engine] invalid startup context\n" << std::flush;
    return nullptr;
  }
  std::cout << "  [engine] EngineStartupThread entered, context=" << context
            << "\n" << std::flush;
  std::cout << "  [engine] context flags: prepare=" << context->run_prepare_jni
            << " setAsset=" << context->run_set_asset_path
            << " initWithParams=" << context->run_init_with_params << '\n'
            << std::flush;

  EngineLog("thread entered");
  EngineLogPtr("context", context);
  EngineLogPtr("JavaVM", context->java_vm);
  std::cout << "  [engine] thread java_vm=" << context->java_vm
            << " shared_vm=" << context->vm << '\n' << std::flush;
  bool attached_to_thread = false;
  JNIEnv* env = nullptr;
  const bool skip_attach = IsEnabled("MOCKTAIL_ENGINE_SKIP_ATTACH");
  if (skip_attach) {
    std::cout << "  [engine] skipping AttachCurrentThread (MOCKTAIL_ENGINE_SKIP_ATTACH)\n"
              << std::flush;
    EngineLog("AttachCurrentThread skipped");
  } else if (context->java_vm) {
    std::cout << "  [engine] entering AttachCurrentThread\n" << std::flush;
    EngineLog("AttachCurrentThread");
    void* raw_env = nullptr;
    jint attach_result =
        context->java_vm->AttachCurrentThread(&raw_env, nullptr);
    std::cout << "  [engine] AttachCurrentThread result=" << attach_result
              << " raw_env=" << raw_env << '\n' << std::flush;
    if (attach_result == JNI_OK) {
      env = static_cast<JNIEnv*>(raw_env);
      g_stage6_jni_env = reinterpret_cast<uintptr_t>(env);
      attached_to_thread = true;
    }
    EngineLog("AttachCurrentThread returned");
    EngineLogPtr("attached JNIEnv", env);
  }
  if (!env) {
    EngineLog("fallback GetJNIEnv");
  }
  if (!env) {
    env = context->vm->GetJNIEnv();
    g_stage6_jni_env = reinterpret_cast<uintptr_t>(env);
  }
  auto ensure_env = [&]() -> JNIEnv* {
    if (!env) {
      env = context->vm->GetJNIEnv();
    }
    return env;
  };
  if (!env) {
    std::cerr << "  [engine] failed to acquire JNIEnv\n";
    return nullptr;
  }
  if (!InitializeActiveHostAbiThread()) {
    std::cerr << "  [engine] allocator TLS init failed on startup thread\n"
              << std::flush;
    return nullptr;
  }
  std::cout << "  [engine] native allocator TLS initialized\n"
            << std::flush;
  EngineLogPtr("reset JNIEnv", env);
  EngineLog("PublishCurrentJniEnv");
  PublishCurrentJniEnv(env);

  // Keep EGL unbound by default so the worker that runs real Roblox surface
  // calls can become the first owner of the context. Binding here makes later
  // worker MakeCurrent fail with EGL_BAD_ACCESS on Mesa/Wayland.
  if (mocktail::window::IsInitialised() &&
      IsEnabled("MOCKTAIL_BIND_EGL_ON_STARTUP_THREAD")) {
    std::cout << "  [engine] binding EGL context on engine thread\n"
              << std::flush;
    mocktail::window::MakeCurrentOnThread();
  }
  // JNI_OnLoad replaces env->functions; Stage 6 needs the pseudo-VM table.
  if (env && context->vm) {
    context->vm->RestoreFunctions();
    env = context->vm->GetJNIEnv();
    std::cerr << "  [engine] env->functions restored: " << (void*)env->functions << "\n" << std::flush;
  }
  EngineLogPtr("JNIEnv", env);
  EngineLogPtr("JNIEnv.functions", env ? env->functions : nullptr);

  if (!context->run_prepare_jni) {
    EngineLog("JNI prep disabled");
    if (attached_to_thread) {
      EngineLog("DetachCurrentThread");
      jint detach_result = context->java_vm->DetachCurrentThread();
      if (detach_result != JNI_OK) {
        std::cerr << "  [engine] DetachCurrentThread failed: " << detach_result
                  << '\n' << std::flush;
      }
      attached_to_thread = false;
    }
    return nullptr;
  }

  EngineLog("FindClass NativeGLInterface");
  jclass native_gl_class =
      env->FindClass("com/roblox/engine/jni/NativeGLInterface");
  jclass native_input_class =
      env->FindClass("com/roblox/engine/jni/NativeInputInterface");
  if (!native_input_class) {
    native_input_class = native_gl_class;
  }
  jclass native_settings_class =
      env->FindClass("com/roblox/engine/jni/NativeSettingsInterface");
  jclass native_app_bridge_class =
      env->FindClass("com/roblox/engine/jni/NativeAppBridgeInterface");
  jclass native_gl_java_class =
      env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface");
  if (!native_app_bridge_class) {
    native_app_bridge_class = native_gl_class;
  }
  jclass startup_activity_class =
      env->FindClass("com/roblox/client/startup/MainGameActivity");
  if (!startup_activity_class) {
    startup_activity_class = native_gl_class;
  }
  EngineLog("FindClass returned");
  EngineLog("NewStringUTF args");
  jstring empty_string = env->NewStringUTF("");
  jstring base_url = env->NewStringUTF("https://www.roblox.com/");
  jstring user_agent = NewStringFromEnvDefault(
      env, "MOCKTAIL_USER_AGENT",
      "Roblox/unknown (Linux; Android 33; Mocktail)");
  jstring android_id =
      NewStringFromEnvDefault(env, "MOCKTAIL_ANDROID_ID", "0000000000000000");
  jstring launch_source =
      NewStringFromEnvDefault(env, "MOCKTAIL_LAUNCH_SOURCE", "AppAndroidV");
  jstring client_settings = NewClientSettingsString(env);
  jstring client_settings_overrides =
      NewStringFromEnvDefault(env, "MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON",
                              "{}");
  jstring client_settings_signature =
      NewStringFromEnvDefault(env, "MOCKTAIL_CLIENT_SETTINGS_SIGNATURE", "");
  jstring client_settings_group =
      NewStringFromEnvDefault(env, "MOCKTAIL_CLIENT_SETTINGS_GROUP",
                              "GoogleAndroidApp");
  jstring fast_flags =
      NewStringFromEnvDefault(env, "MOCKTAIL_FAST_FLAGS_JSON", "{}");
  const bool is_headless = IsHeadlessMode();
  const char* launch_mode = std::getenv("MOCKTAIL_LAUNCH_MODE");
  const char* default_launch_mode = is_headless ? "headless" : "normal";
  std::string default_app_params =
      std::string("{\"launchMode\":\"") + (launch_mode != nullptr &&
                                                  launch_mode[0] != '\0'
                                              ? launch_mode
                                              : default_launch_mode) +
      "\",\"placeId\":0}";
  jstring app_params = NewStringFromEnvDefault(
      env, "MOCKTAIL_APP_PARAMS_JSON", default_app_params.c_str());
  EngineLogPtr("JNIEnv.functions", env ? env->functions : nullptr);
  jstring asset_path =
      env->NewStringUTF(GetEnvStringDefaultPath("MOCKTAIL_ASSET_PATH",
                                                DefaultAssetPath())
                            .c_str());
  std::string asset_path_value = JStringToString(env, asset_path);
  if (asset_path_value.empty()) {
    asset_path_value = DefaultAssetPath();
    asset_path = env->NewStringUTF(asset_path_value.c_str());
  }
  const bool app_bridge_init_headless =
      is_headless || IsEnabled("MOCKTAIL_APP_BRIDGE_HEADLESS_INIT_PARAMS");
  if (app_bridge_init_headless != is_headless) {
    std::cout << "  [engine] AppBridge init uses headless params while "
              << "windowed surface/start stay enabled\n"
              << std::flush;
  }
  if (IsEnabled("MOCKTAIL_ENGINE_TRACE")) {
    std::cout << "  [engine] asset path prepared: " << asset_path_value
              << '\n'
              << std::flush;
  }
  jobject surface = BuildMockSurface(env);
  jobject game_activity = NewObject(env, "com/roblox/client/startup/MainGameActivity");
  jobject game_surface_activity =
      IsEnabled("MOCKTAIL_PASS_ACTIVITY_TO_GAME_SURFACE_PARAMS")
          ? game_activity
          : nullptr;
  jobject app_bridge_notification_listener =
      NewObject(env,
                "com/roblox/engine/jni/OnAppBridgeNotificationListener");
  jobject game_activity_asset_manager =
      NewObject(env, "android/content/res/AssetManager");
  jobject game_activity_config = BuildConfiguration(env);
  jobject platform_params = BuildPlatformParams(env, surface, is_headless);
  jobject device_params = BuildDeviceParams(env);
  const jnivm::RobloxAuthIdentity& account_identity =
      context->account_identity;
  jobject start_game_params =
      BuildStartGameParams(env, platform_params, device_params, surface,
                           game_activity, "MOCKTAIL_GAME_PARAMS_JSON",
                           account_identity);
  jobject start_app_params = BuildStartAppParams(env, app_params, platform_params,
                                                 surface, is_headless,
                                                 launch_mode, account_identity);
  InstallNativeGlJavaImplementation(env, native_gl_java_class);
  EngineLog("NewStringUTF returned");

  jlong game_activity_handle = 0;
  bool game_activity_init_attempted = false;
  auto run_game_activity_initialize = [&]() -> jlong {
    if (!context->run_game_activity_init ||
        context->native_game_activity_init == nullptr) {
      return 0;
    }
    if (game_activity_init_attempted) {
      return game_activity_handle;
    }
    game_activity_init_attempted = true;
    env = ensure_env();
    std::cout << "  [engine] GameActivity.initializeNativeCode\n"
              << std::flush;
    jstring internal_data_dir =
        env->NewStringUTF("/data/user/0/com.roblox.client/files");
    jstring obb_dir =
        env->NewStringUTF("/sdcard/Android/obb/com.roblox.client");
    jstring external_data_dir =
        env->NewStringUTF("/sdcard/Android/data/com.roblox.client/files");
    if (sigsetjmp(g_game_activity_init_jmp_buf, 1) == 0) {
      g_game_activity_init_recovery_in_progress = 1;
      g_saved_game_activity = game_activity;
      game_activity_handle = context->native_game_activity_init(
          env, game_activity, internal_data_dir, obb_dir, external_data_dir,
          game_activity_asset_manager, nullptr, game_activity_config);
      g_game_activity_init_recovery_in_progress = 0;
    } else {
      g_game_activity_init_recovery_in_progress = 0;
      game_activity_handle = 0;
      std::cerr << "  [engine] GameActivity.initializeNativeCode recovered\n"
                << std::flush;
    }
    g_game_activity_native_handle =
        static_cast<uintptr_t>(game_activity_handle);
    std::cout << "  [engine] GameActivity.initializeNativeCode returned "
              << game_activity_handle << '\n'
              << std::flush;
    if (game_activity_handle != 0 &&
        IsEnabled("MOCKTAIL_DUMP_GAME_ACTIVITY_HANDLE")) {
      auto** game_activity_slots = reinterpret_cast<void**>(
          static_cast<uintptr_t>(game_activity_handle));
      std::cout << "  [engine] GameActivity handle slots:";
      for (int i = 0; i < 40; ++i) {
        std::cout << " [" << i << "]=" << game_activity_slots[i];
      }
      std::cout << '\n' << std::flush;
    }
    return game_activity_handle;
  };

  if (context->run_game_activity_init) {
    run_game_activity_initialize();
  }

  if (context->run_set_asset_path) {
    env = ensure_env();
    std::cout << "  [engine] nativeSetAssetPath\n" << std::flush;
    if (context->call_real_set_asset_path) {
      if (sigsetjmp(g_set_asset_path_jmp_buf, 1) == 0) {
        g_set_asset_path_recovery_in_progress = 1;
        if (EngineTraceEnabled()) {
          std::cerr << "  [engine] nativeSetAssetPath env=" << (void*)env
                    << " class=" << (void*)startup_activity_class
                    << " path=" << asset_path_value << '\n'
                    << std::flush;
        }
        context->native_set_asset_path(env, startup_activity_class, asset_path);
        g_set_asset_path_recovery_in_progress = 0;
      } else {
        std::cerr << "  [engine] nativeSetAssetPath recovered\n" << std::flush;
        MocktailSetAssetPath(env, asset_path);
      }
    } else {
      MocktailSetAssetPath(env, asset_path);
    }
    std::cout << "  [engine] nativeSetAssetPath returned\n" << std::flush;
  }

  if (context->run_native_settings) {
    env = ensure_env();
    std::cout << "  [engine] NativeSettings directories\n" << std::flush;
    ConfigureNativeSettings(env, native_settings_class, context);
    ConfigureLocalStorage(env, context);
    if (context->native_base_url_protocol_init != nullptr) {
      jclass base_url_protocol_class =
          env->FindClass("com/roblox/universalapp/linking/JNIBaseUrlProtocol");
      std::cout << "  [engine] JNIBaseUrlProtocol.init\n" << std::flush;
      context->native_base_url_protocol_init(env, base_url_protocol_class,
                                             game_activity);
      std::cout << "  [engine] JNIBaseUrlProtocol.init returned\n"
                << std::flush;
    }
    std::cout << "  [engine] NativeSettings directories returned\n"
              << std::flush;
  }

  if (context->run_global_init) {
    env = ensure_env();
    std::cout << "  [engine] nativeGameGlobalInit\n" << std::flush;
    if (sigsetjmp(g_game_global_init_jmp_buf, 1) == 0) {
      g_game_global_init_recovery_in_progress = 1;
      context->native_global_init(env, native_gl_class);
      g_game_global_init_recovery_in_progress = 0;
    } else {
      g_game_global_init_recovery_in_progress = 0;
      const char msg[] = "  [engine] nativeGameGlobalInit recovered\n";
      write(2, msg, sizeof(msg) - 1);
    }
    const char msg[] = "  [engine] nativeGameGlobalInit returned\n";
    write(1, msg, sizeof(msg) - 1);
  }

  if (context->native_update_adapter_init &&
      !IsDisabled("MOCKTAIL_UPDATE_ADAPTER_INIT")) {
    env = ensure_env();
    std::cout << "  [engine] nativeUpdateAdapterInit\n" << std::flush;
    context->native_update_adapter_init(env, native_gl_class);
    std::cout << "  [engine] nativeUpdateAdapterInit returned\n" << std::flush;
  }

  if (context->run_update_screen_orientation &&
      context->native_update_screen_orientation != nullptr) {
    env = ensure_env();
    if (sigsetjmp(g_update_screen_orientation_jmp_buf, 1) == 0) {
      const jint orientation =
          static_cast<jint>(GetEnvInt("MOCKTAIL_SCREEN_ORIENTATION", 2));
      std::cout << "  [engine] nativeUpdateScreenOrientation orientation="
                << orientation << '\n'
                << std::flush;
      g_update_screen_orientation_recovery_in_progress = 1;
      context->native_update_screen_orientation(env, native_input_class,
                                                orientation);
      g_update_screen_orientation_recovery_in_progress = 0;
      std::cout << "  [engine] nativeUpdateScreenOrientation returned\n"
                << std::flush;
    } else {
      g_update_screen_orientation_recovery_in_progress = 0;
      std::cerr << "  [engine] nativeUpdateScreenOrientation recovered\n"
                << std::flush;
    }
  }

  if (context->run_init_client_settings) {
    env = ensure_env();
    std::cout << "  [engine] nativeInitClientSettings\n" << std::flush;
    if (sigsetjmp(g_init_client_settings_jmp_buf, 1) == 0) {
      g_init_client_settings_recovery_in_progress = 1;
      const char* variant = std::getenv("MOCKTAIL_INIT_CLIENT_SETTINGS_VARIANT");
      if (variant == nullptr || variant[0] == '\0') {
        variant = "classic";
      }
      jint settings_result = 0;
      const jlong client_settings_timestamp_seconds =
          static_cast<jlong>(time(nullptr));
      if (std::strcmp(variant, "compressed") == 0 &&
          context->native_init_client_settings_cached_compressed != nullptr) {
        jbyteArray empty_compressed_settings = env->NewByteArray(0);
        settings_result =
            context->native_init_client_settings_cached_compressed(
                env, native_gl_class, empty_compressed_settings,
                client_settings_overrides, client_settings_group, empty_string,
                client_settings_timestamp_seconds, JNI_FALSE);
      } else if (std::strcmp(variant, "cached") == 0 &&
                 context->native_init_client_settings_cached != nullptr) {
        settings_result = context->native_init_client_settings_cached(
            env, native_gl_class, client_settings, client_settings_overrides,
            client_settings_group, empty_string,
            client_settings_timestamp_seconds);
      } else if (std::strcmp(variant, "signed") == 0 &&
                 context->native_init_client_settings_signed != nullptr) {
        settings_result = context->native_init_client_settings_signed(
            env, native_gl_class, client_settings, client_settings_signature,
            client_settings_overrides, client_settings_group);
      } else if (context->native_init_client_settings != nullptr) {
        variant = "classic";
        settings_result = context->native_init_client_settings(
            env, native_gl_class, client_settings, client_settings_overrides,
            client_settings_group);
      } else {
        std::cerr << "  [engine] nativeInitClientSettings has no usable "
                  << "variant\n"
                  << std::flush;
      }
      g_init_client_settings_recovery_in_progress = 0;
      std::cout << "  [engine] nativeInitClientSettings returned "
                << settings_result << " via " << variant << '\n'
                << std::flush;
      if (context->run_native_settings &&
          ShouldRunStartupStep("MOCKTAIL_REAPPLY_NATIVE_SETTINGS_AFTER_INIT",
                               true)) {
        std::cout << "  [engine] reapplying NativeSettings after "
                  << "nativeInitClientSettings\n"
                  << std::flush;
        ConfigureNativeSettings(env, native_settings_class, context);
      }
    } else {
      g_init_client_settings_recovery_in_progress = 0;
      std::cerr << "  [engine] nativeInitClientSettings recovered\n" << std::flush;
    }
  }

  if (context->run_post_client_settings) {
    env = ensure_env();
    std::cout << "  [engine] nativePostClientSettingsLoadedInitialization3\n"
              << std::flush;
    if (IsEnabled("MOCKTAIL_TRACE_POST_CLIENT_SETTINGS_JNI")) {
      setenv("MOCKTAIL_JNI_TRACE", "1", 1);
    }
    if (sigsetjmp(g_post_client_settings_jmp_buf, 1) == 0) {
      g_post_client_settings_recovery_in_progress = 1;
      jobject application_exit_info_list = BuildApplicationExitInfoList(env);
      context->native_post_client_settings(env, native_gl_class,
                                           application_exit_info_list);
      g_post_client_settings_recovery_in_progress = 0;
    } else {
      g_post_client_settings_recovery_in_progress = 0;
      std::cerr << "  [engine] nativePostClientSettingsLoadedInitialization3 recovered\n"
                << std::flush;
    }
    std::cout
        << "  [engine] nativePostClientSettingsLoadedInitialization3 returned\n"
        << std::flush;
  }

  if (context->native_initialize_native_flags != nullptr &&
      ShouldRunStartupStep("MOCKTAIL_INITIALIZE_NATIVE_FLAGS", false)) {
    env = ensure_env();
    std::cout << "  [engine] nativeInitializeNativeFlags\n" << std::flush;
    jclass flag_jni_class =
        env->FindClass("com/roblox/client/flags/FlagJniInterface");
    jclass string_class = env->FindClass("java/lang/String");
    jobjectArray native_flag_keys =
        env->NewObjectArray(0, string_class, nullptr);
    if (sigsetjmp(g_initialize_native_flags_jmp_buf, 1) == 0) {
      g_initialize_native_flags_recovery_in_progress = 1;
      jobject init_result = context->native_initialize_native_flags(
          env, flag_jni_class, native_flag_keys);
      g_initialize_native_flags_recovery_in_progress = 0;
      std::cout << "  [engine] nativeInitializeNativeFlags returned "
                << init_result << '\n'
                << std::flush;
    } else {
      g_initialize_native_flags_recovery_in_progress = 0;
      std::cerr << "  [engine] nativeInitializeNativeFlags recovered\n"
                << std::flush;
    }
  }

  if (context->run_set_init_params) {
    env = ensure_env();
    if (!context->run_native_settings && context->native_set_device_info &&
        ShouldRunStartupStep("MOCKTAIL_NATIVE_SET_DEVICE_INFO", true)) {
      std::cout << "  [engine] NativeSettings deviceInfo\n" << std::flush;
      jobject device_params = BuildDeviceParams(env);
      context->native_set_device_info(env, native_settings_class,
                                      device_params);
      std::cout << "  [engine] NativeSettings deviceInfo returned\n"
                << std::flush;
    }
    std::cout << "  [engine] nativeAppBridgeSetInitParams\n" << std::flush;
    jobject init_params =
        BuildAppBridgeInitParams(env, client_settings, fast_flags, app_params,
                                 asset_path, is_headless);
    context->native_set_init_params(env, startup_activity_class, init_params);
    std::cout << "  [engine] nativeAppBridgeSetInitParams returned\n"
              << std::flush;
    if (context->native_retry_init && IsEnabled("MOCKTAIL_RETRY_INIT")) {
      std::cout << "  [engine] nativeRetryInit\n" << std::flush;
      context->native_retry_init(env, startup_activity_class);
      std::cout << "  [engine] nativeRetryInit returned\n" << std::flush;
    }
  }

  if (!context->run_app_bridge_app_start &&
      IsEnabled("MOCKTAIL_SET_APP_BRIDGE_NOTIFICATION_LISTENER") &&
      native_gl_java_class && app_bridge_notification_listener) {
    env = ensure_env();
    std::cout << "  [engine] NativeGLJavaInterface."
              << "setAppBridgeNotificationListener\n"
              << std::flush;
    jmethodID set_listener = env->GetStaticMethodID(
        native_gl_java_class, "setAppBridgeNotificationListener",
        "(Lcom/roblox/engine/jni/OnAppBridgeNotificationListener;)V");
    env->CallStaticVoidMethod(native_gl_java_class, set_listener,
                              app_bridge_notification_listener);
    std::cout << "  [engine] NativeGLJavaInterface."
              << "setAppBridgeNotificationListener returned\n"
              << std::flush;
  }

  if (context->run_app_bridge_app_start &&
      context->native_app_bridge_app_start) {
    env = ensure_env();
    if (IsEnabled("MOCKTAIL_TRACE_APP_BRIDGE_APP_START_JNI")) {
      setenv("MOCKTAIL_JNI_TRACE", "1", 1);
    }
    if (context->native_set_is_first_install) {
      std::cout << "  [engine] NativeAppBridgeInterface.setIsFirstInstall\n"
                << std::flush;
      context->native_set_is_first_install(env, native_app_bridge_class,
                                           JNI_FALSE);
      std::cout << "  [engine] NativeAppBridgeInterface.setIsFirstInstall "
                << "returned\n"
                << std::flush;
    }
    if (native_gl_java_class && app_bridge_notification_listener &&
        ShouldRunStartupStep("MOCKTAIL_SET_APP_BRIDGE_NOTIFICATION_LISTENER",
                             true)) {
      std::cout << "  [engine] NativeGLJavaInterface."
                << "setAppBridgeNotificationListener\n"
                << std::flush;
      jmethodID set_listener = env->GetStaticMethodID(
          native_gl_java_class, "setAppBridgeNotificationListener",
          "(Lcom/roblox/engine/jni/OnAppBridgeNotificationListener;)V");
      env->CallStaticVoidMethod(native_gl_java_class, set_listener,
                                app_bridge_notification_listener);
      std::cout << "  [engine] NativeGLJavaInterface."
                << "setAppBridgeNotificationListener returned\n"
                << std::flush;
    }
    std::cout << "  [engine] nativeAppBridgeAppStart\n" << std::flush;
    if (sigsetjmp(g_app_bridge_app_start_jmp_buf, 1) == 0) {
      g_app_bridge_app_start_recovery_in_progress = 1;
      context->native_app_bridge_app_start(
          env, native_app_bridge_class, base_url, user_agent, JNI_FALSE,
          android_id, launch_source, empty_string);
      g_app_bridge_app_start_recovery_in_progress = 0;
    } else {
      g_app_bridge_app_start_recovery_in_progress = 0;
      std::cerr << "  [engine] nativeAppBridgeAppStart recovered\n"
                << std::flush;
    }
    std::cout << "  [engine] nativeAppBridgeAppStart returned\n"
              << std::flush;
  }

  if (context->run_init_with_params) {
    env = ensure_env();
    std::cout << "  [engine] nativeAppBridgeV2InitWithParams\n" << std::flush;
    if (context->call_real_init_with_params) {
      jobject init_params =
          BuildAppBridgeInitParams(env, client_settings, fast_flags, app_params,
                                   asset_path, app_bridge_init_headless);
      if (sigsetjmp(g_init_with_params_jmp_buf, 1) == 0) {
        g_init_with_params_recovery_in_progress = 1;
        context->native_init_with_params(env, native_gl_class, init_params);
        g_init_with_params_recovery_in_progress = 0;
      } else {
std::cerr << "  [engine] nativeAppBridgeV2InitWithParams recovered\n"
                  << std::flush;
        MocktailAppBridgeInit(env, app_params);
      }
    } else {
      MocktailAppBridgeInit(env, app_params);
    }
    std::cout << "  [engine] nativeAppBridgeV2InitWithParams returned\n"
              << std::flush;
    g_main_thread_message_pump_ready.store(1);
    if (IsEnabled("MOCKTAIL_TRACE_MAIN_THREAD_PUMP")) {
      std::cerr << "  [engine] nativeCallMessagesFromMainThread pump ready\n"
                << std::flush;
    }
  }

  if (context->run_game_activity_init && context->native_game_activity_init) {
    env = ensure_env();
    jlong handle = run_game_activity_initialize();
    if (context->run_game_activity_surface && handle != 0) {
      if (!IsDisabled("MOCKTAIL_GAME_ACTIVITY_CLEAR_APP_CMD_SLOT")) {
        auto** game_activity_slots = reinterpret_cast<void**>(
            static_cast<uintptr_t>(handle));
        std::cout << "  [engine] GameActivity slot[1] before clear="
                  << game_activity_slots[1] << '\n'
                  << std::flush;
        game_activity_slots[1] = nullptr;
      }
      auto* on_start = reinterpret_cast<GameActivityLifecycleFn>(
          mocktail_gameactivity_on_start_native);
      auto* on_resume = reinterpret_cast<GameActivityLifecycleFn>(
          mocktail_gameactivity_on_resume_native);
      auto* on_surface_created = reinterpret_cast<GameActivitySurfaceCreatedFn>(
          mocktail_gameactivity_on_surface_created_native);
      auto* on_surface_changed = reinterpret_cast<GameActivitySurfaceChangedFn>(
          mocktail_gameactivity_on_surface_changed_native);
      auto* on_surface_redraw_needed =
          reinterpret_cast<GameActivitySurfaceCreatedFn>(
              mocktail_gameactivity_on_surface_redraw_needed_native);
      std::cout << "  [engine] GameActivity callbacks:"
                << " start=" << reinterpret_cast<void*>(on_start)
                << " resume=" << reinterpret_cast<void*>(on_resume)
                << " created=" << reinterpret_cast<void*>(on_surface_created)
                << " changed=" << reinterpret_cast<void*>(on_surface_changed)
                << " redraw="
                << reinterpret_cast<void*>(on_surface_redraw_needed) << '\n'
                << std::flush;
      const bool run_lifecycle_callbacks =
          IsEnabled("MOCKTAIL_GAME_ACTIVITY_LIFECYCLE_CALLBACKS");
      if (sigsetjmp(g_game_activity_surface_jmp_buf, 1) == 0) {
        g_game_activity_surface_recovery_in_progress = 1;
      if (run_lifecycle_callbacks && on_start) {
        on_start(env, game_activity, handle);
      }
      if (run_lifecycle_callbacks && on_resume) {
        on_resume(env, game_activity, handle);
      }
      if (run_lifecycle_callbacks && on_surface_created) {
        on_surface_created(env, game_activity, handle, surface);
      }
      if (run_lifecycle_callbacks && on_surface_changed) {
        on_surface_changed(env, game_activity, handle, surface, 4, 1280, 720);
      }
      if (run_lifecycle_callbacks && on_surface_redraw_needed) {
        on_surface_redraw_needed(env, game_activity, handle, surface);
      }
        g_game_activity_surface_recovery_in_progress = 0;
        std::cout << "  [engine] GameActivity surface callbacks returned\n"
                  << std::flush;
      } else {
        g_game_activity_surface_recovery_in_progress = 0;
        std::cerr << "  [engine] GameActivity surface callbacks recovered\n"
                  << std::flush;
      }
    }
  }

  if (context->run_activity_lifecycle) {
    env = ensure_env();
    if (context->vm != nullptr) {
      context->vm->RestoreFunctions();
      env = context->vm->GetJNIEnv();
      PublishCurrentJniEnv(env);
    }
    jobject lifecycle_callbacks =
        NewObject(env,
                  "com/roblox/universalapp/activitylifecyclecallbacks/"
                  "JNIActivityLifecycleCallbacks");
    const char* activity_name_env =
        std::getenv("MOCKTAIL_ACTIVITY_LIFECYCLE_ACTIVITY_NAME");
    const char* activity_name =
        activity_name_env != nullptr && activity_name_env[0] != '\0'
            ? activity_name_env
            : "MainGameActivity";
    jstring activity_name_string = env->NewStringUTF(activity_name);
    std::cout << "  [engine] activity lifecycle for " << activity_name << '\n'
              << std::flush;
    auto invoke_lifecycle_callback =
        [&](const char* label, NativeActivityLifecycleStringFn callback) {
          if (callback == nullptr) {
            return;
          }
          std::cout << "  [engine] JNIActivityLifecycleCallbacks." << label
                    << '\n'
                    << std::flush;
          if (sigsetjmp(g_activity_lifecycle_jmp_buf, 1) == 0) {
            g_activity_lifecycle_recovery_in_progress = 1;
            callback(env, lifecycle_callbacks, activity_name_string);
            g_activity_lifecycle_recovery_in_progress = 0;
            std::cout << "  [engine] JNIActivityLifecycleCallbacks." << label
                      << " returned\n"
                      << std::flush;
          } else {
            g_activity_lifecycle_recovery_in_progress = 0;
            std::cerr << "  [engine] JNIActivityLifecycleCallbacks." << label
                      << " recovered from crash\n"
                      << std::flush;
          }
        };
    invoke_lifecycle_callback(
        "nativeOnPreCreated",
        context->activity_lifecycle_callbacks.on_pre_created);
    invoke_lifecycle_callback(
        "nativeOnCreated", context->activity_lifecycle_callbacks.on_created);
    invoke_lifecycle_callback(
        "nativeOnPostCreated",
        context->activity_lifecycle_callbacks.on_post_created);
    invoke_lifecycle_callback(
        "nativeOnPreStarted",
        context->activity_lifecycle_callbacks.on_pre_started);
    invoke_lifecycle_callback(
        "nativeOnStarted", context->activity_lifecycle_callbacks.on_started);
    invoke_lifecycle_callback(
        "nativeOnPostStarted",
        context->activity_lifecycle_callbacks.on_post_started);
    invoke_lifecycle_callback(
        "nativeOnPreResumed",
        context->activity_lifecycle_callbacks.on_pre_resumed);
    invoke_lifecycle_callback(
        "nativeOnResumed", context->activity_lifecycle_callbacks.on_resumed);
    invoke_lifecycle_callback(
        "nativeOnPostResumed",
        context->activity_lifecycle_callbacks.on_post_resumed);
    std::cout << "  [engine] activity lifecycle returned\n" << std::flush;
  }

  if (context->run_app_lifecycle_active &&
      context->native_app_lifecycle_set_active) {
    env = ensure_env();
    std::cout << "  [engine] JNIAppLifecycleNativeAdapter.setActive\n"
              << std::flush;
    context->native_app_lifecycle_set_active(env, native_gl_class);
    std::cout << "  [engine] JNIAppLifecycleNativeAdapter.setActive returned\n"
              << std::flush;
  }

  if (context->run_native_fragment_start &&
      context->native_on_fragment_start) {
    env = ensure_env();
    std::cout << "  [engine] NativeGLInterface.nativeOnFragmentStart\n"
              << std::flush;
    if (sigsetjmp(g_native_fragment_start_jmp_buf, 1) == 0) {
      g_native_fragment_start_recovery_in_progress = kStage6RecoveryInline;
      context->native_on_fragment_start(env, native_gl_class);
      g_native_fragment_start_recovery_in_progress = kStage6RecoveryInactive;
      std::cout << "  [engine] NativeGLInterface.nativeOnFragmentStart returned\n"
                << std::flush;
    } else {
      g_native_fragment_start_recovery_in_progress = kStage6RecoveryInactive;
      std::cerr << "  [engine] NativeGLInterface.nativeOnFragmentStart recovered\n"
                << std::flush;
    }
  }

  const mocktail::platform::DisplayRefreshCapabilities display_refresh =
      mocktail::window::GetDisplayRefreshCapabilities();
  if (context->run_display_refresh_rate && display_refresh.valid() &&
      context->native_pass_current_display_refresh_rate) {
    env = ensure_env();
    std::cout << "  [engine] NativeGLInterface.nativePassCurrentDisplayRefreshRate "
              << display_refresh.current_hz << '\n'
              << std::flush;
    if (sigsetjmp(g_display_refresh_rate_jmp_buf, 1) == 0) {
      g_display_refresh_rate_recovery_in_progress = kStage6RecoveryInline;
      context->native_pass_current_display_refresh_rate(env, native_gl_class,
                                                        display_refresh.current_hz);
      g_display_refresh_rate_recovery_in_progress = kStage6RecoveryInactive;
      std::cout
          << "  [engine] NativeGLInterface.nativePassCurrentDisplayRefreshRate returned\n"
          << std::flush;
    } else {
      g_display_refresh_rate_recovery_in_progress = kStage6RecoveryInactive;
      std::cerr
          << "  [engine] NativeGLInterface.nativePassCurrentDisplayRefreshRate recovered\n"
          << std::flush;
    }
  }

  if (context->run_display_refresh_rate && display_refresh.valid() &&
      context->native_pass_supported_refresh_rates &&
      IsEnabled("MOCKTAIL_PASS_SUPPORTED_REFRESH_RATES")) {
    env = ensure_env();
    std::cout << "  [engine] NativeGLInterface.nativePassSupportedRefreshRates"
              << " count=" << display_refresh.supported_hz.size() << '\n'
              << std::flush;
    if (sigsetjmp(g_display_refresh_rate_jmp_buf, 1) == 0) {
      g_display_refresh_rate_recovery_in_progress = kStage6RecoveryInline;
      const jsize count =
          static_cast<jsize>(display_refresh.supported_hz.size());
      jfloatArray refresh_rates = env->NewFloatArray(count);
      if (refresh_rates != nullptr && count > 0) {
        env->SetFloatArrayRegion(refresh_rates, 0, count,
                                 display_refresh.supported_hz.data());
        context->native_pass_supported_refresh_rates(env, native_gl_class,
                                                     refresh_rates);
      }
      g_display_refresh_rate_recovery_in_progress = kStage6RecoveryInactive;
      std::cout
          << "  [engine] NativeGLInterface.nativePassSupportedRefreshRates returned\n"
          << std::flush;
    } else {
      g_display_refresh_rate_recovery_in_progress = kStage6RecoveryInactive;
      std::cerr
          << "  [engine] NativeGLInterface.nativePassSupportedRefreshRates recovered\n"
          << std::flush;
    }
  }

  if (context->native_update_app_ui_sizes &&
      ShouldRunStartupStep("MOCKTAIL_UPDATE_APP_UI_SIZES", false)) {
    env = ensure_env();
    std::cout << "  [engine] updateAppUISizes\n" << std::flush;
    context->native_update_app_ui_sizes(env, native_gl_class, 1280, 720, 0, 0,
                                        0);
    std::cout << "  [engine] updateAppUISizes returned\n" << std::flush;
  }

  int init_delay_ms = GetEnvInt("MOCKTAIL_APPBRIDGE_INIT_DELAY_MS", 0);
  if (init_delay_ms > 0) {
    std::cout << "  [engine] wait after AppBridge init: " << init_delay_ms
              << " ms\n"
              << std::flush;
    usleep(static_cast<useconds_t>(init_delay_ms) * 1000);
  }

  if (context->run_start_lua_app_dm && context->native_start_lua_app_dm &&
      !IsEnabled("MOCKTAIL_START_LUA_APP_DM_AFTER_START_APP")) {
    int delay_ms = GetEnvInt("MOCKTAIL_START_LUA_APP_DM_DELAY_MS", 1500);
    if (IsEnabled("MOCKTAIL_START_LUA_APP_DM_INLINE")) {
      if (delay_ms > 0) {
        std::cout << "  [engine] nativeAppBridgeStartLuaAppDM wait "
                  << delay_ms << " ms\n"
                  << std::flush;
        usleep(static_cast<useconds_t>(delay_ms) * 1000);
      }
      env = ensure_env();
      if (IsEnabled("MOCKTAIL_TRACE_START_LUA_JNI")) {
        setenv("MOCKTAIL_JNI_TRACE", "1", 1);
      }
      std::cout << "  [engine] nativeAppBridgeStartLuaAppDM\n"
                << std::flush;
      if (sigsetjmp(g_start_lua_app_dm_jmp_buf, 1) == 0) {
            g_start_lua_app_dm_recovery_in_progress = kStage6RecoveryInline;
        context->native_start_lua_app_dm(env, native_gl_class);
        g_start_lua_app_dm_recovery_in_progress = kStage6RecoveryInactive;
      } else {
        g_start_lua_app_dm_recovery_in_progress = kStage6RecoveryInactive;
        std::cerr << "  [engine] nativeAppBridgeStartLuaAppDM recovered\n"
                  << std::flush;
      }
      std::cout << "  [engine] nativeAppBridgeStartLuaAppDM returned\n"
                << std::flush;
    } else {
      g_pending_main_thread_start_lua_app_dm =
          context->native_start_lua_app_dm;
      g_pending_main_thread_start_lua_due_ms =
          MonotonicMillis() + static_cast<uint64_t>(delay_ms);
      g_pending_main_thread_start_lua_started = false;
      std::cout
          << "  [engine] delayed nativeAppBridgeStartLuaAppDM scheduled on "
          << "main thread\n"
          << std::flush;
    }
  }

	  if (context->run_update_surface_app) {
	    env = ensure_env();
	    std::cout << "  [engine] nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams\n"
	              << std::flush;
      if (context->call_real_update_surface_app) {
        context->native_update_surface_app(env, native_gl_class, surface,
                                           platform_params);
        std::cout
            << "  [engine] nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams returned\n"
            << std::flush;
      } else {
        std::cout
            << "  [engine] nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams bypassed\n"
            << std::flush;
      }
	  }

  if (IsEnabled("MOCKTAIL_ASMA_START_TASK_SCHEDULER_FOREGROUND") &&
      context->native_set_task_scheduler_background_mode) {
    env = ensure_env();
    if (!RunTaskSchedulerForegroundOnMainThread(
            context->native_set_task_scheduler_background_mode,
            native_gl_class)) {
      InvokeTaskSchedulerForeground(
          env, native_gl_class,
          context->native_set_task_scheduler_background_mode, "engine");
    }
  }

	  const bool effective_run_start_app_with_params =
	      HasEnvValue("MOCKTAIL_STEP_START_APP_WITH_PARAMS")
	          ? IsEnabled("MOCKTAIL_STEP_START_APP_WITH_PARAMS")
	          : context->run_start_app_with_params;
  const bool effective_call_real_start_app_with_params =
      effective_run_start_app_with_params &&
      (HasEnvValue("MOCKTAIL_CALL_REAL_APP_BRIDGE_START")
           ? IsEnabled("MOCKTAIL_CALL_REAL_APP_BRIDGE_START")
           : context->call_real_start_app_with_params);
  const bool effective_start_game_with_params =
      IsEnabled("MOCKTAIL_START_GAME_WITH_PARAM") &&
      context->game_session_runtime != nullptr;
  const bool force_inline_start_app_with_params =
      effective_start_game_with_params &&
      IsEnabled("MOCKTAIL_SYNC_START_APP_WITH_GAME");
  if (context->run_start_app_with_params != effective_run_start_app_with_params ||
      context->call_real_start_app_with_params !=
          effective_call_real_start_app_with_params) {
    std::cerr << "  [engine] start-app flags drifted: context_run_start_app_with_params="
              << (context->run_start_app_with_params ? 1 : 0)
              << " effective=" << (effective_run_start_app_with_params ? 1 : 0)
              << " context_call_real_start_app_with_params="
              << (context->call_real_start_app_with_params ? 1 : 0)
              << " effective_call_real_start_app_with_params="
              << (effective_call_real_start_app_with_params ? 1 : 0) << '\n'
              << std::flush;
  }

	  if (effective_run_start_app_with_params) {
	    env = ensure_env();
	    std::cout << "  [engine] nativeAppBridgeV2StartAppWithParams\n"
	              << std::flush;
    volatile sig_atomic_t start_app_recovered = 0;
    if (effective_call_real_start_app_with_params) {
      if (force_inline_start_app_with_params) {
        std::cout << "  [engine] forcing inline StartAppWithParams because "
                     "StartGameWithParam is enabled\n"
                  << std::flush;
      }
      if (sigsetjmp(g_start_app_with_params_jmp_buf, 1) == 0) {
            g_start_app_with_params_recovery_in_progress =
            kStage6RecoveryInline;
        context->native_start_app_with_params(env, native_gl_class,
                                              start_app_params);
        g_start_app_with_params_recovery_in_progress =
            kStage6RecoveryInactive;
      } else {
        start_app_recovered = 1;
        g_start_app_with_params_recovery_in_progress =
            kStage6RecoveryInactive;
        std::cerr <<
            "  [engine] nativeAppBridgeV2StartAppWithParams recovered\n"
                    << std::flush;
      }
    } else {
      MocktailAppBridgeStart(env, app_params);
    }
    std::cout << "  [engine] nativeAppBridgeV2StartAppWithParams returned\n"
              << std::flush;
	    if (start_app_recovered) {
	      std::cerr
	          << "  [engine] startup path cannot continue after start_app recovery\n"
	          << std::flush;
	      pthread_exit(nullptr);
	    }
	  }


  if (IsEnabled("MOCKTAIL_START_LUA_APP_DM_AFTER_START_APP") &&
      context->run_start_lua_app_dm && context->native_start_lua_app_dm) {
    int delay_ms = GetEnvInt("MOCKTAIL_START_LUA_APP_DM_DELAY_MS", 0);
    if (delay_ms > 0) {
      std::cout << "  [engine] post-StartApp nativeAppBridgeStartLuaAppDM wait "
                << delay_ms << " ms\n"
                << std::flush;
      usleep(static_cast<useconds_t>(delay_ms) * 1000);
    }
    env = ensure_env();
    if (IsEnabled("MOCKTAIL_TRACE_START_LUA_JNI")) {
      setenv("MOCKTAIL_JNI_TRACE", "1", 1);
    }
    std::cout << "  [engine] post-StartApp nativeAppBridgeStartLuaAppDM\n"
              << std::flush;
    if (sigsetjmp(g_start_lua_app_dm_jmp_buf, 1) == 0) {
        g_start_lua_app_dm_recovery_in_progress = kStage6RecoveryInline;
      context->native_start_lua_app_dm(env, native_gl_class);
      g_start_lua_app_dm_recovery_in_progress = kStage6RecoveryInactive;
    } else {
      g_start_lua_app_dm_recovery_in_progress = kStage6RecoveryInactive;
      std::cerr
          << "  [engine] post-StartApp nativeAppBridgeStartLuaAppDM recovered\n"
          << std::flush;
    }
    std::cout << "  [engine] post-StartApp nativeAppBridgeStartLuaAppDM returned\n"
              << std::flush;
  }

  if (context->game_session_runtime != nullptr) {
    env = ensure_env();
    mocktail::runtime::GameSessionPrincipal principal;
    principal.kind = context->account_identity.user_id > 0
                         ? mocktail::runtime::GameSessionPrincipalKind::
                               kAuthenticated
                         : mocktail::runtime::GameSessionPrincipalKind::
                               kLocalGuest;
    principal.generation = 1;
    principal.principal_id = context->account_identity.user_id > 0
                                 ? std::to_string(
                                       context->account_identity.user_id)
                                 : std::string();
    principal.base_url = "https://www.roblox.com/";
    std::string launch_parameters =
        GetEnvString("MOCKTAIL_GAME_PARAMS_JSON", "{}");
    if (launch_parameters.empty()) {
      launch_parameters = "{}";
    }
    mocktail::runtime::GameJoinRequest request{
        1, GetEnvLong("MOCKTAIL_PLACE_ID", 0), std::move(launch_parameters)};
    const mocktail::window::WindowSurfaceSnapshot window_surface =
        mocktail::window::GetWindowSurfaceSnapshot();
    if (!window_surface.available) {
      std::cerr << "  [game-session] initial typed window surface is "
                   "unavailable\n";
      return nullptr;
    }
    mocktail::runtime::GameSurface game_surface{
        window_surface.generation, window_surface.native_window,
        window_surface.width, window_surface.height};
    mocktail::runtime::RobloxGameSessionBinding binding{
        {native_gl_class, surface, platform_params, game_surface_activity,
         start_game_params},
        principal,
        request,
        game_surface};
    const mocktail::Status status =
        context->game_session_runtime->InitializeAndStart(
            binding, std::move(principal), std::move(request), game_surface);
    if (!status.ok()) {
      std::cerr << "  [game-session] typed lifecycle startup failed: "
                << status.message() << '\n'
                << std::flush;
      return nullptr;
    }
    std::cout << "  [game-session] typed lifecycle startup completed: "
              << mocktail::runtime::GameSessionStateName(
                     context->game_session_runtime->Snapshot().state)
              << '\n'
              << std::flush;
  }

  if (context->native_send_app_ready &&
      ShouldRunStartupStep("MOCKTAIL_SEND_APP_READY", false)) {
    env = ensure_env();
    volatile sig_atomic_t send_app_ready_recovered = 0;
    std::cout << "  [engine] nativeAppBridgeV2SendAppEventOnAppReady\n"
              << std::flush;
    if (sigsetjmp(g_send_app_ready_jmp_buf, 1) == 0) {
      g_send_app_ready_recovery_in_progress = kStage6RecoveryInline;
      jstring empty_ready_arg = env->NewStringUTF("");
      jstring home_feature = env->NewStringUTF("Home");
      context->native_send_app_ready(env, native_gl_class, empty_ready_arg,
                                     empty_ready_arg, empty_ready_arg,
                                     home_feature);
      g_send_app_ready_recovery_in_progress = kStage6RecoveryInactive;
    } else {
      send_app_ready_recovered = 1;
      g_send_app_ready_recovery_in_progress = kStage6RecoveryInactive;
      std::cerr
          << "  [engine] nativeAppBridgeV2SendAppEventOnAppReady recovered\n"
          << std::flush;
    }
    if (send_app_ready_recovered == 0) {
      std::cout
          << "  [engine] nativeAppBridgeV2SendAppEventOnAppReady returned\n"
          << std::flush;
    }
  }

  if (context->native_send_game_loaded &&
      ShouldRunStartupStep("MOCKTAIL_SEND_GAME_LOADED", false)) {
    env = ensure_env();
    volatile sig_atomic_t send_game_loaded_recovered = 0;
    std::cout << "  [engine] nativeAppBridgeV2SendAppEventOnGameLoaded\n"
              << std::flush;
    if (sigsetjmp(g_send_game_loaded_jmp_buf, 1) == 0) {
      g_send_game_loaded_recovery_in_progress = kStage6RecoveryInline;
      jstring empty_game_loaded_arg = env->NewStringUTF("");
      jstring home_feature = env->NewStringUTF("Home");
      context->native_send_game_loaded(env, native_gl_class, home_feature,
                                       empty_game_loaded_arg,
                                       empty_game_loaded_arg);
      g_send_game_loaded_recovery_in_progress = kStage6RecoveryInactive;
    } else {
      send_game_loaded_recovered = 1;
      g_send_game_loaded_recovery_in_progress = kStage6RecoveryInactive;
      std::cerr
          << "  [engine] nativeAppBridgeV2SendAppEventOnGameLoaded recovered\n"
          << std::flush;
    }
    if (send_game_loaded_recovered == 0) {
      std::cout
          << "  [engine] nativeAppBridgeV2SendAppEventOnGameLoaded returned\n"
          << std::flush;
    }
  }

  int keepalive_ms = GetEnvInt("MOCKTAIL_KEEPALIVE_MS", 0);
  if (keepalive_ms > 0) {
    std::cout << "  [engine] keepalive: " << keepalive_ms << " ms\n"
              << std::flush;
    usleep(static_cast<useconds_t>(keepalive_ms) * 1000);
    std::cout << "  [engine] keepalive returned\n" << std::flush;
  }

  if (IsEnabled("MOCKTAIL_KEEPALIVE")) {
    std::cout << "  [engine] keepalive: forever\n" << std::flush;
    while (true) {
      sleep(1);
    }
  }

  if (attached_to_thread) {
    EngineLog("DetachCurrentThread");
    jint detach_result = context->java_vm->DetachCurrentThread();
    if (detach_result != JNI_OK) {
      std::cerr << "  [engine] DetachCurrentThread failed: " << detach_result
                << '\n' << std::flush;
    }
    attached_to_thread = false;
  }

  return nullptr;
}

}  // namespace

int mocktail::legacy::Run(const runtime::CommandLineOptions& options,
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

  const mocktail::compat::BuildProfile& build_profile =
      *profile_result.profile;
  const mocktail::compat::HostAbiProfile* host_abi_profile =
      mocktail::compat::FindHostAbiProfile(build_profile.elf_build_id);
  const bool experiment_allowed =
      build_profile.default_allowed || options.allow_unverified_build;
  g_allow_legacy_binary_patches.store(
      build_profile.allow_legacy_binary_patches, std::memory_order_release);
  const bool allow_host_abi_bridges =
      build_profile.allow_host_abi_bridges && experiment_allowed &&
      host_abi_profile != nullptr &&
      host_abi_profile->bridge_entry_count > 0;
  const bool allow_host_constructor_replay =
      build_profile.allow_host_constructor_replay &&
      allow_host_abi_bridges && host_abi_profile->init_array_offset != 0 &&
      host_abi_profile->HasValidConstructorRanges();
  g_allow_host_abi_bridges.store(allow_host_abi_bridges,
                                 std::memory_order_release);
  g_allow_host_constructor_replay.store(allow_host_constructor_replay,
                                        std::memory_order_release);
  g_active_host_abi_profile.store(host_abi_profile,
                                  std::memory_order_release);
  g_host_abi_install_attempted = false;
  g_host_abi_install_result = {};
  SetEnvDefault("MOCKTAIL_ROBLOX_VERSION",
                build_profile.version_name.c_str());
  const std::string roblox_version_code =
      std::to_string(build_profile.version_code);
  SetEnvDefault("MOCKTAIL_ROBLOX_VERSION_CODE",
                roblox_version_code.c_str());
  const std::string default_user_agent =
      "Roblox/" + build_profile.version_name +
      " (Linux; Android 33; Mocktail)";
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
            << (allow_host_abi_bridges ? "allowed" : "denied")
            << '\n';
  std::cout << "  [compat] Build-ID constructor replay: "
            << (allow_host_constructor_replay ? "allowed" : "denied")
            << '\n'
            << std::flush;
  if (!build_profile.default_allowed && !options.allow_unverified_build) {
    std::cerr << "[FATAL] This Roblox build is not enabled for normal runs: "
              << build_profile.reason << '\n'
              << "  Use --allow-unverified-build only for an explicit "
                 "compatibility check.\n";
    return EXIT_FAILURE;
  }

  ApplyAuthStartupDefaults(!dependencies.roblox_credential().empty(),
                           user_overrode_start_lua_app_dm,
                           user_overrode_start_lua_step,
                           user_overrode_start_app_step,
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
    std::cerr
        << "[FATAL] Legacy binary patches and signal recovery were removed.\n"
        << "  Build ID " << build_profile.elf_build_id
        << " is not supported on this runtime.\n";
    return EXIT_FAILURE;
  }
  std::cout << "  [compat] signal-recovery handler disabled for this Build "
               "ID\n";
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
      "nativeInitClientSettings", "()V",
      [](JNIEnv* /*env*/, jobject /*obj*/) {
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
  jni_vm->RegisterClass("com/roblox/engine/jni/OnAppBridgeNotificationListener");
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
  jni_vm->RegisterClass("com/google/androidgamesdk/gametextinput/InputConnection");
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

  std::cout << "  Registered " << jni_vm->GetClassCount()
            << " JNI class(es)\n";

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
      std::cout << "  [window] windowed startup: leaving real UpdateSurfaceAppWithPlatformParams opt-in\n"
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
      "libc.so", "libdl.so", "libm.so", "libz.so",
      "libandroid.so", "liblog.so", "libmediandk.so",
      "libOpenSLES.so", "libOpenMAXAL.so",
      "libEGL.so", "libGLESv2.so",
  };
  if (IsEnabled("MOCKTAIL_PRELOAD_VULKAN_SHIM")) {
    stub_names.push_back("libvulkan.so");
  }

  std::vector<void*> stub_handles;
  void* bionic_vulkan_adapter_handle = nullptr;
  for (const char* name : stub_names) {
    void* h = nullptr;
    bool exact_adapter = false;
    if (std::strcmp(name, "libEGL.so") == 0 &&
        bionic_egl_bridge.IsLoaded()) {
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
      std::cerr << "  [warn]  Could not preload " << name
                << ": " << ::dlerror() << '\n';
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
    auto result = ResolveSymbolForBionic(sym, has_window, real_gles_handle,
                                         stub_handles);
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
  linker::RegisterSymbol(
      "setsockopt", reinterpret_cast<void*>(mocktail_bionic_setsockopt));
  linker::RegisterSymbol("sendmsg",
                         reinterpret_cast<void*>(mocktail_bionic_sendmsg));
  linker::RegisterSymbol("mprotect", reinterpret_cast<void*>(mocktail_mprotect));
  linker::RegisterSymbol("pthread_condattr_init",
                         reinterpret_cast<void*>(mocktail_pthread_condattr_init));
  linker::RegisterSymbol("pthread_condattr_destroy",
                         reinterpret_cast<void*>(mocktail_pthread_condattr_destroy));
  linker::RegisterSymbol("pthread_condattr_setclock",
                         reinterpret_cast<void*>(mocktail_pthread_condattr_setclock));
  linker::RegisterSymbol("pthread_cond_init",
                         reinterpret_cast<void*>(mocktail_pthread_cond_init));
  linker::RegisterSymbol("pthread_cond_destroy",
                         reinterpret_cast<void*>(mocktail_pthread_cond_destroy));
  linker::RegisterSymbol("pthread_cond_signal",
                         reinterpret_cast<void*>(mocktail_pthread_cond_signal));
  linker::RegisterSymbol("pthread_cond_broadcast",
                         reinterpret_cast<void*>(mocktail_pthread_cond_broadcast));
  linker::RegisterSymbol("pthread_cond_wait",
                         reinterpret_cast<void*>(mocktail_pthread_cond_wait));
  linker::RegisterSymbol("pthread_cond_timedwait",
                         reinterpret_cast<void*>(mocktail_pthread_cond_timedwait));
  linker::RegisterSymbol("pthread_mutexattr_init",
                         reinterpret_cast<void*>(mocktail_pthread_mutexattr_init));
  linker::RegisterSymbol("pthread_mutexattr_destroy",
                         reinterpret_cast<void*>(mocktail_pthread_mutexattr_destroy));
  linker::RegisterSymbol("pthread_mutexattr_settype",
                         reinterpret_cast<void*>(mocktail_pthread_mutexattr_settype));
  linker::RegisterSymbol("pthread_mutex_init",
                         reinterpret_cast<void*>(mocktail_pthread_mutex_init));
  linker::RegisterSymbol("pthread_mutex_destroy",
                         reinterpret_cast<void*>(mocktail_pthread_mutex_destroy));
  linker::RegisterSymbol("pthread_mutex_lock",
                         reinterpret_cast<void*>(mocktail_pthread_mutex_lock));
  linker::RegisterSymbol("pthread_mutex_trylock",
                         reinterpret_cast<void*>(mocktail_pthread_mutex_trylock));
  linker::RegisterSymbol("pthread_mutex_unlock",
                         reinterpret_cast<void*>(mocktail_pthread_mutex_unlock));
  linker::RegisterSymbol("pthread_once",
                         reinterpret_cast<void*>(mocktail_pthread_once));
  linker::RegisterSymbol("pthread_spin_init",
                         reinterpret_cast<void*>(mocktail_pthread_spin_init));
  linker::RegisterSymbol("pthread_spin_destroy",
                         reinterpret_cast<void*>(mocktail_pthread_spin_destroy));
  linker::RegisterSymbol("pthread_spin_lock",
                         reinterpret_cast<void*>(mocktail_pthread_spin_lock));
  linker::RegisterSymbol("pthread_spin_trylock",
                         reinterpret_cast<void*>(mocktail_pthread_spin_trylock));
  linker::RegisterSymbol("pthread_spin_unlock",
                         reinterpret_cast<void*>(mocktail_pthread_spin_unlock));
  linker::RegisterSymbol("pthread_barrier_init",
                         reinterpret_cast<void*>(mocktail_pthread_barrier_init));
  linker::RegisterSymbol(
      "pthread_barrier_destroy",
      reinterpret_cast<void*>(mocktail_pthread_barrier_destroy));
  linker::RegisterSymbol("pthread_barrier_wait",
                         reinterpret_cast<void*>(mocktail_pthread_barrier_wait));
  linker::RegisterSymbol(
      "pthread_create",
      reinterpret_cast<void*>(mocktail_bionic_pthread_create));
  linker::RegisterSymbol("abort", reinterpret_cast<void*>(mocktail_abort));
  linker::RegisterSymbol("__stack_chk_fail", reinterpret_cast<void*>(mocktail_recover_stack_chk_fail));
  (void)registered;
  std::cout << "  [linker] Registered " << registered << " / " << total_symbols
            << " known symbols.\n";
  std::cout << "  [linker] GL symbol resolution: total=" << gl_symbol_count
            << ", window=" << gl_from_window
            << ", real_gles=" << gl_from_real_gles
            << ", stub=" << gl_from_stub << ", host=" << gl_from_host
            << ", unresolved=" << gl_unresolved << '\n';
  if (IsEnabled("MOCKTAIL_REQUIRE_REAL_GRAPHICS") && has_window &&
      !IsEnabled("MOCKTAIL_GLES_FORCE_STUB") &&
      !IsEnabled("MOCKTAIL_GLES_NOOP_DRAW_CALLS")) {
    if (gl_from_stub > 0 || gl_unresolved > 0 || (gl_from_window == 0 &&
                                                  gl_from_real_gles == 0 &&
                                                  gl_from_host == 0)) {
      std::cerr << "[FATAL] Mocktail cannot guarantee real GL symbols for "
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

  // Register imports before loading synthetic SONAMEs.
  {
    int fd = ::open(library_path.c_str(), O_RDONLY);
    if (fd >= 0) {
      struct stat st;
      if (::fstat(fd, &st) == 0) {
        void* map = ::mmap(nullptr, static_cast<size_t>(st.st_size),
                           PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
          auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(map);
          auto* shdr = reinterpret_cast<Elf64_Shdr*>(
              reinterpret_cast<char*>(map) + ehdr->e_shoff);
          Elf64_Shdr* dynsym_hdr = nullptr;
          Elf64_Shdr* dynstr_hdr = nullptr;
          for (int si = 0; si < ehdr->e_shnum; ++si) {
            if (shdr[si].sh_type == SHT_DYNSYM) dynsym_hdr = &shdr[si];
            else if (shdr[si].sh_type == SHT_STRTAB && si != ehdr->e_shstrndx)
              dynstr_hdr = &shdr[si];
          }
          if (dynsym_hdr && dynstr_hdr) {
            auto* syms = reinterpret_cast<Elf64_Sym*>(
                reinterpret_cast<char*>(map) + dynsym_hdr->sh_offset);
            const char* strtab = reinterpret_cast<const char*>(map)
                                 + dynstr_hdr->sh_offset;
            size_t num_syms = dynsym_hdr->sh_size / sizeof(Elf64_Sym);
            int auto_registered = 0;
            for (size_t si = 0; si < num_syms; ++si) {
              if (syms[si].st_shndx != SHN_UNDEF) continue;
              const char* name = strtab + syms[si].st_name;
              if (!name || name[0] == '\0') continue;
              if (linker::GetBionicSymbols().count(name)) continue;
              auto result = ResolveSymbolForBionic(name, has_window,
                                                  real_gles_handle,
                                                  stub_handles);
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

  auto* jni_onload =
      reinterpret_cast<JniOnLoadFn>(
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
  int jni_timeout_ms =
      GetEnvInt("MOCKTAIL_JNI_ONLOAD_TIMEOUT_MS", 3000);
  bool soft_timeout_enabled = IsEnabled("MOCKTAIL_JNI_ONLOAD_SOFT_TIMEOUT");
  bool soft_timeout_forced =
      IsEnabled("MOCKTAIL_JNI_ONLOAD_SOFT_TIMEOUT_FORCE");
  g_stage6_jni_env =
      reinterpret_cast<uintptr_t>(jni_vm->GetJNIEnv());
  if (soft_timeout_enabled && !soft_timeout_forced) {
    std::cerr
        << "  [jvm] JNI_OnLoad soft timeout disabled in safe mode (set "
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

  std::cout << "  JNI_OnLoad returned. Requested JNI version: 0x"
            << std::hex << std::setw(8) << std::setfill('0') << jni_version
            << std::dec << '\n';

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
    std::cout << "  [compat] skipped all fixed-offset post-JNI patches\n"
              << std::flush;
  }

  // Restore the pseudo-JNI table if JNI_OnLoad replaced it.
  {
    JNIEnv* main_env = jni_vm->GetJNIEnv();
    if (main_env) {
      std::cerr << "  [jvm] restoring env->functions after JNI_OnLoad"
                << " (was " << (void*)main_env->functions << ")\n" << std::flush;
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
        surface_config.is_keyboard_device =
            input_capabilities.keyboard_enabled;
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
        reinterpret_cast<NativeInitStorageManagerFn>(linker::ResolveSymbol(
            roblox_handle,
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
    auto* native_retry_init =
        reinterpret_cast<NativeNoArgFn>(linker::ResolveSymbol(
            roblox_handle,
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
    auto* native_on_fragment_start =
        reinterpret_cast<NativeNoArgFn>(linker::ResolveSymbol(
            roblox_handle,
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
    auto* native_send_app_ready =
        reinterpret_cast<NativeSendAppReadyFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeGLInterface_"
            "nativeAppBridgeV2SendAppEventOnAppReady"));
    auto* native_send_game_loaded =
        reinterpret_cast<NativeSendGameLoadedFn>(linker::ResolveSymbol(
            roblox_handle,
            "Java_com_roblox_engine_jni_NativeGLInterface_"
            "nativeAppBridgeV2SendAppEventOnGameLoaded"));
    auto* native_set_asset_path =
        reinterpret_cast<NativeSetStringParamFn>(linker::ResolveSymbol(
            roblox_handle,
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
    auto* native_game_activity_init =
        reinterpret_cast<NativeGameActivityInitFn>(linker::ResolveSymbol(
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
    std::cout << "    native_init_with_params="
              << reinterpret_cast<const void*>(native_init_with_params) << '\n'
              << "    native_init_client_settings="
              << reinterpret_cast<const void*>(native_init_client_settings)
              << '\n'
              << "    native_init_client_settings_signed="
              << reinterpret_cast<const void*>(
                     native_init_client_settings_signed)
              << '\n'
              << "    native_init_client_settings_cached="
              << reinterpret_cast<const void*>(
                     native_init_client_settings_cached)
              << '\n'
              << "    native_init_client_settings_cached_compressed="
              << reinterpret_cast<const void*>(
                     native_init_client_settings_cached_compressed)
              << '\n'
              << "    native_initialize_native_flags="
              << reinterpret_cast<const void*>(
                     native_initialize_native_flags)
              << '\n'
              << "    native_app_bridge_app_start="
              << reinterpret_cast<const void*>(native_app_bridge_app_start)
              << '\n'
              << "    native_set_is_first_install="
              << reinterpret_cast<const void*>(native_set_is_first_install)
              << '\n'
              << "    native_update_adapter_init="
              << reinterpret_cast<const void*>(native_update_adapter_init)
              << '\n'
              << "    native_update_app_ui_sizes="
              << reinterpret_cast<const void*>(native_update_app_ui_sizes)
              << '\n'
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
              << reinterpret_cast<const void*>(
                     native_call_messages_from_main_thread)
              << '\n'
              << "    native_base_url_protocol_init="
              << reinterpret_cast<const void*>(native_base_url_protocol_init)
              << '\n'
              << "    native_set_device_info="
              << reinterpret_cast<const void*>(native_set_device_info)
              << '\n'
              << "    native_override_channel_platform_name="
              << reinterpret_cast<const void*>(
                     native_override_channel_platform_name)
              << '\n'
              << "    native_set_roblox_version="
              << reinterpret_cast<const void*>(native_set_roblox_version)
              << '\n'
              << "    native_set_http_client_proxy="
              << reinterpret_cast<const void*>(native_set_http_client_proxy)
              << '\n'
              << "    native_init_fast_log="
              << reinterpret_cast<const void*>(native_init_fast_log) << '\n'
              << "    native_set_multiple_cookies="
              << reinterpret_cast<const void*>(native_set_multiple_cookies)
              << '\n'
              << "    native_cookie_manager_set_cookie="
              << reinterpret_cast<const void*>(
                     native_cookie_manager_set_cookie)
              << '\n'
              << "    native_set_platform_headers_with_idfa="
              << reinterpret_cast<const void*>(
                     native_set_platform_headers_with_idfa)
              << '\n'
              << "    native_local_storage_set_platform_impl="
              << reinterpret_cast<const void*>(
                     native_local_storage_set_platform_impl)
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
              << reinterpret_cast<const void*>(native_game_activity_init)
              << '\n'
              << "    native_app_lifecycle_set_active="
              << reinterpret_cast<const void*>(native_app_lifecycle_set_active)
              << '\n'
              << "    native_on_fragment_start="
              << reinterpret_cast<const void*>(native_on_fragment_start)
              << '\n'
              << "    native_pass_supported_refresh_rates="
              << reinterpret_cast<const void*>(
                     native_pass_supported_refresh_rates)
              << '\n'
              << "    native_pass_current_display_refresh_rate="
              << reinterpret_cast<const void*>(
                     native_pass_current_display_refresh_rate)
              << '\n'
              << std::flush;
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

    std::cout << "  [compat] skipped fixed-offset Stage 6 patch set\n"
              << std::flush;
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
    const bool run_game_activity_init =
        ShouldRunStartupStep("MOCKTAIL_STEP_GAME_ACTIVITY_INIT",
                             has_window || IsEnabled("MOCKTAIL_GAME_ACTIVITY_INIT"));
    const bool run_set_init_params =
        ShouldRunStartupStep("MOCKTAIL_STEP_SET_INIT_PARAMS",
                             IsEnabled("MOCKTAIL_SET_INIT_PARAMS") ||
                                 (run_game_activity_init &&
                                  !run_init_with_params));
    const bool run_game_activity_surface =
        ShouldRunStartupStep(
            "MOCKTAIL_STEP_GAME_ACTIVITY_SURFACE",
            has_window && run_game_activity_init &&
                IsEnabled("MOCKTAIL_GAME_ACTIVITY_SURFACE"));
    const bool run_app_lifecycle_active =
        ShouldRunStartupStep("MOCKTAIL_STEP_APP_LIFECYCLE_ACTIVE",
                             has_window && IsEnabled("MOCKTAIL_APP_LIFECYCLE_ACTIVE"));
    const bool run_native_fragment_start =
        ShouldRunStartupStep("MOCKTAIL_STEP_NATIVE_FRAGMENT_START",
                             has_window &&
                                 IsEnabled("MOCKTAIL_NATIVE_FRAGMENT_START")) &&
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
                             run_init_with_params ||
                                 run_start_app_with_params);
    const bool run_prepare_jni =
        ShouldRunStartupStep("MOCKTAIL_STEP_PREP_JNI",
                                 run_set_asset_path || run_global_init ||
                                     run_init_client_settings ||
                                     run_post_client_settings ||
                                     run_app_bridge_app_start ||
                                     run_native_settings ||
                                     run_set_init_params ||
                                     run_init_with_params ||
                                     run_update_screen_orientation ||
                                     run_activity_lifecycle ||
                                     run_game_activity_init ||
                                     run_game_activity_surface ||
                                     run_app_lifecycle_active ||
                                     run_native_fragment_start ||
                                     run_display_refresh_rate ||
                                     run_update_surface_app ||
                                     run_start_app_with_params ||
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
              << (run_start_app_with_params ? 1 : 0) << '\n' << std::flush;

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
                << '\n' << std::flush;
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
          startup_thread.WaitFor(startup_timeout_ms > 0 ? startup_timeout_ms
                                                        : -1,
                                 10, &PumpStartupOwnerThread);
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

        const int cancel_join_grace_ms = std::clamp(
            GetEnvInt("MOCKTAIL_STARTUP_CANCEL_JOIN_GRACE_MS", 5000), 100,
            60000);
        const mocktail::runtime::OwnedPthreadCancelResult cancel_result =
            startup_thread.CancelAndJoinFor(
                cancel_join_grace_ms, 10, &PumpStartupOwnerThread);
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
        std::cerr << "  [main] SDL event loop tick #" << main_loop_ticks
                  << '\n'
                  << std::flush;
      }
      RunPendingMainThreadTaskSchedulerForeground();
      if (g_pending_main_thread_start_lua_app_dm &&
          !g_pending_main_thread_start_lua_started &&
          g_vm_for_main_thread_pump &&
          g_native_gl_class_for_main_thread &&
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
        std::cout << "  [main] delayed nativeAppBridgeStartLuaAppDM returned\n"
                  << std::flush;
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
      const uint64_t fps_after_platform_ns =
          fps_trace ? MonotonicNanos() : 0;
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
      input_shutdown_completed = window_input_runtime->Shutdown().ok() &&
                                 input_shutdown_completed;
    }
    bool experience_destroyed_app = false;
    if (experience_composition != nullptr) {
      jni_vm->ClearRobloxExperienceLifecycleCallbacks();
      experience_lifecycle_target.reset();
      const mocktail::runtime::GameSessionSnapshot snapshot =
          experience_composition->Snapshot();
      experience_destroyed_app =
          snapshot.game_running || snapshot.game_paused ||
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
          shutdown_result.state == mocktail::runtime::GameSessionState::kStopped;
      std::cout << "  [main] Roblox lifecycle shutdown: "
                << mocktail::runtime::GameSessionStateName(shutdown_result.state)
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
            lifecycle.Shutdown(shutdown_env,
                               g_native_gl_class_for_main_thread);
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
      const mocktail::Status& failure =
          resize_stop_status.ok() ? resize_completion_status
                                  : resize_stop_status;
      std::cerr << "[FATAL] Real resize readiness failed: "
                << failure.message() << '\n';
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
    std::cerr << "[FATAL] Windowed runtime exited without completing typed input shutdown.\n";
    return EXIT_FAILURE;
  }
  if (!resize_readiness_completed) {
    std::cerr << "[FATAL] Windowed runtime exited without completing the real "
                 "resize/rebind readiness sequence.\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
