#include "runtime/runtime_config_bootstrap.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::string_view kDefaultRuntimeConfig =
    R"yaml(# Mocktail configuration.
# The file is created once with mode 0600. Mocktail never replaces your edits.

# Integer: configuration schema version. Only version 1 is supported.
version: 1

# One-line device preset. Short aliases pc, mobile, and console are accepted.
# Presets: pc-windows-11, mobile-pixel-7, console-ps5.
# Use the mapping below for a custom profile.
device: pc-windows-11

# Detailed alternative (replace the scalar device line above):
# device:
#   type: mobile
#   platform: Android
#   name: Google Pixel 9 Pro
#   manufacturer: Google
#   model: Pixel 9 Pro
#   brand: google
#   code: komodo
#   sku: pixel-9-pro
#   soc_model: Google Tensor G4
#   touch: true
#   mouse: false
#   keyboard: false

runtime:
  # Boolean (default: false): start without a visible SDL window.
  headless: false
  # String (optional): use a specific x86_64 libroblox.so instead of the
  # active verified payload.
  # roblox_library: /absolute/path/to/libroblox.so

appearance:
  theme: system

graphics:
  # String (default: direct-vulkan): graphics backend. Use opengl on hardware
  # without a working Vulkan driver; OpenGL ES 3.0 or newer is required.
  # Recommended values: direct-vulkan, opengl, system, angle-vulkan.
  backend: direct-vulkan
  # String or integer (default: display): requested frame-rate policy.
  # Supported values: display, unlimited, 30, 60, 120, 144, 240. Unlimited
  # selects Roblox's unmodified 240-FPS scheduler maximum.
  frame_rate_limit: display
  # String/integer (default: auto): pin the OpenGL ES client version for the
  # system-egl (OpenGL) backend. Supported: auto, 3.0, 3.1, 3.2 (or 30/31/32).
  # auto lets the driver and Roblox negotiate the highest compatible version.
  gles_version: auto
  # String (default: auto): presentation synchronization: auto, on, or off.
  vsync: auto

performance:
  # Boolean (default: false): size Roblox scheduler/render queues from every
  # physical CPU core. A place's Lua/main thread can still remain serial.
  multithreaded_rendering: false
  # String (default: throughput): `throughput` makes every physical core
  # available to Roblox worker pools; `latency` preserves Roblox-managed pool
  # sizes and coalesces midphase work. Supported: auto, latency, throughput.
  physics_worker_mode: throughput
  # Integer MiB (default: 0): hard RAM cap for the Mocktail process tree.
  # 0 disables the cap. cgroup v2 also disables swap; other Linux systems use
  # an RSS+swap watchdog. 6144 is a conservative starting point for 32 GiB RAM.
  memory_limit_mb: 0
  # String (default: auto): request Feral GameMode when its host daemon and
  # client library are available. Supported values: auto, on, off.
  gamemode: auto

audio:
  # String (default: default): `default` follows the host system output.
  # To pin output, copy an exact SDL device name printed during startup.
  output_device: default
  # String (default: default): input device name, `id:<number>` printed during
  # startup, or `disabled`. IDs disambiguate duplicate names but may change
  # between boots; prefer the exact device name when it is unique.
  input_device: default

integrations:
  discord_rpc:
    # Boolean (default: false): publish Mocktail activity to Discord Desktop.
    # This never signs in to Discord and never reads an account token.
    enabled: false
    # Boolean (default: true): show the current Roblox experience name.
    show_place_name: true
    # Boolean (default: true): show how long the current session has run.
    show_elapsed_time: true
    join:
      # Boolean (default: true): let friends open the current experience. When
      # Roblox provides a public server ID, the button targets that server.
      enabled: true
      # Boolean (default: true): never expose private or reserved joins.
      public_servers_only: true
      # button_label: Join Server
    # English defaults. Uncomment only the lines you want to customize.
    # text:
    #   browsing: Browsing experiences
    #   joining: Joining an experience
    #   playing: "{place_name}"
    #   state: Playing Roblox
    #   unknown_place: Unknown experience
    # Advanced: overrides the application bundled by the package maintainer.
    # application_id: 123456789012345678

window:
  # Integer (default: 1280): initial window width in pixels.
  width: 1280
  # Integer (default: 720): initial window height in pixels.
  height: 720
  # String (default: Roblox): window title.
  title: Roblox

network:
  # Boolean (default: false): follow the host HTTP/SOCKS5 proxy and
  # automatically use its current port. Do not combine with a fixed proxy.
  use_system_proxy: false
  # Optional fixed HTTP proxy. Uncomment both values together. The host must
  # not contain a scheme such as http://.
  # String (optional, default: disabled): proxy host name or IP address.
  # proxy_host: 127.0.0.1
  # Integer (optional, default: disabled): proxy TCP port from 1 to 65535.
  # proxy_port: 8080
  # Absolute path (optional): custom PEM CA bundle. Mocktail reads this file
  # directly and does not replace it during Roblox payload updates.
  # ca_bundle: /home/user/.config/mocktail/cacert.pem

