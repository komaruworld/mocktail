#include "runtime/runtime_config.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdlib>
#include <optional>

#ifndef MOCKTAIL_DISCORD_APPLICATION_ID
#define MOCKTAIL_DISCORD_APPLICATION_ID ""
#endif

namespace mocktail {
namespace runtime {
namespace {

constexpr std::array<std::string_view, 7> kUnsafeDetachedThreadOverrides = {
    "MOCKTAIL_APP_BRIDGE_APP_START_THREAD",
    "MOCKTAIL_CALL_REAL_APP_BRIDGE_INIT_THREAD",
    "MOCKTAIL_START_LUA_APP_DM_THREAD",
    "MOCKTAIL_CALL_REAL_APP_BRIDGE_UPDATE_SURFACE_THREAD",
    "MOCKTAIL_CALL_REAL_APP_BRIDGE_START_THREAD",
    "MOCKTAIL_SEND_APP_READY_THREAD",
    "MOCKTAIL_SEND_GAME_LOADED_THREAD",
};

bool LegacyEnabled(const Environment& environment, std::string_view name) {
  const std::optional<std::string> value = environment.Get(name);
  return value.has_value() && !value->empty() && *value != "0";
}

bool IsDiscordApplicationId(std::string_view value) {
  return value.empty() ||
         (value.size() >= 17 && value.size() <= 20 &&
          std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= '0' && byte <= '9';
          }));
}

bool IsDiscordText(std::string_view value, std::size_t maximum) {
  return !value.empty() && value.size() <= maximum &&
         std::none_of(value.begin(), value.end(), [](unsigned char byte) {
           return byte < 0x20 || byte == 0x7f;
         });
}

bool ReadBoolean(const Environment& environment, std::string_view name,
                 bool default_value, bool* valid) {
  const std::optional<std::string> value = environment.Get(name);
  if (!value.has_value()) {
    return default_value;
  }
  if (*value == "1" || *value == "true" || *value == "on") {
    return true;
  }
  if (*value == "0" || *value == "false" || *value == "off") {
    return false;
  }
  *valid = false;
  return default_value;
}

std::optional<bool> InputEnabled(const Environment& environment,
                                 std::string_view name) {
  const std::optional<std::string> value = environment.Get(name);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return *value == "on" || *value == "1" || *value == "true";
}

bool ApplyDeviceProfileOverride(const Environment& environment,
                                std::string_view variable, std::size_t maximum,
                                std::string* target, bool* customized) {
  const std::optional<std::string> value = environment.Get(variable);
  if (!value.has_value()) {
    return true;
  }
  if (!IsValidDeviceProfileValue(*value, maximum)) {
    return false;
  }
  if (*target != *value) {
    *customized = true;
    *target = *value;
  }
  return true;
}

bool DesktopPlayabilityEnabled(const Environment& environment) {
  const std::optional<std::string> value =
      environment.Get("MOCKTAIL_DESKTOP_PLAYABILITY");
  return !value.has_value() ||
         (*value != "0" && *value != "off" && *value != "false");
}

int ReadPositiveInt(const Environment& environment, std::string_view name,
                    int default_value) {
  const std::optional<std::string> value = environment.Get(name);
  if (!value.has_value() || value->empty()) {
    return default_value;
  }

  errno = 0;
  char* end = nullptr;
  const long parsed = std::strtol(value->c_str(), &end, 10);
  if (end == value->c_str() || errno == ERANGE || parsed <= 0 ||
      parsed > INT_MAX) {
    return default_value;
  }
  return static_cast<int>(parsed);
}

std::optional<NetworkProxyConfig> ReadNetworkProxy(
    const Environment& environment) {
  const std::optional<std::string> host =
      environment.Get("MOCKTAIL_HTTP_PROXY_HOST");
  const std::optional<std::string> port =
      environment.Get("MOCKTAIL_HTTP_PROXY_PORT");
  const std::string scheme =
      environment.GetOr("MOCKTAIL_HTTP_PROXY_SCHEME", "http");
  if (!host.has_value() || host->empty() || !port.has_value()) {
    return std::nullopt;
  }
  return ParseNetworkProxyConfig(*host, *port, scheme);
}

}  // namespace

