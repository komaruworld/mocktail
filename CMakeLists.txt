# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

cmake_minimum_required(VERSION 3.20)
project(Mocktail
  VERSION 1.0.3
  DESCRIPTION "Android x86-64 Roblox compatibility runtime for Linux"
  LANGUAGES CXX
)

include(GNUInstallDirs)
find_package(Threads REQUIRED)

set(MOCKTAIL_BUILD_GIT_COMMIT "unknown")
set(MOCKTAIL_BUILD_GIT_DIRTY "unknown")
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE MOCKTAIL_GIT_REV_PARSE_RESULT
    OUTPUT_VARIABLE MOCKTAIL_GIT_REV_PARSE_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(MOCKTAIL_GIT_REV_PARSE_RESULT EQUAL 0 AND
     MOCKTAIL_GIT_REV_PARSE_OUTPUT MATCHES "^[0-9a-fA-F]+$")
    string(TOLOWER "${MOCKTAIL_GIT_REV_PARSE_OUTPUT}"
      MOCKTAIL_BUILD_GIT_COMMIT)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      RESULT_VARIABLE MOCKTAIL_GIT_STATUS_RESULT
      OUTPUT_VARIABLE MOCKTAIL_GIT_STATUS_OUTPUT
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(MOCKTAIL_GIT_STATUS_RESULT EQUAL 0)
      if(MOCKTAIL_GIT_STATUS_OUTPUT STREQUAL "")
        set(MOCKTAIL_BUILD_GIT_DIRTY "false")
      else()
        set(MOCKTAIL_BUILD_GIT_DIRTY "true")
      endif()
    endif()
  endif()
endif()

set(MOCKTAIL_DEFAULT_COMPATIBILITY_MANIFEST
  "${CMAKE_SOURCE_DIR}/config/roblox_compatibility.json"
  CACHE FILEPATH
  "Fallback compatibility manifest used outside an installed runtime"
)
set(MOCKTAIL_DEFAULT_SIGNING_TRUST_MANIFEST
  "${CMAKE_SOURCE_DIR}/config/roblox_signing_certificates.json"
  CACHE FILEPATH
  "Fallback signing trust manifest used outside an installed runtime"
)
set(MOCKTAIL_DISCORD_APPLICATION_ID "1537088975720812655" CACHE STRING
  "Discord application ID baked into Mocktail Rich Presence releases"
)
string(LENGTH "${MOCKTAIL_DISCORD_APPLICATION_ID}"
  MOCKTAIL_DISCORD_APPLICATION_ID_LENGTH)
if(NOT MOCKTAIL_DISCORD_APPLICATION_ID STREQUAL "" AND
   (NOT MOCKTAIL_DISCORD_APPLICATION_ID MATCHES "^[0-9]+$" OR
    MOCKTAIL_DISCORD_APPLICATION_ID_LENGTH LESS 17 OR
    MOCKTAIL_DISCORD_APPLICATION_ID_LENGTH GREATER 20))
  message(FATAL_ERROR
    "MOCKTAIL_DISCORD_APPLICATION_ID must be empty or a Discord snowflake")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# libjnivm uses C++ exceptions, so these cannot be directory-wide.
add_library(mocktail_compile_options INTERFACE)
target_compile_options(mocktail_compile_options INTERFACE
  "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Wno-unused-parameter;-fno-exceptions;-g>"
)

function(mocktail_apply_compile_options target_name)
  target_link_libraries(${target_name} PRIVATE mocktail_compile_options)
endfunction()

option(MOCKTAIL_ENABLE_UPSTREAM_JNIVM
  "Build upstream libjnivm for the incremental JNI runtime migration" ON)
if(MOCKTAIL_ENABLE_UPSTREAM_JNIVM)
  set(JNIVM_ENABLE_TRACE OFF CACHE BOOL
    "Enable tracing inside upstream libjnivm")
  set(JNIVM_ENABLE_GC ON CACHE BOOL
    "Enable local/global reference tracking inside upstream libjnivm")
  set(JNIVM_ENABLE_DEBUG OFF CACHE BOOL
    "Enable upstream libjnivm class-dump/code-generation support")
  set(JNIVM_ENABLE_RETURN_NON_ZERO OFF CACHE BOOL
    "Allow upstream libjnivm to fabricate unresolved object return values")
  set(JNIVM_FAKE_JNI_MINECRAFT_LINUX_COMPAT OFF CACHE BOOL
    "Enable legacy fake-jni static method promotion")
  set(JNIVM_ENABLE_TESTS OFF CACHE BOOL
    "Build upstream libjnivm's own test suite")
  set(JNIVM_BUILD_EXAMPLES OFF CACHE BOOL
    "Build upstream libjnivm examples")

  add_subdirectory(third_party/libjnivm EXCLUDE_FROM_ALL)
  # Work around a missing include in libjnivm.
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(jnivm PRIVATE -include string)
  endif()
  target_link_libraries(jnivm PUBLIC Threads::Threads)
  add_library(Mocktail::UpstreamJni ALIAS jnivm)
