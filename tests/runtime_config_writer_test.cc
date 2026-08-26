#include "runtime/runtime_config_writer.h"

#include "runtime/runtime_config_file.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_config_writer_XXXXXX";
    char* created = mkdtemp(pattern);
    if (created != nullptr) {
      path_ = created;
    }
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string Apply(std::string_view yaml,
                  const std::vector<ConfigAssignment>& assignments) {
  std::string updated;
  std::string error;
  EXPECT_TRUE(ApplyConfigAssignments(yaml, assignments, &updated, &error))
      << error;
  return updated;
}

constexpr char kDocumentedConfig[] = R"(# Mocktail configuration.
# The file is created once with mode 0600. Mocktail never replaces your edits.

# Integer: configuration schema version. Only version 1 is supported.
version: 1

# One-line device preset. Short aliases pc, mobile, and console are accepted.
device: pc-windows-11

graphics:
  # String (default: direct-vulkan): graphics backend.
  backend: direct-vulkan
  # String or integer (default: display): requested frame-rate policy.
  frame_rate_limit: display
  # String (default: auto): presentation synchronization: auto, on, or off.
  vsync: auto

network:
  # Boolean (default: false): follow the host HTTP/SOCKS5 proxy.
  use_system_proxy: false
  # String (optional, default: disabled): proxy host name or IP address.
  # proxy_host: 127.0.0.1
  # Integer (optional, default: disabled): proxy TCP port from 1 to 65535.
  # proxy_port: 8080
)";

TEST(RuntimeConfigWriterTest, KeepsEveryCommentAndUntouchedValue) {
  const std::string updated =
      Apply(kDocumentedConfig, {{{"graphics", "backend"}, "opengl"}});

  EXPECT_NE(updated.find("# Mocktail configuration."), std::string::npos);
  EXPECT_NE(updated.find("# String (default: direct-vulkan): graphics backend."),
            std::string::npos);
  EXPECT_NE(updated.find("  backend: opengl"), std::string::npos);
  EXPECT_EQ(updated.find("direct-vulkan\n"), std::string::npos);
  EXPECT_NE(updated.find("  frame_rate_limit: display"), std::string::npos);
  EXPECT_NE(updated.find("version: 1"), std::string::npos);
  // Same number of lines: an existing key is edited, never re-inserted.
  const auto count = [](const std::string& text) {
    return std::count(text.begin(), text.end(), '\n');
  };
  EXPECT_EQ(count(updated), count(kDocumentedConfig));
}

TEST(RuntimeConfigWriterTest, EditsSeveralKeysAcrossSections) {
  const std::string updated = Apply(kDocumentedConfig,
                                    {
                                        {{"graphics", "vsync"}, "off"},
                                        {{"graphics", "frame_rate_limit"}, "144"},
                                        {{"network", "use_system_proxy"}, "true"},
                                        {{"device"}, "mobile-pixel-7"},
                                    });

  EXPECT_NE(updated.find("  vsync: off"), std::string::npos);
  EXPECT_NE(updated.find("  frame_rate_limit: 144"), std::string::npos);
  EXPECT_NE(updated.find("  use_system_proxy: true"), std::string::npos);
  EXPECT_NE(updated.find("device: mobile-pixel-7"), std::string::npos);
}

TEST(RuntimeConfigWriterTest, InsertsAMissingKeyAtItsDocumentedSlot) {
  const std::string updated =
      Apply(kDocumentedConfig, {{{"network", "proxy_host"}, "10.0.0.2"}});

  const std::size_t documentation =
      updated.find("  # String (optional, default: disabled): proxy host");
  const std::size_t inserted = updated.find("  proxy_host: 10.0.0.2");
  const std::size_t template_line = updated.find("  # proxy_host: 127.0.0.1");
  ASSERT_NE(inserted, std::string::npos);
  ASSERT_NE(template_line, std::string::npos);
  // The value lands right after the commented template, under its own comment.
  EXPECT_LT(documentation, template_line);
  EXPECT_LT(template_line, inserted);
  EXPECT_LT(inserted, updated.find("  # Integer (optional, default: disabled)"));
}

TEST(RuntimeConfigWriterTest, AppendsAMissingSection) {
  const std::string updated =
      Apply(kDocumentedConfig, {{{"audio", "output_device"}, "default"}});

  EXPECT_NE(updated.find("audio:"), std::string::npos);
  EXPECT_NE(updated.find("  output_device: default"), std::string::npos);
  EXPECT_NE(updated.find("version: 1"), std::string::npos);
  EXPECT_NE(updated.find("  use_system_proxy: false"), std::string::npos);
}

TEST(RuntimeConfigWriterTest, RefusesToFlattenAMappingDeviceProfile) {
  constexpr char kMappingDevice[] = R"(version: 1
device:
  type: mobile
  name: Google Pixel 9 Pro
  touch: true
)";
  std::string updated;
  std::string error;
  EXPECT_FALSE(ApplyConfigAssignments(kMappingDevice,
                                      {{{"device"}, "pc-windows-11"}}, &updated,
                                      &error));
  EXPECT_NE(error.find("opens a section"), std::string::npos);
  // A nested assignment into the same mapping is still allowed.
  EXPECT_NE(Apply(kMappingDevice, {{{"device", "touch"}, "false"}})
                .find("  touch: false"),
            std::string::npos);
}

