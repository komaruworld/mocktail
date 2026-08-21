#include "update/readiness_canary.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern char** environ;

namespace mocktail::update {
namespace {

constexpr std::string_view kVulkanOverrides =
    R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})";

bool IsEnvironmentEntry(std::string_view entry, std::string_view name) {
  return entry.size() > name.size() && entry[name.size()] == '=' &&
         entry.substr(0, name.size()) == name;
}

std::optional<std::string> EnvironmentValue(std::string_view name) {
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    if (IsEnvironmentEntry(*entry, name)) {
      return std::string(*entry + name.size() + 1U);
    }
  }
  return std::nullopt;
}

void AddInherited(std::string_view name, std::vector<std::string>* output) {
  const auto value = EnvironmentValue(name);
  if (value.has_value() && value->find('\0') == std::string::npos) {
    output->push_back(std::string(name) + "=" + *value);
  }
}

std::filesystem::path UniqueDirectory(const std::filesystem::path& parent,
                                      std::string_view prefix,
                                      std::string* error) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(parent, filesystem_error);
  if (filesystem_error) {
    *error = "cannot create canary parent directory";
    return {};
  }
  std::string pattern = (parent / (std::string(prefix) + "-XXXXXX")).string();
  std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
  mutable_pattern.push_back('\0');
  char* created = mkdtemp(mutable_pattern.data());
  if (created == nullptr) {
    *error = "cannot create isolated canary directory";
    return {};
  }
  return created;
}

std::string ReadLog(const std::filesystem::path& path, std::string* error) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    *error = "canary readiness log is unavailable";
    return {};
  }
  struct stat metadata = {};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 || metadata.st_size > 64 * 1024 * 1024) {
    close(descriptor);
    *error = "canary readiness log is invalid or excessive";
    return {};
  }
  std::string contents;
  contents.reserve(static_cast<std::size_t>(metadata.st_size));
  std::array<char, 64U * 1024U> buffer{};
  while (true) {
    const ssize_t bytes = read(descriptor, buffer.data(), buffer.size());
    if (bytes == 0) break;
    if (bytes < 0) {
      if (errno == EINTR) continue;
      close(descriptor);
      *error = "cannot read canary readiness log";
      return {};
    }
    contents.append(buffer.data(), static_cast<std::size_t>(bytes));
  }
  close(descriptor);
  return contents;
}

bool HasPositiveNumberAfter(std::string_view log, std::string_view marker) {
  std::size_t position = 0;
  while ((position = log.find(marker, position)) != std::string_view::npos) {
    position += marker.size();
    std::uint64_t value = 0;
    bool digit = false;
    while (position < log.size() && log[position] >= '0' &&
           log[position] <= '9') {
      digit = true;
      value = value * 10U + static_cast<unsigned char>(log[position] - '0');
      ++position;
    }
    if (digit && value > 0) return true;
  }
  return false;
}

bool ValidateAudio(std::string_view log, std::string* error) {
  const bool opensl = HasPositiveNumberAfter(
      log, "[mocktail][audio] OpenSL playback activity: submitted_buffers=");
  const bool fmod = HasPositiveNumberAfter(
      log, "[mocktail][audio] fmod_java first_submission submitted_buffers=");
  if (log.find("[mocktail][audio] fmod_java init_failed") != log.npos ||
      log.find("[mocktail][audio] fmod_java write_failed") != log.npos ||
      log.find("[mocktail][audio] fmod_java close_failed") != log.npos) {
    *error = "graphics canary observed an FMOD audio failure";
    return false;
  }
  if (opensl &&
      (!HasPositiveNumberAfter(
           log,
           "[mocktail][audio] OpenSL playback evidence: consumed_buffers=") ||
       log.find("[mocktail][audio] OpenSL player shutdown: submitted=") ==
           log.npos ||
       log.find("clean=true") == log.npos)) {
    *error =
        "graphics canary submitted OpenSL audio without clean consumption";
    return false;
  }
  if (fmod &&
      (!HasPositiveNumberAfter(log, "consumed_buffers=") ||
       log.find("[mocktail][audio] fmod_java shutdown submitted_buffers=") ==
           log.npos ||
       log.find("pending_buffers=0") == log.npos)) {
    *error = "graphics canary submitted FMOD audio without clean consumption";
    return false;
  }
  return true;
}

}  // namespace

