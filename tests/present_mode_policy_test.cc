#include "mocktail/graphics/present_mode_policy.h"

#include <gtest/gtest.h>

namespace mocktail {
namespace graphics {
namespace {

TEST(PresentModePolicyTest, DerivesDisplayAndUnlimitedDefaults) {
  EXPECT_EQ(ResolvePresentModePolicy("auto", "display"),
            PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("auto", "unlimited"),
            PresentModePolicy::kUnthrottled);
}

TEST(PresentModePolicyTest, FixedFrameRateStaysThrottledUnderAutoVsync) {
  // A fixed cap is a request to render less, so `auto` must not hand Roblox an
  // unfiltered mode list it would answer with MAILBOX or IMMEDIATE.
  EXPECT_EQ(ResolvePresentModePolicy("auto", "60"), PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("auto", "120"), PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("auto", "240"), PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("", "120"), PresentModePolicy::kVsync);
}

TEST(PresentModePolicyTest, ExplicitVsyncRequestsStillWin) {
  EXPECT_EQ(ResolvePresentModePolicy("off", "120"),
            PresentModePolicy::kUnthrottled);
  EXPECT_EQ(ResolvePresentModePolicy("on", "unlimited"),
            PresentModePolicy::kVsync);
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

} // namespace
} // namespace graphics
} // namespace mocktail
