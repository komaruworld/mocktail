#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <limits>
#include <vector>

#include "mocktail/graphics/android_vulkan_wsi_adapter.h"
#include "mocktail/graphics/angle_probe.h"
#include "mocktail/graphics/graphics_backend.h"
#include "mocktail/graphics/sdl_vulkan_wsi.h"
#include "mocktail/platform/sdl_application_metadata.h"
#include "mocktail/platform/sdl_event_converter.h"
#include "mocktail/platform/sdl_platform_runtime.h"
#include "mocktail/platform/sdl_window_icon.h"

namespace mocktail {
namespace {

using graphics::BackendCapability;
using graphics::BackendSelectionPolicy;
using graphics::CapabilityState;
using graphics::GraphicsBackendKind;
using graphics::HardwareAcceleration;

TEST(SdlApplicationMetadataTest, MatchesInstalledDesktopIdentity) {
  Status status = platform::ConfigureSdlApplicationMetadata();

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_STREQ(SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING),
               platform::kMocktailApplicationName);
  EXPECT_STREQ(
      SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING),
      platform::kMocktailApplicationIdentifier);
  EXPECT_STREQ(SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_TYPE_STRING),
               "game");
}

TEST(GraphicsBackendSelectionTest, FailsClosedWithoutRequestedBackend) {
  BackendSelectionPolicy policy;
  policy.requested = GraphicsBackendKind::kAngleVulkan;

  const std::vector<BackendCapability> capabilities = {
      {GraphicsBackendKind::kDirectVulkan, CapabilityState::kReady,
       HardwareAcceleration::kHardware, "direct Vulkan ready"}};

  auto result = graphics::SelectGraphicsBackend(policy, capabilities);
  EXPECT_FALSE(result.status.ok());
  EXPECT_EQ(result.backend, GraphicsBackendKind::kAngleVulkan);
}

TEST(GraphicsBackendSelectionTest, UsesFallbackOnlyWhenExplicitlyAllowed) {
  BackendSelectionPolicy policy;
  policy.requested = GraphicsBackendKind::kAngleVulkan;
  policy.allow_fallback = true;

  const std::vector<BackendCapability> capabilities = {
      {GraphicsBackendKind::kDirectVulkan, CapabilityState::kReady,
       HardwareAcceleration::kHardware, "direct Vulkan ready"}};

  auto result = graphics::SelectGraphicsBackend(policy, capabilities);
  ASSERT_TRUE(result.status.ok()) << result.status.message();
  EXPECT_EQ(result.backend, GraphicsBackendKind::kDirectVulkan);
}

TEST(GraphicsBackendSelectionTest, RejectsUnvalidatedLoaderByDefault) {
  const std::vector<BackendCapability> capabilities = {
      {GraphicsBackendKind::kAngleVulkan, CapabilityState::kLoadable,
       HardwareAcceleration::kHardware, "symbols exist"}};

  auto result = graphics::SelectGraphicsBackend({}, capabilities);
  EXPECT_FALSE(result.status.ok());
}

TEST(AngleProbeTest, RequiresExplicitPinnedLibraryPaths) {
  auto capability = graphics::ProbeAngleVulkan({});
  EXPECT_EQ(capability.state, CapabilityState::kUnavailable);
  EXPECT_NE(capability.detail.find("required"), std::string::npos);
}

TEST(AngleProbeTest, ValidatesConfiguredAngleVulkanDistribution) {
  const char* egl = std::getenv("MOCKTAIL_TEST_ANGLE_EGL_LIBRARY");
  const char* gles = std::getenv("MOCKTAIL_TEST_ANGLE_GLES_LIBRARY");
  if (egl == nullptr || gles == nullptr) {
    GTEST_SKIP() << "pinned ANGLE test paths were not provided";
  }

  graphics::AngleProbeOptions options;
  options.egl_library_path = egl;
  options.gles_library_path = gles;
  auto capability = graphics::ProbeAngleVulkan(options);
  EXPECT_EQ(capability.state, CapabilityState::kReady) << capability.detail;
  EXPECT_EQ(capability.acceleration, HardwareAcceleration::kHardware);
}

TEST(SdlPlatformRuntimeTest, RejectsInvalidWindowDimensionsBeforeSdlInit) {
  auto runtime = platform::CreateSdlPlatformRuntime();
  platform::WindowOptions options;
  options.width = 0;
  Status status = runtime->Initialize(options);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_FALSE(runtime->IsInitialized());
}

TEST(SdlWindowIconTest, RejectsNullWindow) {
  EXPECT_EQ(platform::ApplySdlWindowIcon(nullptr).code(),
            StatusCode::kInvalidArgument);
}

TEST(SdlWindowIconTest, AppliesEmbeddedPngToRealWindow) {
  const char* enabled = std::getenv("MOCKTAIL_TEST_SDL_WINDOW");
  if (enabled == nullptr || std::string(enabled) != "1") {
    GTEST_SKIP() << "real SDL window test was not explicitly enabled";
  }

  ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO)) << SDL_GetError();
  SDL_Window* window =
      SDL_CreateWindow("Mocktail icon test", 64, 64, SDL_WINDOW_HIDDEN);
  ASSERT_NE(window, nullptr) << SDL_GetError();

  const Status status = platform::ApplySdlWindowIcon(window);
  EXPECT_TRUE(status.ok()) << status.message();

  SDL_DestroyWindow(window);
  SDL_Quit();
}

