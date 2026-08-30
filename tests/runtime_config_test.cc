#include "runtime/runtime_config.h"
#include "runtime/system_proxy.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

class MapEnvironment final : public Environment {
 public:
  explicit MapEnvironment(
      std::unordered_map<std::string, std::string> values = {})
      : values_(std::move(values)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    const auto found = values_.find(std::string(name));
    if (found == values_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

 private:
  std::unordered_map<std::string, std::string> values_;
};

TEST(RuntimeConfigTest, UsesSupportedDefaults) {
  const MapEnvironment environment;
  const RuntimeConfig config = RuntimeConfig::FromEnvironment(environment);

  EXPECT_FALSE(config.headless());
  EXPECT_EQ(config.roblox_library_path(), "rbx_bin/libroblox.so");
  EXPECT_EQ(config.graphics_backend(), GraphicsBackend::kVulkan);
  EXPECT_EQ(config.graphics_backend_name(), "direct-vulkan");
  EXPECT_EQ(config.window().width, 1280);
  EXPECT_EQ(config.window().height, 720);
  EXPECT_EQ(config.window().title, "Roblox");
  EXPECT_FALSE(config.window().high_dpi);
  EXPECT_TRUE(config.window().high_dpi_valid);
  EXPECT_FALSE(config.input_capabilities().touch_enabled);
  EXPECT_TRUE(config.input_capabilities().mouse_enabled);
  EXPECT_TRUE(config.input_capabilities().keyboard_enabled);
  EXPECT_EQ(config.device_profile().name, "pc-windows-11");
  EXPECT_EQ(config.device_profile().cache_key, "pc-windows-11");
  EXPECT_EQ(config.device_profile().display_name, "Windows 11 PC");
  EXPECT_TRUE(config.desktop_playability());
  ASSERT_TRUE(config.roblox_http_user_agent().has_value());
  EXPECT_EQ(*config.roblox_http_user_agent(), kRobloxDesktopHttpUserAgent);
  EXPECT_EQ(config.frame_rate().mode, FrameRateLimitMode::kDisplay);
  EXPECT_FALSE(config.performance().multithreaded_rendering);
  EXPECT_GT(config.performance().physical_core_count, 0);
  EXPECT_EQ(config.performance().memory_limit_mb, 0U);
  EXPECT_TRUE(config.performance().memory_limit_valid);
  EXPECT_EQ(config.performance().game_mode, GameModePolicy::kAuto);
  EXPECT_TRUE(config.performance().game_mode_valid);
  EXPECT_EQ(config.performance().physics_worker_mode,
            PhysicsWorkerMode::kThroughput);
  EXPECT_TRUE(config.performance().physics_worker_mode_valid);
  EXPECT_EQ(config.audio_output_device(), "default");
  EXPECT_TRUE(config.audio_output_device_valid());
  EXPECT_EQ(config.audio_input_device(), "default");
  EXPECT_TRUE(config.audio_input_device_valid());
  EXPECT_TRUE(config.microphone_enabled());
  EXPECT_FALSE(config.use_system_proxy());
  EXPECT_FALSE(config.network_proxy().has_value());
  EXPECT_FALSE(config.discord_rpc().enabled);
  EXPECT_TRUE(config.discord_rpc().show_place_name);
  EXPECT_TRUE(config.discord_rpc().show_elapsed_time);
  EXPECT_TRUE(config.discord_rpc().join_enabled);
  EXPECT_TRUE(config.discord_rpc().public_servers_only);
  EXPECT_EQ(config.discord_rpc().join_button_label, "Join Server");
  EXPECT_EQ(config.discord_rpc().application_id, "1537088975720812655");
  EXPECT_EQ(config.discord_rpc().text.browsing, "Browsing experiences");
  EXPECT_EQ(config.discord_rpc().text.joining, "Joining an experience");
  EXPECT_EQ(config.discord_rpc().text.playing, "{place_name}");
  EXPECT_EQ(config.discord_rpc().text.state, "Playing Roblox");
  EXPECT_EQ(config.discord_rpc().text.unknown_place, "Unknown experience");
  EXPECT_TRUE(config.discord_rpc_valid());
  EXPECT_FALSE(config.has_unsafe_detached_thread_overrides());
  EXPECT_TRUE(config.unsafe_detached_thread_overrides().empty());
}

TEST(RuntimeConfigTest, ReadsTypedRuntimeValues) {
  const MapEnvironment environment({
      {"MOCKTAIL_HEADLESS", "1"},
      {"ROBLOX_LIB_PATH", "/tmp/libroblox.so"},
      {"MOCKTAIL_GRAPHICS_BACKEND", "vulkan"},
      {"MOCKTAIL_WIN_WIDTH", "1920"},
      {"MOCKTAIL_WIN_HEIGHT", "1080"},
      {"MOCKTAIL_WIN_TITLE", "Mocktail Test"},
      {"MOCKTAIL_WIN_HIGH_DPI", "true"},
      {"MOCKTAIL_TOUCH_MODE", "on"},
      {"MOCKTAIL_DESKTOP_PLAYABILITY", "0"},
      {"MOCKTAIL_FRAME_RATE_LIMIT", "144"},
      {"MOCKTAIL_MULTITHREADED_RENDERING", "true"},
      {"MOCKTAIL_MEMORY_LIMIT_MB", "6144"},
      {"MOCKTAIL_GAMEMODE", "off"},
      {"MOCKTAIL_PHYSICS_WORKER_MODE", "latency"},
      {"MOCKTAIL_AUDIO_OUTPUT_DEVICE", "USB Headset"},
      {"MOCKTAIL_AUDIO_INPUT_DEVICE", "id:42"},
      {"MOCKTAIL_HTTP_PROXY_HOST", "proxy.example.test"},
      {"MOCKTAIL_HTTP_PROXY_PORT", "3128"},
      {"MOCKTAIL_DISCORD_RPC_ENABLED", "1"},
      {"MOCKTAIL_DISCORD_RPC_SHOW_PLACE_NAME", "0"},
      {"MOCKTAIL_DISCORD_RPC_SHOW_ELAPSED_TIME", "0"},
      {"MOCKTAIL_DISCORD_RPC_JOIN_ENABLED", "0"},
      {"MOCKTAIL_DISCORD_RPC_PUBLIC_SERVERS_ONLY", "0"},
      {"MOCKTAIL_DISCORD_RPC_JOIN_BUTTON_LABEL", "Play Together"},
      {"MOCKTAIL_DISCORD_APPLICATION_ID", "123456789012345678"},
      {"MOCKTAIL_DISCORD_RPC_TEXT_PLAYING", "In {place_name}"},
  });
  const RuntimeConfig config = RuntimeConfig::FromEnvironment(environment);

  EXPECT_TRUE(config.headless());
  EXPECT_EQ(config.roblox_library_path(), "/tmp/libroblox.so");
  EXPECT_EQ(config.graphics_backend(), GraphicsBackend::kVulkan);
  EXPECT_EQ(config.window().width, 1920);
  EXPECT_EQ(config.window().height, 1080);
  EXPECT_EQ(config.window().title, "Mocktail Test");
  EXPECT_TRUE(config.window().high_dpi);
  EXPECT_TRUE(config.window().high_dpi_valid);
  EXPECT_TRUE(config.input_capabilities().touch_enabled);
  EXPECT_FALSE(config.desktop_playability());
  EXPECT_FALSE(config.roblox_http_user_agent().has_value());
  EXPECT_EQ(config.frame_rate().fixed_fps, 144);
  EXPECT_TRUE(config.performance().multithreaded_rendering);
  EXPECT_GT(config.performance().physical_core_count, 0);
  EXPECT_EQ(config.performance().memory_limit_mb, 6144U);
  EXPECT_TRUE(config.performance().memory_limit_enabled());
  EXPECT_EQ(config.performance().game_mode, GameModePolicy::kOff);
  EXPECT_TRUE(config.performance().game_mode_valid);
  EXPECT_EQ(config.performance().physics_worker_mode,
            PhysicsWorkerMode::kLatency);
  EXPECT_EQ(config.audio_output_device(), "USB Headset");
  EXPECT_TRUE(config.audio_output_device_valid());
  EXPECT_EQ(config.audio_input_device(), "id:42");
  EXPECT_TRUE(config.audio_input_device_valid());
  ASSERT_TRUE(config.network_proxy().has_value());
  EXPECT_EQ(config.network_proxy()->host, "proxy.example.test");
  EXPECT_EQ(config.network_proxy()->port, 3128);
  EXPECT_EQ(BuildNetworkProxyUrl(*config.network_proxy()),
            "http://proxy.example.test:3128");
  EXPECT_TRUE(config.discord_rpc().enabled);
  EXPECT_FALSE(config.discord_rpc().show_place_name);
  EXPECT_FALSE(config.discord_rpc().show_elapsed_time);
  EXPECT_FALSE(config.discord_rpc().join_enabled);
  EXPECT_FALSE(config.discord_rpc().public_servers_only);
  EXPECT_EQ(config.discord_rpc().join_button_label, "Play Together");
  EXPECT_EQ(config.discord_rpc().application_id, "123456789012345678");
  EXPECT_EQ(config.discord_rpc().text.playing, "In {place_name}");
  EXPECT_TRUE(config.discord_rpc_valid());
}

TEST(RuntimeConfigTest, BuildsBracketedIpv6ProxyUrl) {
  const std::optional<NetworkProxyConfig> proxy =
      ParseNetworkProxyConfig("::1", "8080");
  ASSERT_TRUE(proxy.has_value());
  EXPECT_EQ(BuildNetworkProxyUrl(*proxy), "http://[::1]:8080");
}

TEST(SystemProxyTest, SelectsHttpAndSocksProxies) {
  const SystemProxyResult https =
      SelectSystemProxy({"https://127.0.0.1:7890"});
  ASSERT_TRUE(https) << https.error;
  ASSERT_TRUE(https.proxy.has_value());
  EXPECT_EQ(BuildNetworkProxyUrl(*https.proxy), "http://127.0.0.1:7890");

  const SystemProxyResult socks =
      SelectSystemProxy({"socks://127.0.0.1:1080"});
  ASSERT_TRUE(socks) << socks.error;
  ASSERT_TRUE(socks.proxy.has_value());
  EXPECT_EQ(BuildNetworkProxyUrl(*socks.proxy), "socks5h://127.0.0.1:1080");
}

TEST(RuntimeConfigTest, RejectsMalformedDiscordApplicationId) {
  const RuntimeConfig config = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_DISCORD_APPLICATION_ID", "not-a-snowflake"}}));

  EXPECT_FALSE(config.discord_rpc_valid());

  const RuntimeConfig invalid_boolean = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_DISCORD_RPC_ENABLED", "sometimes"}}));
  EXPECT_FALSE(invalid_boolean.discord_rpc_valid());
}

TEST(RuntimeConfigTest, RejectsInvalidEnvironmentProxyValues) {
  for (const std::pair<const char*, const char*> invalid : {
           std::pair{"proxy.example.test", "not-a-port"},
           std::pair{"proxy.example.test", "0"},
           std::pair{"proxy.example.test", "65536"},
           std::pair{"https://proxy.example.test", "443"},
           std::pair{"proxy.example.test/path", "443"},
           std::pair{"proxy.example.test\nInjected", "443"},
       }) {
    SCOPED_TRACE(invalid.first);
    const MapEnvironment environment({
        {"MOCKTAIL_HTTP_PROXY_HOST", invalid.first},
        {"MOCKTAIL_HTTP_PROXY_PORT", invalid.second},
    });
    EXPECT_FALSE(RuntimeConfig::FromEnvironment(environment)
                     .network_proxy()
                     .has_value());
  }
}

TEST(RuntimeConfigTest, KeepsUnknownTouchModeDisabled) {
  const MapEnvironment environment({{"MOCKTAIL_TOUCH_MODE", "tablet"}});

  EXPECT_FALSE(RuntimeConfig::FromEnvironment(environment)
                   .input_capabilities()
                   .touch_enabled);
}

TEST(RuntimeConfigTest, PreservesExplicitRobloxHttpUserAgent) {
  const MapEnvironment environment({
      {"MOCKTAIL_USER_AGENT", "Roblox/TestDesktop"},
      {"MOCKTAIL_DESKTOP_PLAYABILITY", "0"},
      {"MOCKTAIL_TOUCH_MODE", "on"},
  });

  const RuntimeConfig config = RuntimeConfig::FromEnvironment(environment);

  EXPECT_FALSE(config.desktop_playability());
  ASSERT_TRUE(config.roblox_http_user_agent().has_value());
  EXPECT_EQ(*config.roblox_http_user_agent(), "Roblox/TestDesktop");
}

TEST(RuntimeConfigTest, KeepsLegacyTouchAndPlayabilityOverridesIndependent) {
  const RuntimeConfig touch_desktop = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_TOUCH_MODE", "on"}}));
  EXPECT_TRUE(touch_desktop.input_capabilities().touch_enabled);
  EXPECT_TRUE(touch_desktop.desktop_playability());
  ASSERT_TRUE(touch_desktop.roblox_http_user_agent().has_value());
  EXPECT_EQ(*touch_desktop.roblox_http_user_agent(),
            kRobloxDesktopHttpUserAgent);

  const RuntimeConfig no_touch_mobile = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_DESKTOP_PLAYABILITY", "false"}}));
  EXPECT_FALSE(no_touch_mobile.input_capabilities().touch_enabled);
  EXPECT_FALSE(no_touch_mobile.desktop_playability());
  EXPECT_FALSE(no_touch_mobile.roblox_http_user_agent().has_value());
}

TEST(RuntimeConfigTest, ResolvesOneLineDevicePresetsAndAliases) {
  struct ExpectedProfile {
    const char* configured;
    const char* canonical;
    DeviceClass device_class;
    const char* display_name;
    bool touch;
    bool mouse;
    bool keyboard;
    const char* user_agent;
  };
  for (const ExpectedProfile& expected : {
           ExpectedProfile{"pc", "pc-windows-11", DeviceClass::kPc,
                           "Windows 11 PC", false, true, true,
                           "Roblox/WinInet"},
           ExpectedProfile{"mobile", "mobile-pixel-7", DeviceClass::kMobile,
                           "Google Pixel 7", true, false, false, nullptr},
           ExpectedProfile{"console", "console-ps5", DeviceClass::kConsole,
                           "PlayStation 5", false, true, true,
                           "Roblox/XboxOne"},
       }) {
    SCOPED_TRACE(expected.configured);
    const RuntimeConfig config = RuntimeConfig::FromEnvironment(
        MapEnvironment({{"MOCKTAIL_DEVICE_PROFILE", expected.configured}}));
    ASSERT_TRUE(config.device_profile_valid());
    EXPECT_EQ(config.device_profile().name, expected.canonical);
    EXPECT_EQ(config.device_profile().device_class, expected.device_class);
    EXPECT_EQ(config.device_profile().display_name, expected.display_name);
    EXPECT_EQ(config.input_capabilities().touch_enabled, expected.touch);
    EXPECT_EQ(config.input_capabilities().mouse_enabled, expected.mouse);
    EXPECT_EQ(config.input_capabilities().keyboard_enabled, expected.keyboard);
    if (expected.user_agent == nullptr) {
      EXPECT_FALSE(config.roblox_http_user_agent().has_value());
    } else {
      ASSERT_TRUE(config.roblox_http_user_agent().has_value());
      EXPECT_EQ(*config.roblox_http_user_agent(), expected.user_agent);
    }
  }
}

TEST(RuntimeConfigTest, ExplicitPresetWinsOverDeprecatedPlayabilityVariable) {
  const RuntimeConfig config = RuntimeConfig::FromEnvironment(MapEnvironment({
      {"MOCKTAIL_DEVICE_PROFILE", "console-xbox-series-x"},
      {"MOCKTAIL_DESKTOP_PLAYABILITY", "1"},
      {"MOCKTAIL_TOUCH_MODE", "on"},
  }));

  EXPECT_EQ(config.device_profile().device_class, DeviceClass::kConsole);
  EXPECT_EQ(config.device_profile().name, "console-ps5");
  EXPECT_EQ(config.device_profile().display_name, "PlayStation 5");
  EXPECT_TRUE(config.input_capabilities().touch_enabled);
  EXPECT_FALSE(config.desktop_playability());
  ASSERT_TRUE(config.roblox_http_user_agent().has_value());
  EXPECT_EQ(*config.roblox_http_user_agent(), kRobloxConsoleAdmissionUserAgent);
}

TEST(RuntimeConfigTest, ResolvesDetailedIdentityAndInputOverrides) {
  const RuntimeConfig config = RuntimeConfig::FromEnvironment(MapEnvironment({
      {"MOCKTAIL_DEVICE_PROFILE", "mobile"},
      {"MOCKTAIL_DEVICE_PLATFORM_NAME", "Android"},
      {"MOCKTAIL_DEVICE_NAME", "Google Pixel 9 Pro"},
      {"MOCKTAIL_DEVICE_MANUFACTURER", "Google"},
      {"MOCKTAIL_DEVICE_MODEL", "Pixel 9 Pro"},
      {"MOCKTAIL_DEVICE_BRAND", "google"},
      {"MOCKTAIL_DEVICE_CODE", "komodo"},
      {"MOCKTAIL_DEVICE_SKU", "komodo"},
      {"MOCKTAIL_DEVICE_SOC_MODEL", "Google Tensor G4"},
      {"MOCKTAIL_TOUCH_MODE", "on"},
      {"MOCKTAIL_MOUSE_MODE", "on"},
      {"MOCKTAIL_KEYBOARD_MODE", "off"},
  }));

  ASSERT_TRUE(config.device_profile_valid());
  EXPECT_EQ(config.device_profile().name, "mobile-pixel-7");
  EXPECT_EQ(config.device_profile().display_name, "Google Pixel 9 Pro");
  EXPECT_EQ(config.device_profile().model, "Pixel 9 Pro");
  EXPECT_EQ(config.device_profile().device_code, "komodo");
  EXPECT_EQ(config.device_profile().soc_model, "Google Tensor G4");
  EXPECT_NE(config.device_profile().cache_key, config.device_profile().name);
  EXPECT_EQ(config.device_profile().cache_key,
            BuildCustomDeviceProfileCacheKey(config.device_profile()));
  EXPECT_TRUE(config.input_capabilities().touch_enabled);
  EXPECT_TRUE(config.input_capabilities().mouse_enabled);
  EXPECT_FALSE(config.input_capabilities().keyboard_enabled);
  EXPECT_FALSE(config.roblox_http_user_agent().has_value());
}

TEST(RuntimeConfigTest, RejectsUnsafeDetailedIdentityFromEnvironment) {
  const RuntimeConfig empty = RuntimeConfig::FromEnvironment(MapEnvironment({
      {"MOCKTAIL_DEVICE_PROFILE", "pc"},
      {"MOCKTAIL_DEVICE_MODEL", ""},
  }));
  EXPECT_FALSE(empty.device_profile_valid());

  const RuntimeConfig control = RuntimeConfig::FromEnvironment(MapEnvironment({
      {"MOCKTAIL_DEVICE_PROFILE", "pc"},
      {"MOCKTAIL_DEVICE_MODEL", "Windows\nInjected"},
  }));
  EXPECT_FALSE(control.device_profile_valid());
}

TEST(RuntimeConfigTest, MarksUnknownDeviceProfileInvalid) {
  const RuntimeConfig config = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_DEVICE_PROFILE", "smart-fridge"}}));

  EXPECT_FALSE(config.device_profile_valid());
  EXPECT_EQ(config.device_profile().name, kDefaultDeviceProfileName);
}

TEST(RuntimeConfigTest, RetainsLegacyBooleanSemantics) {
  const MapEnvironment false_text({{"MOCKTAIL_HEADLESS", "false"}});
  const MapEnvironment zero({{"MOCKTAIL_HEADLESS", "0"}});

  EXPECT_TRUE(RuntimeConfig::FromEnvironment(false_text).headless());
  EXPECT_FALSE(RuntimeConfig::FromEnvironment(zero).headless());
}

TEST(RuntimeConfigTest, RejectsUnsafeWindowSizes) {
  const MapEnvironment environment({
      {"MOCKTAIL_WIN_WIDTH", "-1"},
      {"MOCKTAIL_WIN_HEIGHT", "999999999999999999999"},
  });
  const RuntimeConfig config = RuntimeConfig::FromEnvironment(environment);

  EXPECT_EQ(config.window().width, 1280);
  EXPECT_EQ(config.window().height, 720);
}

TEST(RuntimeConfigTest, RejectsUnsafeAudioOutputDevice) {
  const RuntimeConfig empty = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_AUDIO_OUTPUT_DEVICE", ""}}));
  EXPECT_TRUE(empty.audio_output_device_valid());
  EXPECT_EQ(empty.audio_output_device(), "default");

  const RuntimeConfig control = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_AUDIO_OUTPUT_DEVICE", "Speaker\nInjected"}}));
  EXPECT_FALSE(control.audio_output_device_valid());
}

TEST(RuntimeConfigTest, ValidatesAudioInputDeviceAndMicrophonePolicy) {
  const RuntimeConfig empty = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_AUDIO_INPUT_DEVICE", ""}}));
  EXPECT_TRUE(empty.audio_input_device_valid());
  EXPECT_EQ(empty.audio_input_device(), "default");
  EXPECT_TRUE(empty.microphone_enabled());

  const RuntimeConfig disabled = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_AUDIO_INPUT_DEVICE", "disabled"}}));
  EXPECT_TRUE(disabled.audio_input_device_valid());
  EXPECT_FALSE(disabled.microphone_enabled());

  const RuntimeConfig control = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_AUDIO_INPUT_DEVICE", "Mic\nInjected"}}));
  EXPECT_FALSE(control.audio_input_device_valid());
}

TEST(RuntimeConfigTest, PreservesUnknownBackendName) {
  const MapEnvironment environment(
      {{"MOCKTAIL_GRAPHICS_BACKEND", "future-backend"}});
  const RuntimeConfig config = RuntimeConfig::FromEnvironment(environment);

  EXPECT_EQ(config.graphics_backend(), GraphicsBackend::kUnknown);
  EXPECT_EQ(config.graphics_backend_name(), "future-backend");
}

TEST(RuntimeConfigTest, RejectsEveryUnsafeDetachedThreadOverride) {
  const std::vector<std::string> unsafe_overrides = {
      "MOCKTAIL_APP_BRIDGE_APP_START_THREAD",
      "MOCKTAIL_CALL_REAL_APP_BRIDGE_INIT_THREAD",
      "MOCKTAIL_START_LUA_APP_DM_THREAD",
      "MOCKTAIL_CALL_REAL_APP_BRIDGE_UPDATE_SURFACE_THREAD",
      "MOCKTAIL_CALL_REAL_APP_BRIDGE_START_THREAD",
      "MOCKTAIL_SEND_APP_READY_THREAD",
      "MOCKTAIL_SEND_GAME_LOADED_THREAD",
  };

  for (const std::string& name : unsafe_overrides) {
    SCOPED_TRACE(name);
    const MapEnvironment environment({{name, "enabled"}});
    const RuntimeConfig config = RuntimeConfig::FromEnvironment(environment);

    ASSERT_TRUE(config.has_unsafe_detached_thread_overrides());
    EXPECT_EQ(config.unsafe_detached_thread_overrides(),
              std::vector<std::string>({name}));
  }
}

TEST(RuntimeConfigTest, AllowsOnlyEmptyOrZeroDetachedThreadOverrides) {
  const MapEnvironment environment({
      {"MOCKTAIL_APP_BRIDGE_APP_START_THREAD", ""},
      {"MOCKTAIL_CALL_REAL_APP_BRIDGE_INIT_THREAD", "0"},
      {"MOCKTAIL_START_LUA_APP_DM_THREAD", "0"},
      {"MOCKTAIL_CALL_REAL_APP_BRIDGE_UPDATE_SURFACE_THREAD", ""},
      {"MOCKTAIL_CALL_REAL_APP_BRIDGE_START_THREAD", "0"},
      {"MOCKTAIL_SEND_APP_READY_THREAD", ""},
      {"MOCKTAIL_SEND_GAME_LOADED_THREAD", "0"},
  });
  const RuntimeConfig config = RuntimeConfig::FromEnvironment(environment);

  EXPECT_FALSE(config.has_unsafe_detached_thread_overrides());
  EXPECT_TRUE(config.unsafe_detached_thread_overrides().empty());
}

TEST(RuntimeConfigTest, ReportsUnsafeOverridesInStablePolicyOrder) {
  const MapEnvironment environment({
      {"MOCKTAIL_SEND_GAME_LOADED_THREAD", "1"},
      {"MOCKTAIL_APP_BRIDGE_APP_START_THREAD", "false"},
      {"MOCKTAIL_CALL_REAL_APP_BRIDGE_START_THREAD", "yes"},
  });
  const RuntimeConfig config = RuntimeConfig::FromEnvironment(environment);

  EXPECT_EQ(config.unsafe_detached_thread_overrides(),
            std::vector<std::string>({
                "MOCKTAIL_APP_BRIDGE_APP_START_THREAD",
                "MOCKTAIL_CALL_REAL_APP_BRIDGE_START_THREAD",
                "MOCKTAIL_SEND_GAME_LOADED_THREAD",
            }));
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
