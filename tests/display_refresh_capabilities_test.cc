#include "mocktail/platform/display_refresh_capabilities.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace mocktail {
namespace platform {
namespace {

TEST(DisplayRefreshCapabilitiesTest, NormalizesAndIncludesCurrentMode) {
  const DisplayRefreshCapabilities result = NormalizeDisplayRefreshCapabilities(
      74.91f, {120.0f, 60.0f, 120.004f, -1.0f,
               std::numeric_limits<float>::quiet_NaN()});

  ASSERT_TRUE(result.valid());
  EXPECT_FLOAT_EQ(result.current_hz, 74.91f);
  EXPECT_EQ(result.supported_hz, (std::vector<float>{60.0f, 74.91f, 120.0f}));
}

TEST(DisplayRefreshCapabilitiesTest, PreservesDistinctFractionalRates) {
  const DisplayRefreshCapabilities result =
      NormalizeDisplayRefreshCapabilities(60.0f, {59.94f, 60.0f});

  EXPECT_EQ(result.supported_hz, (std::vector<float>{59.94f, 60.0f}));
}

TEST(DisplayRefreshCapabilitiesTest, InvalidCurrentRateFailsClosed) {
  EXPECT_FALSE(NormalizeDisplayRefreshCapabilities(0.0f, {60.0f}).valid());
  EXPECT_FALSE(NormalizeDisplayRefreshCapabilities(
                   std::numeric_limits<float>::infinity(), {60.0f})
                   .valid());
}

} // namespace
} // namespace platform
} // namespace mocktail
