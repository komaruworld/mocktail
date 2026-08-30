#include "runtime/runtime_config_file.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/runtime_config_bootstrap.h"

#ifndef MOCKTAIL_TEST_SOURCE_DIR
#error "MOCKTAIL_TEST_SOURCE_DIR must point at the Mocktail source tree"
#endif

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
    return found == values_.end() ? std::nullopt
                                  : std::optional<std::string>(found->second);
  }

 private:
  std::unordered_map<std::string, std::string> values_;
};

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_runtime_config_XXXXXX";
    char* created = mkdtemp(pattern);
    if (created != nullptr) {
      path_ = created;
    }
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  std::filesystem::path Write(std::string_view contents) const {
    const std::filesystem::path file = path_ / "config.yaml";
    std::ofstream output(file);
    output << contents;
    return file;
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

TEST(RuntimeConfigBootstrapTest, CreatesCompletePrivateFirstRunFile) {
  TemporaryDirectory temporary;
  const std::filesystem::path file =
      temporary.path() / "nested/mocktail/config.yaml";

  const mode_t previous_umask = umask(0);
  const RuntimeConfigBootstrapResult bootstrapped =
      EnsureRuntimeConfigFile(file);
  umask(previous_umask);

  ASSERT_TRUE(bootstrapped) << bootstrapped.error;
  EXPECT_TRUE(bootstrapped.created());
  struct stat metadata = {};
  ASSERT_EQ(lstat(file.c_str(), &metadata), 0);
  EXPECT_TRUE(S_ISREG(metadata.st_mode));
  EXPECT_EQ(metadata.st_mode & 0777, 0600);
  ASSERT_EQ(lstat(file.parent_path().c_str(), &metadata), 0);
  EXPECT_TRUE(S_ISDIR(metadata.st_mode));
  EXPECT_EQ(metadata.st_mode & 0777, 0700);
  EXPECT_EQ(ReadFile(file), DefaultRuntimeConfigYaml());

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_TRUE(loaded.file_loaded);
  EXPECT_EQ(loaded.config.graphics_backend(), GraphicsBackend::kVulkan);
  EXPECT_EQ(loaded.config.frame_rate().mode, FrameRateLimitMode::kDisplay);
  EXPECT_EQ(loaded.config.vsync_mode(), "auto");
  EXPECT_FALSE(loaded.config.performance().multithreaded_rendering);
  EXPECT_EQ(loaded.config.performance().memory_limit_mb, 0U);
  EXPECT_EQ(loaded.config.performance().game_mode, GameModePolicy::kAuto);
  EXPECT_EQ(loaded.config.performance().physics_worker_mode,
            PhysicsWorkerMode::kThroughput);
  EXPECT_EQ(loaded.config.audio_output_device(), "default");
  EXPECT_EQ(loaded.config.audio_input_device(), "default");
  EXPECT_FALSE(loaded.config.input_capabilities().touch_enabled);
  EXPECT_TRUE(loaded.config.desktop_playability());
  EXPECT_EQ(loaded.config.device_profile().name, "pc-windows-11");
}

TEST(RuntimeConfigBootstrapTest,
     MatchesShippedExampleAndDocumentsEveryDefault) {
  const std::filesystem::path example =
      std::filesystem::path(MOCKTAIL_TEST_SOURCE_DIR) /
      "config/mocktail.example.yaml";
  const std::string defaults(DefaultRuntimeConfigYaml());

  EXPECT_EQ(ReadFile(example), defaults);
  for (const std::string_view documented_setting : {
           "# Integer: configuration schema version. Only version 1 is "
           "supported.\nversion: 1",
           "# Presets: pc-windows-11, mobile-pixel-7, "
           "console-ps5.\n# Use the mapping below for a custom "
           "profile.\ndevice: pc-windows-11",
           "# Boolean (default: false): start without a visible SDL "
           "window.\n  headless: false",
           "# Recommended values: direct-vulkan, opengl, system, "
           "angle-vulkan.\n  backend: direct-vulkan",
           "# selects Roblox's unmodified 240-FPS scheduler maximum.\n  "
           "frame_rate_limit: display",
           "# String (default: auto): presentation synchronization: auto, "
           "on, or off.\n  vsync: auto",
           "# physical CPU core. A place's Lua/main thread can still remain "
           "serial.\n  multithreaded_rendering: false",
           "# sizes and coalesces midphase work. Supported: auto, latency, "
           "throughput.\n  physics_worker_mode: throughput",
           "# Integer MiB (default: 0): hard RAM cap for the Mocktail process "
           "tree.\n  # 0 disables the cap.",
           "# String (default: auto): request Feral GameMode when its host "
           "daemon and\n  # client library are available. Supported values: "
           "auto, on, off.\n  gamemode: auto",
           "# To pin output, copy an exact SDL device name printed during "
           "startup.\n  output_device: default",
           "# between boots; prefer the exact device name when it is unique.\n"
           "  input_device: default",
           "# Boolean (default: false): publish Mocktail activity to Discord "
           "Desktop.\n    # This never signs in to Discord and never reads an "
           "account token.\n    enabled: false",
           "# Boolean (default: true): show the current Roblox experience "
           "name.\n    show_place_name: true",
           "# Boolean (default: true): show how long the current session has "
           "run.\n    show_elapsed_time: true",
           "# Boolean (default: true): let friends open the current "
           "experience. When\n      # Roblox provides a public server ID, the "
           "button targets that server.\n      enabled: true",
           "# Boolean (default: true): never expose private or reserved "
           "joins.\n      public_servers_only: true",
           "#   playing: \"{place_name}\"\n    #   state: Playing Roblox",
            "# Integer (default: 1280): initial window width in logical desktop "
            "units.\n  "
            "width: 1280",
            "# Integer (default: 720): initial window height in logical desktop "
            "units.\n  "
            "height: 720",
            "# String (default: Roblox): window title.\n  title: Roblox",
            "# Boolean (default: false): render at physical display-pixel density instead\n  "
            "# of the logical desktop resolution. Enable only for sharper high-DPI output.\n  "
            "high_dpi: false",
           "# HostAbi profile, run two isolated canaries with the selected "
           "graphics\n  "
           "# backend, and promote only on success. An existing current "
           "payload is\n  "
           "# preserved when probation fails.\n  "
           "automatic: true",
           "# String (default: apk-pure): direct x86_64 APK provider "
           "selection. Both\n  "
           "# accepted values resolve the provider chain, which currently "
           "starts with\n  "
           "# APKPure. Supported values: auto, apk-pure.\n  "
           "source: apk-pure",
           "# Reserved for desktop update integrations. `mocktail_updater` "
           "itself never\n  # launches another process after changing the "
           "active payload.\n  launch_after_update: false",
       }) {
    EXPECT_NE(defaults.find(documented_setting), std::string::npos)
        << documented_setting;
  }
  EXPECT_EQ(defaults.find("testing_latest_only"), std::string::npos);
}

TEST(RuntimeConfigBootstrapTest, PreservesExistingRegularFile) {
  TemporaryDirectory temporary;
  const std::filesystem::path regular = temporary.Write("user: settings\n");
  RuntimeConfigBootstrapResult bootstrapped = EnsureRuntimeConfigFile(regular);
  ASSERT_TRUE(bootstrapped) << bootstrapped.error;
  EXPECT_FALSE(bootstrapped.created());
  EXPECT_EQ(ReadFile(regular), "user: settings\n");
}

TEST(RuntimeConfigBootstrapTest, RejectsSymlinkAndNonRegularEntry) {
  TemporaryDirectory temporary;
  const std::filesystem::path target = temporary.path() / "owned.yaml";
  std::ofstream(target) << "owned: true\n";
  const std::filesystem::path symlink = temporary.path() / "linked.yaml";
  ASSERT_EQ(::symlink(target.c_str(), symlink.c_str()), 0);

  RuntimeConfigBootstrapResult bootstrapped = EnsureRuntimeConfigFile(symlink);
  EXPECT_FALSE(bootstrapped);
  EXPECT_NE(bootstrapped.error.find("symlink"), std::string::npos);
  EXPECT_TRUE(
      std::filesystem::is_symlink(std::filesystem::symlink_status(symlink)));
  EXPECT_EQ(ReadFile(target), "owned: true\n");

  const std::filesystem::path directory = temporary.path() / "directory.yaml";
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  bootstrapped = EnsureRuntimeConfigFile(directory);
  EXPECT_FALSE(bootstrapped);
  EXPECT_NE(bootstrapped.error.find("regular file"), std::string::npos);
  EXPECT_TRUE(std::filesystem::is_directory(directory));
}

TEST(RuntimeConfigBootstrapTest, PublishesOnlyOneWinnerDuringFirstRunRace) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.path() / "race/config.yaml";
  std::vector<RuntimeConfigBootstrapResult> results(12);
  std::vector<std::thread> workers;
  workers.reserve(results.size());
  for (std::size_t index = 0; index < results.size(); ++index) {
    workers.emplace_back([&file, &results, index]() {
      results[index] = EnsureRuntimeConfigFile(file);
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  std::size_t created = 0;
  for (const RuntimeConfigBootstrapResult& result : results) {
    ASSERT_TRUE(result) << result.error;
    created += result.created() ? 1 : 0;
  }
  EXPECT_EQ(created, 1);
  EXPECT_EQ(ReadFile(file), DefaultRuntimeConfigYaml());
}

TEST(RuntimeConfigFileTest, UsesDefaultsWhenFileDoesNotExist) {
  const MapEnvironment environment;
  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(environment, "/does/not/exist/config.yaml");

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_FALSE(loaded.file_loaded);
  EXPECT_EQ(loaded.config.graphics_backend(), GraphicsBackend::kVulkan);
  EXPECT_EQ(loaded.config.roblox_library_path(), "rbx_bin/libroblox.so");
}

TEST(RuntimeConfigFileTest, RejectsSymlinkAndOversizedConfiguration) {
  TemporaryDirectory temporary;
  const std::filesystem::path target = temporary.Write("version: 1\n");
  const std::filesystem::path symlink = temporary.path() / "linked.yaml";
  ASSERT_EQ(::symlink(target.c_str(), symlink.c_str()), 0);

  RuntimeConfigLoadResult loaded = LoadRuntimeConfig(MapEnvironment(), symlink);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("cannot open"), std::string::npos);

  const std::filesystem::path oversized = temporary.path() / "large.yaml";
  std::ofstream output(oversized, std::ios::binary);
  output.seekp(1024 * 1024);
  output.put('\n');
  output.close();
  loaded = LoadRuntimeConfig(MapEnvironment(), oversized);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("no larger than 1 MiB"), std::string::npos);
}

TEST(RuntimeConfigFileTest, LoadsTypedDesktopAndGraphicsSettings) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
runtime:
  headless: false
  roblox_library: /payload/libroblox.so
appearance:
  theme: dark
graphics:
  backend: vulkan
  frame_rate_limit: unlimited
  vsync: off
performance:
  multithreaded_rendering: true
  physics_worker_mode: latency
  memory_limit_mb: 6144
  gamemode: off
audio:
  output_device: Built-in Audio Analog Stereo
window:
  width: 1920
  height: 1080
  title: Mocktail Desktop
  high_dpi: true
input:
  touch_enabled: false
network:
  proxy_host: proxy.example.test
  proxy_port: 3128
integrations:
  discord_rpc:
    enabled: true
    show_place_name: false
    show_elapsed_time: false
    application_id: 123456789012345678
    join:
      enabled: true
      public_servers_only: true
      button_label: Play Together
    text:
      browsing: Looking for games
      joining: Connecting
      playing: "Inside {place_name}"
      state: Playing Roblox on Linux
      unknown_place: Unknown world
updates:
  automatic: true
)yaml");

  const MapEnvironment environment;
  const RuntimeConfigLoadResult loaded = LoadRuntimeConfig(environment, file);

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_TRUE(loaded.file_loaded);
  EXPECT_EQ(loaded.config.roblox_library_path(), "/payload/libroblox.so");
  EXPECT_EQ(loaded.config.graphics_backend(), GraphicsBackend::kVulkan);
  EXPECT_EQ(loaded.config.frame_rate().mode, FrameRateLimitMode::kUnlimited);
  EXPECT_EQ(loaded.config.vsync_mode(), "off");
  EXPECT_EQ(loaded.config.theme_mode(), "dark");
  EXPECT_TRUE(loaded.config.performance().multithreaded_rendering);
  EXPECT_EQ(loaded.config.performance().physics_worker_mode,
            PhysicsWorkerMode::kLatency);
  EXPECT_GT(loaded.config.performance().physical_core_count, 0);
  EXPECT_EQ(loaded.config.performance().memory_limit_mb, 6144U);
  EXPECT_EQ(loaded.config.performance().game_mode, GameModePolicy::kOff);
  EXPECT_EQ(loaded.config.audio_output_device(),
            "Built-in Audio Analog Stereo");
  EXPECT_EQ(loaded.config.window().width, 1920);
  EXPECT_EQ(loaded.config.window().height, 1080);
  EXPECT_EQ(loaded.config.window().title, "Mocktail Desktop");
  EXPECT_TRUE(loaded.config.window().high_dpi);
  EXPECT_FALSE(loaded.config.input_capabilities().touch_enabled);
  EXPECT_TRUE(loaded.config.desktop_playability());
  ASSERT_TRUE(loaded.config.roblox_http_user_agent().has_value());
  EXPECT_EQ(*loaded.config.roblox_http_user_agent(),
            kRobloxDesktopHttpUserAgent);
  ASSERT_TRUE(loaded.config.network_proxy().has_value());
  EXPECT_EQ(loaded.config.network_proxy()->host, "proxy.example.test");
  EXPECT_EQ(loaded.config.network_proxy()->port, 3128);
  EXPECT_TRUE(loaded.config.discord_rpc().enabled);
  EXPECT_FALSE(loaded.config.discord_rpc().show_place_name);
  EXPECT_FALSE(loaded.config.discord_rpc().show_elapsed_time);
  EXPECT_EQ(loaded.config.discord_rpc().application_id,
            "123456789012345678");
  EXPECT_TRUE(loaded.config.discord_rpc().join_enabled);
  EXPECT_TRUE(loaded.config.discord_rpc().public_servers_only);
  EXPECT_EQ(loaded.config.discord_rpc().join_button_label, "Play Together");
  EXPECT_EQ(loaded.config.discord_rpc().text.browsing, "Looking for games");
  EXPECT_EQ(loaded.config.discord_rpc().text.joining, "Connecting");
  EXPECT_EQ(loaded.config.discord_rpc().text.playing,
            "Inside {place_name}");
  EXPECT_EQ(loaded.config.discord_rpc().text.state,
            "Playing Roblox on Linux");
  EXPECT_EQ(loaded.config.discord_rpc().text.unknown_place, "Unknown world");
}

