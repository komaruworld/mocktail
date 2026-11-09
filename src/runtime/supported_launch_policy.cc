#include "runtime/supported_launch_policy.h"

#include <unistd.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#ifndef MOCKTAIL_INSTALL_LIBDIR
#define MOCKTAIL_INSTALL_LIBDIR "lib"
#endif

namespace mocktail {
namespace runtime {
namespace {

bool SetDefault(const char* name, const std::string& value,
                std::string* error) {
  const char* current = std::getenv(name);
  if (current != nullptr && current[0] != '\0') {
    return true;
  }
  if (setenv(name, value.c_str(), 1) == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string("cannot publish supported launch default: ") + name;
  }
  return false;
}

std::filesystem::path CurrentExecutable() {
  std::array<char, 4097> path{};
  const ssize_t length =
      readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (length <= 0 || static_cast<std::size_t>(length) >= path.size()) {
    return {};
  }
  path[static_cast<std::size_t>(length)] = '\0';
  return path.data();
}

bool ApplyInstalledResourceDefaults(std::string* error) {
  const std::filesystem::path executable = CurrentExecutable();
  if (executable.empty()) {
    return true;
  }
  std::filesystem::path runtime_directory = executable.parent_path();
  const std::filesystem::path library_directory = MOCKTAIL_INSTALL_LIBDIR;
  std::filesystem::path prefix = runtime_directory.parent_path();
  if (runtime_directory.filename() == "mocktail") {
    for (auto component = library_directory.end();
         component != library_directory.begin();) {
      if (prefix.filename() != *--component) {
        return true;
      }
      prefix = prefix.parent_path();
    }
  } else if (runtime_directory.filename() == "bin") {
    runtime_directory = prefix / library_directory / "mocktail";
  } else {
    return true;
  }
  const std::filesystem::path helper =
      runtime_directory / "mocktail_webview_helper";
  const std::filesystem::path progress_helper =
      runtime_directory / "mocktail_failure_dialog";
  const std::filesystem::path update_helper =
      runtime_directory / "mocktail_updater";
  const std::filesystem::path data_directory = prefix / "share/mocktail";
  const std::filesystem::path metadata_directory = data_directory / "metadata";
  std::error_code filesystem_error;
  if (!std::filesystem::is_directory(metadata_directory, filesystem_error)) {
    return true;
  }

  const std::vector<std::pair<const char*, std::filesystem::path>> defaults = {
      {"MOCKTAIL_PROJECT_ROOT", data_directory},
      {"MOCKTAIL_COMPATIBILITY_MANIFEST",
       metadata_directory / "roblox_compatibility.json"},
      {"MOCKTAIL_UPDATE_COMPATIBILITY_PATH",
       metadata_directory / "roblox_compatibility.json"},
      {"MOCKTAIL_UPDATE_SIGNING_TRUST_PATH",
       metadata_directory / "roblox_signing_certificates.json"},
      {"MOCKTAIL_UPDATE_DOWNLOAD_SOURCES_PATH",
       metadata_directory / "roblox_download_sources.json"},
      {"MOCKTAIL_BOOTSTRAP_SOURCES_PATH",
       metadata_directory / "roblox_bootstrap_sources.json"},
      {"MOCKTAIL_UPDATE_CANARY_BIN", executable},
      {"MOCKTAIL_UPDATE_HELPER", update_helper},
      {"MOCKTAIL_RUNTIME_LIBRARY_DIR", runtime_directory},
      {"MOCKTAIL_WEBVIEW_HELPER", helper},
      {"MOCKTAIL_UPDATE_PROGRESS_HELPER", progress_helper},
  };
  for (const auto& [name, value] : defaults) {
    if (!SetDefault(name, value.string(), error)) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool ApplySupportedLaunchPolicy(bool interactive, std::string* error) {
  // A packaging launcher may reload its environment for each native child.
  // Keep the bundled default separate from the updater's per-candidate
  // manifest, which must survive both normal launches and isolated canaries.
  const char* packaged_manifest =
      std::getenv("MOCKTAIL_PACKAGED_COMPATIBILITY_MANIFEST");
  if (packaged_manifest != nullptr && packaged_manifest[0] != '\0' &&
      !SetDefault("MOCKTAIL_COMPATIBILITY_MANIFEST", packaged_manifest, error)) {
    return false;
  }
  if (!ApplyInstalledResourceDefaults(error)) {
    return false;
  }
  if (!interactive) {
    return true;
  }

  const std::vector<std::pair<const char*, std::string>> defaults = {
      {"MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1"},
      {"MOCKTAIL_VALIDATE_ROBLOX_COOKIE", "0"},
      {"MOCKTAIL_ENGINE_DETACH", "0"},
      {"MOCKTAIL_CALL_REAL_APP_BRIDGE_INIT", "1"},
      {"MOCKTAIL_CALL_REAL_APP_BRIDGE_INIT_THREAD", "0"},
      {"MOCKTAIL_SKIP_LIBROBLOX_CTORS", "0"},
      {"MOCKTAIL_HOST_JNI_SINGLETON_SEED", "0"},
      {"MOCKTAIL_INIT_CLIENT_SETTINGS", "1"},
      {"MOCKTAIL_HEADLESS", "0"},
      {"MOCKTAIL_STARTUP_THREAD_TIMEOUT_MS", "120000"},
      {"MOCKTAIL_CALL_REAL_APP_BRIDGE_START", "1"},
      {"MOCKTAIL_STEP_START_APP_WITH_PARAMS", "1"},
      {"MOCKTAIL_UPDATE_SURFACE_APP_AFTER_START_APP", "1"},
      {"MOCKTAIL_CALL_REAL_APP_BRIDGE_UPDATE_SURFACE", "1"},
      {"MOCKTAIL_START_LUA_APP_DM", "1"},
      {"MOCKTAIL_STEP_START_LUA_APP_DM", "1"},
      {"MOCKTAIL_POST_CLIENT_SETTINGS", "1"},
      {"MOCKTAIL_STEP_POST_CLIENT_SETTINGS", "1"},
      {"MOCKTAIL_FETCH_CLIENT_SETTINGS", "1"},
      {"MOCKTAIL_START_LUA_APP_DM_AFTER_START_APP", "0"},
      {"MOCKTAIL_START_LUA_APP_DM_INLINE", "1"},
      {"MOCKTAIL_START_LUA_APP_DM_DELAY_MS", "0"},
      {"MOCKTAIL_START_GAME_WITH_PARAM", "0"},
      {"MOCKTAIL_RESUME_GAME_WITH_PLATFORM_PARAMS_AFTER_START_GAME", "0"},
      {"MOCKTAIL_SEND_APP_READY", "0"},
      {"MOCKTAIL_SEND_GAME_LOADED", "0"},
      {"MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS", "0"},
  };
  for (const auto& [name, value] : defaults) {
    if (!SetDefault(name, value, error)) {
      return false;
    }
  }
  return true;
}

}  // namespace runtime
}  // namespace mocktail