updates:
  # Boolean (default: true): check the latest Roblox version, derive an exact
  # HostAbi profile, run two isolated canaries with the selected graphics
  # backend, and promote only on success. An existing current payload is
  # preserved when probation fails.
  automatic: true
  # String (default: apk-pure): direct x86_64 APK provider selection. Both
  # accepted values resolve the provider chain, which currently starts with
  # APKPure. Supported values: auto, apk-pure.
  source: apk-pure
  # Reserved for desktop update integrations. `mocktail_updater` itself never
  # launches another process after changing the active payload.
  launch_after_update: false
)yaml";

std::string ErrorMessage(std::string_view action, int error_number) {
  return std::string(action) + ": " + std::strerror(error_number);
}

bool WriteAll(int file, std::string_view contents, std::string* error) {
  const char* next = contents.data();
  std::size_t remaining = contents.size();
  while (remaining != 0) {
    const ssize_t written = write(file, next, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      *error = ErrorMessage("cannot write first-run configuration", errno);
      return false;
    }
    next += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

bool InspectExistingConfig(const std::filesystem::path& config_file,
                           bool* exists, std::string* error) {
  struct stat metadata = {};
  if (lstat(config_file.c_str(), &metadata) != 0) {
    if (errno == ENOENT) {
      *exists = false;
      return true;
    }
    *error = ErrorMessage("cannot inspect first-run configuration", errno);
    return false;
  }
  *exists = true;
  if (!S_ISREG(metadata.st_mode)) {
    *error = S_ISLNK(metadata.st_mode)
                 ? "refusing symlink runtime configuration"
                 : "runtime configuration must be a regular file";
    return false;
  }
  return true;
}

}  // namespace

std::string_view DefaultRuntimeConfigYaml() { return kDefaultRuntimeConfig; }

RuntimeConfigBootstrapResult EnsureRuntimeConfigFile(
    const std::filesystem::path& config_file) {
  RuntimeConfigBootstrapResult result;
  if (config_file.empty() || config_file.filename().empty()) {
    result.error = "first-run configuration path must name a file";
    return result;
  }

  bool exists = false;
  if (!InspectExistingConfig(config_file, &exists, &result.error)) {
    return result;
  }
  if (exists) {
    return result;
  }

  std::filesystem::path parent = config_file.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  std::error_code filesystem_error;
  const bool parent_created =
      std::filesystem::create_directories(parent, filesystem_error);
  if (filesystem_error) {
    result.error = "cannot create configuration directory " + parent.string() +
                   ": " + filesystem_error.message();
    return result;
  }
  if (parent_created) {
    const int parent_directory =
        open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (parent_directory < 0 || fchmod(parent_directory, S_IRWXU) != 0) {
      const int operation_error = errno;
      if (parent_directory >= 0) {
        (void)close(parent_directory);
      }
      result.error = ErrorMessage("cannot secure configuration directory",
                                  operation_error);
      return result;
    }
    if (close(parent_directory) != 0) {
      result.error =
          ErrorMessage("cannot close configuration directory", errno);
      return result;
    }
  }

  std::string temporary_pattern =
      (parent / ("." + config_file.filename().string() + ".tmp.XXXXXX"))
          .string();
  std::vector<char> temporary_path(temporary_pattern.begin(),
                                   temporary_pattern.end());
  temporary_path.push_back('\0');
  const int temporary_file = mkstemp(temporary_path.data());
  if (temporary_file < 0) {
    result.error = ErrorMessage("cannot create temporary configuration", errno);
    return result;
  }

  const auto discard_temporary = [&temporary_path]() {
    (void)unlink(temporary_path.data());
  };
  if (fcntl(temporary_file, F_SETFD, FD_CLOEXEC) != 0 ||
      fchmod(temporary_file, S_IRUSR | S_IWUSR) != 0) {
    const int operation_error = errno;
    (void)close(temporary_file);
    discard_temporary();
    result.error =
        ErrorMessage("cannot secure temporary configuration", operation_error);
    return result;
  }
  if (!WriteAll(temporary_file, kDefaultRuntimeConfig, &result.error)) {
    (void)close(temporary_file);
    discard_temporary();
    return result;
  }
  if (fsync(temporary_file) != 0) {
    const int operation_error = errno;
    (void)close(temporary_file);
    discard_temporary();
    result.error = ErrorMessage("cannot synchronize first-run configuration",
                                operation_error);
    return result;
  }
  if (close(temporary_file) != 0) {
    const int operation_error = errno;
    discard_temporary();
    result.error =
        ErrorMessage("cannot close first-run configuration", operation_error);
    return result;
  }

  if (link(temporary_path.data(), config_file.c_str()) != 0) {
    const int publish_error = errno;
    discard_temporary();
    if (publish_error == EEXIST) {
      bool raced_entry_exists = false;
      if (!InspectExistingConfig(config_file, &raced_entry_exists,
                                 &result.error)) {
        return result;
      }
      if (!raced_entry_exists) {
        result.error = "runtime configuration disappeared during creation";
      }
      return result;
    }
    result.error =
        ErrorMessage("cannot publish first-run configuration", publish_error);
    return result;
  }
  result.status = RuntimeConfigBootstrapStatus::kCreated;
  if (unlink(temporary_path.data()) != 0) {
    result.error = ErrorMessage(
        "configuration was created but its temporary link cannot be removed",
        errno);
  }
  return result;
}

}  // namespace runtime
}  // namespace mocktail