TEST(RuntimeConfigFileTest, EnvironmentOverridesYaml) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
graphics:
  backend: system
  frame_rate_limit: 60
  vsync: on
performance:
  multithreaded_rendering: true
  physics_worker_mode: latency
  memory_limit_mb: 4096
  gamemode: on
audio:
  output_device: YAML Speakers
  input_device: YAML Microphone
input:
  touch_enabled: true
compatibility:
  desktop_playability: false
window:
  high_dpi: true
network:
  proxy_host: yaml-proxy.example.test
  proxy_port: 8080
)yaml");
  const MapEnvironment environment({
      {"MOCKTAIL_GRAPHICS_BACKEND", "vulkan"},
      {"MOCKTAIL_FRAME_RATE_LIMIT", "144"},
      {"MOCKTAIL_VSYNC", "off"},
      {"MOCKTAIL_MULTITHREADED_RENDERING", "0"},
      {"MOCKTAIL_PHYSICS_WORKER_MODE", "auto"},
      {"MOCKTAIL_MEMORY_LIMIT_MB", "8192"},
      {"MOCKTAIL_GAMEMODE", "off"},
      {"MOCKTAIL_AUDIO_OUTPUT_DEVICE", "Environment Headset"},
      {"MOCKTAIL_AUDIO_INPUT_DEVICE", "Environment Microphone"},
      {"MOCKTAIL_TOUCH_MODE", "off"},
      {"MOCKTAIL_DESKTOP_PLAYABILITY", "1"},
      {"MOCKTAIL_WIN_HIGH_DPI", "0"},
      {"MOCKTAIL_HTTP_PROXY_HOST", "env-proxy.example.test"},
      {"MOCKTAIL_HTTP_PROXY_PORT", "1080"},
  });

  const RuntimeConfigLoadResult loaded = LoadRuntimeConfig(environment, file);

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.graphics_backend(), GraphicsBackend::kVulkan);
  EXPECT_EQ(loaded.config.frame_rate().fixed_fps, 144);
  EXPECT_EQ(loaded.config.vsync_mode(), "off");
  EXPECT_FALSE(loaded.config.performance().multithreaded_rendering);
  EXPECT_EQ(loaded.config.performance().physics_worker_mode,
            PhysicsWorkerMode::kAuto);
  EXPECT_EQ(loaded.config.performance().memory_limit_mb, 8192U);
  EXPECT_EQ(loaded.config.performance().game_mode, GameModePolicy::kOff);
  EXPECT_EQ(loaded.config.audio_output_device(), "Environment Headset");
  EXPECT_EQ(loaded.config.audio_input_device(), "Environment Microphone");
  EXPECT_FALSE(loaded.config.input_capabilities().touch_enabled);
  EXPECT_FALSE(loaded.config.window().high_dpi);
  EXPECT_TRUE(loaded.config.desktop_playability());
  ASSERT_TRUE(loaded.config.roblox_http_user_agent().has_value());
  EXPECT_EQ(*loaded.config.roblox_http_user_agent(),
            kRobloxDesktopHttpUserAgent);
  ASSERT_TRUE(loaded.config.network_proxy().has_value());
  EXPECT_EQ(loaded.config.network_proxy()->host, "env-proxy.example.test");
  EXPECT_EQ(loaded.config.network_proxy()->port, 1080);
}

