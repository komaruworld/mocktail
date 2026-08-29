#include "runtime/runtime_config_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yaml.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::size_t kMaximumRuntimeConfigBytes = 1024U * 1024U;

using ValueMap = std::unordered_map<std::string, std::string>;

class LayeredEnvironment final : public Environment {
 public:
  LayeredEnvironment(const Environment& primary, ValueMap fallback)
      : primary_(primary), fallback_(std::move(fallback)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    const std::optional<std::string> primary = primary_.Get(name);
    if (primary.has_value()) {
      return primary;
    }
    const auto found = fallback_.find(std::string(name));
    return found == fallback_.end() ? std::nullopt
                                    : std::optional<std::string>(found->second);
  }

 private:
  const Environment& primary_;
  ValueMap fallback_;
};

std::string Scalar(const yaml_node_t* node) {
  if (node == nullptr || node->type != YAML_SCALAR_NODE) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(node->data.scalar.value),
                     node->data.scalar.length);
}

bool ReadMapping(yaml_document_t* document, const yaml_node_t* mapping,
                 const std::string& section, ValueMap* values,
                 std::string* error) {
  if (mapping == nullptr || mapping->type != YAML_MAPPING_NODE) {
    *error = section + " must be a mapping";
    return false;
  }
  std::unordered_set<std::string> keys;
  for (yaml_node_pair_t* pair = mapping->data.mapping.pairs.start;
       pair != mapping->data.mapping.pairs.top; ++pair) {
    const yaml_node_t* key_node = yaml_document_get_node(document, pair->key);
    const yaml_node_t* value_node =
        yaml_document_get_node(document, pair->value);
    const std::string key = Scalar(key_node);
    if (key.empty() || value_node == nullptr) {
      *error = section + " contains a non-scalar or empty key";
      return false;
    }
    if (!keys.insert(key).second) {
      *error = section + " contains duplicate key: " + key;
      return false;
    }
    const std::string value = Scalar(value_node);
    if (value_node->type != YAML_SCALAR_NODE) {
      *error = section + "." + key + " must be a scalar";
      return false;
    }
    (*values)[section + "." + key] = value;
  }
  return true;
}

bool ReadNestedMapping(yaml_document_t* document, const yaml_node_t* mapping,
                       const std::string& section, int remaining_depth,
                       ValueMap* values, std::string* error) {
  if (mapping == nullptr || mapping->type != YAML_MAPPING_NODE) {
    *error = section + " must be a mapping";
    return false;
  }
  std::unordered_set<std::string> keys;
  for (yaml_node_pair_t* pair = mapping->data.mapping.pairs.start;
       pair != mapping->data.mapping.pairs.top; ++pair) {
    const yaml_node_t* key_node = yaml_document_get_node(document, pair->key);
    const yaml_node_t* value_node =
        yaml_document_get_node(document, pair->value);
    const std::string key = Scalar(key_node);
    if (key.empty() || value_node == nullptr) {
      *error = section + " contains a non-scalar or empty key";
      return false;
    }
    if (!keys.insert(key).second) {
      *error = section + " contains duplicate key: " + key;
      return false;
    }
    const std::string path = section + "." + key;
    if (value_node->type == YAML_SCALAR_NODE) {
      (*values)[path] = Scalar(value_node);
      continue;
    }
    if (value_node->type != YAML_MAPPING_NODE || remaining_depth <= 0) {
      *error = path + " must be a scalar";
      return false;
    }
    if (!ReadNestedMapping(document, value_node, path, remaining_depth - 1,
                           values, error)) {
      return false;
    }
  }
  return true;
}

bool ParseBoolean(std::string_view value, bool* parsed) {
  if (value == "true") {
    *parsed = true;
    return true;
  }
  if (value == "false") {
    *parsed = false;
    return true;
  }
  return false;
}

bool ParsePositiveInt(std::string_view value, int* parsed) {
  const char* begin = value.data();
  const char* end = begin + value.size();
  const auto result = std::from_chars(begin, end, *parsed);
  return result.ec == std::errc() && result.ptr == end && *parsed > 0;
}

