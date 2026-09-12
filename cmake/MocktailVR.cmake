# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

include_guard(GLOBAL)

option(MOCKTAIL_ENABLE_VR "Build experimental native OpenXR support" OFF)

add_library(mocktail_vr STATIC src/vr/diagnostics.cc)
add_library(Mocktail::VR ALIAS mocktail_vr)
target_include_directories(mocktail_vr PUBLIC "${CMAKE_SOURCE_DIR}/include")
mocktail_apply_compile_options(mocktail_vr)

if(MOCKTAIL_ENABLE_VR)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Mocktail OpenXR support currently targets Linux")
  endif()
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/OpenXR-SDK/CMakeLists.txt")
    message(FATAL_ERROR
      "Initialize OpenXR: git submodule update --init third_party/OpenXR-SDK")
  endif()

  # Scope upstream's generic option names to this dependency. Static linking
  # packages the pinned loader without requiring a matching system loader.
  function(mocktail_add_openxr_sdk)
    set(BUILD_LOADER ON)
    set(DYNAMIC_LOADER OFF)
    set(BUILD_WITH_SYSTEM_JSONCPP OFF)
    set(BUILD_TESTS OFF)
    set(BUILD_API_LAYERS OFF)
    set(BUILD_CONFORMANCE_TESTS OFF)
    set(BUILD_SDK_TESTS OFF)
    set(BUILD_FORCE_GENERATION OFF)
    add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/OpenXR-SDK"
      "${CMAKE_BINARY_DIR}/third_party/OpenXR-SDK" EXCLUDE_FROM_ALL)
  endfunction()
  mocktail_add_openxr_sdk()
  target_sources(mocktail_vr PRIVATE src/vr/openxr_probe.cc src/vr/openxr_preview.cc)
  # Error unwinding is confined to this native session implementation; its
  # public entry point catches errors before returning to the guest runtime.
  set_source_files_properties(src/vr/openxr_preview.cc PROPERTIES
    COMPILE_OPTIONS -fexceptions)
  target_compile_definitions(mocktail_vr PRIVATE XR_USE_GRAPHICS_API_VULKAN)
  target_link_libraries(mocktail_vr PRIVATE OpenXR::openxr_loader Vulkan::Headers ${CMAKE_DL_LIBS})
else()
  target_sources(mocktail_vr PRIVATE src/vr/openxr_disabled.cc)
endif()

add_executable(mocktail_vr_probe src/vr/probe_main.cc)
set_target_properties(mocktail_vr_probe PROPERTIES OUTPUT_NAME mocktail-vr-probe)
target_link_libraries(mocktail_vr_probe PRIVATE Mocktail::VR)
mocktail_apply_compile_options(mocktail_vr_probe)
install(TARGETS mocktail_vr_probe RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# Development launcher for an independently built Monado runtime. --prepare
# keeps its shared library and a relative manifest under this build directory.
configure_file("${CMAKE_SOURCE_DIR}/scripts/vr_simulated.py"
  "${CMAKE_BINARY_DIR}/mocktail-vr-simulated" COPYONLY)
file(CHMOD "${CMAKE_BINARY_DIR}/mocktail-vr-simulated"
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE)