TEST(RuntimeConfigWriterTest, RefusesToDescendIntoAScalar) {
  std::string updated;
  std::string error;
  EXPECT_FALSE(ApplyConfigAssignments(kDocumentedConfig,
                                      {{{"version", "minor"}, "2"}}, &updated,
                                      &error));
  EXPECT_NE(error.find("holds a value"), std::string::npos);
  EXPECT_FALSE(ApplyConfigAssignments(kDocumentedConfig, {{{}, "x"}}, &updated,
                                      &error));
}

TEST(RuntimeConfigWriterTest, PreservesATrailingComment) {
  const std::string updated =
      Apply("graphics:\n  vsync: auto # keep me\n", {{{"graphics", "vsync"}, "on"}});
  EXPECT_EQ(updated, "graphics:\n  vsync: on # keep me\n");
}

TEST(RuntimeConfigWriterTest, QuotesOnlyWhatWouldNotRoundTrip) {
  EXPECT_EQ(EncodeConfigScalar("Roblox"), "Roblox");
  EXPECT_EQ(EncodeConfigScalar("direct-vulkan"), "direct-vulkan");
  EXPECT_EQ(EncodeConfigScalar("144"), "144");
  EXPECT_EQ(EncodeConfigScalar("My Device 2"), "My Device 2");
  EXPECT_EQ(EncodeConfigScalar(""), "\"\"");
  EXPECT_EQ(EncodeConfigScalar(" padded "), "\" padded \"");
  EXPECT_EQ(EncodeConfigScalar("a: b"), "\"a: b\"");
  EXPECT_EQ(EncodeConfigScalar("name #1"), "\"name #1\"");
  EXPECT_EQ(EncodeConfigScalar("{place_name}"), "\"{place_name}\"");
  EXPECT_EQ(EncodeConfigScalar("say \"hi\""), "\"say \\\"hi\\\"\"");
}

TEST(RuntimeConfigWriterTest, EncodedScalarSurvivesTheWriter) {
  const std::string updated = Apply(
      "window:\n  title: Roblox\n",
      {{{"window", "title"}, EncodeConfigScalar("My # Window")}});
  EXPECT_EQ(updated, "window:\n  title: \"My # Window\"\n");
}

TEST(RuntimeConfigWriterTest, HandlesAnEmptyOrNewlineFreeDocument) {
  EXPECT_EQ(Apply("", {{{"graphics", "vsync"}, "on"}}),
            "graphics:\n  vsync: on\n");
  // No trailing newline in, none added.
  EXPECT_EQ(Apply("graphics:\n  vsync: auto", {{{"graphics", "vsync"}, "on"}}),
            "graphics:\n  vsync: on");
}

TEST(RuntimeConfigWriterTest, WritesAtomicallyWithOwnerOnlyMode) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path config = temporary.path() / "config.yaml";
  {
    std::ofstream output(config);
    output << kDocumentedConfig;
  }
  ASSERT_EQ(chmod(config.c_str(), 0600), 0);

  std::string error;
  ASSERT_TRUE(WriteConfigAssignments(
      config, {{{"graphics", "backend"}, "opengl"}}, &error))
      << error;

  std::ifstream input(config);
  std::stringstream buffer;
  buffer << input.rdbuf();
  EXPECT_NE(buffer.str().find("  backend: opengl"), std::string::npos);
  EXPECT_NE(buffer.str().find("# Mocktail configuration."), std::string::npos);

  struct stat metadata{};
  ASSERT_EQ(stat(config.c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & 0777, 0600u);
  // No temporary file is left behind.
  for (const auto& entry : std::filesystem::directory_iterator(temporary.path())) {
    EXPECT_EQ(entry.path().filename().string().find(".mocktail-config.tmp."),
              std::string::npos);
  }
}

TEST(RuntimeConfigWriterTest, CreatesTheFileWhenItIsMissing) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path config = temporary.path() / "new/config.yaml";
  std::string error;
  ASSERT_TRUE(WriteConfigAssignments(config, {{{"graphics", "vsync"}, "off"}},
                                     &error))
      << error;
  std::ifstream input(config);
  std::stringstream buffer;
  buffer << input.rdbuf();
  EXPECT_EQ(buffer.str(), "graphics:\n  vsync: off\n");
}

TEST(RuntimeConfigWriterTest, RejectsSymlinkedAndRelativeTargets) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path real = temporary.path() / "real.yaml";
  {
    std::ofstream output(real);
    output << "graphics:\n  vsync: auto\n";
  }
  const std::filesystem::path link = temporary.path() / "link.yaml";
  std::error_code code;
  std::filesystem::create_symlink(real, link, code);
  ASSERT_FALSE(code);

  std::string error;
  EXPECT_FALSE(
      WriteConfigAssignments(link, {{{"graphics", "vsync"}, "on"}}, &error));
  EXPECT_NE(error.find("symlink"), std::string::npos);
  EXPECT_FALSE(WriteConfigAssignments("relative.yaml",
                                      {{{"graphics", "vsync"}, "on"}}, &error));
}