TEST(RuntimeConfigFileTest, RejectsUnsafeAudioOutputDevice) {
  TemporaryDirectory temporary;
  const std::filesystem::path empty = temporary.Write(R"yaml(
version: 1
audio:
  output_device: ""
)yaml");
  RuntimeConfigLoadResult loaded = LoadRuntimeConfig(MapEnvironment(), empty);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("audio.output_device"), std::string::npos);

  const RuntimeConfig from_environment = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_AUDIO_OUTPUT_DEVICE", "Speaker\nInjected"}}));
  EXPECT_FALSE(from_environment.audio_output_device_valid());
  std::string error;
  EXPECT_FALSE(ExportRuntimeConfigEnvironment(from_environment, &error));
  EXPECT_NE(error.find("invalid audio output device"), std::string::npos);
}

TEST(RuntimeConfigFileTest, RejectsInvalidHighDpiPolicy) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
window:
  high_dpi: yes
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("window.high_dpi"), std::string::npos);
}

TEST(RuntimeConfigFileTest, RejectsUnsafeAudioInputDevice) {
  TemporaryDirectory temporary;
  const std::filesystem::path empty = temporary.Write(R"yaml(
version: 1
audio:
  input_device: ""
)yaml");
  RuntimeConfigLoadResult loaded = LoadRuntimeConfig(MapEnvironment(), empty);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("audio.input_device"), std::string::npos);

  const RuntimeConfig from_environment = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_AUDIO_INPUT_DEVICE", "Mic\nInjected"}}));
  EXPECT_FALSE(from_environment.audio_input_device_valid());
  std::string error;
  EXPECT_FALSE(ExportRuntimeConfigEnvironment(from_environment, &error));
  EXPECT_NE(error.find("invalid audio input device"), std::string::npos);
}