bool ValidateAndMap(const ValueMap& yaml, ValueMap* environment,
                    std::string* error) {
  const auto value =
      [&yaml](std::string_view key) -> std::optional<std::string> {
    const auto found = yaml.find(std::string(key));
    return found == yaml.end() ? std::nullopt
                               : std::optional<std::string>(found->second);
  };
  const std::unordered_set<std::string> supported = {
      "version",
      "device",
      "device.__mapping",
      "device.type",
      "device.platform",
      "device.name",
      "device.manufacturer",
      "device.model",
      "device.brand",
      "device.code",
      "device.sku",
      "device.soc_model",
      "device.touch",
      "device.mouse",
      "device.keyboard",
      "runtime.headless",
      "runtime.roblox_library",
      "appearance.theme",
      "graphics.backend",
      "graphics.frame_rate_limit",
      "graphics.gles_version",
      "graphics.vsync",
      "performance.multithreaded_rendering",
      "performance.physics_worker_mode",
      "performance.memory_limit_mb",
      "performance.gamemode",
      "audio.output_device",
      "audio.input_device",
      "window.width",
      "window.height",
      "window.title",
      "input.touch_enabled",
      "compatibility.desktop_playability",
      "network.use_system_proxy",
      "network.proxy_host",
      "network.proxy_port",
      "network.ca_bundle",
      "integrations.discord_rpc.enabled",
      "integrations.discord_rpc.show_place_name",
      "integrations.discord_rpc.show_elapsed_time",
      "integrations.discord_rpc.application_id",
      "integrations.discord_rpc.join.enabled",
      "integrations.discord_rpc.join.public_servers_only",
      "integrations.discord_rpc.join.button_label",
      "integrations.discord_rpc.text.browsing",
      "integrations.discord_rpc.text.joining",
      "integrations.discord_rpc.text.playing",
      "integrations.discord_rpc.text.state",
      "integrations.discord_rpc.text.unknown_place",
  };
  for (const auto& [key, ignored] : yaml) {
    if (supported.find(key) == supported.end()) {
      *error = "unknown runtime configuration key: " + key;
      return false;
    }
  }

  if (const auto version = value("version");
      version.has_value() && *version != "1") {
    *error = "unsupported runtime configuration version: " + *version;
    return false;
  }
  const auto scalar_device = value("device");
  const bool detailed_device = value("device.__mapping").has_value();
  const auto detailed_type = value("device.type");
  if (detailed_device && !detailed_type.has_value()) {
    *error = "device.type is required for detailed device configuration";
    return false;
  }
  const std::optional<std::string> selected_device =
      scalar_device.has_value() ? scalar_device : detailed_type;
  if (selected_device.has_value()) {
    if (FindDeviceProfile(*selected_device) == nullptr) {
      *error =
          "device must be pc, mobile, console, pc-windows-11, "
          "mobile-pixel-7, or console-ps5";
      return false;
    }
    if (value("input.touch_enabled").has_value() ||
        value("compatibility.desktop_playability").has_value()) {
      *error =
          "device cannot be combined with input.touch_enabled or "
          "compatibility.desktop_playability";
      return false;
    }
    (*environment)["MOCKTAIL_DEVICE_PROFILE"] = *selected_device;
  }
  if (detailed_device) {
    struct StringField {
      std::string_view yaml;
      std::string_view variable;
      std::size_t maximum;
    };
    for (const StringField& field : {
             StringField{"device.platform", "MOCKTAIL_DEVICE_PLATFORM_NAME",
                         128},
             {"device.name", "MOCKTAIL_DEVICE_NAME", 128},
             {"device.manufacturer", "MOCKTAIL_DEVICE_MANUFACTURER", 128},
             {"device.model", "MOCKTAIL_DEVICE_MODEL", 128},
             {"device.brand", "MOCKTAIL_DEVICE_BRAND", 64},
             {"device.code", "MOCKTAIL_DEVICE_CODE", 64},
             {"device.sku", "MOCKTAIL_DEVICE_SKU", 64},
             {"device.soc_model", "MOCKTAIL_DEVICE_SOC_MODEL", 128},
         }) {
      const std::optional<std::string> configured = value(field.yaml);
      if (!configured.has_value()) {
        continue;
      }
      if (!IsValidDeviceProfileValue(*configured, field.maximum)) {
        *error = std::string(field.yaml) +
                 " must be non-empty, bounded, and contain no control bytes";
        return false;
      }
      (*environment)[std::string(field.variable)] = *configured;
    }
    for (const auto& [key, variable] : {
             std::pair<std::string_view, std::string_view>(
                 "device.touch", "MOCKTAIL_TOUCH_MODE"),
             {"device.mouse", "MOCKTAIL_MOUSE_MODE"},
             {"device.keyboard", "MOCKTAIL_KEYBOARD_MODE"},
         }) {
      const std::optional<std::string> configured = value(key);
      if (!configured.has_value()) {
        continue;
      }
      bool parsed = false;
      if (!ParseBoolean(*configured, &parsed)) {
        *error = std::string(key) + " must be true or false";
        return false;
      }
      (*environment)[std::string(variable)] = parsed ? "on" : "off";
    }
  }
  if (const auto headless = value("runtime.headless"); headless.has_value()) {
    bool parsed = false;
    if (!ParseBoolean(*headless, &parsed)) {
      *error = "runtime.headless must be true or false";
      return false;
    }
    (*environment)["MOCKTAIL_HEADLESS"] = parsed ? "1" : "0";
  }
  if (const auto library = value("runtime.roblox_library");
      library.has_value()) {
    if (library->empty()) {
      *error = "runtime.roblox_library must not be empty";
      return false;
    }
    (*environment)["ROBLOX_LIB_PATH"] = *library;
  }
  if (const auto theme = value("appearance.theme"); theme.has_value()) {
    if (*theme != "system" && *theme != "light" && *theme != "dark") {
      *error = "appearance.theme must be system, light, or dark";
      return false;
    }
    (*environment)["MOCKTAIL_THEME"] = *theme;
  }
  if (const auto backend = value("graphics.backend"); backend.has_value()) {
    if (RuntimeConfig::ParseGraphicsBackend(*backend) ==
        GraphicsBackend::kUnknown) {
      *error = "graphics.backend is not supported: " + *backend;
      return false;
    }
    (*environment)["MOCKTAIL_GRAPHICS_BACKEND"] = *backend;
  }
  if (const auto frame_rate = value("graphics.frame_rate_limit");
      frame_rate.has_value()) {
    if (!ParseFrameRatePolicy(*frame_rate).valid()) {
      *error = "graphics.frame_rate_limit is not supported: " + *frame_rate;
      return false;
    }
    (*environment)["MOCKTAIL_FRAME_RATE_LIMIT"] = *frame_rate;
  }
  if (const auto gles_version = value("graphics.gles_version");
      gles_version.has_value()) {
    const std::string& v = *gles_version;
    if (v == "auto" || v == "0" || v.empty()) {
      // auto: leave MOCKTAIL_SYSTEM_GLES_VERSION unset so the driver and
      // Roblox negotiate the highest compatible OpenGL ES version.
    } else if (v == "3.0" || v == "30") {
      (*environment)["MOCKTAIL_SYSTEM_GLES_VERSION"] = "30";
    } else if (v == "3.1" || v == "31") {
      (*environment)["MOCKTAIL_SYSTEM_GLES_VERSION"] = "31";
    } else if (v == "3.2" || v == "32") {
      (*environment)["MOCKTAIL_SYSTEM_GLES_VERSION"] = "32";
    } else {
      *error = "graphics.gles_version is not supported: " + v +
               " (use auto, 3.0, 3.1, 3.2, or 30/31/32)";
      return false;
    }
  }
  if (const auto vsync = value("graphics.vsync"); vsync.has_value()) {
    if (*vsync != "auto" && *vsync != "on" && *vsync != "off") {
      *error = "graphics.vsync must be auto, on, or off";
      return false;
    }
    (*environment)["MOCKTAIL_VSYNC"] = *vsync;
  }
  if (const auto multithreaded_rendering =
          value("performance.multithreaded_rendering");
      multithreaded_rendering.has_value()) {
    bool parsed = false;
    if (!ParseBoolean(*multithreaded_rendering, &parsed)) {
      *error = "performance.multithreaded_rendering must be true or false";
      return false;
    }
    (*environment)["MOCKTAIL_MULTITHREADED_RENDERING"] = parsed ? "1" : "0";
  }
  if (const auto physics_worker_mode = value("performance.physics_worker_mode");
      physics_worker_mode.has_value()) {
    PhysicsWorkerMode parsed = PhysicsWorkerMode::kAuto;
    if (!ParsePhysicsWorkerMode(*physics_worker_mode, &parsed)) {
      *error =
          "performance.physics_worker_mode must be auto, latency, or "
          "throughput";
      return false;
    }
    (*environment)["MOCKTAIL_PHYSICS_WORKER_MODE"] =
        std::string(PhysicsWorkerModeName(parsed));
  }
  if (const auto memory_limit = value("performance.memory_limit_mb");
      memory_limit.has_value()) {
    const PerformancePolicy parsed = ParsePerformancePolicy("0", *memory_limit);
    if (!parsed.memory_limit_valid) {
      *error = "performance.memory_limit_mb must be a non-negative integer";
      return false;
    }
    (*environment)["MOCKTAIL_MEMORY_LIMIT_MB"] =
        std::to_string(parsed.memory_limit_mb);
  }
  if (const auto game_mode = value("performance.gamemode");
      game_mode.has_value()) {
    GameModePolicy parsed = GameModePolicy::kAuto;
    if (!ParseGameModePolicy(*game_mode, &parsed)) {
      *error = "performance.gamemode must be auto, on, or off";
      return false;
    }
    (*environment)["MOCKTAIL_GAMEMODE"] = GameModePolicyName(parsed);
  }
  if (const auto output_device = value("audio.output_device");
      output_device.has_value()) {
    if (!IsValidDeviceProfileValue(*output_device, 512)) {
      *error =
          "audio.output_device must be non-empty, bounded, and contain no "
          "control bytes";
      return false;
    }
    (*environment)["MOCKTAIL_AUDIO_OUTPUT_DEVICE"] = *output_device;
  }
  if (const auto input_device = value("audio.input_device");
      input_device.has_value()) {
    if (!IsValidDeviceProfileValue(*input_device, 512)) {
      *error =
          "audio.input_device must be non-empty, bounded, and contain no "
          "control bytes";
      return false;
    }
    (*environment)["MOCKTAIL_AUDIO_INPUT_DEVICE"] = *input_device;
  }
  for (const auto& [key, variable] : {
           std::pair<std::string_view, std::string_view>("window.width",
                                                         "MOCKTAIL_WIN_WIDTH"),
           {"window.height", "MOCKTAIL_WIN_HEIGHT"},
       }) {
    if (const auto configured = value(key); configured.has_value()) {
      int parsed = 0;
      if (!ParsePositiveInt(*configured, &parsed)) {
        *error = std::string(key) + " must be a positive integer";
        return false;
      }
      (*environment)[std::string(variable)] = std::to_string(parsed);
    }
  }
  if (const auto title = value("window.title"); title.has_value()) {
    if (title->empty()) {
      *error = "window.title must not be empty";
      return false;
    }
    (*environment)["MOCKTAIL_WIN_TITLE"] = *title;
  }
  if (const auto touch = value("input.touch_enabled"); touch.has_value()) {
    bool parsed = false;
    if (!ParseBoolean(*touch, &parsed)) {
      *error = "input.touch_enabled must be true or false";
      return false;
    }
    (*environment)["MOCKTAIL_TOUCH_MODE"] = parsed ? "on" : "off";
  }
  if (const auto desktop_playability =
          value("compatibility.desktop_playability");
      desktop_playability.has_value()) {
    bool parsed = false;
    if (!ParseBoolean(*desktop_playability, &parsed)) {
      *error = "compatibility.desktop_playability must be true or false";
      return false;
    }
    (*environment)["MOCKTAIL_DESKTOP_PLAYABILITY"] = parsed ? "1" : "0";
  }
  const auto proxy_host = value("network.proxy_host");
  const auto proxy_port = value("network.proxy_port");
  bool use_system_proxy = false;
  if (const auto system_proxy = value("network.use_system_proxy");
      system_proxy.has_value()) {
    if (!ParseBoolean(*system_proxy, &use_system_proxy)) {
      *error = "network.use_system_proxy must be true or false";
      return false;
    }
    (*environment)["MOCKTAIL_USE_SYSTEM_PROXY"] =
        use_system_proxy ? "1" : "0";
  }
  if (proxy_host.has_value() != proxy_port.has_value()) {
    *error =
        "network.proxy_host and network.proxy_port must be configured together";
    return false;
  }
  if (use_system_proxy && proxy_host.has_value()) {
    *error = "network.use_system_proxy cannot be combined with a fixed proxy";
    return false;
  }
  if (proxy_host.has_value()) {
    if (!ParseNetworkProxyConfig(*proxy_host, *proxy_port).has_value()) {
      *error =
          "network proxy must use a valid host without a scheme and a port "
          "from 1 to 65535";
      return false;
    }
    (*environment)["MOCKTAIL_HTTP_PROXY_HOST"] = *proxy_host;
    (*environment)["MOCKTAIL_HTTP_PROXY_PORT"] = *proxy_port;
  }
  if (const auto ca_bundle = value("network.ca_bundle");
      ca_bundle.has_value()) {
    const std::filesystem::path path(*ca_bundle);
    if (ca_bundle->empty() || !path.is_absolute()) {
      *error = "network.ca_bundle must be an absolute file path";
      return false;
    }
    (*environment)["MOCKTAIL_CA_BUNDLE"] = *ca_bundle;
  }
  for (const auto& [key, variable] : {
           std::pair<std::string_view, std::string_view>(
               "integrations.discord_rpc.enabled",
               "MOCKTAIL_DISCORD_RPC_ENABLED"),
           {"integrations.discord_rpc.show_place_name",
            "MOCKTAIL_DISCORD_RPC_SHOW_PLACE_NAME"},
           {"integrations.discord_rpc.show_elapsed_time",
            "MOCKTAIL_DISCORD_RPC_SHOW_ELAPSED_TIME"},
           {"integrations.discord_rpc.join.enabled",
            "MOCKTAIL_DISCORD_RPC_JOIN_ENABLED"},
           {"integrations.discord_rpc.join.public_servers_only",
            "MOCKTAIL_DISCORD_RPC_PUBLIC_SERVERS_ONLY"},
       }) {
    const std::optional<std::string> configured = value(key);
    if (!configured.has_value()) {
      continue;
    }
    bool parsed = false;
    if (!ParseBoolean(*configured, &parsed)) {
      *error = std::string(key) + " must be true or false";
      return false;
    }
    (*environment)[std::string(variable)] = parsed ? "1" : "0";
  }
  if (const auto application_id =
          value("integrations.discord_rpc.application_id");
      application_id.has_value()) {
    if (application_id->size() < 17 || application_id->size() > 20 ||
        !std::all_of(application_id->begin(), application_id->end(),
                     [](unsigned char byte) {
                       return byte >= '0' && byte <= '9';
                     })) {
      *error = "integrations.discord_rpc.application_id must be a Discord "
               "snowflake";
      return false;
    }
    (*environment)["MOCKTAIL_DISCORD_APPLICATION_ID"] = *application_id;
  }
  struct DiscordStringField {
    std::string_view yaml;
    std::string_view variable;
    std::size_t maximum;
  };
  for (const DiscordStringField& field : {
           DiscordStringField{"integrations.discord_rpc.join.button_label",
                              "MOCKTAIL_DISCORD_RPC_JOIN_BUTTON_LABEL", 32},
           {"integrations.discord_rpc.text.browsing",
            "MOCKTAIL_DISCORD_RPC_TEXT_BROWSING", 128},
           {"integrations.discord_rpc.text.joining",
            "MOCKTAIL_DISCORD_RPC_TEXT_JOINING", 128},
           {"integrations.discord_rpc.text.playing",
            "MOCKTAIL_DISCORD_RPC_TEXT_PLAYING", 128},
           {"integrations.discord_rpc.text.state",
            "MOCKTAIL_DISCORD_RPC_TEXT_STATE", 128},
           {"integrations.discord_rpc.text.unknown_place",
            "MOCKTAIL_DISCORD_RPC_TEXT_UNKNOWN_PLACE", 128},
       }) {
    const std::optional<std::string> configured = value(field.yaml);
    if (!configured.has_value()) {
      continue;
    }
    if (configured->empty() || configured->size() > field.maximum ||
        std::any_of(configured->begin(), configured->end(),
                    [](unsigned char byte) {
                      return byte < 0x20 || byte == 0x7f;
                    })) {
      *error = std::string(field.yaml) +
               " must be non-empty, bounded, and contain no control bytes";
      return false;
    }
    (*environment)[std::string(field.variable)] = *configured;
  }
  return true;
}