TEST(SdlEventConverterTest, ConvertsFocusWithoutOwningTheSdlQueue) {
  SDL_Event source{};
  source.type = SDL_EVENT_WINDOW_FOCUS_LOST;
  source.window.timestamp = 123;
  platform::PlatformEvent event;
  auto* non_owning_window = reinterpret_cast<SDL_Window*>(0x1);

  ASSERT_TRUE(platform::ConvertSdlEvent(non_owning_window, source, &event));
  EXPECT_EQ(event.timestamp_ns, 123U);
  const auto* focus = std::get_if<platform::WindowFocusEvent>(&event.payload);
  ASSERT_NE(focus, nullptr);
  EXPECT_FALSE(focus->focused);

  source.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
  ASSERT_TRUE(platform::ConvertSdlEvent(non_owning_window, source, &event));
  focus = std::get_if<platform::WindowFocusEvent>(&event.payload);
  ASSERT_NE(focus, nullptr);
  EXPECT_TRUE(focus->focused);
}

TEST(SdlEventConverterTest, RejectsInvalidArgumentsAndUnsupportedEvents) {
  SDL_Event source{};
  source.type = SDL_EVENT_USER;
  platform::PlatformEvent event;
  auto* non_owning_window = reinterpret_cast<SDL_Window*>(0x1);

  EXPECT_FALSE(platform::ConvertSdlEvent(nullptr, source, &event));
  EXPECT_FALSE(platform::ConvertSdlEvent(non_owning_window, source, nullptr));
  EXPECT_FALSE(platform::ConvertSdlEvent(non_owning_window, source, &event));
}

TEST(SdlPlatformRuntimeTest, CreatesConfiguredRealNativeWindow) {
  const char* enabled = std::getenv("MOCKTAIL_TEST_SDL_WINDOW");
  if (enabled == nullptr || std::string(enabled) != "1") {
    GTEST_SKIP() << "real SDL window test was not explicitly enabled";
  }

  auto runtime = platform::CreateSdlPlatformRuntime();
  platform::WindowOptions options;
  options.title = "Mocktail foundation test";
  options.width = 320;
  options.height = 180;
  options.initially_hidden = true;
  options.require_native_window = true;
  Status status = runtime->Initialize(options);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_TRUE(runtime->IsInitialized());
  EXPECT_NE(runtime->GetNativeWindow().backend_window, nullptr);
  EXPECT_TRUE(runtime->GetNativeWindow().HasPlatformHandle());
  status = runtime->ShowWindow();
  ASSERT_TRUE(status.ok()) << status.message();

  if (std::getenv("MOCKTAIL_TEST_SDL_WINDOW_HOLD") != nullptr) {
    SDL_Delay(3000);
  }

  SDL_Event source{};
  source.type = SDL_EVENT_KEY_DOWN;
  source.key.timestamp = 42;
  source.key.down = true;
  source.key.scancode = SDL_SCANCODE_A;
  source.key.key = SDLK_A;
  ASSERT_TRUE(SDL_PushEvent(&source));

  bool received_key = false;
  for (int i = 0; i < 32 && !received_key; ++i) {
    platform::PlatformEvent event;
    bool has_event = false;
    status = runtime->PollEvent(&event, &has_event);
    ASSERT_TRUE(status.ok()) << status.message();
    if (!has_event) {
      break;
    }
    const auto* key = std::get_if<platform::KeyEvent>(&event.payload);
    if (key != nullptr && key->pressed &&
        key->scancode == static_cast<std::uint32_t>(SDL_SCANCODE_A)) {
      received_key = true;
      EXPECT_EQ(event.timestamp_ns, 42u);
    }
  }
  EXPECT_TRUE(received_key);
  runtime->Shutdown();
  EXPECT_FALSE(runtime->IsInitialized());
}

TEST(SdlVulkanWsiTest, DiscoversHostWsiWithoutCreatingRenderer) {
  const char* enabled = std::getenv("MOCKTAIL_TEST_SDL_WINDOW");
  if (enabled == nullptr || std::string(enabled) != "1") {
    GTEST_SKIP() << "real SDL window test was not explicitly enabled";
  }

  auto runtime = platform::CreateSdlPlatformRuntime();
  platform::WindowOptions options;
  options.title = "Mocktail Vulkan WSI test";
  options.width = 320;
  options.height = 180;
  options.surface_api = platform::WindowSurfaceApi::kDirectVulkan;
  Status status = runtime->Initialize(options);
  ASSERT_TRUE(status.ok()) << status.message();

  graphics::SdlVulkanWsi wsi;
  status = wsi.Initialize(runtime->GetNativeWindow());
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_FALSE(wsi.required_instance_extensions().empty());
  EXPECT_NE(wsi.GetInstanceProcAddress(), nullptr);
  EXPECT_EQ(wsi.DescribeCapability().state, CapabilityState::kLoadable);
  wsi.Shutdown();
  runtime->Shutdown();
}

