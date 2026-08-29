# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

include_guard(GLOBAL)

get_filename_component(MOCKTAIL_PLATFORM_GRAPHICS_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE
)

find_package(SDL3 3.4 REQUIRED CONFIG)
find_path(MOCKTAIL_EGL_INCLUDE_DIR EGL/egl.h REQUIRED)

set(MOCKTAIL_WINDOW_ICON_PNG
  "${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/packaging/icons/hicolor/48x48/apps/space.bigrat.mocktail.png"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${MOCKTAIL_WINDOW_ICON_PNG}"
)
file(READ "${MOCKTAIL_WINDOW_ICON_PNG}" MOCKTAIL_WINDOW_ICON_HEX HEX)
string(LENGTH "${MOCKTAIL_WINDOW_ICON_HEX}" MOCKTAIL_WINDOW_ICON_HEX_LENGTH)
set(MOCKTAIL_WINDOW_ICON_BYTES "")
set(MOCKTAIL_WINDOW_ICON_HEX_OFFSET 0)
set(MOCKTAIL_WINDOW_ICON_COLUMN 0)
while(MOCKTAIL_WINDOW_ICON_HEX_OFFSET LESS MOCKTAIL_WINDOW_ICON_HEX_LENGTH)
  string(SUBSTRING "${MOCKTAIL_WINDOW_ICON_HEX}"
    ${MOCKTAIL_WINDOW_ICON_HEX_OFFSET} 2 MOCKTAIL_WINDOW_ICON_BYTE
  )
  string(APPEND MOCKTAIL_WINDOW_ICON_BYTES
    "0x${MOCKTAIL_WINDOW_ICON_BYTE}, "
  )
  math(EXPR MOCKTAIL_WINDOW_ICON_HEX_OFFSET
    "${MOCKTAIL_WINDOW_ICON_HEX_OFFSET} + 2"
  )
  math(EXPR MOCKTAIL_WINDOW_ICON_COLUMN
    "${MOCKTAIL_WINDOW_ICON_COLUMN} + 1"
  )
  if(MOCKTAIL_WINDOW_ICON_COLUMN EQUAL 12)
    string(APPEND MOCKTAIL_WINDOW_ICON_BYTES "\n    ")
    set(MOCKTAIL_WINDOW_ICON_COLUMN 0)
  endif()
endwhile()

