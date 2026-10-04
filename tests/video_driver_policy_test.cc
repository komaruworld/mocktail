#include "window/video_driver_policy.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "window/window.h"

namespace mocktail {
namespace window {
namespace {

VideoDriverPolicyInput NvidiaWaylandDirectVulkan() {
  VideoDriverPolicyInput input;
  input.prefer_wayland = true;
  input.has_wayland_session = true;
  input.has_x11_display = true;
  input.uses_direct_vulkan = true;
  input.has_nvidia_kernel_driver = true;
  return input;
}

TEST(VideoDriverPolicyTest, UsesXwaylandForNvidiaDirectVulkanByDefault) {
  EXPECT_EQ(ResolveVideoDriverChoice(NvidiaWaylandDirectVulkan()),
            VideoDriverChoice::kNvidiaDirectVulkanX11);
}

TEST(VideoDriverPolicyTest, ExplicitSdlDriverRemainsAuthoritative) {
  VideoDriverPolicyInput input = NvidiaWaylandDirectVulkan();
  input.has_explicit_sdl_driver = true;

  EXPECT_EQ(ResolveVideoDriverChoice(input), VideoDriverChoice::kSdlDefault);
}

TEST(VideoDriverPolicyTest, ForceWaylandOverridesNvidiaFallback) {
  VideoDriverPolicyInput input = NvidiaWaylandDirectVulkan();
  input.force_wayland = true;

  EXPECT_EQ(ResolveVideoDriverChoice(input), VideoDriverChoice::kWayland);
}

TEST(VideoDriverPolicyTest, ForceX11WinsForAvailableDisplay) {
  VideoDriverPolicyInput input;
  input.force_x11 = true;
  input.prefer_wayland = true;
  input.has_wayland_session = true;
  input.has_x11_display = true;

  EXPECT_EQ(ResolveVideoDriverChoice(input), VideoDriverChoice::kX11);
}

TEST(VideoDriverPolicyTest, KeepsWaylandForNonNvidiaAndNonDirectBackends) {
  VideoDriverPolicyInput input = NvidiaWaylandDirectVulkan();
  input.uses_direct_vulkan = false;

  EXPECT_EQ(ResolveVideoDriverChoice(input), VideoDriverChoice::kWayland);

  input.uses_direct_vulkan = true;
  input.has_nvidia_kernel_driver = false;
  EXPECT_EQ(ResolveVideoDriverChoice(input), VideoDriverChoice::kWayland);
}

TEST(VideoDriverPolicyTest, KeepsWaylandWhenXwaylandIsUnavailable) {
  VideoDriverPolicyInput input = NvidiaWaylandDirectVulkan();
  input.has_x11_display = false;

  EXPECT_EQ(ResolveVideoDriverChoice(input), VideoDriverChoice::kWayland);
}

TEST(VideoDriverPolicyTest, UsesSdlDefaultWhenWaylandPreferenceIsDisabled) {
  VideoDriverPolicyInput input;
  input.prefer_wayland = false;
  input.has_wayland_session = true;
  input.has_x11_display = true;

  EXPECT_EQ(ResolveVideoDriverChoice(input), VideoDriverChoice::kSdlDefault);
}

TEST(VideoDriverPolicyTest, NamesOnlySelectedDrivers) {
  EXPECT_STREQ(VideoDriverChoiceName(VideoDriverChoice::kWayland), "wayland");
  EXPECT_STREQ(VideoDriverChoiceName(VideoDriverChoice::kX11), "x11");
  EXPECT_STREQ(VideoDriverChoiceName(VideoDriverChoice::kNvidiaDirectVulkanX11),
               "x11");
  EXPECT_EQ(VideoDriverChoiceName(VideoDriverChoice::kSdlDefault), nullptr);
}

TEST(VideoDriverPolicyTest, AcceptsAnyCompiledDriverFromPriorityList) {
  const std::vector<std::string_view> available = {"wayland", "x11", "dummy"};

  EXPECT_TRUE(HasAvailableVideoDriverCandidate("x11", available));
  EXPECT_TRUE(HasAvailableVideoDriverCandidate("unavailable,x11", available));
  EXPECT_TRUE(
      HasAvailableVideoDriverCandidate("wayland,unavailable", available));
}

TEST(VideoDriverPolicyTest, RejectsUnavailableOrEmptyDriverList) {
  const std::vector<std::string_view> available = {"wayland", "x11"};

  EXPECT_FALSE(HasAvailableVideoDriverCandidate("sdl3", available));
  EXPECT_FALSE(HasAvailableVideoDriverCandidate("sdl3,missing", available));
  EXPECT_FALSE(HasAvailableVideoDriverCandidate("", available));
}

class NvidiaDriverDetectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/mocktail_video_driver_XXXXXX";
    const char* created = mkdtemp(pattern);
    ASSERT_NE(created, nullptr);
    root_ = created;
    proc_version_ = root_ / "proc/driver/nvidia/version";
    pci_drivers_ = root_ / "sys/bus/pci/drivers";
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  bool Detect() const {
    return HasNvidiaKernelDriver(proc_version_, pci_drivers_ / "nvidia");
  }