endif()

add_subdirectory(third_party/mcpelauncher-linker)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(linker PRIVATE -w)
endif()

set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL
  "Build Khronos Vulkan-Headers tests" FORCE)
set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL
  "Install vendored Vulkan-Headers" FORCE)
add_subdirectory(third_party/Vulkan-Headers EXCLUDE_FROM_ALL)

include(CTest)
if(BUILD_TESTING)
  # Package builds disable tests and do not fetch Google Test.
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

add_library(mocktail_crypto_foundation STATIC
  src/foundation/sha256.cc
)
add_library(Mocktail::CryptoFoundation ALIAS mocktail_crypto_foundation)
target_include_directories(mocktail_crypto_foundation PUBLIC include)
mocktail_apply_compile_options(mocktail_crypto_foundation)

include(${CMAKE_SOURCE_DIR}/cmake/MocktailPlatformGraphics.cmake)
foreach(foundation_target IN ITEMS
    mocktail_platform_sdl
    mocktail_graphics_foundation
    mocktail_sdl_vulkan_wsi)
  mocktail_apply_compile_options(${foundation_target})
endforeach()
foreach(graphics_test_target IN ITEMS
    bionic_egl_bridge_test
    platform_graphics_foundation_test)
  if(TARGET ${graphics_test_target})
    mocktail_apply_compile_options(${graphics_test_target})
  endif()
endforeach()

include(${CMAKE_SOURCE_DIR}/cmake/MocktailAudio.cmake)
foreach(audio_target IN ITEMS
    mocktail_audio_core
    mocktail_audio_sdl
    mocktail_audio_fmod_java_runtime
    mocktail_fmod_jni_audio_bridge
    mocktail_audio_opensl_adapter
    mocktail_opensles)
  mocktail_apply_compile_options(${audio_target})
endforeach()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
  # GCC 16 reports a false array-bounds warning in this translation unit.
  target_compile_options(mocktail_audio_opensl_adapter PRIVATE
    -Wno-array-bounds
  )
endif()
if(TARGET audio_foundation_test)
  mocktail_apply_compile_options(audio_foundation_test)
endif()
if(TARGET opensl_playback_runtime_test)
  mocktail_apply_compile_options(opensl_playback_runtime_test)
endif()
if(TARGET fmod_java_audio_runtime_test)
  mocktail_apply_compile_options(fmod_java_audio_runtime_test)
endif()
if(TARGET fmod_jni_audio_bridge_test)
  mocktail_apply_compile_options(fmod_jni_audio_bridge_test)
endif()

include(${CMAKE_SOURCE_DIR}/cmake/MocktailElfCompatibility.cmake)

# Bionic stdio symbols must exist before the linker target.
add_library(mocktail_bionic_stdio_runtime STATIC
  src/compat/bionic_stdio_runtime.cc
)
add_library(Mocktail::BionicStdio ALIAS mocktail_bionic_stdio_runtime)
target_include_directories(mocktail_bionic_stdio_runtime PUBLIC include)
mocktail_apply_compile_options(mocktail_bionic_stdio_runtime)

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBYAML REQUIRED IMPORTED_TARGET yaml-0.1)
pkg_check_modules(MINIZIP REQUIRED IMPORTED_TARGET minizip)
pkg_check_modules(CAPSTONE REQUIRED IMPORTED_TARGET capstone)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET "libadwaita-1>=1.6")
pkg_check_modules(WEBKITGTK6 REQUIRED IMPORTED_TARGET webkitgtk-6.0)

add_library(mocktail_update STATIC
  src/update/android_manifest.cc
  src/update/apk_bundle.cc
  src/update/apk_provider.cc
  src/update/apk_signature.cc
  src/update/apkpure_provider.cc
  src/update/candidate_approval.cc
  src/update/compatibility_catalog.cc
  src/update/http_download.cc
  src/update/host_abi_deriver.cc
  src/update/payload_integrity.cc
  src/update/payload_store.cc
  src/update/readiness_canary.cc
  src/update/unsafe_latest_runner.cc
  src/update/update_config.cc
  src/update/update_coordinator.cc
  src/update/zip_archive.cc
)
add_library(Mocktail::Update ALIAS mocktail_update)
target_include_directories(mocktail_update PUBLIC include)
target_link_libraries(mocktail_update PUBLIC
  Mocktail::Compat
  Mocktail::CryptoFoundation
  CURL::libcurl
  OpenSSL::Crypto
  PkgConfig::LIBYAML
  PkgConfig::MINIZIP
  PkgConfig::CAPSTONE
  nlohmann_json::nlohmann_json
  Threads::Threads
)
mocktail_apply_compile_options(mocktail_update)

