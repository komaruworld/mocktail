#include "runtime/graphics_launch_policy.h"
#include "runtime/supported_launch_policy.h"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

int RunPolicyProbe(bool interactive, bool explicit_override) {
  for (const char* name :
       {"MOCKTAIL_SKIP_LIBROBLOX_CTORS", "MOCKTAIL_INIT_CLIENT_SETTINGS",
        "MOCKTAIL_GRAPHICS_BACKEND", "MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS",
        "MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP"}) {
    if (unsetenv(name) != 0) {
      return 10;
    }
  }
  if (explicit_override) {
    if (setenv("MOCKTAIL_GRAPHICS_BACKEND", "custom-backend", 1) != 0 ||
        setenv("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "0", 1) != 0) {
      return 11;
    }
  }
  std::string error;
  if (!ApplySupportedLaunchPolicy(interactive, &error)) {
    return 12;
  }
  if (!interactive) {
    return getenv("MOCKTAIL_INIT_CLIENT_SETTINGS") == nullptr &&
                   getenv("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP") == nullptr
               ? 0
               : 13;
  }
  const char* skip_constructors = getenv("MOCKTAIL_SKIP_LIBROBLOX_CTORS");
  const char* initialize_settings = getenv("MOCKTAIL_INIT_CLIENT_SETTINGS");
  const char* auto_exit = getenv("MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS");
  const char* allow_guest = getenv("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP");
  if (skip_constructors == nullptr || initialize_settings == nullptr ||
      auto_exit == nullptr || allow_guest == nullptr ||
      std::string(skip_constructors) != "0" ||
      std::string(initialize_settings) != "1" ||
      std::string(auto_exit) != "0" ||
      std::string(allow_guest) != (explicit_override ? "0" : "1")) {
    return 14;
  }
  const char* graphics = getenv("MOCKTAIL_GRAPHICS_BACKEND");
  if (explicit_override) {
    return graphics != nullptr && std::string(graphics) == "custom-backend"
               ? 0
               : 15;
  }
  return graphics == nullptr ? 0 : 16;
}

void ExpectPolicyProbe(bool interactive, bool explicit_override) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    std::_Exit(RunPolicyProbe(interactive, explicit_override));
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(SupportedLaunchPolicyTest, PublishesInteractiveDefaults) {
  ExpectPolicyProbe(true, false);
}

TEST(SupportedLaunchPolicyTest, PreservesExplicitOverrides) {
  ExpectPolicyProbe(true, true);
}

TEST(SupportedLaunchPolicyTest, LeavesResearchModesUnchanged) {
  ExpectPolicyProbe(false, false);
}

void ExpectPackagedManifestProbe(bool candidate_override) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    constexpr const char* packaged = "/relocated AppDir/share/mocktail/default.json";
    constexpr const char* candidate = "/private-canary/candidate.json";
    if (setenv("MOCKTAIL_PACKAGED_COMPATIBILITY_MANIFEST", packaged, 1) != 0 ||
        unsetenv("MOCKTAIL_COMPATIBILITY_MANIFEST") != 0) {
      std::_Exit(10);
    }
    if (candidate_override &&
        setenv("MOCKTAIL_COMPATIBILITY_MANIFEST", candidate, 1) != 0) {
      std::_Exit(11);
    }
    std::string error;
    if (!ApplySupportedLaunchPolicy(false, &error)) std::_Exit(12);
    const char* manifest = getenv("MOCKTAIL_COMPATIBILITY_MANIFEST");
    std::_Exit(manifest != nullptr &&
                       std::string(manifest) == (candidate_override ? candidate : packaged)
                   ? 0 : 13);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(SupportedLaunchPolicyTest, UsesRelocatedPackagedManifestByDefault) {
  ExpectPackagedManifestProbe(false);
}

TEST(SupportedLaunchPolicyTest, PreservesCandidateManifestOverPackagedDefault) {
  ExpectPackagedManifestProbe(true);
}

int RunGraphicsPolicyProbe(const char* backend) {
  for (const char* name : {
           "MOCKTAIL_GRAPHICS_BACKEND",
           "MOCKTAIL_PRELOAD_VULKAN_SHIM",
           "MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK",
           "MOCKTAIL_SOFTWARE_WINDOW_FALLBACK",
           "MOCKTAIL_REQUIRE_REAL_GRAPHICS",
           "MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON",
           "ANV_SYS_MEM_LIMIT",
           "MESA_VK_ENABLE_SUBMIT_THREAD",
       }) {
    if (unsetenv(name) != 0) return 20;
  }
  if (backend != nullptr &&
      setenv("MOCKTAIL_GRAPHICS_BACKEND", backend, 1) != 0) {
    return 21;
  }
  const ProcessEnvironment environment;
  const RuntimeConfig config = RuntimeConfig::FromEnvironment(environment);
  std::string error;
  if (!ApplyGraphicsLaunchPolicy(config, &error)) return 22;

  const bool open_gl = backend != nullptr && std::string(backend) == "opengl";
  const char* resolved = getenv("MOCKTAIL_GRAPHICS_BACKEND");
  const char* preload = getenv("MOCKTAIL_PRELOAD_VULKAN_SHIM");
  if (resolved == nullptr || preload == nullptr ||
      std::string(resolved) != (open_gl ? "opengl" : "direct-vulkan") ||
      std::string(preload) != (open_gl ? "0" : "1")) {
    return 23;
  }
  if (open_gl) {
    const char* disable_angle =
        getenv("MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK");
    const char* software = getenv("MOCKTAIL_SOFTWARE_WINDOW_FALLBACK");
    return disable_angle != nullptr && software != nullptr &&
                   std::string(disable_angle) == "1" &&
                   std::string(software) == "0" &&
                   getenv("MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON") == nullptr &&
                   getenv("ANV_SYS_MEM_LIMIT") == nullptr &&
                   getenv("MESA_VK_ENABLE_SUBMIT_THREAD") == nullptr
               ? 0
               : 24;
  }
  const char* overrides = getenv("MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON");
  const char* anv_memory_limit = getenv("ANV_SYS_MEM_LIMIT");
  const char* submit_thread = getenv("MESA_VK_ENABLE_SUBMIT_THREAD");
  std::string expected_anv_limit = "50";
  {
    std::ifstream input("/proc/meminfo");
    std::string key;
    unsigned long kb = 0;
    std::string unit;
    while (input >> key >> kb >> unit) {
      if (key == "MemTotal:") {
        expected_anv_limit = kb > 4UL * 1024UL * 1024UL ? "75" : "50";
        break;
      }
    }
  }
  return overrides != nullptr && anv_memory_limit != nullptr &&
                 submit_thread != nullptr &&
                 std::string(anv_memory_limit) == expected_anv_limit &&
                 std::string(submit_thread) == "1" &&
                 std::string(overrides).find(
                     "\"FStringGraphicsTextureManager2DenyPattern2\":\".*\"") !=
                     std::string::npos &&
                 std::string(overrides).find(
                     "FStringGraphicsVulkanShaderMTDenyPattern") !=
                     std::string::npos &&
                 std::string(overrides).find(
                     "\"FFlagTextureTranscodeNewRollout\":\"True\"") !=
                     std::string::npos &&
                 std::string(overrides).find(
                     "\"FStringTextureTranscodeRollout\":\"\"") !=
                     std::string::npos
             ? 0
             : 25;
}

void ExpectGraphicsPolicyProbe(const char* backend) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    std::_Exit(RunGraphicsPolicyProbe(backend));
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(GraphicsLaunchPolicyTest, DefaultsToDirectVulkanAfterConfigResolution) {
  ExpectGraphicsPolicyProbe(nullptr);
}

TEST(GraphicsLaunchPolicyTest, MakesOpenGlStrictAndVulkanIndependent) {
  ExpectGraphicsPolicyProbe("opengl");
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