bool LoadYaml(const std::filesystem::path& path, ValueMap* values, bool* loaded,
              std::string* error) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      *loaded = false;
      return true;
    }
    *error = "cannot open runtime configuration: " + path.string();
    return false;
  }

  struct stat metadata = {};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 ||
      static_cast<std::uintmax_t>(metadata.st_size) >
          kMaximumRuntimeConfigBytes) {
    (void)close(descriptor);
    *error =
        "runtime configuration must be a regular file no larger than 1 MiB";
    return false;
  }

  std::string contents;
  contents.reserve(static_cast<std::size_t>(metadata.st_size));
  std::array<char, 16U * 1024U> buffer = {};
  while (true) {
    const ssize_t bytes = read(descriptor, buffer.data(), buffer.size());
    if (bytes == 0) {
      break;
    }
    if (bytes < 0) {
      if (errno == EINTR) {
        continue;
      }
      (void)close(descriptor);
      *error = "cannot read runtime configuration: " + path.string();
      return false;
    }
    if (contents.size() + static_cast<std::size_t>(bytes) >
        kMaximumRuntimeConfigBytes) {
      (void)close(descriptor);
      *error = "runtime configuration exceeds 1 MiB";
      return false;
    }
    contents.append(buffer.data(), static_cast<std::size_t>(bytes));
  }
  if (close(descriptor) != 0) {
    *error = "cannot close runtime configuration: " + path.string();
    return false;
  }

  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser)) {
    *error = "cannot initialize YAML parser";
    return false;
  }
  yaml_document_t document;
  yaml_parser_set_input_string(
      &parser, reinterpret_cast<const unsigned char*>(contents.data()),
      contents.size());
  const bool parsed = yaml_parser_load(&parser, &document) != 0;
  if (!parsed) {
    *error = "invalid YAML in " + path.string();
    yaml_parser_delete(&parser);
    return false;
  }
  yaml_node_t* root = yaml_document_get_root_node(&document);
  bool valid = true;
  if (root == nullptr || root->type != YAML_MAPPING_NODE) {
    *error = "runtime configuration root must be a mapping";
    valid = false;
  } else {
    std::unordered_set<std::string> top_level_keys;
    for (yaml_node_pair_t* pair = root->data.mapping.pairs.start;
         valid && pair != root->data.mapping.pairs.top; ++pair) {
      const yaml_node_t* key_node =
          yaml_document_get_node(&document, pair->key);
      const yaml_node_t* value_node =
          yaml_document_get_node(&document, pair->value);
      const std::string key = Scalar(key_node);
      if (key.empty() || !top_level_keys.insert(key).second) {
        *error = "runtime configuration has an empty or duplicate key";
        valid = false;
      } else if (key == "version") {
        if (value_node == nullptr || value_node->type != YAML_SCALAR_NODE) {
          *error = "version must be a scalar";
          valid = false;
        } else {
          (*values)["version"] = Scalar(value_node);
        }
      } else if (key == "device") {
        if (value_node != nullptr && value_node->type == YAML_SCALAR_NODE) {
          (*values)["device"] = Scalar(value_node);
        } else if (value_node != nullptr &&
                   value_node->type == YAML_MAPPING_NODE) {
          (*values)["device.__mapping"] = "true";
          valid = ReadMapping(&document, value_node, key, values, error);
        } else {
          *error = "device must be a preset scalar or detailed mapping";
          valid = false;
        }
      } else if (key == "runtime" || key == "appearance" ||
                 key == "graphics" || key == "performance" ||
                 key == "audio" || key == "window" || key == "input" ||
                 key == "compatibility" || key == "network") {
        valid = ReadMapping(&document, value_node, key, values, error);
      } else if (key == "integrations") {
        valid = ReadNestedMapping(&document, value_node, key, 2, values, error);
      }
      // Other top-level sections are owned by subsystems such as the
      // updater. This loader ignores rather than misparses them.
    }
  }
  yaml_document_delete(&document);
  yaml_parser_delete(&parser);
  *loaded = valid;
  return valid;
}