add_executable(mocktail_updater src/update/main.cc)
target_link_libraries(mocktail_updater PRIVATE Mocktail::Update)
target_compile_definitions(mocktail_updater PRIVATE
  "MOCKTAIL_DEFAULT_COMPATIBILITY_MANIFEST=\"${MOCKTAIL_DEFAULT_COMPATIBILITY_MANIFEST}\""
  "MOCKTAIL_DEFAULT_SIGNING_TRUST_MANIFEST=\"${MOCKTAIL_DEFAULT_SIGNING_TRUST_MANIFEST}\""
)
mocktail_apply_compile_options(mocktail_updater)

add_library(mocktail_legacy_jnivm STATIC
  src/jnivm/jnivm.cc
)
add_library(Mocktail::LegacyJni ALIAS mocktail_legacy_jnivm)
target_include_directories(mocktail_legacy_jnivm PUBLIC include)
mocktail_apply_compile_options(mocktail_legacy_jnivm)
# A JDK is not required for these ABI-only headers.
set(MOCKTAIL_JNI_INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/third_party/jni/include
)
target_include_directories(mocktail_legacy_jnivm PUBLIC
  ${MOCKTAIL_JNI_INCLUDE_DIRS}
)

add_library(mocktail_runtime STATIC
  src/runtime/command_line.cc
  src/runtime/crash_report_policy.cc
  src/runtime/discord_rpc.cc
  src/runtime/device_profile.cc
  src/runtime/environment.cc
  src/runtime/external_launch_broker.cc
  src/runtime/frame_rate_policy.cc
  src/runtime/game_mode.cc
  src/runtime/game_session_coordinator.cc
  src/runtime/graphics_launch_policy.cc
  src/runtime/http_client_policy.cc
  src/runtime/memory_limit.cc
  src/runtime/owned_pthread.cc
  src/runtime/performance_policy.cc
  src/runtime/platform_cache_migration.cc
  src/runtime/roblox_desktop_app_policy.cc
  src/runtime/roblox_app_lifecycle.cc
  src/runtime/roblox_browser_service_bridge.cc
  src/runtime/roblox_capability_resolver.cc
  src/runtime/roblox_experience_launch_bridge.cc
  src/runtime/roblox_launch_uri.cc
  src/runtime/roblox_game_session_native_adapter.cc
  src/runtime/roblox_web_view_bridge.cc
  src/runtime/payload_update_preflight.cc
  src/runtime/runtime_config.cc
  src/runtime/runtime_config_bootstrap.cc
  src/runtime/runtime_config_file.cc
  src/runtime/runtime_paths.cc
  src/runtime/session_log.cc
  src/runtime/single_instance_lock.cc
  src/runtime/startup_state_machine.cc
  src/runtime/supported_launch_policy.cc
  src/runtime/support_bundle.cc
  src/runtime/system_proxy.cc
)
add_library(Mocktail::Runtime ALIAS mocktail_runtime)
target_include_directories(mocktail_runtime PUBLIC include)
target_include_directories(mocktail_runtime PUBLIC
  ${MOCKTAIL_JNI_INCLUDE_DIRS}
)
target_compile_definitions(mocktail_runtime PRIVATE
  "MOCKTAIL_DISCORD_APPLICATION_ID=\"${MOCKTAIL_DISCORD_APPLICATION_ID}\""
)
set_property(SOURCE src/runtime/session_log.cc APPEND PROPERTY
  COMPILE_DEFINITIONS
  "MOCKTAIL_PROJECT_VERSION=\"${PROJECT_VERSION}\""
  "MOCKTAIL_BUILD_GIT_COMMIT=\"${MOCKTAIL_BUILD_GIT_COMMIT}\""
  "MOCKTAIL_BUILD_GIT_DIRTY=\"${MOCKTAIL_BUILD_GIT_DIRTY}\""
  "MOCKTAIL_BUILD_TYPE=\"$<CONFIG>\""
  "MOCKTAIL_BUILD_COMPILER_ID=\"${CMAKE_CXX_COMPILER_ID}\""
  "MOCKTAIL_BUILD_COMPILER_VERSION=\"${CMAKE_CXX_COMPILER_VERSION}\""
  "MOCKTAIL_BUILD_TARGET_SYSTEM=\"${CMAKE_SYSTEM_NAME}\""
  "MOCKTAIL_BUILD_TARGET_PROCESSOR=\"${CMAKE_SYSTEM_PROCESSOR}\""
)
mocktail_apply_compile_options(mocktail_runtime)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
  # GCC 16 reports a false array-bounds warning in nlohmann::json::dump().
  set_property(SOURCE src/runtime/roblox_launch_uri.cc APPEND PROPERTY
    COMPILE_OPTIONS -Wno-array-bounds
  )