TEST(RuntimeConfigFileTest, RejectsIncompleteOrInvalidNetworkProxy) {
  TemporaryDirectory temporary;
  const std::filesystem::path incomplete = temporary.Write(R"yaml(
version: 1
network:
  proxy_host: proxy.example.test
)yaml");
  RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), incomplete);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("configured together"), std::string::npos);

  const std::filesystem::path scheme = temporary.Write(R"yaml(
version: 1
network:
  proxy_host: http://proxy.example.test
  proxy_port: 8080
)yaml");
  loaded = LoadRuntimeConfig(MapEnvironment(), scheme);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("without a scheme"), std::string::npos);

  const std::filesystem::path path = temporary.Write(R"yaml(
version: 1
network:
  proxy_host: proxy.example.test/path
  proxy_port: 8080
)yaml");
  loaded = LoadRuntimeConfig(MapEnvironment(), path);
  EXPECT_FALSE(loaded);

  const std::filesystem::path port = temporary.Write(R"yaml(
version: 1
network:
  proxy_host: proxy.example.test
  proxy_port: 65536
)yaml");
  loaded = LoadRuntimeConfig(MapEnvironment(), port);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("1 to 65535"), std::string::npos);
}

TEST(RuntimeConfigFileTest, LoadsSystemProxyFlagWithoutFixedEndpoint) {
  TemporaryDirectory temporary;
  const std::filesystem::path path = temporary.Write(R"yaml(
version: 1
network:
  use_system_proxy: true
)yaml");
  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), path);
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_TRUE(loaded.config.use_system_proxy());
  EXPECT_FALSE(loaded.config.network_proxy().has_value());
}

