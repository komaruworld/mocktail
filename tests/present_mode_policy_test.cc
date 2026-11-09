#include "mocktail/graphics/present_mode_policy.h"

#include <gtest/gtest.h>

namespace mocktail {
namespace graphics {
namespace {

TEST(PresentModePolicyTest, DerivesDisplayAndUnlimitedDefaults) {
  EXPECT_EQ(ResolvePresentModePolicy("auto", "-1"),
            PresentModePolicy::kHostDefault);
  EXPECT_EQ(ResolvePresentModePolicy("auto", "display"),
            PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("auto", "60"),
            PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("auto", "unlimited"),
            PresentModePolicy::kUnthrottled);
  EXPECT_EQ(ResolvePresentModePolicy("on", "unlimited"),
            PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("off", "display"),
            PresentModePolicy::kUnthrottled);
}

TEST(PresentModePolicyTest, FiltersWithoutFabricatingHostModes) {
  const std::vector<VkPresentModeKHR> host = {VK_PRESENT_MODE_IMMEDIATE_KHR,
                                              VK_PRESENT_MODE_FIFO_KHR};
  EXPECT_EQ(FilterPresentModes(PresentModePolicy::kVsync, host),
            (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_FIFO_KHR}));
  EXPECT_EQ(FilterPresentModes(PresentModePolicy::kUnthrottled, host),
            (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_IMMEDIATE_KHR}));
  EXPECT_EQ(FilterPresentModes(PresentModePolicy::kUnthrottled,
                               {VK_PRESENT_MODE_FIFO_KHR}),
            (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_FIFO_KHR}));
}

TEST(PresentModePolicyTest, ExplicitModesOverrideVsyncAndUseSupportedFallbacks) {
  const std::vector<VkPresentModeKHR> host = {
      VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR,
      VK_PRESENT_MODE_MAILBOX_KHR};
  for (const char* mode : {"fifo", "mailbox", "immediate"}) {
    SCOPED_TRACE(mode);
    const auto policy = ResolvePresentModePolicy("on", "unlimited", mode);
    ASSERT_TRUE(ExplicitPresentMode(policy).has_value());
    EXPECT_EQ(policy, ResolvePresentModePolicy("off", "display", mode));
    EXPECT_EQ(FilterPresentModes(policy, host),
              (std::vector<VkPresentModeKHR>{*ExplicitPresentMode(policy)}));
    EXPECT_EQ(FilterPresentModes(policy, {VK_PRESENT_MODE_FIFO_KHR}),
              (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_FIFO_KHR}));
    EXPECT_TRUE(FilterPresentModes(policy, {}).empty());
    EXPECT_EQ(ResolveSwapInterval(policy, 0),
              policy == PresentModePolicy::kImmediate ? 0 : 1);
  }
  EXPECT_EQ(ResolveSwapInterval(PresentModePolicy::kHostDefault, 2), 2);
  EXPECT_EQ(ResolveSwapInterval(PresentModePolicy::kUnthrottled, 1), 0);
  EXPECT_EQ(ResolveSwapInterval(PresentModePolicy::kVsync, 0), 1);
}

TEST(PresentModePolicyTest, PrefersMailboxForVsyncWhenHostExposesIt) {
  const std::vector<VkPresentModeKHR> host = {VK_PRESENT_MODE_IMMEDIATE_KHR,
                                              VK_PRESENT_MODE_MAILBOX_KHR,
                                              VK_PRESENT_MODE_FIFO_KHR};
  EXPECT_EQ(FilterPresentModes(PresentModePolicy::kVsync, host),
            (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_MAILBOX_KHR}));
}

TEST(PresentModePolicyTest, PrefersLatestReadyThenRelaxedOverFifo) {
  EXPECT_EQ(FilterPresentModes(
                PresentModePolicy::kVsync,
                {VK_PRESENT_MODE_FIFO_KHR,
                 VK_PRESENT_MODE_FIFO_LATEST_READY_KHR,
                 VK_PRESENT_MODE_MAILBOX_KHR}),
            (std::vector<VkPresentModeKHR>{
                VK_PRESENT_MODE_FIFO_LATEST_READY_KHR}));
  EXPECT_EQ(FilterPresentModes(PresentModePolicy::kVsync,
                               {VK_PRESENT_MODE_FIFO_KHR,
                                VK_PRESENT_MODE_FIFO_RELAXED_KHR}),
            (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_FIFO_RELAXED_KHR}));
}

TEST(PresentModePolicyTest, RequestsExtraSwapchainImagesWhenUnthrottled) {
  EXPECT_EQ(PreferSwapchainMinImageCount(PresentModePolicy::kMailbox, 2, 2, 8),
            3U);
  EXPECT_EQ(PreferSwapchainMinImageCount(PresentModePolicy::kMailbox, 2, 2, 2),
            2U);
  EXPECT_EQ(PreferSwapchainMinImageCount(PresentModePolicy::kFifo, 2, 2, 8), 2U);
  EXPECT_EQ(PreferSwapchainMinImageCount(PresentModePolicy::kVsync, 2, 2, 8),
            4U);
  EXPECT_EQ(
      PreferSwapchainMinImageCount(PresentModePolicy::kUnthrottled, 2, 2, 8),
      5U);
  EXPECT_EQ(
      PreferSwapchainMinImageCount(PresentModePolicy::kUnthrottled, 1, 2, 2),
      2U);
  EXPECT_EQ(
      PreferSwapchainMinImageCount(PresentModePolicy::kUnthrottled, 2, 2, 0),
      5U);
}

} // namespace
} // namespace graphics
} // namespace mocktail
