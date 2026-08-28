#include "mocktail/graphics/present_mode_policy.h"

#include <gtest/gtest.h>

namespace mocktail {
namespace graphics {
namespace {

TEST(PresentModePolicyTest, DerivesDisplayAndUnlimitedDefaults) {
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