endif()
target_link_libraries(mocktail_runtime PUBLIC Threads::Threads)
target_link_libraries(mocktail_runtime PRIVATE
  Mocktail::CryptoFoundation
  CURL::libcurl
  nlohmann_json::nlohmann_json
  PkgConfig::GIO
  PkgConfig::LIBYAML
  ${CMAKE_DL_LIBS}
)

# Keep GTK out of the process that loads Android libraries.
add_library(mocktail_failure_dialog_runtime STATIC
  src/runtime/failure_dialog.cc
)
add_library(Mocktail::FailureDialog ALIAS mocktail_failure_dialog_runtime)
target_include_directories(mocktail_failure_dialog_runtime PUBLIC include)
target_compile_definitions(mocktail_failure_dialog_runtime PRIVATE
  "MOCKTAIL_INSTALL_LIBDIR=\"${CMAKE_INSTALL_LIBDIR}\""
)
mocktail_apply_compile_options(mocktail_failure_dialog_runtime)

add_executable(mocktail_failure_dialog
  src/ui/mocktail_failure_dialog.cc
)
target_link_libraries(mocktail_failure_dialog PRIVATE PkgConfig::LIBADWAITA)
mocktail_apply_compile_options(mocktail_failure_dialog)

add_library(mocktail_services STATIC
  src/services/http_client.cc
  src/services/auth_service.cc
  src/services/browser_tracker_service.cc
  src/services/client_settings_service.cc
)
add_library(Mocktail::Services ALIAS mocktail_services)
target_include_directories(mocktail_services PUBLIC include)
target_link_libraries(mocktail_services PUBLIC
  mocktail_runtime
  CURL::libcurl
  nlohmann_json::nlohmann_json
)
mocktail_apply_compile_options(mocktail_services)

add_library(mocktail_webview_launcher STATIC
  src/runtime/webview_helper_launcher.cc
)
add_library(Mocktail::WebViewLauncher ALIAS mocktail_webview_launcher)
target_include_directories(mocktail_webview_launcher PUBLIC include)
target_link_libraries(mocktail_webview_launcher PUBLIC CURL::libcurl)
mocktail_apply_compile_options(mocktail_webview_launcher)

add_library(mocktail_webview_helper_policy STATIC
  src/webview/webview_helper_policy.cc
)
add_library(Mocktail::WebViewHelperPolicy ALIAS mocktail_webview_helper_policy)
target_include_directories(mocktail_webview_helper_policy PUBLIC src)
target_link_libraries(mocktail_webview_helper_policy PUBLIC
  PkgConfig::WEBKITGTK6
  nlohmann_json::nlohmann_json
)
mocktail_apply_compile_options(mocktail_webview_helper_policy)

add_executable(mocktail_webview_helper
  src/webview/mocktail_webview_helper.cc
)
target_link_libraries(mocktail_webview_helper PRIVATE
  Mocktail::WebViewLauncher
  Mocktail::WebViewHelperPolicy
  Mocktail::Runtime
  PkgConfig::GTK4
  PkgConfig::WEBKITGTK6
)
mocktail_apply_compile_options(mocktail_webview_helper)

add_library(mocktail_auth_runtime_composition STATIC
  src/runtime/auth_runtime_composition.cc
  src/runtime/webview_roblox_cookie.cc
)
add_library(Mocktail::AuthRuntimeComposition ALIAS
  mocktail_auth_runtime_composition)
target_include_directories(mocktail_auth_runtime_composition PUBLIC include)
target_link_libraries(mocktail_auth_runtime_composition PUBLIC
  mocktail_legacy_jnivm
  mocktail_runtime
  mocktail_services
)
mocktail_apply_compile_options(mocktail_auth_runtime_composition)