std::string_view CanaryGraphicsBackendName(CanaryGraphicsBackend backend) {
  switch (backend) {
    case CanaryGraphicsBackend::kDirectVulkan:
      return "direct-vulkan";
    case CanaryGraphicsBackend::kOpenGlEs:
      return "opengl";
    case CanaryGraphicsBackend::kSystemEgl:
      return "system";
    case CanaryGraphicsBackend::kAngleVulkan:
      return "angle-vulkan";
    case CanaryGraphicsBackend::kAngleSwiftShader:
      return "angle-swiftshader";
  }
  return "direct-vulkan";
}

bool ParseCanaryGraphicsBackend(std::string_view name,
                                CanaryGraphicsBackend* backend) {
  if (backend == nullptr) return false;
  if (name.empty() || name == "vulkan" || name == "native-vulkan" ||
      name == "direct-vulkan") {
    *backend = CanaryGraphicsBackend::kDirectVulkan;
    return true;
  }
  if (name == "gles" || name == "opengl") {
    *backend = CanaryGraphicsBackend::kOpenGlEs;
    return true;
  }
  if (name == "auto" || name == "system") {
    *backend = CanaryGraphicsBackend::kSystemEgl;
    return true;
  }
  if (name == "angle-vulkan") {
    *backend = CanaryGraphicsBackend::kAngleVulkan;
    return true;
  }
  if (name == "angle-swiftshader") {
    *backend = CanaryGraphicsBackend::kAngleSwiftShader;
    return true;
  }
  return false;
}

bool ValidateReadinessLog(CanaryGraphicsBackend backend, std::string_view log,
                          std::string* error) {
  constexpr std::array<std::string_view, 4> kRequiredMarkers = {
      "[compat] legacy binary patches: disabled",
      "[compat] signal-recovery handler disabled",
      "[compat] native allocator retained; host allocator bridges disabled",
      "[main] Roblox lifecycle shutdown: Stopped",
  };
  for (const std::string_view marker : kRequiredMarkers) {
    if (log.find(marker) == log.npos) {
      *error = "graphics readiness marker is missing: " + std::string(marker);
      return false;
    }
  }
  if (backend == CanaryGraphicsBackend::kDirectVulkan) {
    for (const std::string_view marker : {
             std::string_view("[window] first Roblox Vulkan frame presented"),
             std::string_view("[vulkan] SDL WSI adapter shut down")}) {
      if (log.find(marker) == log.npos) {
        *error =
            "Vulkan readiness marker is missing: " + std::string(marker);
        return false;
      }
    }
    // A numbered queue observation proves that the candidate reached the real
    // Vulkan queue instead of merely initializing a loader or window.
    if (!HasPositiveNumberAfter(log, "[window] vkQueuePresentKHR #")) {
      *error = "Vulkan readiness marker is missing: real queue present";
      return false;
    }
  } else {
    for (const std::string_view marker : {
             std::string_view(
                 "[window] EGL context and surface initialized via SDL3"),
             std::string_view("[window] OpenGL ES context version=3."),
             std::string_view("[window] first Roblox frame presented")}) {
      if (log.find(marker) == log.npos) {
        *error =
            "OpenGL readiness marker is missing: " + std::string(marker);
        return false;
      }
    }
    if (!HasPositiveNumberAfter(log, "[window] SwapBuffers #")) {
      *error = "OpenGL readiness marker is missing: real EGL swap";
      return false;
    }
  }
  if (log.find("[FATAL]") != log.npos ||
      log.find("Segmentation fault") != log.npos ||
      log.find("core dumped") != log.npos) {
    *error = "graphics readiness log contains a fatal process failure";
    return false;
  }
  return ValidateAudio(log, error);
}