TEST(RuntimeConfigWriterTest, LeavesTheFileAloneWhenNothingChanges) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path config = temporary.path() / "config.yaml";
  {
    std::ofstream output(config);
    output << kDocumentedConfig;
  }
  const auto before = std::filesystem::last_write_time(config);
  std::string error;
  ASSERT_TRUE(WriteConfigAssignments(
      config, {{{"graphics", "backend"}, "direct-vulkan"}}, &error))
      << error;
  EXPECT_EQ(std::filesystem::last_write_time(config), before);
}

#ifdef MOCKTAIL_EXAMPLE_CONFIG_PATH
class NoEnvironment final : public Environment {
 public:
  std::optional<std::string> Get(std::string_view) const override {
    return std::nullopt;
  }
};

std::size_t CountCommentLines(const std::string& text) {
  std::size_t count = 0;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    const std::size_t indent = line.find_first_not_of(' ');
    if (indent != std::string::npos && line[indent] == '#') {
      ++count;
    }
  }
  return count;
}

// The shipped config is the writer's real contract: 80+ comment lines that a
// YAML round-trip would delete, plus every documented key.
TEST(RuntimeConfigWriterTest, RewritesTheShippedExampleConfigLosslessly) {
  std::ifstream input(MOCKTAIL_EXAMPLE_CONFIG_PATH);
  ASSERT_TRUE(input.good()) << MOCKTAIL_EXAMPLE_CONFIG_PATH;
  std::stringstream buffer;
  buffer << input.rdbuf();
  const std::string original = buffer.str();
  ASSERT_FALSE(original.empty());

  const std::string updated =
      Apply(original, {
                          {{"device"}, "mobile-pixel-7"},
                          {{"runtime", "headless"}, "true"},
                          {{"appearance", "theme"}, "dark"},
                          {{"graphics", "backend"}, "opengl"},
                          {{"graphics", "frame_rate_limit"}, "144"},
                          {{"graphics", "vsync"}, "off"},
                          {{"performance", "multithreaded_rendering"}, "true"},
                          {{"performance", "physics_worker_mode"}, "latency"},
                          {{"performance", "memory_limit_mb"}, "6144"},
                          {{"performance", "gamemode"}, "on"},
                          {{"audio", "input_device"}, "disabled"},
                          {{"window", "width"}, "1920"},
                          {{"window", "height"}, "1080"},
                          {{"window", "title"}, EncodeConfigScalar("My # Roblox")},
                          {{"network", "use_system_proxy"}, "true"},
                          {{"integrations", "discord_rpc", "enabled"}, "true"},
                          {{"integrations", "discord_rpc", "join",
                            "public_servers_only"},
                           "false"},
                          {{"updates", "automatic"}, "false"},
                      });

  EXPECT_EQ(CountCommentLines(updated), CountCommentLines(original));
  EXPECT_EQ(std::count(updated.begin(), updated.end(), '\n'),
            std::count(original.begin(), original.end(), '\n'));

  // The result must still parse, and every value must survive the trip.
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path config = temporary.path() / "config.yaml";
  {
    std::ofstream output(config);
    output << updated;
  }
  const NoEnvironment environment;
  const RuntimeConfigLoadResult loaded = LoadRuntimeConfig(environment, config);
  ASSERT_TRUE(loaded) << loaded.error;
  EXPECT_TRUE(loaded.file_loaded);
  EXPECT_EQ(loaded.config.device_profile().name, "mobile-pixel-7");
  EXPECT_EQ(loaded.config.device_profile().device_class, DeviceClass::kMobile);
  EXPECT_TRUE(loaded.config.headless());
  EXPECT_EQ(loaded.config.theme_mode(), "dark");
  EXPECT_EQ(loaded.config.graphics_backend_name(), "opengl");
  EXPECT_EQ(loaded.config.vsync_mode(), "off");
  EXPECT_TRUE(loaded.config.performance().multithreaded_rendering);
  EXPECT_EQ(loaded.config.performance().physics_worker_mode,
            PhysicsWorkerMode::kLatency);
  EXPECT_EQ(loaded.config.performance().memory_limit_mb, 6144u);
  EXPECT_EQ(loaded.config.window().width, 1920);
  EXPECT_EQ(loaded.config.window().height, 1080);
  EXPECT_EQ(loaded.config.window().title, "My # Roblox");
  EXPECT_FALSE(loaded.config.microphone_enabled());
  EXPECT_TRUE(loaded.config.use_system_proxy());
  EXPECT_TRUE(loaded.config.discord_rpc().enabled);
  EXPECT_FALSE(loaded.config.discord_rpc().public_servers_only);
}
#endif

}  // namespace
}  // namespace runtime
}  // namespace mocktail
