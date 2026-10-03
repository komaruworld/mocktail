#include "window/video_driver_policy.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
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

}  // namespace
}  // namespace window
}  // namespace mocktail