  std::filesystem::path root_;
  std::filesystem::path proc_version_;
  std::filesystem::path pci_drivers_;
};

TEST_F(NvidiaDriverDetectionTest, DetectsProcDriverWithoutSysfs) {
  std::filesystem::create_directories(proc_version_.parent_path());
  std::ofstream(proc_version_) << "NVRM version: NVIDIA UNIX Kernel Module\n";

  EXPECT_TRUE(Detect());
}

TEST_F(NvidiaDriverDetectionTest, HybridGpuUsesXwaylandWhenProcIsHidden) {
  std::filesystem::create_directories(pci_drivers_ / "i915");
  std::filesystem::create_directories(pci_drivers_ / "nvidia");
  VideoDriverPolicyInput input = NvidiaWaylandDirectVulkan();
  input.has_nvidia_kernel_driver = Detect();

  EXPECT_TRUE(input.has_nvidia_kernel_driver);
  EXPECT_EQ(ResolveVideoDriverChoice(input),
            VideoDriverChoice::kNvidiaDirectVulkanX11);
  input.force_wayland = true;
  EXPECT_EQ(ResolveVideoDriverChoice(input), VideoDriverChoice::kWayland);
  input.has_explicit_sdl_driver = true;
  EXPECT_EQ(ResolveVideoDriverChoice(input), VideoDriverChoice::kSdlDefault);
}

TEST_F(NvidiaDriverDetectionTest, DoesNotTreatNouveauAsTheNvidiaDriver) {
  std::filesystem::create_directories(pci_drivers_ / "nouveau");
  std::filesystem::create_directories(pci_drivers_ / "i915");
  std::filesystem::create_directories(pci_drivers_ / "amdgpu");

  EXPECT_FALSE(Detect());
}

TEST_F(NvidiaDriverDetectionTest, IgnoresMissingAndUnrelatedDeviceInformation) {
  EXPECT_FALSE(Detect());
  std::filesystem::create_directories(pci_drivers_ / "nvidia_drm");
  std::ofstream(pci_drivers_ / "nvidia") << "not a registered driver\n";
  EXPECT_FALSE(Detect());
}

TEST(VideoDriverPolicyIntegrationTest,
     DropsUnavailableLegacyOverrideBeforeSdlInit) {
  bool has_dummy_driver = false;
  for (int index = 0; index < SDL_GetNumVideoDrivers(); ++index) {
    const char* driver = SDL_GetVideoDriver(index);
    has_dummy_driver = has_dummy_driver ||
                       (driver != nullptr && std::strcmp(driver, "dummy") == 0);
  }
  if (!has_dummy_driver) {
    GTEST_SKIP() << "linked SDL does not provide its non-display dummy driver";
  }

  ASSERT_EQ(setenv("SDL_VIDEODRIVER", "sdl3", 1), 0);
  ASSERT_EQ(setenv("SDL_VIDEO_DRIVER", "dummy", 1), 0);
  ASSERT_EQ(setenv("MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK", "1", 1), 0);
  ASSERT_EQ(setenv("MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS", "1", 1), 0);

  ASSERT_TRUE(Init(320, 180, "SDL video-driver recovery test"));
  EXPECT_EQ(std::getenv("SDL_VIDEODRIVER"), nullptr);
  ASSERT_NE(SDL_GetCurrentVideoDriver(), nullptr);
  EXPECT_STREQ(SDL_GetCurrentVideoDriver(), "dummy");
  Shutdown();
}

class AngleWindowInitializationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/mocktail_angle_window_XXXXXX";
    const char* created = mkdtemp(pattern);
    ASSERT_NE(created, nullptr);
    directory_ = created;
    for (const char* name : {"MOCKTAIL_GRAPHICS_BACKEND", "MOCKTAIL_ANGLE_LIB_DIR",
                             "MOCKTAIL_EGL_LIBRARY", "MOCKTAIL_GLES_LIBRARY",
                             "MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK",
                             "MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS",
                             "MOCKTAIL_SOFTWARE_WINDOW_FALLBACK",
                             "ANGLE_DEFAULT_PLATFORM", "NODEVICE_SELECT",
                             "DISABLE_LAYER_MESA_ANTI_LAG",
                             "SDL_VIDEO_DRIVER", "SDL_VIDEODRIVER",
                             "SDL_EGL_LIBRARY", "SDL_OPENGL_LIBRARY"}) {
      const char* value = std::getenv(name);
      environment_.emplace_back(name, value != nullptr
                                          ? std::optional<std::string>(value)
                                          : std::nullopt);
      ASSERT_EQ(unsetenv(name), 0);
    }
    ASSERT_EQ(setenv("SDL_VIDEO_DRIVER", "dummy", 1), 0);
    ASSERT_EQ(setenv("MOCKTAIL_GRAPHICS_BACKEND", "angle-vulkan", 1), 0);
    ASSERT_EQ(setenv("MOCKTAIL_ANGLE_LIB_DIR", directory_.c_str(), 1), 0);
    ASSERT_EQ(setenv("MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK", "1", 1), 0);
    ASSERT_EQ(setenv("MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS", "1", 1), 0);
  }