add_library(mocktail_linker STATIC
  src/linker/linker.cc
)
target_include_directories(mocktail_linker PUBLIC include)
target_link_libraries(mocktail_linker PUBLIC
  dl
  linker
  mocktail_compat
  mocktail_bionic_stdio_runtime
  mocktail_bionic_abi_exports
)
mocktail_apply_compile_options(mocktail_linker)
target_compile_definitions(mocktail_linker PUBLIC MOCKTAIL_USE_BIONIC_LINKER=1)
# The Bionic linker expects IPLT boundaries that host executables do not have.
target_link_options(mocktail_linker INTERFACE
  -Wl,--defsym=__rela_iplt_start=0
  -Wl,--defsym=__rela_iplt_end=0
)

add_library(libc_shim STATIC
  src/libc_shim/libc_shim.cc
)
target_include_directories(libc_shim PUBLIC include)
mocktail_apply_compile_options(libc_shim)
target_link_libraries(mocktail_linker PUBLIC libc_shim)

add_subdirectory(stubs)
foreach(stub_target IN ITEMS
    stub_sys_m stub_sys_z stub_sys_dl stub_sys_c
    stub_android stub_log stub_mediandk stub_openmax
    stub_egl stub_glesv2 stub_vulkan)
  mocktail_apply_compile_options(${stub_target})
endforeach()

add_library(mocktail_window STATIC
  src/window/roblox_fullscreen_menu_request_gate.cc
  src/window/video_driver_policy.cc
  src/window/window.cc
  src/window/window_fullscreen_request_gate.cc
  src/window/window_game_surface_bridge.cc
  src/window/window_pointer_capture_owner.cc
  src/window/window_resize_readiness_gate.cc
  src/window/window_state_store.cc
  src/window/window_surface_lifecycle.cc
  src/window/window_text_input_owner.cc
)
target_include_directories(mocktail_window PUBLIC src)
mocktail_apply_compile_options(mocktail_window)
target_link_libraries(mocktail_window PUBLIC
  Mocktail::PlatformSdl
  SDL3::SDL3
  ${CMAKE_DL_LIBS}
  PRIVATE Mocktail::Runtime
)

add_library(mocktail_roblox_output_device_runtime STATIC
  src/audio/roblox_output_device_bridge.cc
)
target_include_directories(mocktail_roblox_output_device_runtime PUBLIC include)
target_include_directories(mocktail_roblox_output_device_runtime PRIVATE src)
target_link_libraries(mocktail_roblox_output_device_runtime PUBLIC
  mocktail_compat
  Mocktail::AudioSdl
  PRIVATE mocktail_linker
)
mocktail_apply_compile_options(mocktail_roblox_output_device_runtime)

add_library(mocktail_roblox_fullscreen_runtime STATIC
  src/runtime/roblox_fullscreen_runtime_bridge.cc
)
target_include_directories(mocktail_roblox_fullscreen_runtime PUBLIC include)
target_include_directories(mocktail_roblox_fullscreen_runtime PRIVATE src)
target_link_libraries(mocktail_roblox_fullscreen_runtime PUBLIC
  mocktail_compat
  PRIVATE
    mocktail_linker
    mocktail_window
    stub_log
)
mocktail_apply_compile_options(mocktail_roblox_fullscreen_runtime)

include(${CMAKE_SOURCE_DIR}/cmake/MocktailInput.cmake)

add_library(mocktail_experience_runtime STATIC
  src/runtime/jnivm_platform_web_callbacks.cc
  src/runtime/roblox_experience_composition.cc
  src/runtime/roblox_fresh_game_launch_controller.cc
  src/runtime/roblox_platform_web_symbols.cc
)
add_library(Mocktail::ExperienceRuntime ALIAS mocktail_experience_runtime)
target_include_directories(mocktail_experience_runtime PUBLIC include)
target_include_directories(mocktail_experience_runtime PRIVATE src)
target_link_libraries(mocktail_experience_runtime PUBLIC
  Mocktail::Runtime
  Mocktail::WebViewLauncher
  PkgConfig::UTF8PROC
  PRIVATE
    Mocktail::AuthRuntimeComposition
    Mocktail::LegacyJni
    mocktail_linker
    mocktail_window
)
mocktail_apply_compile_options(mocktail_experience_runtime)

# Compatibility exports still used by the old startup path.
add_library(mocktail_bionic_abi_exports STATIC
  src/compat/bionic_abi_exports.cc
  src/compat/http_client_spin_guard.cc
)
target_include_directories(mocktail_bionic_abi_exports
  PUBLIC include
  PRIVATE src
)
mocktail_apply_compile_options(mocktail_bionic_abi_exports)
# These exported Bionic FORTIFY symbols intentionally overlap glibc's
# __*_chk entry points.  When the bridge is visible to whole-program LTO,
# GCC can fold a fortified host call back into its own compatibility wrapper
# and turn the resulting tail recursion into an infinite loop.  Keep this
# translation unit opaque to interprocedural optimization and make its calls
# to the host C library explicit external calls.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_property(SOURCE src/compat/bionic_abi_exports.cc APPEND PROPERTY
    COMPILE_OPTIONS
      -fno-lto
      -fno-builtin
  )
