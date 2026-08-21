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