TEST(AndroidVulkanWsiAdapterTest, RewritesAndroidSurfaceToHostWsiExtensions) {
  const std::vector<std::string> android_extensions = {
      "VK_KHR_surface", graphics::kAndroidSurfaceExtension,
      "VK_EXT_debug_utils",
      "VK_KHR_surface"};
  const std::vector<std::string> host_wsi_extensions = {
      "VK_KHR_surface", "VK_KHR_xcb_surface"};
  std::vector<std::string> translated;

  Status status = graphics::TranslateAndroidVulkanInstanceExtensions(
      android_extensions, host_wsi_extensions, &translated);

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(translated,
            (std::vector<std::string>{"VK_KHR_surface",
                                      "VK_EXT_debug_utils",
                                      "VK_KHR_xcb_surface"}));
}

TEST(AndroidVulkanWsiAdapterTest, RejectsSurfaceWithoutHostWsiExtensions) {
  std::vector<std::string> translated;
  Status status = graphics::TranslateAndroidVulkanInstanceExtensions(
      {graphics::kAndroidSurfaceExtension}, {}, &translated);

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kFailedPrecondition);
}

TEST(AndroidVulkanWsiAdapterTest, LeavesNonSurfaceExtensionsUnchanged) {
  std::vector<std::string> translated;
  Status status = graphics::TranslateAndroidVulkanInstanceExtensions(
      {"VK_EXT_debug_utils"}, {"VK_KHR_surface", "VK_KHR_wayland_surface"},
      &translated);

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(translated,
            (std::vector<std::string>{"VK_EXT_debug_utils"}));
}

TEST(AndroidVulkanWsiAdapterTest, NormalizesHostSuboptimalForAndroidClient) {
  EXPECT_EQ(graphics::NormalizeAndroidSwapchainResult(VK_SUBOPTIMAL_KHR),
            VK_SUCCESS);
  EXPECT_EQ(graphics::NormalizeAndroidSwapchainResult(VK_SUCCESS),
            VK_SUCCESS);
  constexpr VkResult kOutOfDate = static_cast<VkResult>(-1000001004);
  EXPECT_EQ(graphics::NormalizeAndroidSwapchainResult(kOutOfDate), kOutOfDate);
}

TEST(AndroidVulkanWsiAdapterTest, BoundsOnlyInfiniteHostImageAcquire) {
  constexpr std::uint64_t kFiniteTimeout = 250000000ULL;

  EXPECT_EQ(graphics::BoundHostImageAcquireTimeout(
                std::numeric_limits<std::uint64_t>::max()),
            graphics::kHostImageAcquireWatchdogTimeoutNs);
  EXPECT_EQ(graphics::BoundHostImageAcquireTimeout(kFiniteTimeout),
            kFiniteTimeout);
}

TEST(AndroidVulkanWsiAdapterTest,
     ConvertsOnlyWatchdogTimeoutIntoSurfaceRecovery) {
  const std::uint64_t infinite_timeout =
      std::numeric_limits<std::uint64_t>::max();
  constexpr std::uint64_t kFiniteTimeout = 250000000ULL;

  EXPECT_TRUE(graphics::IsHostImageAcquireWatchdogTimeout(infinite_timeout,
                                                          VK_TIMEOUT));
  EXPECT_EQ(
      graphics::NormalizeHostImageAcquireResult(infinite_timeout, VK_TIMEOUT),
      VK_ERROR_OUT_OF_DATE_KHR);
  EXPECT_FALSE(
      graphics::IsHostImageAcquireWatchdogTimeout(kFiniteTimeout, VK_TIMEOUT));
  EXPECT_EQ(
      graphics::NormalizeHostImageAcquireResult(kFiniteTimeout, VK_TIMEOUT),
      VK_TIMEOUT);
}

TEST(AndroidVulkanWsiAdapterTest,
     SlicesInfiniteSynchronizationWaitWithoutCompletingIt) {
  const std::uint64_t infinite_timeout =
      std::numeric_limits<std::uint64_t>::max();
  constexpr std::uint64_t kFiniteTimeout = 750000000ULL;

  EXPECT_EQ(graphics::BoundHostSynchronizationWaitTimeout(infinite_timeout),
            graphics::kHostInfiniteWaitDiagnosticSliceNs);
  EXPECT_EQ(graphics::BoundHostSynchronizationWaitTimeout(kFiniteTimeout),
            kFiniteTimeout);
  EXPECT_TRUE(graphics::ShouldContinueHostSynchronizationWait(infinite_timeout,
                                                               VK_TIMEOUT));
  EXPECT_FALSE(graphics::ShouldContinueHostSynchronizationWait(kFiniteTimeout,
                                                                VK_TIMEOUT));
  EXPECT_FALSE(graphics::ShouldContinueHostSynchronizationWait(infinite_timeout,
                                                                VK_SUCCESS));
}

}  // namespace
}  // namespace mocktail
