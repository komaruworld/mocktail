# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

function(configure_stub TARGET SONAME)
  string(REGEX REPLACE "^lib" "" _base "${SONAME}")
  string(REGEX REPLACE "\\.so$" "" _base "${_base}")

  set_target_properties(${TARGET} PROPERTIES
    OUTPUT_NAME              "${_base}"
    PREFIX                   "lib"
    SUFFIX                   ".so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )

  target_link_options(${TARGET} PRIVATE "-Wl,-soname,${SONAME}")

  target_compile_options(${TARGET} PRIVATE -Wno-unused-parameter)
endfunction()

foreach(_pair
  "libm.so|m"
  "libz.so|z"
  "libdl.so|dl"
)
  string(REPLACE "|" ";" _parts "${_pair}")
  list(GET _parts 0 _soname)
  list(GET _parts 1 _link)

  string(REGEX REPLACE "[^a-z0-9]" "_" _target "stub_sys_${_link}")
  add_library(${_target} SHARED empty.cc)
  configure_stub(${_target} ${_soname})
  target_link_options(${_target} PRIVATE "LINKER:--no-as-needed")
  target_link_libraries(${_target} PRIVATE -l${_link})
endforeach()

# GCC links libc implicitly.
add_library(stub_sys_c SHARED empty.cc)
configure_stub(stub_sys_c "libc.so")
target_link_options(stub_sys_c PRIVATE "LINKER:--no-as-needed")

add_library(stub_android SHARED
  libandroid_stub.cc
  ${CMAKE_SOURCE_DIR}/src/libc_shim/vulkan_etc1_sky_transcoder.cc
)
configure_stub(stub_android "libandroid.so")
target_include_directories(stub_android PUBLIC ${CMAKE_SOURCE_DIR}/include)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MINIZIP REQUIRED IMPORTED_TARGET minizip)
target_link_libraries(stub_android PRIVATE PkgConfig::MINIZIP)

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_INCLUDES ${MINIZIP_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES PkgConfig::MINIZIP)
check_cxx_source_compiles("#include <minizip/unzip.h>
int main() {
  unzFile file = nullptr;
  return unzGetCurrentFileZStreamPos64(file) == 0;
}"
  MOCKTAIL_MINIZIP_HAS_ZSTREAM_POS)
check_cxx_source_compiles("#include <minizip/unzip.h>
#include <minizip/mz_strm.h>
int main() {
  unzFile file = nullptr;
  return mz_stream_tell(unzGetStream(file)) < 0;
}"
  MOCKTAIL_MINIZIP_HAS_STREAM_TELL)
cmake_pop_check_state()

if(MOCKTAIL_MINIZIP_HAS_ZSTREAM_POS)
  target_compile_definitions(stub_android PRIVATE
    MOCKTAIL_MINIZIP_HAS_ZSTREAM_POS=1)
elseif(MOCKTAIL_MINIZIP_HAS_STREAM_TELL)
  target_compile_definitions(stub_android PRIVATE
    MOCKTAIL_MINIZIP_HAS_STREAM_TELL=1)
else()
  message(FATAL_ERROR "The installed minizip cannot report ZIP entry offsets")
endif()

add_library(stub_log SHARED liblog_stub.cc)
configure_stub(stub_log "liblog.so")
target_include_directories(stub_log PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(stub_mediandk SHARED libmediandk_stub.cc)
configure_stub(stub_mediandk "libmediandk.so")

add_library(stub_openmax SHARED libopensl_stub.cc)
configure_stub(stub_openmax "libOpenMAXAL.so")

# Fake EGL handles require MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS=1.
add_library(stub_egl SHARED libegl_stub.cc)
configure_stub(stub_egl "libEGL.so")
target_link_libraries(stub_egl PRIVATE dl)

# Production uses ANGLE or the system GLES library.
add_library(stub_glesv2 SHARED libgles_stub.cc)
configure_stub(stub_glesv2 "libGLESv2.so")
target_link_libraries(stub_glesv2 PRIVATE dl)

# The host Vulkan loader owns device commands; this target adapts Android WSI.
add_library(stub_vulkan SHARED
  ${CMAKE_SOURCE_DIR}/src/graphics/bionic_vulkan_loader_adapter.cc
  ${CMAKE_SOURCE_DIR}/src/graphics/vulkan_text_overlay_compositor.cc
)
configure_stub(stub_vulkan "libvulkan.so")
target_include_directories(stub_vulkan PRIVATE ${CMAKE_SOURCE_DIR}/include)
pkg_check_modules(LIBPLACEBO REQUIRED IMPORTED_TARGET libplacebo)
target_link_libraries(stub_vulkan PRIVATE
  Mocktail::SdlVulkanWsi
  PkgConfig::LIBPLACEBO
  dl
)

add_custom_target(all_stubs DEPENDS
  stub_sys_m stub_sys_z stub_sys_dl stub_sys_c
  stub_android stub_log stub_mediandk mocktail_opensles stub_openmax
  stub_egl stub_glesv2 stub_vulkan
)