  void TearDown() override {
    Shutdown();
    SDL_Quit();
    SDL_ResetHint(SDL_HINT_EGL_LIBRARY);
    SDL_ResetHint(SDL_HINT_OPENGL_LIBRARY);
    SDL_ResetHint(SDL_HINT_OPENGL_ES_DRIVER);
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    for (const auto& [name, value] : environment_) {
      if (value) {
        setenv(name.c_str(), value->c_str(), 1);
      } else {
        unsetenv(name.c_str());
      }
    }
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  std::filesystem::path directory_;
  std::vector<std::pair<std::string, std::optional<std::string>>> environment_;
};

TEST_F(AngleWindowInitializationTest, MissingAngleNeverFallsBackToSystemEgl) {
  EXPECT_FALSE(Init(320, 180, "missing ANGLE"));
  EXPECT_FALSE(IsInitialised());
  EXPECT_EQ(SDL_WasInit(SDL_INIT_VIDEO), 0U);
}

TEST_F(AngleWindowInitializationTest, PartialAngleNeverMixesWithInstalledBrowsers) {
  std::ofstream(directory_ / "libEGL.so") << "fixture";
  EXPECT_FALSE(Init(320, 180, "partial ANGLE"));
  EXPECT_FALSE(IsInitialised());
  EXPECT_EQ(SDL_WasInit(SDL_INIT_VIDEO), 0U);
}

TEST_F(AngleWindowInitializationTest, UnloadableAngleNeverCreatesWaitingWindow) {
  std::ofstream(directory_ / "libEGL.so") << "invalid EGL library";
  std::ofstream(directory_ / "libGLESv2.so") << "invalid GLES library";
  EXPECT_FALSE(Init(320, 180, "unloadable ANGLE"));
  EXPECT_FALSE(IsInitialised());
}

TEST_F(AngleWindowInitializationTest, ReinitializationUsesCurrentBackend) {
  ASSERT_EQ(unsetenv("MOCKTAIL_ANGLE_LIB_DIR"), 0);
  ASSERT_EQ(setenv("MOCKTAIL_GRAPHICS_BACKEND", "system", 1), 0);
  ASSERT_TRUE(Init(320, 180, "initial backend"));
  Shutdown();

  ASSERT_EQ(setenv("MOCKTAIL_GRAPHICS_BACKEND", "direct-vulkan", 1), 0);
  EXPECT_FALSE(Init(320, 180, "new backend"));
  EXPECT_FALSE(IsInitialised());
}

TEST_F(AngleWindowInitializationTest, RealAngleContextBindsOnRenderThread) {
  const char* directory = std::getenv("MOCKTAIL_TEST_ANGLE_LIB_DIR");
  if (directory == nullptr) {
    GTEST_SKIP() << "real ANGLE libraries were not explicitly provided";
  }
  ASSERT_EQ(setenv("MOCKTAIL_ANGLE_LIB_DIR", directory, 1), 0);
  ASSERT_EQ(setenv("SDL_VIDEO_DRIVER", "x11", 1), 0);
  ASSERT_EQ(setenv("MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS", "0", 1), 0);
  ASSERT_EQ(setenv("MOCKTAIL_SOFTWARE_WINDOW_FALLBACK", "0", 1), 0);
  ASSERT_EQ(setenv("SDL_EGL_LIBRARY", "/missing/generic/libEGL.so", 1), 0);
  ASSERT_EQ(setenv("SDL_OPENGL_LIBRARY", "/missing/generic/libGLESv2.so", 1), 0);
  ASSERT_EQ(setenv("ANGLE_DEFAULT_PLATFORM", "opengl", 1), 0);
  ASSERT_TRUE(Init(320, 180, "ANGLE context handoff")) << SDL_GetError();
  ASSERT_EQ(SDL_GL_GetCurrentContext(), nullptr);

  bool bound = false;
  bool released = false;
  std::string renderer;
  std::string error;
  std::thread render([&] {
    bound = MakeCurrentOnThread();
    if (bound) {
      using GetString = const unsigned char* (*)(unsigned int);
      const auto get_string =
          reinterpret_cast<GetString>(GetGLProcAddress("glGetString"));
      const unsigned char* value = get_string != nullptr ? get_string(0x1f01)
                                                         : nullptr;
      if (value != nullptr) renderer = reinterpret_cast<const char*>(value);
      released = ReleaseCurrentOnThread();
    }
    error = SDL_GetError();
  });
  render.join();
  EXPECT_TRUE(bound) << error;
  EXPECT_TRUE(released) << error;
  EXPECT_NE(renderer.find("ANGLE"), std::string::npos) << renderer;
  EXPECT_NE(renderer.find("Vulkan"), std::string::npos) << renderer;
}

}  // namespace
}  // namespace window
}  // namespace mocktail