std::optional<NetworkProxyConfig> ParseNetworkProxyConfig(
    std::string_view host, std::string_view port, std::string_view scheme) {
  std::string normalized_scheme;
  if (scheme == "http" || scheme == "https") {
    // Desktop resolvers commonly label the proxy selected for an HTTPS URL as
    // `https://` even when the local endpoint speaks plain HTTP CONNECT.
    normalized_scheme = "http";
  } else if (scheme == "socks" || scheme == "socks5" ||
             scheme == "socks5h") {
    normalized_scheme = "socks5h";
  } else {
    return std::nullopt;
  }
  if (host.empty() || port.empty() || host.find("://") != std::string::npos ||
      std::any_of(host.begin(), host.end(), [](unsigned char character) {
        return character <= 0x20 || character == 0x7f || character == '/' ||
               character == '\\' || character == '@' || character == '[' ||
               character == ']' || character == '?' || character == '#';
      })) {
    return std::nullopt;
  }
  int parsed_port = 0;
  const auto conversion =
      std::from_chars(port.data(), port.data() + port.size(), parsed_port);
  if (conversion.ec != std::errc() ||
      conversion.ptr != port.data() + port.size() || parsed_port <= 0 ||
      parsed_port > 65535) {
    return std::nullopt;
  }
  return NetworkProxyConfig{std::move(normalized_scheme), std::string(host),
                            parsed_port};
}

std::string BuildNetworkProxyUrl(const NetworkProxyConfig& proxy) {
  const bool ipv6 = proxy.host.find(':') != std::string::npos;
  return proxy.scheme + "://" + std::string(ipv6 ? "[" : "") + proxy.host +
         (ipv6 ? "]" : "") + ":" + std::to_string(proxy.port);
}