CanaryResult RunReadinessCanary(const CanaryOptions& options) {
  CanaryResult result;
  if (options.timeout_seconds < 1 || options.timeout_seconds > 600) {
    result.error = "canary timeout must be between 1 and 600 seconds";
    return result;
  }
  std::error_code filesystem_error;
  const auto runtime_status =
      std::filesystem::symlink_status(options.runtime_binary, filesystem_error);
  if (filesystem_error || !std::filesystem::is_regular_file(runtime_status) ||
      std::filesystem::is_symlink(runtime_status) ||
      access(options.runtime_binary.c_str(), X_OK) != 0) {
    result.error = "canary runtime is not an executable regular file";
    return result;
  }
  const std::filesystem::path isolated = UniqueDirectory(
      options.cache_root, ".native-update-canary", &result.error);
  if (!result.error.empty()) return result;
  for (const std::string_view child :
       {"home", "data", "cache", "state", "config"}) {
    std::filesystem::create_directories(isolated / child, filesystem_error);
    if (filesystem_error) {
      result.error = "cannot create isolated canary roots";
      std::filesystem::remove_all(isolated, filesystem_error);
      return result;
    }
  }
  const std::filesystem::path log_directory =
      options.state_root / "logs/update-canary";
  std::filesystem::create_directories(log_directory, filesystem_error);
  result.log_path =
      log_directory /
      (std::string(CanaryGraphicsBackendName(options.graphics_backend)) + "-" +
       std::to_string(getpid()) + "-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".log");
  const int log_descriptor =
      open(result.log_path.c_str(),
           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (log_descriptor < 0) {
    result.error = "cannot create canary readiness log";
    std::filesystem::remove_all(isolated, filesystem_error);
    return result;
  }

  std::vector<std::string> environment;
  for (const std::string_view name : {
           "DISPLAY",
           "WAYLAND_DISPLAY",
           "XDG_RUNTIME_DIR",
           "DBUS_SESSION_BUS_ADDRESS",
           "PULSE_SERVER",
           "PIPEWIRE_REMOTE",
           "XAUTHORITY",
           "LANG",
           "LANGUAGE",
           "TZ",
           "USER",
           "LOGNAME",
           "VK_DRIVER_FILES",
           "VK_ICD_FILENAMES",
           "MESA_LOADER_DRIVER_OVERRIDE",
           "DRI_PRIME",
           "__NV_PRIME_RENDER_OFFLOAD",
           "__VK_LAYER_NV_optimus",
           "__GLX_VENDOR_LIBRARY_NAME",
       }) {
    AddInherited(name, &environment);
  }
  const auto add = [&](std::string_view name,
                       const std::filesystem::path& value) {
    environment.push_back(std::string(name) + "=" + value.string());
  };
  add("HOME", isolated / "home");
  add("XDG_DATA_HOME", isolated / "data");
  add("XDG_CACHE_HOME", isolated / "cache");
  add("XDG_STATE_HOME", isolated / "state");
  add("XDG_CONFIG_HOME", isolated / "config");
  add("MOCKTAIL_DATA_ROOT", isolated / "data");
  add("MOCKTAIL_CACHE_ROOT", isolated / "cache");
  add("MOCKTAIL_STATE_ROOT", isolated / "state");
  add("MOCKTAIL_CONFIG_ROOT", isolated / "config");
  add("ROBLOX_LIB_PATH", options.payload_directory / "libroblox.so");
  add("MOCKTAIL_ASSET_ROOT", options.payload_directory / "assets");
  add("MOCKTAIL_ASSET_PATH", options.payload_directory / "assets/content");
  add("MOCKTAIL_COMPATIBILITY_MANIFEST", options.compatibility_manifest);
  if (options.probation_candidate()) {
    add("MOCKTAIL_HOST_ABI_PROFILE_FILE", options.host_abi_profile);
    environment.push_back("MOCKTAIL_HOST_ABI_CANARY=1");
    environment.push_back("MOCKTAIL_ALLOW_CANDIDATE_HOST_ABI=1");
  }
  environment.insert(environment.end(),
                     {
                         "PATH=/usr/bin:/bin",
                         "MOCKTAIL_SKIP_UPDATE_CHECK=1",
                         "MOCKTAIL_ISOLATED_CANARY=1",
                         "MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP=1",
                         "MOCKTAIL_VALIDATE_ROBLOX_COOKIE=0",
                         "MOCKTAIL_IGNORE_WINDOW_CLOSE=1",
                         "MOCKTAIL_DISABLE_SUPPORT_BUNDLE=1",
                         "MOCKTAIL_SKIP_LIBROBLOX_CTORS=0",
                         "MOCKTAIL_HOST_JNI_SINGLETON_SEED=0",
                         "MOCKTAIL_ENGINE_TRACE=1",
                         "MOCKTAIL_WINDOW_TRACE=1",
                         "MOCKTAIL_EGL_TRACE=1",
                         "MOCKTAIL_REQUIRE_REAL_GRAPHICS=1",
                         "MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS=5000",
                         "MOCKTAIL_STARTUP_THREAD_TIMEOUT_MS=120000",
                     });
  if (options.graphics_backend == CanaryGraphicsBackend::kDirectVulkan) {
    environment.insert(
        environment.end(),
        {"MOCKTAIL_GRAPHICS_BACKEND=direct-vulkan",
         "MOCKTAIL_PRELOAD_VULKAN_SHIM=1",
         "MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON=" +
             std::string(kVulkanOverrides)});
  } else {
    environment.insert(
        environment.end(),
        {"MOCKTAIL_GRAPHICS_BACKEND=" +
             std::string(CanaryGraphicsBackendName(options.graphics_backend)),
         "MOCKTAIL_PRELOAD_VULKAN_SHIM=0",
         "MOCKTAIL_SOFTWARE_WINDOW_FALLBACK=0",
         "MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON={}"});
    if (options.graphics_backend == CanaryGraphicsBackend::kOpenGlEs) {
      environment.push_back("MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK=1");
    }
  }
  std::vector<char*> environment_pointers;
  for (std::string& entry : environment)
    environment_pointers.push_back(entry.data());
  environment_pointers.push_back(nullptr);
  std::string binary = options.runtime_binary.string();
  char windowed[] = "--windowed";
  char allow_unverified[] = "--allow-unverified-build";
  char* arguments[] = {
      binary.data(), windowed,
      options.probation_candidate() ? allow_unverified : nullptr, nullptr};
  posix_spawn_file_actions_t actions;
  int spawn_status = posix_spawn_file_actions_init(&actions);
  if (spawn_status == 0) {
    spawn_status = posix_spawn_file_actions_adddup2(&actions, log_descriptor,
                                                    STDOUT_FILENO);
  }
  if (spawn_status == 0) {
    spawn_status = posix_spawn_file_actions_adddup2(&actions, log_descriptor,
                                                    STDERR_FILENO);
  }
  pid_t child = -1;
  if (spawn_status == 0) {
    spawn_status = posix_spawn(&child, options.runtime_binary.c_str(), &actions,
                               nullptr, arguments, environment_pointers.data());
  }
  posix_spawn_file_actions_destroy(&actions);
  close(log_descriptor);
  if (spawn_status != 0) {
    result.error = "cannot start graphics canary: " +
                   std::string(std::strerror(spawn_status));
    std::filesystem::remove_all(isolated, filesystem_error);
    return result;
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(options.timeout_seconds);
  int status = 0;
  while (true) {
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) break;
    if (waited < 0 && errno != EINTR) {
      result.error = "cannot wait for graphics canary";
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(child, SIGTERM);
      const auto kill_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (std::chrono::steady_clock::now() < kill_deadline &&
             waitpid(child, &status, WNOHANG) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (waitpid(child, &status, WNOHANG) == 0) {
        kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
      }
      result.error = "graphics canary timed out";
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::filesystem::remove_all(isolated, filesystem_error);
  if (!result.error.empty()) return result;
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  if (result.exit_code != 0) {
    result.error =
        "graphics canary exited with status " +
        std::to_string(result.exit_code);
    return result;
  }
  const std::string log = ReadLog(result.log_path, &result.error);
  if (!result.error.empty()) return result;
  if (!ValidateReadinessLog(options.graphics_backend, log, &result.error)) {
    return result;
  }
  return result;
}

}  // namespace mocktail::update