endif()
target_link_libraries(mocktail_bionic_abi_exports PRIVATE
  mocktail_bionic_stdio_runtime
  mocktail_window
  Threads::Threads
)

include(CheckCXXSourceCompiles)
include(CheckIncludeFileCXX)
include(CMakePushCheckState)

check_include_file_cxx(execinfo.h MOCKTAIL_HAVE_EXECINFO_H)
if(NOT MOCKTAIL_HAVE_EXECINFO_H)
  message(FATAL_ERROR
    "execinfo.h is required for crash diagnostics; on musl install "
    "libexecinfo-devel"
  )
endif()

set(MOCKTAIL_EXECINFO_LINK_LIBRARY "")
set(_mocktail_execinfo_link_probe [=[
  #include <execinfo.h>
  int main() {
    void* frames[1] = {};
    const int count = backtrace(frames, 1);
    return backtrace_symbols(frames, count) == nullptr;
  }
]=])
check_cxx_source_compiles(
  "${_mocktail_execinfo_link_probe}"
  MOCKTAIL_EXECINFO_IN_HOST_LIBC
)
if(NOT MOCKTAIL_EXECINFO_IN_HOST_LIBC)
  find_library(MOCKTAIL_EXECINFO_LIBRARY NAMES execinfo REQUIRED)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_LIBRARIES "${MOCKTAIL_EXECINFO_LIBRARY}")
  check_cxx_source_compiles(
    "${_mocktail_execinfo_link_probe}"
    MOCKTAIL_EXECINFO_IN_EXTERNAL_LIBRARY
  )
  cmake_pop_check_state()
  if(NOT MOCKTAIL_EXECINFO_IN_EXTERNAL_LIBRARY)
    message(FATAL_ERROR
      "execinfo.h is available, but backtrace and backtrace_symbols cannot "
      "be linked"
    )
  endif()
  set(MOCKTAIL_EXECINFO_LINK_LIBRARY "${MOCKTAIL_EXECINFO_LIBRARY}")
endif()
unset(_mocktail_execinfo_link_probe)

add_library(mocktail_legacy_runtime STATIC
  # ABI and platform foundations.
  src/legacy/bionic_runtime_wrappers.cc
  src/legacy/host_abi_runtime.cc
  src/legacy/memory_inspection.cc
  src/legacy/runtime_environment.cc
  src/legacy/runtime_paths.cc
  src/legacy/symbol_resolver.cc

  # Headless recovery and diagnostics.
  src/legacy/headless_patches.cc
  src/legacy/headless_signal_handler.cc
  src/legacy/headless_signal_support.cc
  src/legacy/runtime_diagnostics.cc

  # Runtime entry, orchestration, and adapters.
  src/legacy/engine_startup.cc
  src/legacy/legacy_runtime_entry.cc
  src/legacy/legacy_runtime_runner.cc
  src/legacy/main_thread_runtime.cc
  src/legacy/rbxm_diagnostics.cc
  src/legacy/runtime_adapters.cc

  # Stage 6 compatibility and recovery.
  src/legacy/stage6_binary_patches.cc
  src/legacy/stage6_platform_patches.cc
  src/legacy/stage6_rbxm_fallbacks.cc
  src/legacy/stage6_start_lua_fallbacks.cc
  src/legacy/stage6_start_lua_signal_recovery.cc
  src/runtime/roblox_text_input_jni_bridge.cc
)
target_include_directories(mocktail_legacy_runtime PRIVATE include src)
mocktail_apply_compile_options(mocktail_legacy_runtime)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # Diagnostics from signal handlers cannot handle a failed write.
  set_property(SOURCE
    src/legacy/headless_signal_handler.cc
    src/legacy/headless_signal_support.cc
    src/legacy/stage6_start_lua_signal_recovery.cc
    APPEND PROPERTY
    COMPILE_OPTIONS -Wno-unused-result
  )
endif()
target_link_libraries(mocktail_legacy_runtime PRIVATE
  mocktail_legacy_jnivm
  mocktail_compat
  mocktail_runtime
  mocktail_input_runtime
  mocktail_experience_runtime
  mocktail_services
  mocktail_linker
  mocktail_bionic_abi_exports
  libc_shim
  mocktail_window
  mocktail_graphics_foundation
  ${MOCKTAIL_EXECINFO_LINK_LIBRARY}
)
target_compile_definitions(mocktail_legacy_runtime PRIVATE
  "MOCKTAIL_DEFAULT_COMPATIBILITY_MANIFEST=\"${MOCKTAIL_DEFAULT_COMPATIBILITY_MANIFEST}\""
)