set(MOCKTAIL_PLATFORM_GENERATED_INCLUDE_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
file(MAKE_DIRECTORY
  "${MOCKTAIL_PLATFORM_GENERATED_INCLUDE_DIR}/mocktail/platform"
)
configure_file(
  "${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/cmake/templates/sdl_window_icon_data.h.in"
  "${MOCKTAIL_PLATFORM_GENERATED_INCLUDE_DIR}/mocktail/platform/sdl_window_icon_data.h"
  @ONLY
)

set(MOCKTAIL_ANGLE_HEADERS_INCLUDE_DIR
  "${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/third_party/angle_headers/include"
)
if(NOT EXISTS
    "${MOCKTAIL_ANGLE_HEADERS_INCLUDE_DIR}/EGL/eglext_angle.h")
  message(FATAL_ERROR "pinned ANGLE EGL extension header is unavailable")
endif()
add_library(mocktail_angle_headers INTERFACE)
target_include_directories(mocktail_angle_headers SYSTEM INTERFACE
  "${MOCKTAIL_ANGLE_HEADERS_INCLUDE_DIR}"
)
add_library(Mocktail::AngleHeaders ALIAS mocktail_angle_headers)

add_library(mocktail_platform_sdl STATIC
  ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/src/platform/sdl_application_metadata.cc
  ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/src/platform/sdl_display_refresh_capabilities.cc
  ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/src/platform/sdl_event_converter.cc
  ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/src/platform/sdl_platform_runtime.cc
  ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/src/platform/sdl_text_clipboard.cc
  ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/src/platform/sdl_window_icon.cc
)
target_include_directories(mocktail_platform_sdl PUBLIC
  ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/include
)
target_include_directories(mocktail_platform_sdl PRIVATE
  ${MOCKTAIL_PLATFORM_GENERATED_INCLUDE_DIR}
)
target_link_libraries(mocktail_platform_sdl PUBLIC SDL3::SDL3)
target_compile_features(mocktail_platform_sdl PUBLIC cxx_std_17)
target_compile_definitions(mocktail_platform_sdl PRIVATE
  MOCKTAIL_PROJECT_VERSION="${PROJECT_VERSION}"
)
add_library(Mocktail::PlatformSdl ALIAS mocktail_platform_sdl)

add_library(mocktail_graphics_foundation STATIC
  ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/src/graphics/graphics_backend.cc
  ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/src/graphics/angle_probe.cc
  ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/src/graphics/bionic_egl_bridge.cc
  ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/src/graphics/system_egl_probe.cc
)
target_include_directories(mocktail_graphics_foundation
  PUBLIC ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/include
  PRIVATE ${MOCKTAIL_EGL_INCLUDE_DIR}
)
target_link_libraries(mocktail_graphics_foundation PRIVATE
  Mocktail::AngleHeaders
  SDL3::SDL3
  ${CMAKE_DL_LIBS}
)
target_compile_features(mocktail_graphics_foundation PUBLIC cxx_std_17)
add_library(Mocktail::GraphicsFoundation ALIAS mocktail_graphics_foundation)

add_library(mocktail_sdl_vulkan_wsi STATIC
  ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/src/graphics/sdl_vulkan_wsi.cc
  ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/src/graphics/android_vulkan_wsi_adapter.cc
  ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/src/graphics/present_mode_policy.cc
)
set_target_properties(mocktail_sdl_vulkan_wsi PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(mocktail_sdl_vulkan_wsi PUBLIC
  ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/include
)
target_link_libraries(mocktail_sdl_vulkan_wsi PUBLIC SDL3::SDL3)
target_link_libraries(mocktail_sdl_vulkan_wsi PUBLIC Vulkan::Headers)
target_compile_features(mocktail_sdl_vulkan_wsi PUBLIC cxx_std_17)
add_library(Mocktail::SdlVulkanWsi ALIAS mocktail_sdl_vulkan_wsi)

if(BUILD_TESTING AND TARGET GTest::gtest_main)
  add_executable(bionic_egl_bridge_test
    ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/tests/bionic_egl_bridge_test.cc
  )
  target_link_libraries(bionic_egl_bridge_test PRIVATE
    Mocktail::GraphicsFoundation
    GTest::gtest_main
  )
  add_dependencies(bionic_egl_bridge_test stub_egl)

  add_executable(platform_graphics_foundation_test
    ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/tests/platform_graphics_foundation_test.cc
  )
  add_executable(system_egl_probe_test
    ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/tests/system_egl_probe_test.cc
  )
  target_link_libraries(system_egl_probe_test PRIVATE
    Mocktail::GraphicsFoundation
    SDL3::SDL3
    GTest::gtest_main
  )
  add_executable(display_refresh_capabilities_test
    ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/tests/display_refresh_capabilities_test.cc
  )
  add_executable(present_mode_policy_test
    ${MOCKTAIL_PLATFORM_GRAPHICS_ROOT}/tests/present_mode_policy_test.cc
  )
  target_link_libraries(present_mode_policy_test PRIVATE
    Mocktail::SdlVulkanWsi
    GTest::gtest_main
  )
  target_link_libraries(display_refresh_capabilities_test PRIVATE
    Mocktail::PlatformSdl
    GTest::gtest_main
  )
  target_link_libraries(platform_graphics_foundation_test PRIVATE
    Mocktail::PlatformSdl
    Mocktail::GraphicsFoundation
    Mocktail::SdlVulkanWsi
    GTest::gtest_main
  )
  include(GoogleTest)
  gtest_discover_tests(bionic_egl_bridge_test)
  gtest_discover_tests(platform_graphics_foundation_test)
  gtest_discover_tests(system_egl_probe_test)
  gtest_discover_tests(display_refresh_capabilities_test)
  gtest_discover_tests(present_mode_policy_test)
endif()
