# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License")

add_executable(jnivm_test
  jnivm_test.cc
)
target_link_libraries(jnivm_test PRIVATE
  Mocktail::LegacyJni
  GTest::gtest_main
)

add_executable(runtime_config_test runtime_config_test.cc)
target_link_libraries(runtime_config_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)

add_executable(runtime_config_file_test runtime_config_file_test.cc)
target_link_libraries(runtime_config_file_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)
target_compile_definitions(runtime_config_file_test PRIVATE
  MOCKTAIL_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_executable(discord_rpc_test discord_rpc_test.cc)
target_link_libraries(discord_rpc_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)

add_executable(native_update_helper_fixture native_update_helper_fixture.cc)
add_executable(native_progress_helper_fixture native_progress_helper_fixture.cc)
mocktail_apply_compile_options(native_update_helper_fixture)
mocktail_apply_compile_options(native_progress_helper_fixture)

add_executable(payload_update_preflight_test payload_update_preflight_test.cc)
target_link_libraries(payload_update_preflight_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)
target_compile_definitions(payload_update_preflight_test PRIVATE
  "MOCKTAIL_TEST_UPDATE_HELPER=\"$<TARGET_FILE:native_update_helper_fixture>\""
  "MOCKTAIL_TEST_PROGRESS_HELPER=\"$<TARGET_FILE:native_progress_helper_fixture>\""
)
add_dependencies(payload_update_preflight_test
  native_update_helper_fixture
  native_progress_helper_fixture
)

add_executable(native_update_test native_update_test.cc)
target_link_libraries(native_update_test PRIVATE
  Mocktail::Update
  GTest::gtest_main
)

add_executable(apk_provider_test apk_provider_test.cc)
target_link_libraries(apk_provider_test PRIVATE
  Mocktail::Update
  GTest::gtest_main
)

add_executable(support_bundle_runtime_test support_bundle_runtime_test.cc)
target_link_libraries(support_bundle_runtime_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)

add_executable(session_log_test session_log_test.cc)
target_link_libraries(session_log_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)

add_executable(failure_dialog_test failure_dialog_test.cc)
target_link_libraries(failure_dialog_test PRIVATE
  Mocktail::FailureDialog
  GTest::gtest_main
)

add_executable(frame_rate_policy_test frame_rate_policy_test.cc)
target_link_libraries(frame_rate_policy_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)

add_executable(performance_policy_test performance_policy_test.cc)
target_link_libraries(performance_policy_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)

add_executable(http_client_policy_test http_client_policy_test.cc)
target_link_libraries(http_client_policy_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)

add_executable(http_client_spin_guard_test http_client_spin_guard_test.cc)
target_link_libraries(http_client_spin_guard_test PRIVATE
  mocktail_bionic_abi_exports
  GTest::gtest_main
)

add_executable(crash_report_policy_test crash_report_policy_test.cc)
target_link_libraries(crash_report_policy_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)

add_executable(game_mode_test game_mode_test.cc)
target_link_libraries(game_mode_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)

add_library(game_mode_namespace_fixture SHARED
  game_mode_namespace_fixture.cc
)
set_target_properties(game_mode_namespace_fixture PROPERTIES
  OUTPUT_NAME gamemode
  VERSION 0.0.0
  SOVERSION 0
  LIBRARY_OUTPUT_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/game_mode_namespace_fixture"
)
target_link_libraries(game_mode_namespace_fixture PRIVATE ${CMAKE_DL_LIBS})

add_executable(game_mode_link_namespace_test
  game_mode_link_namespace_test.cc
)
target_link_libraries(game_mode_link_namespace_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)
target_link_options(game_mode_link_namespace_test PRIVATE
  -Wl,--export-dynamic
)
add_dependencies(game_mode_link_namespace_test game_mode_namespace_fixture)

add_executable(memory_limit_test memory_limit_test.cc)
target_link_libraries(memory_limit_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(memory_limit_test)

add_executable(runtime_paths_test runtime_paths_test.cc)
target_link_libraries(runtime_paths_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)

add_executable(single_instance_lock_test single_instance_lock_test.cc)
target_link_libraries(single_instance_lock_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)

add_executable(external_launch_broker_test external_launch_broker_test.cc)
target_link_libraries(external_launch_broker_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(external_launch_broker_test)

add_executable(supported_launch_policy_test supported_launch_policy_test.cc)
target_link_libraries(supported_launch_policy_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)

add_executable(platform_cache_migration_test
  platform_cache_migration_test.cc
)
target_link_libraries(platform_cache_migration_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)

add_executable(roblox_desktop_app_policy_test
  roblox_desktop_app_policy_test.cc
)
target_link_libraries(roblox_desktop_app_policy_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)
mocktail_apply_compile_options(roblox_desktop_app_policy_test)

add_executable(browser_tracker_service_test browser_tracker_service_test.cc)
target_link_libraries(browser_tracker_service_test PRIVATE
  Mocktail::Services
  GTest::gtest_main
)
mocktail_apply_compile_options(browser_tracker_service_test)

add_executable(command_line_test command_line_test.cc)
target_link_libraries(command_line_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)
mocktail_apply_compile_options(command_line_test)

add_executable(roblox_launch_uri_test roblox_launch_uri_test.cc)
target_link_libraries(roblox_launch_uri_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)
mocktail_apply_compile_options(roblox_launch_uri_test)

add_executable(webview_helper_launcher_test webview_helper_launcher_test.cc)
target_link_libraries(webview_helper_launcher_test PRIVATE
  Mocktail::WebViewLauncher
  GTest::gtest_main
)
mocktail_apply_compile_options(webview_helper_launcher_test)

add_executable(webview_helper_policy_test webview_helper_policy_test.cc)
target_link_libraries(webview_helper_policy_test PRIVATE
  Mocktail::WebViewHelperPolicy
  GTest::gtest_main
)
mocktail_apply_compile_options(webview_helper_policy_test)

add_executable(webview_roblox_cookie_test webview_roblox_cookie_test.cc)
target_link_libraries(webview_roblox_cookie_test PRIVATE
  Mocktail::AuthRuntimeComposition
  GTest::gtest_main
)
mocktail_apply_compile_options(webview_roblox_cookie_test)

add_executable(startup_state_machine_test startup_state_machine_test.cc)
target_link_libraries(startup_state_machine_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
  Threads::Threads
)

add_executable(game_session_coordinator_test
  game_session_coordinator_test.cc
)
target_link_libraries(game_session_coordinator_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(game_session_coordinator_test)

add_executable(owned_pthread_test owned_pthread_test.cc)
target_link_libraries(owned_pthread_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(owned_pthread_test)

add_executable(roblox_capability_resolver_test
  roblox_capability_resolver_test.cc
)
target_link_libraries(roblox_capability_resolver_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)

add_executable(roblox_app_lifecycle_test
  roblox_app_lifecycle_test.cc
)
target_link_libraries(roblox_app_lifecycle_test PRIVATE
  Mocktail::Runtime
  GTest::gtest_main
)

add_executable(roblox_game_session_native_adapter_test
  roblox_game_session_native_adapter_test.cc
)
target_link_libraries(roblox_game_session_native_adapter_test PRIVATE
  Mocktail::LegacyJni
  Mocktail::Runtime
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(roblox_game_session_native_adapter_test)

add_executable(roblox_experience_launch_bridge_test
  roblox_experience_launch_bridge_test.cc
)
target_link_libraries(roblox_experience_launch_bridge_test PRIVATE
  Mocktail::LegacyJni
  Mocktail::Runtime
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(roblox_experience_launch_bridge_test)

add_executable(roblox_web_view_bridge_test
  roblox_web_view_bridge_test.cc
)
target_link_libraries(roblox_web_view_bridge_test PRIVATE
  Mocktail::LegacyJni
  Mocktail::Runtime
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(roblox_web_view_bridge_test)

add_executable(roblox_browser_service_bridge_test
  roblox_browser_service_bridge_test.cc
)
target_link_libraries(roblox_browser_service_bridge_test PRIVATE
  Mocktail::LegacyJni
  Mocktail::Runtime
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(roblox_browser_service_bridge_test)

add_executable(roblox_fresh_game_launch_controller_test
  roblox_fresh_game_launch_controller_test.cc
)
target_link_libraries(roblox_fresh_game_launch_controller_test PRIVATE
  Mocktail::ExperienceRuntime
  Mocktail::LegacyJni
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(roblox_fresh_game_launch_controller_test)

add_executable(roblox_experience_composition_test
  roblox_experience_composition_test.cc
)
target_link_libraries(roblox_experience_composition_test PRIVATE
  Mocktail::ExperienceRuntime
  Mocktail::LegacyJni
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(roblox_experience_composition_test)

add_executable(jnivm_message_bus_test
  jnivm_message_bus_test.cc
)
target_link_libraries(jnivm_message_bus_test PRIVATE
  Mocktail::LegacyJni
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(jnivm_message_bus_test)

add_executable(jnivm_experience_lifecycle_test
  jnivm_experience_lifecycle_test.cc
)
target_link_libraries(jnivm_experience_lifecycle_test PRIVATE
  Mocktail::LegacyJni
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(jnivm_experience_lifecycle_test)

add_executable(jnivm_native_text_box_info_test
  jnivm_native_text_box_info_test.cc
)
target_link_libraries(jnivm_native_text_box_info_test PRIVATE
  Mocktail::LegacyJni
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(jnivm_native_text_box_info_test)

add_executable(vulkan_present_observer_test
  vulkan_present_observer_test.cc
)
target_link_libraries(vulkan_present_observer_test PRIVATE
  mocktail_window
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(vulkan_present_observer_test)

add_executable(input_pump_pacer_test
  input_pump_pacer_test.cc
)
target_link_libraries(input_pump_pacer_test PRIVATE
  mocktail_window
  GTest::gtest_main
)
mocktail_apply_compile_options(input_pump_pacer_test)

add_executable(vulkan_surface_recovery_gate_test
  vulkan_surface_recovery_gate_test.cc
)
target_link_libraries(vulkan_surface_recovery_gate_test PRIVATE
  mocktail_window
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(vulkan_surface_recovery_gate_test)

add_executable(vulkan_present_progress_gate_test
  vulkan_present_progress_gate_test.cc
)
target_link_libraries(vulkan_present_progress_gate_test PRIVATE
  mocktail_window
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(vulkan_present_progress_gate_test)

if(NOT TARGET PkgConfig::LIBPLACEBO)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBPLACEBO REQUIRED IMPORTED_TARGET libplacebo)
endif()
add_executable(vulkan_text_overlay_compositor_test
  vulkan_text_overlay_compositor_test.cc
  ${PROJECT_SOURCE_DIR}/src/graphics/vulkan_text_overlay_compositor.cc
)
target_link_libraries(vulkan_text_overlay_compositor_test PRIVATE
  Mocktail::SdlVulkanWsi
  PkgConfig::LIBPLACEBO
  GTest::gtest_main
  ${CMAKE_DL_LIBS}
)
mocktail_apply_compile_options(vulkan_text_overlay_compositor_test)

add_executable(video_driver_policy_test
  video_driver_policy_test.cc
)
target_link_libraries(video_driver_policy_test PRIVATE
  mocktail_window
  GTest::gtest_main
)
mocktail_apply_compile_options(video_driver_policy_test)

add_executable(window_surface_lifecycle_test
  window_surface_lifecycle_test.cc
)
target_link_libraries(window_surface_lifecycle_test PRIVATE
  mocktail_window
  GTest::gtest_main
)
mocktail_apply_compile_options(window_surface_lifecycle_test)

add_executable(window_creation_policy_test
  window_creation_policy_test.cc
)
target_link_libraries(window_creation_policy_test PRIVATE
  mocktail_window
  GTest::gtest_main
)
mocktail_apply_compile_options(window_creation_policy_test)

add_executable(window_game_surface_bridge_test
  window_game_surface_bridge_test.cc
)
target_link_libraries(window_game_surface_bridge_test PRIVATE
  mocktail_window
  GTest::gtest_main
)
mocktail_apply_compile_options(window_game_surface_bridge_test)

add_executable(window_resize_readiness_gate_test
  window_resize_readiness_gate_test.cc
)
target_link_libraries(window_resize_readiness_gate_test PRIVATE
  mocktail_window
  GTest::gtest_main
)
mocktail_apply_compile_options(window_resize_readiness_gate_test)

add_executable(window_fullscreen_request_gate_test
  window_fullscreen_request_gate_test.cc
)
target_link_libraries(window_fullscreen_request_gate_test PRIVATE
  mocktail_window
  GTest::gtest_main
)
mocktail_apply_compile_options(window_fullscreen_request_gate_test)

add_executable(window_state_store_test
  window_state_store_test.cc
)
target_link_libraries(window_state_store_test PRIVATE
  mocktail_window
  GTest::gtest_main
)
mocktail_apply_compile_options(window_state_store_test)

add_executable(roblox_fullscreen_menu_request_gate_test
  roblox_fullscreen_menu_request_gate_test.cc
)
target_link_libraries(roblox_fullscreen_menu_request_gate_test PRIVATE
  mocktail_window
  GTest::gtest_main
)
mocktail_apply_compile_options(roblox_fullscreen_menu_request_gate_test)

add_executable(roblox_fullscreen_runtime_bridge_test
  roblox_fullscreen_runtime_bridge_test.cc
)
target_link_libraries(roblox_fullscreen_runtime_bridge_test PRIVATE
  mocktail_roblox_fullscreen_runtime
  GTest::gtest_main
)
target_include_directories(roblox_fullscreen_runtime_bridge_test PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)
mocktail_apply_compile_options(roblox_fullscreen_runtime_bridge_test)

add_executable(roblox_output_device_bridge_test
  roblox_output_device_bridge_test.cc
)
target_link_libraries(roblox_output_device_bridge_test PRIVATE
  mocktail_roblox_output_device_runtime
  GTest::gtest_main
)
target_include_directories(roblox_output_device_bridge_test PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)
mocktail_apply_compile_options(roblox_output_device_bridge_test)

add_executable(window_text_input_owner_test
  window_text_input_owner_test.cc
)
target_link_libraries(window_text_input_owner_test PRIVATE
  mocktail_window
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(window_text_input_owner_test)

add_executable(window_pointer_capture_owner_test
  window_pointer_capture_owner_test.cc
)
target_link_libraries(window_pointer_capture_owner_test PRIVATE
  mocktail_window
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(window_pointer_capture_owner_test)

add_executable(roblox_text_input_jni_bridge_test
  roblox_text_input_jni_bridge_test.cc
)
target_link_libraries(roblox_text_input_jni_bridge_test PRIVATE
  Mocktail::LegacyJni
  mocktail_legacy_runtime
  GTest::gtest_main
  Threads::Threads
)
target_include_directories(roblox_text_input_jni_bridge_test PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)
mocktail_apply_compile_options(roblox_text_input_jni_bridge_test)

add_executable(auth_service_test auth_service_test.cc)
target_link_libraries(auth_service_test PRIVATE
  Mocktail::Services
  GTest::gtest_main
)

add_executable(sha256_test sha256_test.cc)
target_link_libraries(sha256_test PRIVATE
  Mocktail::CryptoFoundation
  GTest::gtest_main
)
mocktail_apply_compile_options(sha256_test)

add_executable(auth_runtime_composition_test
  auth_runtime_composition_test.cc
)
target_link_libraries(auth_runtime_composition_test PRIVATE
  Mocktail::AuthRuntimeComposition
  GTest::gtest_main
)
mocktail_apply_compile_options(auth_runtime_composition_test)

add_executable(client_settings_service_test client_settings_service_test.cc)
target_link_libraries(client_settings_service_test PRIVATE
  Mocktail::Services
  GTest::gtest_main
)

add_executable(elf_build_id_test elf_build_id_test.cc)
target_link_libraries(elf_build_id_test PRIVATE
  Mocktail::Compat
  GTest::gtest_main
)

add_executable(build_profile_test build_profile_test.cc)
target_link_libraries(build_profile_test PRIVATE
  Mocktail::Compat
  GTest::gtest_main
)
target_compile_definitions(build_profile_test PRIVATE
  MOCKTAIL_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_library(host_abi_profile_fixture SHARED host_abi_profile_fixture.cc)
set_target_properties(host_abi_profile_fixture PROPERTIES
  CXX_VISIBILITY_PRESET default
  VISIBILITY_INLINES_HIDDEN OFF
)
mocktail_apply_compile_options(host_abi_profile_fixture)

add_executable(host_abi_profile_test host_abi_profile_test.cc)
target_link_libraries(host_abi_profile_test PRIVATE
  Mocktail::Compat
  GTest::gtest_main
  ${CMAKE_DL_LIBS}
)
target_compile_definitions(host_abi_profile_test PRIVATE
  MOCKTAIL_HOST_ABI_FIXTURE_PATH="$<TARGET_FILE:host_abi_profile_fixture>"
)
add_dependencies(host_abi_profile_test host_abi_profile_fixture)

add_executable(payload_compatibility_host_abi_gate_test
  payload_compatibility_host_abi_gate_test.cc
)
target_link_libraries(payload_compatibility_host_abi_gate_test PRIVATE
  Mocktail::Compat
  GTest::gtest_main
)
target_compile_definitions(payload_compatibility_host_abi_gate_test PRIVATE
  MOCKTAIL_HOST_ABI_FIXTURE_PATH="$<TARGET_FILE:host_abi_profile_fixture>"
)
add_dependencies(payload_compatibility_host_abi_gate_test
  host_abi_profile_fixture
)

add_executable(host_abi_experiment_test host_abi_experiment_test.cc)
target_link_libraries(host_abi_experiment_test PRIVATE
  Mocktail::Compat
  GTest::gtest_main
)

add_executable(host_allocator_bridge_test host_allocator_bridge_test.cc)
target_link_libraries(host_allocator_bridge_test PRIVATE
  Mocktail::Compat
  GTest::gtest_main
)

add_executable(bionic_pthread_key_runtime_test
  bionic_pthread_key_runtime_test.cc
)
target_link_libraries(bionic_pthread_key_runtime_test PRIVATE
  Mocktail::Compat
  GTest::gtest_main
  Threads::Threads
)

add_executable(bionic_atfork_runtime_test
  bionic_atfork_runtime_test.cc
)
target_link_libraries(bionic_atfork_runtime_test PRIVATE
  Mocktail::Compat
  GTest::gtest_main
  Threads::Threads
)

add_executable(bionic_dns_runtime_test
  bionic_dns_runtime_test.cc
)
target_link_libraries(bionic_dns_runtime_test PRIVATE
  Mocktail::Compat
  GTest::gtest_main
)

add_executable(bionic_host_libc_runtime_test
  bionic_host_libc_runtime_test.cc
)
target_link_libraries(bionic_host_libc_runtime_test PRIVATE
  Mocktail::Compat
  libc_shim
  GTest::gtest_main
  Threads::Threads
)

add_executable(bionic_large_file_runtime_test
  bionic_large_file_runtime_test.cc
)
target_link_libraries(bionic_large_file_runtime_test PRIVATE
  Mocktail::Compat
  GTest::gtest_main
)

add_executable(bionic_pthread_create_runtime_test
  bionic_pthread_create_runtime_test.cc
)
target_link_libraries(bionic_pthread_create_runtime_test PRIVATE
  Mocktail::Compat
  GTest::gtest_main
  Threads::Threads
)
target_link_options(bionic_pthread_create_runtime_test PRIVATE
  "-Wl,--wrap=pthread_create"
)

add_executable(bionic_sysconf_test bionic_sysconf_test.cc)
target_link_libraries(bionic_sysconf_test PRIVATE
  Mocktail::Compat
  GTest::gtest_main
)

add_executable(bionic_signal_runtime_test bionic_signal_runtime_test.cc)
target_link_libraries(bionic_signal_runtime_test PRIVATE
  Mocktail::Compat
  GTest::gtest_main
)

add_executable(bionic_semaphore_runtime_test
  bionic_semaphore_runtime_test.cc
)

add_executable(bionic_rwlock_runtime_test bionic_rwlock_runtime_test.cc)
target_link_libraries(bionic_rwlock_runtime_test PRIVATE
  Mocktail::Compat
  GTest::gtest_main
  Threads::Threads
)
target_link_libraries(bionic_semaphore_runtime_test PRIVATE
  Mocktail::Compat
  GTest::gtest_main
  Threads::Threads
)

add_executable(bionic_abi_exports_test bionic_abi_exports_test.cc)
target_link_libraries(bionic_abi_exports_test PRIVATE
  mocktail_bionic_abi_exports
  GTest::gtest_main
  Threads::Threads
)
mocktail_apply_compile_options(bionic_abi_exports_test)

add_executable(bionic_stdio_runtime_test bionic_stdio_runtime_test.cc)
target_link_libraries(bionic_stdio_runtime_test PRIVATE
  Mocktail::BionicStdio
  GTest::gtest_main
)
mocktail_apply_compile_options(bionic_stdio_runtime_test)

# A tiny ELF with a valid, unknown GNU Build ID. Its constructor
# aborts, proving the compatibility gate exits before any native loading.
add_library(unknown_roblox_fixture SHARED
  fixtures/unknown_roblox_fixture.cc
)
set_target_properties(unknown_roblox_fixture PROPERTIES
  PREFIX ""
  OUTPUT_NAME "unknown_roblox_fixture"
)
target_link_options(unknown_roblox_fixture PRIVATE -Wl,--build-id=sha1)
add_test(
  NAME CompatibilityGateTest.UnknownBuildFailsBeforeNativeLoad
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/compatibility_gate_test.sh
          $<TARGET_FILE:mocktail>
          $<TARGET_FILE:unknown_roblox_fixture>
          ${PROJECT_SOURCE_DIR}/config/roblox_compatibility.json
)
set_tests_properties(
  CompatibilityGateTest.UnknownBuildFailsBeforeNativeLoad
  PROPERTIES RESOURCE_LOCK mocktail_current_user_instance
)

if(MOCKTAIL_ENABLE_UPSTREAM_JNIVM)
  add_library(upstream_jni_fixture SHARED
    fixtures/upstream_jni_fixture.cc
  )
  target_include_directories(upstream_jni_fixture PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/libjnivm/include
  )
  set_target_properties(upstream_jni_fixture PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
  )

  add_executable(upstream_jnivm_integration_test
    upstream_jnivm_integration_test.cc
  )
  target_link_libraries(upstream_jnivm_integration_test PRIVATE
    Mocktail::UpstreamJni
    GTest::gtest_main
    Threads::Threads
    ${CMAKE_DL_LIBS}
  )
  target_compile_definitions(upstream_jnivm_integration_test PRIVATE
    MOCKTAIL_UPSTREAM_JNI_FIXTURE_PATH="$<TARGET_FILE:upstream_jni_fixture>"
  )
  add_dependencies(upstream_jnivm_integration_test upstream_jni_fixture)
endif()

add_executable(libc_shim_test
  libc_shim_test.cc
)
target_link_libraries(libc_shim_test PRIVATE
  libc_shim
  GTest::gtest_main
)

add_executable(asset_manager_test
  asset_manager_test.cc
)
target_link_libraries(asset_manager_test PRIVATE
  stub_android
  GTest::gtest_main
)

add_executable(android_looper_test
  android_looper_test.cc
)
target_link_libraries(android_looper_test PRIVATE
  stub_android
  GTest::gtest_main
  Threads::Threads
)

add_executable(android_native_window_test
  android_native_window_test.cc
)
target_link_libraries(android_native_window_test PRIVATE
  stub_android
  GTest::gtest_main
)

add_executable(headless_graphics_test
  headless_graphics_test.cc
)
target_link_libraries(headless_graphics_test PRIVATE
  stub_egl
  stub_glesv2
  GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(jnivm_test)
gtest_discover_tests(runtime_config_test)
gtest_discover_tests(runtime_config_file_test)
gtest_discover_tests(payload_update_preflight_test)
gtest_discover_tests(apk_provider_test)
gtest_discover_tests(native_update_test)
gtest_discover_tests(support_bundle_runtime_test)
gtest_discover_tests(session_log_test)
gtest_discover_tests(failure_dialog_test)
gtest_discover_tests(frame_rate_policy_test)
gtest_discover_tests(performance_policy_test)
gtest_discover_tests(http_client_policy_test)
gtest_discover_tests(http_client_spin_guard_test)
gtest_discover_tests(crash_report_policy_test)
gtest_discover_tests(game_mode_test)
gtest_discover_tests(game_mode_link_namespace_test PROPERTIES
  ENVIRONMENT
    "LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}/game_mode_namespace_fixture"
)
gtest_discover_tests(memory_limit_test)
gtest_discover_tests(runtime_paths_test)
gtest_discover_tests(single_instance_lock_test)
gtest_discover_tests(supported_launch_policy_test)
gtest_discover_tests(platform_cache_migration_test)
gtest_discover_tests(roblox_desktop_app_policy_test)
gtest_discover_tests(browser_tracker_service_test)
gtest_discover_tests(command_line_test)
gtest_discover_tests(roblox_launch_uri_test)
gtest_discover_tests(external_launch_broker_test)
gtest_discover_tests(webview_helper_launcher_test)
gtest_discover_tests(webview_helper_policy_test)
gtest_discover_tests(webview_roblox_cookie_test)
gtest_discover_tests(startup_state_machine_test)
gtest_discover_tests(game_session_coordinator_test)
gtest_discover_tests(owned_pthread_test)
gtest_discover_tests(roblox_capability_resolver_test)
gtest_discover_tests(roblox_app_lifecycle_test)
gtest_discover_tests(roblox_game_session_native_adapter_test)
gtest_discover_tests(roblox_experience_launch_bridge_test)
gtest_discover_tests(roblox_web_view_bridge_test)
gtest_discover_tests(roblox_browser_service_bridge_test)
gtest_discover_tests(roblox_fresh_game_launch_controller_test)
gtest_discover_tests(roblox_experience_composition_test)
gtest_discover_tests(jnivm_message_bus_test)
gtest_discover_tests(jnivm_experience_lifecycle_test)
gtest_discover_tests(jnivm_native_text_box_info_test)
gtest_discover_tests(vulkan_present_observer_test)
gtest_discover_tests(input_pump_pacer_test)
gtest_discover_tests(vulkan_surface_recovery_gate_test)
gtest_discover_tests(vulkan_present_progress_gate_test)
gtest_discover_tests(vulkan_text_overlay_compositor_test)
gtest_discover_tests(video_driver_policy_test)
gtest_discover_tests(window_surface_lifecycle_test)
gtest_discover_tests(window_creation_policy_test)
gtest_discover_tests(window_game_surface_bridge_test)
gtest_discover_tests(window_resize_readiness_gate_test)
gtest_discover_tests(window_fullscreen_request_gate_test)
gtest_discover_tests(window_state_store_test)
gtest_discover_tests(roblox_fullscreen_menu_request_gate_test)
gtest_discover_tests(roblox_fullscreen_runtime_bridge_test)
gtest_discover_tests(roblox_output_device_bridge_test)
gtest_discover_tests(window_text_input_owner_test)
gtest_discover_tests(window_pointer_capture_owner_test)
gtest_discover_tests(roblox_text_input_jni_bridge_test)
gtest_discover_tests(auth_service_test)
gtest_discover_tests(auth_runtime_composition_test)
gtest_discover_tests(sha256_test)
gtest_discover_tests(client_settings_service_test)
gtest_discover_tests(elf_build_id_test)
gtest_discover_tests(build_profile_test)
gtest_discover_tests(host_abi_profile_test)
gtest_discover_tests(payload_compatibility_host_abi_gate_test)
gtest_discover_tests(host_abi_experiment_test)
gtest_discover_tests(host_allocator_bridge_test)
gtest_discover_tests(bionic_pthread_key_runtime_test)
gtest_discover_tests(bionic_atfork_runtime_test)
gtest_discover_tests(bionic_dns_runtime_test)
gtest_discover_tests(bionic_host_libc_runtime_test)
gtest_discover_tests(bionic_large_file_runtime_test)
gtest_discover_tests(bionic_pthread_create_runtime_test)
gtest_discover_tests(bionic_sysconf_test)
gtest_discover_tests(bionic_signal_runtime_test)
gtest_discover_tests(bionic_semaphore_runtime_test)
gtest_discover_tests(bionic_rwlock_runtime_test)
gtest_discover_tests(bionic_abi_exports_test)
gtest_discover_tests(bionic_stdio_runtime_test)
if(MOCKTAIL_ENABLE_UPSTREAM_JNIVM)
  gtest_discover_tests(upstream_jnivm_integration_test)
endif()
gtest_discover_tests(libc_shim_test)
gtest_discover_tests(discord_rpc_test)
gtest_discover_tests(asset_manager_test)
gtest_discover_tests(android_looper_test)
gtest_discover_tests(android_native_window_test)
gtest_discover_tests(headless_graphics_test)

add_executable(linker_registry_test
  linker_registry_test.cc
)
target_link_libraries(linker_registry_test PRIVATE
  mocktail_linker
  GTest::gtest_main
)
gtest_discover_tests(linker_registry_test)

add_test(
  NAME AppImageAppRunTest.ResolvesCanonicalBundleRoot
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/appimage_apprun_test.sh
          ${CMAKE_SOURCE_DIR}/packaging/AppRun
)

add_test(
  NAME UrlHandlerRegistrationTest.ExplicitPerUserDefault
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/url_handler_registration_test.sh
          ${CMAKE_SOURCE_DIR}/scripts/register_url_handler.sh
          ${CMAKE_SOURCE_DIR}/packaging/space.bigrat.mocktail.desktop
)

add_test(
  NAME UpstreamDependencyProvenanceTest.PinnedLayout
  COMMAND ${CMAKE_COMMAND}
          -DMOCKTAIL_SOURCE_DIR=${CMAKE_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/upstream_dependency_provenance_test.cmake
)

add_test(
  NAME RobloxSigningTrustTest.ActivePayloadIsPinned
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/roblox_signing_trust_test.sh
          ${CMAKE_SOURCE_DIR}/config/roblox_signing_certificates.json
          ${CMAKE_SOURCE_DIR}/config/roblox_payload.json
)

find_program(MOCKTAIL_PATCHELF_EXECUTABLE patchelf)
if(MOCKTAIL_BUILD_FREEBSD_SOCKET_HELPER AND MOCKTAIL_PATCHELF_EXECUTABLE)
  add_test(
    NAME PortablePackagingTest.RelocatableReadOnlyBundle
    COMMAND ${CMAKE_COMMAND} -E env
            MOCKTAIL_PORTABLE_TEST_BUILD_DIR=${CMAKE_BINARY_DIR}
            bash ${CMAKE_CURRENT_SOURCE_DIR}/portable_packaging_test.sh
  )
endif()

add_test(
  NAME ThinDependencyBootstrapTest.PackageManagersAndLaunch
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/thin_dependency_bootstrap_test.sh
)

add_test(
  NAME ReleaseBuildTest.SelectedLibcAndModeContract
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/release_build_contract_test.sh
          ${CMAKE_SOURCE_DIR}
          ${CMAKE_BINARY_DIR}
)

add_test(
  NAME ManagedPayloadBuildTest.NoManualApkOrSharedObject
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/managed_payload_build_contract_test.sh
          ${CMAKE_SOURCE_DIR}
)

add_test(
  NAME FlatpakPackagingTest.ReproducibleSandboxedBuild
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/flatpak_packaging_test.sh
          ${CMAKE_SOURCE_DIR}
)

add_test(
  NAME NativePackagingTest.DistributionArtifacts
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/native_packaging_test.sh
          ${CMAKE_SOURCE_DIR}
)

if(MOCKTAIL_BUILD_FREEBSD_SOCKET_HELPER)
add_test(
  NAME SystemInstallTest.DynamicRuntimeLayout
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/system_install_test.sh
          ${CMAKE_BINARY_DIR}
)
endif()