add_executable(mocktail src/main.cc)
target_include_directories(mocktail PRIVATE include src)
mocktail_apply_compile_options(mocktail)

target_compile_definitions(mocktail PRIVATE
  "MOCKTAIL_DEFAULT_COMPATIBILITY_MANIFEST=\"${MOCKTAIL_DEFAULT_COMPATIBILITY_MANIFEST}\""
)
target_link_libraries(mocktail PRIVATE
  mocktail_legacy_runtime
  mocktail_auth_runtime_composition
  mocktail_failure_dialog_runtime
  mocktail_fmod_jni_audio_bridge
  mocktail_roblox_output_device_runtime
  mocktail_roblox_fullscreen_runtime
  mocktail_window
  libc_shim
)

# Libraries loaded with dlopen() need the stub symbols.
target_link_options(mocktail PRIVATE
  -Wl,--export-dynamic
)

# Native FreeBSD helper for Linuxulator's unsupported DF socket option.
option(MOCKTAIL_BUILD_FREEBSD_SOCKET_HELPER
  "Build the native FreeBSD Linuxulator socket helper" ON)
if(MOCKTAIL_BUILD_FREEBSD_SOCKET_HELPER)
find_program(MOCKTAIL_LLD_EXECUTABLE NAMES ld.lld REQUIRED)
find_program(MOCKTAIL_ELFEDIT_EXECUTABLE NAMES elfedit REQUIRED)
get_filename_component(MOCKTAIL_LLD_DIRECTORY
  "${MOCKTAIL_LLD_EXECUTABLE}" DIRECTORY)
set(MOCKTAIL_FREEBSD_SOCKET_HELPER
  "${CMAKE_BINARY_DIR}/mocktail_freebsd_socket_helper")
add_custom_command(
  OUTPUT "${MOCKTAIL_FREEBSD_SOCKET_HELPER}"
  COMMAND "${CMAKE_CXX_COMPILER}"
    -x assembler-with-cpp
    -nostdlib
    -static
    -no-pie
    "-B${MOCKTAIL_LLD_DIRECTORY}/"
    -fuse-ld=lld
    -Wl,-e,_start,--build-id=none,-z,noexecstack
    -o "${MOCKTAIL_FREEBSD_SOCKET_HELPER}"
    "${CMAKE_SOURCE_DIR}/src/compat/freebsd_socket_helper.S"
  COMMAND "${MOCKTAIL_ELFEDIT_EXECUTABLE}"
    --output-osabi FreeBSD "${MOCKTAIL_FREEBSD_SOCKET_HELPER}"
  DEPENDS "${CMAKE_SOURCE_DIR}/src/compat/freebsd_socket_helper.S"
  COMMENT "Building native FreeBSD Linuxulator socket helper"
  VERBATIM
)
add_custom_target(mocktail_freebsd_socket_helper_build ALL
  DEPENDS "${MOCKTAIL_FREEBSD_SOCKET_HELPER}"
)
add_dependencies(mocktail mocktail_freebsd_socket_helper_build)
endif()

set_target_properties(mocktail PROPERTIES
  BUILD_RPATH   "\$ORIGIN"
  INSTALL_RPATH "\$ORIGIN;\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/mocktail"
)

set(MOCKTAIL_INSTALL_RUNTIME_DIR "${CMAKE_INSTALL_LIBDIR}/mocktail")
set(MOCKTAIL_INSTALL_DATA_DIR "${CMAKE_INSTALL_DATADIR}/mocktail")