TEST(RuntimeConfigFileTest, ExportsProxyVariablesOnlyWhenConfigured) {
  unsetenv("MOCKTAIL_HTTP_PROXY_HOST");
  unsetenv("MOCKTAIL_HTTP_PROXY_PORT");
  unsetenv("MOCKTAIL_HTTP_PROXY_SCHEME");
  unsetenv("MOCKTAIL_NATIVE_SET_HTTP_CLIENT_PROXY");
  std::string error;
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(
      RuntimeConfig::FromEnvironment(MapEnvironment()), &error))
      << error;
  EXPECT_EQ(getenv("MOCKTAIL_HTTP_PROXY_HOST"), nullptr);
  EXPECT_EQ(getenv("MOCKTAIL_HTTP_PROXY_PORT"), nullptr);
  EXPECT_EQ(getenv("MOCKTAIL_NATIVE_SET_HTTP_CLIENT_PROXY"), nullptr);

  const RuntimeConfig configured = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_HTTP_PROXY_HOST", "127.0.0.1"},
                      {"MOCKTAIL_HTTP_PROXY_PORT", "7890"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(configured, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_HTTP_PROXY_HOST"), nullptr);
  ASSERT_NE(getenv("MOCKTAIL_HTTP_PROXY_PORT"), nullptr);
  ASSERT_NE(getenv("MOCKTAIL_HTTP_PROXY_SCHEME"), nullptr);
  ASSERT_NE(getenv("MOCKTAIL_NATIVE_SET_HTTP_CLIENT_PROXY"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_HTTP_PROXY_HOST"), "127.0.0.1");
  EXPECT_STREQ(getenv("MOCKTAIL_HTTP_PROXY_PORT"), "7890");
  EXPECT_STREQ(getenv("MOCKTAIL_HTTP_PROXY_SCHEME"), "http");
  EXPECT_STREQ(getenv("MOCKTAIL_NATIVE_SET_HTTP_CLIENT_PROXY"), "1");
  unsetenv("MOCKTAIL_HTTP_PROXY_HOST");
  unsetenv("MOCKTAIL_HTTP_PROXY_PORT");
  unsetenv("MOCKTAIL_HTTP_PROXY_SCHEME");
  unsetenv("MOCKTAIL_NATIVE_SET_HTTP_CLIENT_PROXY");
}

TEST(RuntimeConfigFileTest, ExportsMultithreadedRenderingPolicy) {
  unsetenv("MOCKTAIL_MULTITHREADED_RENDERING");
  std::string error;
  const RuntimeConfig enabled = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_MULTITHREADED_RENDERING", "1"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(enabled, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_MULTITHREADED_RENDERING"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_MULTITHREADED_RENDERING"), "1");

  const RuntimeConfig disabled =
      RuntimeConfig::FromEnvironment(MapEnvironment());
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(disabled, &error)) << error;
  EXPECT_STREQ(getenv("MOCKTAIL_MULTITHREADED_RENDERING"), "0");
  unsetenv("MOCKTAIL_MULTITHREADED_RENDERING");
}

TEST(RuntimeConfigFileTest, ExportsHighDpiWindowPolicy) {
  unsetenv("MOCKTAIL_WIN_HIGH_DPI");
  std::string error;
  const RuntimeConfig default_config =
      RuntimeConfig::FromEnvironment(MapEnvironment());
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(default_config, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_WIN_HIGH_DPI"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_WIN_HIGH_DPI"), "0");

  const RuntimeConfig high_dpi = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_WIN_HIGH_DPI", "1"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(high_dpi, &error)) << error;
  EXPECT_STREQ(getenv("MOCKTAIL_WIN_HIGH_DPI"), "1");
  unsetenv("MOCKTAIL_WIN_HIGH_DPI");
}

TEST(RuntimeConfigFileTest, ExportsPhysicsWorkerMode) {
  unsetenv("MOCKTAIL_PHYSICS_WORKER_MODE");
  std::string error;
  const RuntimeConfig latency = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_PHYSICS_WORKER_MODE", "latency"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(latency, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_PHYSICS_WORKER_MODE"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_PHYSICS_WORKER_MODE"), "latency");

  const RuntimeConfig default_config =
      RuntimeConfig::FromEnvironment(MapEnvironment());
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(default_config, &error)) << error;
  EXPECT_STREQ(getenv("MOCKTAIL_PHYSICS_WORKER_MODE"), "throughput");
  unsetenv("MOCKTAIL_PHYSICS_WORKER_MODE");
}

