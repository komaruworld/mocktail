#include "runtime/frame_rate_policy.h"

#include <gtest/gtest.h>

#include <string>

namespace mocktail {
namespace runtime {
namespace {

TEST(FrameRatePolicyTest, ParsesSupportedModes) {
  EXPECT_EQ(ParseFrameRatePolicy("display").mode, FrameRateLimitMode::kDisplay);
  EXPECT_EQ(ParseFrameRatePolicy("144").fixed_fps, 144);
  EXPECT_EQ(ParseFrameRatePolicy("unlimited").mode,
            FrameRateLimitMode::kUnlimited);
  EXPECT_FALSE(ParseFrameRatePolicy("999").valid());
}

TEST(FrameRatePolicyTest, MergesUnlimitedWithoutLosingGraphicsPolicy) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeFrameRateClientSettingsOverrides(
      ParseFrameRatePolicy("unlimited"),
      R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})", &merged,
      &error));
  EXPECT_NE(merged.find("DFIntTaskSchedulerTargetFps\":\"240"),
            std::string::npos);
  EXPECT_EQ(merged.find("DFIntTaskSchedulerTargetFps\":\"0"),
            std::string::npos);
  EXPECT_EQ(merged.find("FFlagTaskSchedulerLimitTargetFpsTo2402"),
            std::string::npos);
  EXPECT_NE(merged.find("FStringGraphicsVulkanShaderMTDenyPattern"),
            std::string::npos);
}

TEST(FrameRatePolicyTest, RejectsMalformedAndConflictingOverrides) {
  std::string merged;
  std::string error;
  EXPECT_FALSE(MergeFrameRateClientSettingsOverrides(
      ParseFrameRatePolicy("120"), "not-json", &merged, &error));
  EXPECT_FALSE(MergeFrameRateClientSettingsOverrides(
      ParseFrameRatePolicy("120"), R"({"DFIntTaskSchedulerTargetFps":"60"})",
      &merged, &error));
}

} // namespace
} // namespace runtime
} // namespace mocktail