install(TARGETS mocktail
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(TARGETS
    mocktail
    mocktail_updater
    mocktail_failure_dialog
    mocktail_webview_helper
    mocktail_audio_sdl
    mocktail_opensles
    stub_sys_m
    stub_sys_z
    stub_sys_dl
    stub_sys_c
    stub_android
    stub_log
    stub_mediandk
    stub_openmax
    stub_egl
    stub_glesv2
    stub_vulkan
  RUNTIME DESTINATION "${MOCKTAIL_INSTALL_RUNTIME_DIR}"
  LIBRARY DESTINATION "${MOCKTAIL_INSTALL_RUNTIME_DIR}"
)
if(MOCKTAIL_BUILD_FREEBSD_SOCKET_HELPER)
install(PROGRAMS "${MOCKTAIL_FREEBSD_SOCKET_HELPER}"
  DESTINATION "${MOCKTAIL_INSTALL_RUNTIME_DIR}"
)
endif()
install(FILES
    config/roblox_compatibility.json
    config/roblox_bootstrap_sources.json
    config/roblox_host_abi_reference.json
    config/roblox_signing_certificates.json
  DESTINATION "${MOCKTAIL_INSTALL_DATA_DIR}/metadata"
)
install(FILES packaging/space.bigrat.mocktail.desktop
  DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
)
install(FILES packaging/space.bigrat.mocktail.metainfo.xml
  DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo"
)
install(FILES packaging/space.bigrat.mocktail.svg
  DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
)
set(MOCKTAIL_HICOLOR_ICON_SIZES
  16 22 24 32 36 48 64 72 96 128 192 256 512
)
# Generated icons are committed, so ImageMagick is not a build dependency.
foreach(icon_size IN LISTS MOCKTAIL_HICOLOR_ICON_SIZES)
  install(FILES
      "packaging/icons/hicolor/${icon_size}x${icon_size}/apps/space.bigrat.mocktail.png"
    DESTINATION
      "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps"
  )
endforeach()
install(CODE "
  if(NOT DEFINED ENV{DESTDIR} OR \"\$ENV{DESTDIR}\" STREQUAL \"\")
    find_program(MOCKTAIL_GTK_UPDATE_ICON_CACHE gtk-update-icon-cache)
    if(MOCKTAIL_GTK_UPDATE_ICON_CACHE)
      execute_process(
        COMMAND \"\${MOCKTAIL_GTK_UPDATE_ICON_CACHE}\" --force
                --ignore-theme-index
                \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/icons/hicolor\"
        RESULT_VARIABLE MOCKTAIL_ICON_CACHE_RESULT
      )
      if(NOT MOCKTAIL_ICON_CACHE_RESULT EQUAL 0)
        message(WARNING \"Failed to refresh the hicolor icon cache\")
      endif()
    endif()
    find_program(MOCKTAIL_UPDATE_DESKTOP_DATABASE update-desktop-database)
    if(MOCKTAIL_UPDATE_DESKTOP_DATABASE)
      execute_process(
        COMMAND \"\${MOCKTAIL_UPDATE_DESKTOP_DATABASE}\" -q
                \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/applications\"
        RESULT_VARIABLE MOCKTAIL_DESKTOP_DATABASE_RESULT
      )
      if(NOT MOCKTAIL_DESKTOP_DATABASE_RESULT EQUAL 0)
        message(WARNING \"Failed to refresh the desktop MIME cache\")
      endif()
    endif()
  endif()
")

set(MOCKTAIL_PACKAGE_NAME "mocktail" CACHE STRING
  "Native package name"
)
set(MOCKTAIL_PACKAGE_VERSION_SUFFIX "" CACHE STRING
  "Native package version suffix"
)
if(NOT MOCKTAIL_PACKAGE_NAME STREQUAL "mocktail" AND
   NOT MOCKTAIL_PACKAGE_NAME STREQUAL "mocktail-nightly")
  message(FATAL_ERROR
    "MOCKTAIL_PACKAGE_NAME must be mocktail or mocktail-nightly")
endif()

set(CPACK_PACKAGE_NAME "${MOCKTAIL_PACKAGE_NAME}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}${MOCKTAIL_PACKAGE_VERSION_SUFFIX}")
set(CPACK_PACKAGE_VENDOR "Mocktail Project")
set(CPACK_PACKAGE_CONTACT "komaruworld <rtysus228@gmail.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/komaruworld/mocktail")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_DESCRIPTION
  "Runs the Android x86-64 Roblox client on Linux."
)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES TRUE)

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "komaruworld <rtysus228@gmail.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "games")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)

if(MOCKTAIL_PACKAGE_NAME STREQUAL "mocktail-nightly")
  set(CPACK_DEBIAN_PACKAGE_CONFLICTS "mocktail")
  set(CPACK_DEBIAN_PACKAGE_REPLACES "mocktail")
  set(CPACK_RPM_PACKAGE_CONFLICTS "mocktail")
else()
  set(CPACK_DEBIAN_PACKAGE_CONFLICTS "mocktail-nightly")
  set(CPACK_DEBIAN_PACKAGE_REPLACES "mocktail-nightly")
  set(CPACK_RPM_PACKAGE_CONFLICTS "mocktail-nightly")
endif()
include(CPack)

add_dependencies(mocktail all_stubs)
add_dependencies(mocktail mocktail_failure_dialog)
add_dependencies(mocktail mocktail_webview_helper)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