std::string FrameRateValue(const FrameRatePolicy& policy) {
  if (policy.mode == FrameRateLimitMode::kUnlimited) {
    return "unlimited";
  }
  if (policy.mode == FrameRateLimitMode::kFixed) {
    return std::to_string(policy.fixed_fps);
  }
  return "display";
}

bool SetEnvironmentValue(const char* name, const std::string& value,
                         std::string* error) {
  if (setenv(name, value.c_str(), 1) == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string("cannot export resolved runtime setting: ") + name;
  }
  return false;
}

bool UnsetEnvironmentValue(const char* name, std::string* error) {
  if (unsetenv(name) == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string("cannot clear resolved runtime setting: ") + name;
  }
  return false;
}

}  // namespace

RuntimeConfigLoadResult LoadRuntimeConfig(
    const Environment& environment, const std::filesystem::path& config_file) {
  RuntimeConfigLoadResult result;
  ValueMap yaml;
  if (!LoadYaml(config_file, &yaml, &result.file_loaded, &result.error)) {
    return result;
  }
  ValueMap fallback;
  if (result.file_loaded && !ValidateAndMap(yaml, &fallback, &result.error)) {
    return result;
  }
  const LayeredEnvironment layered(environment, std::move(fallback));
  result.config = RuntimeConfig::FromEnvironment(layered);
  if (!result.config.frame_rate().valid()) {
    result.error = "frame-rate policy is invalid";
  } else if (!result.config.device_profile_valid()) {
    result.error = "device profile is invalid";
  } else if (!result.config.input_capabilities().touch_enabled &&
             !result.config.input_capabilities().mouse_enabled &&
             !result.config.input_capabilities().keyboard_enabled) {
    result.error = "device must expose at least one usable input capability";
  } else if (result.config.graphics_backend() == GraphicsBackend::kUnknown) {
    result.error = "graphics backend is invalid";
  } else if (!result.config.theme_mode_valid()) {
    result.error = "theme mode is invalid";
  } else if (result.config.vsync_mode() != "auto" &&
             result.config.vsync_mode() != "on" &&
             result.config.vsync_mode() != "off") {
    result.error = "VSync policy is invalid";
  } else if (!result.config.audio_output_device_valid()) {
    result.error = "audio output device is invalid";
  } else if (!result.config.audio_input_device_valid()) {
    result.error = "audio input device is invalid";
  } else if (!result.config.performance().memory_limit_valid) {
    result.error = "memory-limit policy is invalid";
  } else if (!result.config.performance().game_mode_valid) {
    result.error = "GameMode policy is invalid";
  } else if (!result.config.performance().physics_worker_mode_valid) {
    result.error = "physics worker policy is invalid";
  } else if (!result.config.ca_bundle_valid()) {
    result.error = "CA bundle path is invalid";
  } else if (!result.config.discord_rpc_valid()) {
    result.error = "Discord Rich Presence configuration is invalid";
  }
  return result;
}