TEST(RuntimeConfigFileTest, ValidatesAndExportsMemoryLimitPolicy) {
  TemporaryDirectory temporary;
  const std::filesystem::path negative = temporary.Write(R"yaml(
version: 1
performance:
  memory_limit_mb: -1
)yaml");
  RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), negative);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("memory_limit_mb"), std::string::npos);

  loaded = LoadRuntimeConfig(
      MapEnvironment({{"MOCKTAIL_MEMORY_LIMIT_MB", "not-a-number"}}),
      "/does/not/exist/config.yaml");
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("memory-limit policy"), std::string::npos);

  unsetenv("MOCKTAIL_MEMORY_LIMIT_MB");
  std::string error;
  const RuntimeConfig configured = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_MEMORY_LIMIT_MB", "6144"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(configured, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_MEMORY_LIMIT_MB"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_MEMORY_LIMIT_MB"), "6144");
  unsetenv("MOCKTAIL_MEMORY_LIMIT_MB");
}

TEST(RuntimeConfigFileTest, ValidatesAndExportsGameModePolicy) {
  TemporaryDirectory temporary;
  const std::filesystem::path invalid = temporary.Write(R"yaml(
version: 1
performance:
  gamemode: required
)yaml");
  RuntimeConfigLoadResult loaded = LoadRuntimeConfig(MapEnvironment(), invalid);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("performance.gamemode"), std::string::npos);

  loaded = LoadRuntimeConfig(MapEnvironment({{"MOCKTAIL_GAMEMODE", "invalid"}}),
                             "/does/not/exist/config.yaml");
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("GameMode policy"), std::string::npos);

  unsetenv("MOCKTAIL_GAMEMODE");
  std::string error;
  const RuntimeConfig configured = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_GAMEMODE", "on"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(configured, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_GAMEMODE"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_GAMEMODE"), "on");
  unsetenv("MOCKTAIL_GAMEMODE");
}

TEST(RuntimeConfigFileTest, ExportsAudioOutputDevice) {
  unsetenv("MOCKTAIL_AUDIO_OUTPUT_DEVICE");
  std::string error;
  const RuntimeConfig configured = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_AUDIO_OUTPUT_DEVICE", "USB Headset"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(configured, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_AUDIO_OUTPUT_DEVICE"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_AUDIO_OUTPUT_DEVICE"), "USB Headset");
  unsetenv("MOCKTAIL_AUDIO_OUTPUT_DEVICE");
}

TEST(RuntimeConfigFileTest, ExportsAudioInputDevice) {
  unsetenv("MOCKTAIL_AUDIO_INPUT_DEVICE");
  std::string error;
  const RuntimeConfig configured = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_AUDIO_INPUT_DEVICE", "USB Microphone"}}));
  ASSERT_TRUE(ExportRuntimeConfigEnvironment(configured, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_AUDIO_INPUT_DEVICE"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_AUDIO_INPUT_DEVICE"), "USB Microphone");
  unsetenv("MOCKTAIL_AUDIO_INPUT_DEVICE");
}

TEST(RuntimeConfigFileTest, ExportsDesktopPlayabilityUserAgent) {
  unsetenv("MOCKTAIL_USER_AGENT");
  unsetenv("MOCKTAIL_DESKTOP_PLAYABILITY");
  unsetenv("MOCKTAIL_DEVICE_PROFILE");
  std::string error;
  const RuntimeConfig desktop =
      RuntimeConfig::FromEnvironment(MapEnvironment());

  ASSERT_TRUE(ExportRuntimeConfigEnvironment(desktop, &error)) << error;
  ASSERT_NE(getenv("MOCKTAIL_DESKTOP_PLAYABILITY"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_DESKTOP_PLAYABILITY"), "1");
  ASSERT_NE(getenv("MOCKTAIL_DEVICE_PROFILE"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_PROFILE"), "pc-windows-11");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_CLASS"), "pc");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_PLATFORM_NAME"), "Windows");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_NAME"), "Windows 11 PC");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_MANUFACTURER"), "Microsoft");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_MODEL"), "Windows 11 PC");
  ASSERT_NE(getenv("MOCKTAIL_USER_AGENT"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_USER_AGENT"),
               kRobloxDesktopHttpUserAgent.data());
  unsetenv("MOCKTAIL_USER_AGENT");
  unsetenv("MOCKTAIL_DESKTOP_PLAYABILITY");
  unsetenv("MOCKTAIL_DEVICE_PROFILE");
}