RuntimeConfig RuntimeConfig::FromEnvironment(const Environment& environment) {
  RuntimeConfig config;
  config.headless_ = LegacyEnabled(environment, "MOCKTAIL_HEADLESS");
  config.roblox_library_path_ = environment.GetOr(
      "ROBLOX_LIB_PATH", config.roblox_library_path_.string());
  config.graphics_backend_name_ = environment.GetOr(
      "MOCKTAIL_GRAPHICS_BACKEND", config.graphics_backend_name_);
  config.graphics_backend_ =
      ParseGraphicsBackend(config.graphics_backend_name_);
  if (const auto gles_version =
          environment.Get("MOCKTAIL_SYSTEM_GLES_VERSION");
      gles_version.has_value()) {
    const std::string& v = *gles_version;
    if (v == "30" || v == "3.0") {
      config.system_egl_gles_version_ = 30;
    } else if (v == "31" || v == "3.1") {
      config.system_egl_gles_version_ = 31;
    } else if (v == "32" || v == "3.2") {
      config.system_egl_gles_version_ = 32;
    }
  }
  config.window_.width =
      ReadPositiveInt(environment, "MOCKTAIL_WIN_WIDTH", config.window_.width);
  config.window_.height = ReadPositiveInt(environment, "MOCKTAIL_WIN_HEIGHT",
                                          config.window_.height);
  config.window_.title =
      environment.GetOr("MOCKTAIL_WIN_TITLE", config.window_.title);
  config.theme_mode_ = environment.GetOr("MOCKTAIL_THEME", "system");
  const std::optional<std::string> configured_device =
      environment.Get("MOCKTAIL_DEVICE_PROFILE");
  const bool has_explicit_device =
      configured_device.has_value() && !configured_device->empty();
  const bool has_legacy_playability =
      environment.Get("MOCKTAIL_DESKTOP_PLAYABILITY").has_value();
  std::string_view selected_device = kDefaultDeviceProfileName;
  if (has_explicit_device) {
    selected_device = *configured_device;
  } else if (has_legacy_playability) {
    selected_device = DesktopPlayabilityEnabled(environment)
                          ? std::string_view("pc-windows-11")
                          : std::string_view("mobile-pixel-7");
  }
  const DeviceProfile* profile = FindDeviceProfile(selected_device);
  config.device_profile_valid_ = profile != nullptr;
  if (profile != nullptr) {
    config.device_profile_ = *profile;
  }
  if (!has_legacy_playability || has_explicit_device) {
    config.input_capabilities_.touch_enabled =
        config.device_profile_.touch_enabled;
    config.input_capabilities_.mouse_enabled =
        config.device_profile_.mouse_enabled;
    config.input_capabilities_.keyboard_enabled =
        config.device_profile_.keyboard_enabled;
  }
  if (const std::optional<bool> touch =
          InputEnabled(environment, "MOCKTAIL_TOUCH_MODE");
      touch.has_value()) {
    config.input_capabilities_.touch_enabled = *touch;
  }
  if (const std::optional<bool> mouse =
          InputEnabled(environment, "MOCKTAIL_MOUSE_MODE");
      mouse.has_value()) {
    config.input_capabilities_.mouse_enabled = *mouse;
  }
  if (const std::optional<bool> keyboard =
          InputEnabled(environment, "MOCKTAIL_KEYBOARD_MODE");
      keyboard.has_value()) {
    config.input_capabilities_.keyboard_enabled = *keyboard;
  }
  bool customized_device = false;
  config.device_profile_valid_ =
      config.device_profile_valid_ &&
      ApplyDeviceProfileOverride(environment, "MOCKTAIL_DEVICE_PLATFORM_NAME",
                                 128, &config.device_profile_.platform_name,
                                 &customized_device) &&
      ApplyDeviceProfileOverride(environment, "MOCKTAIL_DEVICE_NAME", 128,
                                 &config.device_profile_.display_name,
                                 &customized_device) &&
      ApplyDeviceProfileOverride(environment, "MOCKTAIL_DEVICE_MANUFACTURER",
                                 128, &config.device_profile_.manufacturer,
                                 &customized_device) &&
      ApplyDeviceProfileOverride(environment, "MOCKTAIL_DEVICE_MODEL", 128,
                                 &config.device_profile_.model,
                                 &customized_device) &&
      ApplyDeviceProfileOverride(environment, "MOCKTAIL_DEVICE_BRAND", 64,
                                 &config.device_profile_.brand,
                                 &customized_device) &&
      ApplyDeviceProfileOverride(environment, "MOCKTAIL_DEVICE_CODE", 64,
                                 &config.device_profile_.device_code,
                                 &customized_device) &&
      ApplyDeviceProfileOverride(environment, "MOCKTAIL_DEVICE_SKU", 64,
                                 &config.device_profile_.device_sku,
                                 &customized_device) &&
      ApplyDeviceProfileOverride(environment, "MOCKTAIL_DEVICE_SOC_MODEL", 128,
                                 &config.device_profile_.soc_model,
                                 &customized_device);
  if (customized_device && config.device_profile_valid_) {
    config.device_profile_.cache_key =
        BuildCustomDeviceProfileCacheKey(config.device_profile_);
  }
  const std::optional<std::string> configured_user_agent =
      environment.Get("MOCKTAIL_USER_AGENT");
  if (configured_user_agent.has_value() && !configured_user_agent->empty()) {
    config.roblox_http_user_agent_ = *configured_user_agent;
  } else if (!config.device_profile_.roblox_http_user_agent.empty()) {
    config.roblox_http_user_agent_ =
        config.device_profile_.roblox_http_user_agent;
  }
  config.frame_rate_ = ParseFrameRatePolicy(
      environment.GetOr("MOCKTAIL_FRAME_RATE_LIMIT", "display"));
  config.vsync_mode_ = environment.GetOr("MOCKTAIL_VSYNC", "auto");
  config.performance_ = ParsePerformancePolicy(
      environment.GetOr("MOCKTAIL_MULTITHREADED_RENDERING", "0"),
      environment.GetOr("MOCKTAIL_MEMORY_LIMIT_MB", "0"),
      environment.GetOr("MOCKTAIL_GAMEMODE", "auto"),
      environment.GetOr("MOCKTAIL_PHYSICS_WORKER_MODE", "throughput"));
  config.audio_output_device_ = environment.GetOr(
      "MOCKTAIL_AUDIO_OUTPUT_DEVICE", config.audio_output_device_);
  config.audio_output_device_valid_ =
      IsValidDeviceProfileValue(config.audio_output_device_, 512);
  config.audio_input_device_ = environment.GetOr(
      "MOCKTAIL_AUDIO_INPUT_DEVICE", config.audio_input_device_);
  config.audio_input_device_valid_ =
      IsValidDeviceProfileValue(config.audio_input_device_, 512);
  config.use_system_proxy_ =
      LegacyEnabled(environment, "MOCKTAIL_USE_SYSTEM_PROXY");
  config.network_proxy_ = ReadNetworkProxy(environment);
  if (const std::optional<std::string> ca_bundle =
          environment.Get("MOCKTAIL_CA_BUNDLE");
      ca_bundle.has_value()) {
    config.ca_bundle_ = *ca_bundle;
    config.ca_bundle_valid_ = !ca_bundle->empty() &&
                              config.ca_bundle_->is_absolute();
  }
  bool discord_booleans_valid = true;
  config.discord_rpc_.enabled = ReadBoolean(
      environment, "MOCKTAIL_DISCORD_RPC_ENABLED", false,
      &discord_booleans_valid);
  config.discord_rpc_.show_place_name = ReadBoolean(
      environment, "MOCKTAIL_DISCORD_RPC_SHOW_PLACE_NAME", true,
      &discord_booleans_valid);
  config.discord_rpc_.show_elapsed_time = ReadBoolean(
      environment, "MOCKTAIL_DISCORD_RPC_SHOW_ELAPSED_TIME", true,
      &discord_booleans_valid);
  config.discord_rpc_.join_enabled = ReadBoolean(
      environment, "MOCKTAIL_DISCORD_RPC_JOIN_ENABLED", true,
      &discord_booleans_valid);
  config.discord_rpc_.public_servers_only = ReadBoolean(
      environment, "MOCKTAIL_DISCORD_RPC_PUBLIC_SERVERS_ONLY", true,
      &discord_booleans_valid);
  config.discord_rpc_.join_button_label = environment.GetOr(
      "MOCKTAIL_DISCORD_RPC_JOIN_BUTTON_LABEL",
      config.discord_rpc_.join_button_label);
  config.discord_rpc_.application_id = environment.GetOr(
      "MOCKTAIL_DISCORD_APPLICATION_ID", MOCKTAIL_DISCORD_APPLICATION_ID);
  config.discord_rpc_.text.browsing = environment.GetOr(
      "MOCKTAIL_DISCORD_RPC_TEXT_BROWSING",
      config.discord_rpc_.text.browsing);
  config.discord_rpc_.text.joining = environment.GetOr(
      "MOCKTAIL_DISCORD_RPC_TEXT_JOINING", config.discord_rpc_.text.joining);
  config.discord_rpc_.text.playing = environment.GetOr(
      "MOCKTAIL_DISCORD_RPC_TEXT_PLAYING", config.discord_rpc_.text.playing);
  config.discord_rpc_.text.state = environment.GetOr(
      "MOCKTAIL_DISCORD_RPC_TEXT_STATE", config.discord_rpc_.text.state);
  config.discord_rpc_.text.unknown_place = environment.GetOr(
      "MOCKTAIL_DISCORD_RPC_TEXT_UNKNOWN_PLACE",
      config.discord_rpc_.text.unknown_place);
  config.discord_rpc_valid_ = discord_booleans_valid &&
      IsDiscordApplicationId(config.discord_rpc_.application_id) &&
      IsDiscordText(config.discord_rpc_.join_button_label, 32) &&
      IsDiscordText(config.discord_rpc_.text.browsing, 128) &&
      IsDiscordText(config.discord_rpc_.text.joining, 128) &&
      IsDiscordText(config.discord_rpc_.text.playing, 128) &&
      IsDiscordText(config.discord_rpc_.text.state, 128) &&
      IsDiscordText(config.discord_rpc_.text.unknown_place, 128);
  for (const std::string_view name : kUnsafeDetachedThreadOverrides) {
    if (LegacyEnabled(environment, name)) {
      config.unsafe_detached_thread_overrides_.emplace_back(name);
    }
  }
  return config;
}

GraphicsBackend RuntimeConfig::ParseGraphicsBackend(std::string_view name) {
  if (name.empty() || name == "auto") {
    return GraphicsBackend::kAuto;
  }
  if (name == "system" || name == "gles" || name == "opengl") {
    return GraphicsBackend::kSystem;
  }
  if (name == "vulkan" || name == "native-vulkan" || name == "direct-vulkan") {
    return GraphicsBackend::kVulkan;
  }
  if (name == "angle-vulkan") {
    return GraphicsBackend::kAngleVulkan;
  }
  if (name == "angle-swiftshader") {
    return GraphicsBackend::kAngleSwiftShader;
  }
  return GraphicsBackend::kUnknown;
}

}  // namespace runtime
}  // namespace mocktail