bool ExportRuntimeConfigEnvironment(const RuntimeConfig& config,
                                    std::string* error) {
  if (!config.device_profile_valid()) {
    if (error != nullptr) {
      *error = "cannot export an invalid device profile";
    }
    return false;
  }
  if (!config.audio_output_device_valid()) {
    if (error != nullptr) {
      *error = "cannot export an invalid audio output device";
    }
    return false;
  }
  if (!config.audio_input_device_valid()) {
    if (error != nullptr) {
      *error = "cannot export an invalid audio input device";
    }
    return false;
  }
  if (!config.performance().memory_limit_valid) {
    if (error != nullptr) {
      *error = "cannot export an invalid memory-limit policy";
    }
    return false;
  }
  if (!config.performance().game_mode_valid) {
    if (error != nullptr) {
      *error = "cannot export an invalid GameMode policy";
    }
    return false;
  }
  if (!config.performance().physics_worker_mode_valid) {
    if (error != nullptr) {
      *error = "cannot export an invalid physics worker policy";
    }
    return false;
  }
  if (!config.ca_bundle_valid()) {
    if (error != nullptr) {
      *error = "cannot export an invalid CA bundle path";
    }
    return false;
  }
  if (!config.discord_rpc_valid()) {
    if (error != nullptr) {
      *error = "cannot export an invalid Discord Rich Presence policy";
    }
    return false;
  }
  const DeviceProfile& device = config.device_profile();
  const bool base_exported =
      SetEnvironmentValue("MOCKTAIL_HEADLESS", config.headless() ? "1" : "0",
                          error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_PROFILE", std::string(device.name),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_CLASS",
                          std::string(DeviceClassName(device.device_class)),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_PLATFORM_NAME",
                          std::string(device.platform_name), error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_NAME",
                          std::string(device.display_name), error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_MANUFACTURER",
                          std::string(device.manufacturer), error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_MODEL", std::string(device.model),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_BRAND", std::string(device.brand),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_CODE",
                          std::string(device.device_code), error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_SKU", std::string(device.device_sku),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_DEVICE_SOC_MODEL",
                          std::string(device.soc_model), error) &&
      SetEnvironmentValue("ROBLOX_LIB_PATH",
                          config.roblox_library_path().string(), error) &&
      SetEnvironmentValue("MOCKTAIL_GRAPHICS_BACKEND",
                          config.graphics_backend_name(), error) &&
      SetEnvironmentValue("MOCKTAIL_THEME", config.theme_mode(), error) &&
      SetEnvironmentValue("MOCKTAIL_WIN_WIDTH",
                          std::to_string(config.window().width), error) &&
      SetEnvironmentValue("MOCKTAIL_WIN_HEIGHT",
                          std::to_string(config.window().height), error) &&
      SetEnvironmentValue("MOCKTAIL_WIN_TITLE", config.window().title, error) &&
      SetEnvironmentValue(
          "MOCKTAIL_TOUCH_MODE",
          config.input_capabilities().touch_enabled ? "on" : "off", error) &&
      SetEnvironmentValue(
          "MOCKTAIL_MOUSE_MODE",
          config.input_capabilities().mouse_enabled ? "on" : "off", error) &&
      SetEnvironmentValue(
          "MOCKTAIL_KEYBOARD_MODE",
          config.input_capabilities().keyboard_enabled ? "on" : "off", error) &&
      SetEnvironmentValue("MOCKTAIL_DESKTOP_PLAYABILITY",
                          config.desktop_playability() ? "1" : "0", error) &&
      SetEnvironmentValue("MOCKTAIL_FRAME_RATE_LIMIT",
                          FrameRateValue(config.frame_rate()), error) &&
      SetEnvironmentValue("MOCKTAIL_VSYNC", config.vsync_mode(), error) &&
      SetEnvironmentValue(
          "MOCKTAIL_MULTITHREADED_RENDERING",
          config.performance().multithreaded_rendering ? "1" : "0", error) &&
      SetEnvironmentValue("MOCKTAIL_PHYSICS_WORKER_MODE",
                          std::string(PhysicsWorkerModeName(
                              config.performance().physics_worker_mode)),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_MEMORY_LIMIT_MB",
                          std::to_string(config.performance().memory_limit_mb),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_GAMEMODE",
                          GameModePolicyName(config.performance().game_mode),
                          error) &&
      SetEnvironmentValue("MOCKTAIL_AUDIO_OUTPUT_DEVICE",
                          config.audio_output_device(), error) &&
      SetEnvironmentValue("MOCKTAIL_AUDIO_INPUT_DEVICE",
                          config.audio_input_device(), error) &&
      SetEnvironmentValue("MOCKTAIL_USE_SYSTEM_PROXY",
                          config.use_system_proxy() ? "1" : "0", error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_ENABLED",
                          config.discord_rpc().enabled ? "1" : "0", error) &&
      SetEnvironmentValue(
          "MOCKTAIL_DISCORD_RPC_SHOW_PLACE_NAME",
          config.discord_rpc().show_place_name ? "1" : "0", error) &&
      SetEnvironmentValue(
          "MOCKTAIL_DISCORD_RPC_SHOW_ELAPSED_TIME",
          config.discord_rpc().show_elapsed_time ? "1" : "0", error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_JOIN_ENABLED",
                          config.discord_rpc().join_enabled ? "1" : "0",
                          error) &&
      SetEnvironmentValue(
          "MOCKTAIL_DISCORD_RPC_PUBLIC_SERVERS_ONLY",
          config.discord_rpc().public_servers_only ? "1" : "0", error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_JOIN_BUTTON_LABEL",
                          config.discord_rpc().join_button_label, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_APPLICATION_ID",
                          config.discord_rpc().application_id, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_TEXT_BROWSING",
                          config.discord_rpc().text.browsing, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_TEXT_JOINING",
                          config.discord_rpc().text.joining, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_TEXT_PLAYING",
                          config.discord_rpc().text.playing, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_TEXT_STATE",
                          config.discord_rpc().text.state, error) &&
      SetEnvironmentValue("MOCKTAIL_DISCORD_RPC_TEXT_UNKNOWN_PLACE",
                          config.discord_rpc().text.unknown_place, error);
  if (!base_exported) {
    return false;
  }
  if (config.ca_bundle().has_value()) {
    if (!SetEnvironmentValue("MOCKTAIL_CA_BUNDLE",
                             config.ca_bundle()->string(), error)) {
      return false;
    }
  } else if (!UnsetEnvironmentValue("MOCKTAIL_CA_BUNDLE", error)) {
    return false;
  }
  if (config.roblox_http_user_agent().has_value() &&
      !SetEnvironmentValue("MOCKTAIL_USER_AGENT",
                           *config.roblox_http_user_agent(), error)) {
    return false;
  }
  if (!config.roblox_http_user_agent().has_value() &&
      !UnsetEnvironmentValue("MOCKTAIL_USER_AGENT", error)) {
    return false;
  }
  if (!config.network_proxy().has_value()) {
    return base_exported;
  }
  return SetEnvironmentValue("MOCKTAIL_HTTP_PROXY_HOST",
                             config.network_proxy()->host, error) &&
         SetEnvironmentValue("MOCKTAIL_HTTP_PROXY_PORT",
                             std::to_string(config.network_proxy()->port),
                             error) &&
         SetEnvironmentValue("MOCKTAIL_HTTP_PROXY_SCHEME",
                             config.network_proxy()->scheme, error) &&
         SetEnvironmentValue("MOCKTAIL_NATIVE_SET_HTTP_CLIENT_PROXY", "1",
                             error);
}

}  // namespace runtime
}  // namespace mocktail