TEST(RuntimeConfigFileTest, LoadsOneLineDevicePresetsAndAliases) {
  struct ExpectedProfile {
    const char* configured;
    const char* canonical;
    DeviceClass device_class;
    const char* model;
    bool touch;
    const char* user_agent;
  };
  for (const ExpectedProfile& expected : {
           ExpectedProfile{"pc-windows-11", "pc-windows-11", DeviceClass::kPc,
                           "Windows 11 PC", false, "Roblox/WinInet"},
           ExpectedProfile{"mobile", "mobile-pixel-7", DeviceClass::kMobile,
                           "Google Pixel 7", true, nullptr},
           ExpectedProfile{"console", "console-ps5", DeviceClass::kConsole,
                           "PlayStation 5", false, "Roblox/XboxOne"},
       }) {
    SCOPED_TRACE(expected.configured);
    TemporaryDirectory temporary;
    const std::filesystem::path file = temporary.Write(
        "version: 1\ndevice: " + std::string(expected.configured) + "\n");

    const RuntimeConfigLoadResult loaded =
        LoadRuntimeConfig(MapEnvironment(), file);

    ASSERT_TRUE(loaded) << loaded.error;
    EXPECT_EQ(loaded.config.device_profile().name, expected.canonical);
    EXPECT_EQ(loaded.config.device_profile().device_class,
              expected.device_class);
    EXPECT_EQ(loaded.config.device_profile().display_name, expected.model);
    EXPECT_EQ(loaded.config.input_capabilities().touch_enabled, expected.touch);
    if (expected.user_agent == nullptr) {
      EXPECT_FALSE(loaded.config.roblox_http_user_agent().has_value());
    } else {
      ASSERT_TRUE(loaded.config.roblox_http_user_agent().has_value());
      EXPECT_EQ(*loaded.config.roblox_http_user_agent(), expected.user_agent);
    }
  }
}

TEST(RuntimeConfigFileTest, LoadsDetailedDeviceConfiguration) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
device:
  type: mobile
  platform: Android
  name: Google Pixel 9 Pro
  manufacturer: Google
  model: Pixel 9 Pro
  brand: google
  code: komodo
  sku: pixel-9-pro
  soc_model: Google Tensor G4
  touch: true
  mouse: true
  keyboard: false
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.device_profile().name, "mobile-pixel-7");
  EXPECT_EQ(loaded.config.device_profile().platform_name, "Android");
  EXPECT_EQ(loaded.config.device_profile().display_name, "Google Pixel 9 Pro");
  EXPECT_EQ(loaded.config.device_profile().manufacturer, "Google");
  EXPECT_EQ(loaded.config.device_profile().model, "Pixel 9 Pro");
  EXPECT_EQ(loaded.config.device_profile().brand, "google");
  EXPECT_EQ(loaded.config.device_profile().device_code, "komodo");
  EXPECT_EQ(loaded.config.device_profile().device_sku, "pixel-9-pro");
  EXPECT_EQ(loaded.config.device_profile().soc_model, "Google Tensor G4");
  EXPECT_NE(loaded.config.device_profile().cache_key,
            loaded.config.device_profile().name);
  EXPECT_TRUE(loaded.config.input_capabilities().touch_enabled);
  EXPECT_TRUE(loaded.config.input_capabilities().mouse_enabled);
  EXPECT_FALSE(loaded.config.input_capabilities().keyboard_enabled);
}

TEST(RuntimeConfigFileTest, ExportsMobileIdentityAndClearsDesktopUserAgent) {
  ASSERT_EQ(0, setenv("MOCKTAIL_USER_AGENT", "stale-desktop", 1));
  const RuntimeConfig mobile = RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_DEVICE_PROFILE", "mobile-pixel-7"}}));
  std::string error;

  ASSERT_TRUE(ExportRuntimeConfigEnvironment(mobile, &error)) << error;
  EXPECT_EQ(getenv("MOCKTAIL_USER_AGENT"), nullptr);
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_PROFILE"), "mobile-pixel-7");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_CLASS"), "mobile");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_PLATFORM_NAME"), "Android");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_NAME"), "Google Pixel 7");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_MODEL"), "Pixel 7");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_CODE"), "panther");
  EXPECT_STREQ(getenv("MOCKTAIL_DEVICE_SOC_MODEL"), "Google Tensor G2");
  EXPECT_STREQ(getenv("MOCKTAIL_TOUCH_MODE"), "on");
  EXPECT_STREQ(getenv("MOCKTAIL_MOUSE_MODE"), "off");
  EXPECT_STREQ(getenv("MOCKTAIL_KEYBOARD_MODE"), "off");
  EXPECT_STREQ(getenv("MOCKTAIL_DESKTOP_PLAYABILITY"), "0");
}

