#ifndef MOCKTAIL_RUNTIME_RUNTIME_CONFIG_H_
#define MOCKTAIL_RUNTIME_RUNTIME_CONFIG_H_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/device_profile.h"
#include "runtime/environment.h"
#include "runtime/frame_rate_policy.h"
#include "runtime/performance_policy.h"

namespace mocktail {
namespace runtime {

enum class GraphicsBackend {
  kAuto,
  kSystem,
  kVulkan,
  kAngleVulkan,
  kAngleSwiftShader,
  kUnknown,
};

struct WindowConfig {
  int width = 1280;
  int height = 720;
  std::string title = "Roblox";
};

struct InputCapabilityConfig {
  bool touch_enabled = false;
  bool mouse_enabled = true;
  bool keyboard_enabled = true;
};

struct NetworkProxyConfig {
  std::string scheme = "http";
  std::string host;
  int port = 0;
};

struct DiscordRpcTextConfig {
  std::string browsing = "Browsing experiences";
  std::string joining = "Joining an experience";
  std::string playing = "{place_name}";
  std::string state = "Playing Roblox";
  std::string unknown_place = "Unknown experience";
};

struct DiscordRpcConfig {
  bool enabled = false;
  bool show_place_name = true;
  bool show_elapsed_time = true;
  bool join_enabled = true;
  bool public_servers_only = true;
  std::string join_button_label = "Join Server";
  std::string application_id;
  DiscordRpcTextConfig text;
};

std::optional<NetworkProxyConfig> ParseNetworkProxyConfig(
    std::string_view host, std::string_view port,
    std::string_view scheme = "http");
std::string BuildNetworkProxyUrl(const NetworkProxyConfig& proxy);

// Immutable, supported runtime options. Advanced MOCKTAIL_PATCH_* controls
// intentionally do not belong here; they remain isolated in the legacy path.
class RuntimeConfig {
 public:
  static RuntimeConfig FromEnvironment(const Environment& environment);

  bool headless() const { return headless_; }
  const std::filesystem::path& roblox_library_path() const {
    return roblox_library_path_;
  }
  GraphicsBackend graphics_backend() const { return graphics_backend_; }
  const std::string& graphics_backend_name() const {
    return graphics_backend_name_;
  }
  // system-egl (OpenGL) backend GLES version pin: 0 = auto, 30/31/32 = force.
  int system_egl_gles_version() const { return system_egl_gles_version_; }
  const WindowConfig& window() const { return window_; }
  const std::string& theme_mode() const { return theme_mode_; }
  bool theme_mode_valid() const {
    return theme_mode_ == "system" || theme_mode_ == "light" ||
           theme_mode_ == "dark";
  }
  const InputCapabilityConfig& input_capabilities() const {
    return input_capabilities_;
  }
  const DeviceProfile& device_profile() const { return device_profile_; }
  bool device_profile_valid() const { return device_profile_valid_; }
  bool desktop_playability() const {
    return device_profile_.device_class == DeviceClass::kPc;
  }
  const std::optional<std::string>& roblox_http_user_agent() const {
    return roblox_http_user_agent_;
  }
  const FrameRatePolicy& frame_rate() const { return frame_rate_; }
  const std::string& vsync_mode() const { return vsync_mode_; }
  const PerformancePolicy& performance() const { return performance_; }
  const std::string& audio_output_device() const {
    return audio_output_device_;
  }
  bool audio_output_device_valid() const { return audio_output_device_valid_; }
  const std::string& audio_input_device() const { return audio_input_device_; }
  bool audio_input_device_valid() const { return audio_input_device_valid_; }
  bool microphone_enabled() const { return audio_input_device_ != "disabled"; }
  const std::optional<NetworkProxyConfig>& network_proxy() const {
    return network_proxy_;
  }
  const std::optional<std::filesystem::path>& ca_bundle() const {
    return ca_bundle_;
  }
  bool ca_bundle_valid() const { return ca_bundle_valid_; }
  bool use_system_proxy() const { return use_system_proxy_; }
  const DiscordRpcConfig& discord_rpc() const { return discord_rpc_; }
  bool discord_rpc_valid() const { return discord_rpc_valid_; }

  // These legacy opt-ins create workers that do not own their VM/JNI state.
  // Empty and "0" values are disabled; every other non-empty value is unsafe.
  bool has_unsafe_detached_thread_overrides() const {
    return !unsafe_detached_thread_overrides_.empty();
  }
  const std::vector<std::string>& unsafe_detached_thread_overrides() const {
    return unsafe_detached_thread_overrides_;
  }

  static GraphicsBackend ParseGraphicsBackend(std::string_view name);

 private:
  bool headless_ = false;
  std::filesystem::path roblox_library_path_ = "rbx_bin/libroblox.so";
  GraphicsBackend graphics_backend_ = GraphicsBackend::kVulkan;
  std::string graphics_backend_name_ = "direct-vulkan";
  int system_egl_gles_version_ = 0;
  WindowConfig window_;
  std::string theme_mode_ = "system";
  InputCapabilityConfig input_capabilities_;
  DeviceProfile device_profile_ = *FindDeviceProfile(kDefaultDeviceProfileName);
  bool device_profile_valid_ = true;
  std::optional<std::string> roblox_http_user_agent_;
  FrameRatePolicy frame_rate_;
  std::string vsync_mode_ = "auto";
  PerformancePolicy performance_;
  std::string audio_output_device_ = "default";
  bool audio_output_device_valid_ = true;
  std::string audio_input_device_ = "default";
  bool audio_input_device_valid_ = true;
  bool use_system_proxy_ = false;
  std::optional<NetworkProxyConfig> network_proxy_;
  std::optional<std::filesystem::path> ca_bundle_;
  bool ca_bundle_valid_ = true;
  DiscordRpcConfig discord_rpc_;
  bool discord_rpc_valid_ = true;
  std::vector<std::string> unsafe_detached_thread_overrides_;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_RUNTIME_CONFIG_H_