TEST(RuntimeConfigFileTest, EnvironmentDevicePresetOverridesYamlPreset) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
device: mobile-pixel-7
)yaml");

  const RuntimeConfigLoadResult loaded = LoadRuntimeConfig(
      MapEnvironment({{"MOCKTAIL_DEVICE_PROFILE", "console"}}), file);

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.device_profile().name, "console-ps5");
  EXPECT_EQ(loaded.config.device_profile().device_class, DeviceClass::kConsole);
  ASSERT_TRUE(loaded.config.roblox_http_user_agent().has_value());
  EXPECT_EQ(*loaded.config.roblox_http_user_agent(),
            kRobloxConsoleAdmissionUserAgent);
}

TEST(RuntimeConfigFileTest, RejectsUnknownOrContradictoryDevicePreset) {
  TemporaryDirectory temporary;
  const std::filesystem::path unknown = temporary.Write(R"yaml(
version: 1
device: smart-fridge
)yaml");
  RuntimeConfigLoadResult loaded = LoadRuntimeConfig(MapEnvironment(), unknown);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("device must be"), std::string::npos);

  const std::filesystem::path contradictory = temporary.Write(R"yaml(
version: 1
device: mobile-pixel-7
input:
  touch_enabled: false
)yaml");
  loaded = LoadRuntimeConfig(MapEnvironment(), contradictory);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("cannot be combined"), std::string::npos);
}

TEST(RuntimeConfigFileTest, RejectsInvalidDetailedDeviceConfiguration) {
  TemporaryDirectory temporary;
  const std::filesystem::path missing_type = temporary.Write(R"yaml(
version: 1
device:
  model: Pixel 9 Pro
)yaml");
  RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), missing_type);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("device.type is required"), std::string::npos);

  const std::filesystem::path empty_model = temporary.Write(R"yaml(
version: 1
device:
  type: mobile
  model: ""
)yaml");
  loaded = LoadRuntimeConfig(MapEnvironment(), empty_model);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("device.model"), std::string::npos);

  const std::filesystem::path invalid_input = temporary.Write(R"yaml(
version: 1
device:
  type: console
  touch: false
  mouse: false
  keyboard: false
)yaml");
  loaded = LoadRuntimeConfig(MapEnvironment(), invalid_input);
  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("at least one usable input"), std::string::npos);
}

TEST(RuntimeConfigFileTest, LoadsMobilePlayabilityWithoutChangingTouch) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
input:
  touch_enabled: false
compatibility:
  desktop_playability: false
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_FALSE(loaded.config.input_capabilities().touch_enabled);
  EXPECT_FALSE(loaded.config.desktop_playability());
  EXPECT_FALSE(loaded.config.roblox_http_user_agent().has_value());
}

TEST(RuntimeConfigFileTest, RejectsInvalidDesktopPlayabilityValue) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
compatibility:
  desktop_playability: automatic
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("compatibility.desktop_playability"),
            std::string::npos);
}

TEST(RuntimeConfigFileTest, AcceptsProductionDirectVulkanBackendName) {
  MapEnvironment environment({{"MOCKTAIL_GRAPHICS_BACKEND", "direct-vulkan"}});
  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(environment, "/does/not/exist/direct-vulkan.yaml");

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.graphics_backend(), GraphicsBackend::kVulkan);
}

TEST(RuntimeConfigFileTest, AcceptsStrictOpenGlBackendName) {
  MapEnvironment environment({{"MOCKTAIL_GRAPHICS_BACKEND", "opengl"}});
  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(environment, "/does/not/exist/opengl.yaml");

  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.graphics_backend(), GraphicsBackend::kSystem);
  EXPECT_EQ(loaded.config.graphics_backend_name(), "opengl");
}

TEST(RuntimeConfigFileTest, LoadsStrictOpenGlFromYamlWithoutEnvironment) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
graphics:
  backend: opengl
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_EQ(loaded.config.graphics_backend(), GraphicsBackend::kSystem);
  EXPECT_EQ(loaded.config.graphics_backend_name(), "opengl");
}

TEST(RuntimeConfigFileTest, RejectsInvalidKnownSettings) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
graphics:
  frame_rate_limit: fastest
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("frame_rate_limit"), std::string::npos);
}

TEST(RuntimeConfigFileTest, RejectsNonBooleanPerformancePolicy) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
performance:
  multithreaded_rendering: automatic
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("performance.multithreaded_rendering"),
            std::string::npos);
}

TEST(RuntimeConfigFileTest, RejectsUnknownPhysicsWorkerMode) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write(R"yaml(
version: 1
performance:
  physics_worker_mode: maximum
)yaml");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("performance.physics_worker_mode"),
            std::string::npos);
}

TEST(RuntimeConfigFileTest, RejectsMalformedYaml) {
  TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.Write("graphics: [\n");

  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(MapEnvironment(), file);

  EXPECT_FALSE(loaded);
  EXPECT_NE(loaded.error.find("invalid YAML"), std::string::npos);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
