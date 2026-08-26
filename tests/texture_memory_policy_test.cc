#include "runtime/texture_memory_policy.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::uint64_t kMebibyte = 1024U * 1024U;

TEST(TextureMemoryPolicyTest, LeavesTheGuestDefaultOnSmallHosts) {
  EXPECT_EQ(CalculateTextureMemoryBudgetBytes(0), 0U);
  EXPECT_EQ(CalculateTextureMemoryBudgetBytes(2048U * kMebibyte), 0U);
  EXPECT_EQ(CalculateTextureMemoryBudgetBytes(4095U * kMebibyte), 0U);
}

TEST(TextureMemoryPolicyTest, SizesAClampedShareOfHostMemory) {
  EXPECT_EQ(CalculateTextureMemoryBudgetBytes(4096U * kMebibyte),
            512U * kMebibyte);
  EXPECT_EQ(CalculateTextureMemoryBudgetBytes(8192U * kMebibyte),
            1024U * kMebibyte);
  // An eighth of a very large host stays inside the PC-class ceiling.
  EXPECT_EQ(CalculateTextureMemoryBudgetBytes(65536U * kMebibyte),
            1536U * kMebibyte);
}

TEST(TextureMemoryPolicyTest, PublishesTheBudgetWithoutDiscardingOverrides) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeTextureMemoryClientSettingsOverrides(
      1024U * kMebibyte,
      R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})", &merged,
      &error))
      << error;
  const nlohmann::json parsed = nlohmann::json::parse(merged);
  EXPECT_EQ(parsed.at("FIntRenderForceVideoMemorySize"),
            std::to_string(1024U * kMebibyte));
  EXPECT_EQ(parsed.at("FStringGraphicsVulkanShaderMTDenyPattern"), "4318:.*");
}

TEST(TextureMemoryPolicyTest, KeepsAnExplicitUserOverride) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeTextureMemoryClientSettingsOverrides(
      1024U * kMebibyte, R"({"FIntRenderForceVideoMemorySize":"64"})", &merged,
      &error))
      << error;
  EXPECT_EQ(nlohmann::json::parse(merged).at("FIntRenderForceVideoMemorySize"),
            "64");
}

TEST(TextureMemoryPolicyTest, ZeroBudgetPublishesNothing) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeTextureMemoryClientSettingsOverrides(0, "{}", &merged,
                                                        &error))
      << error;
  EXPECT_FALSE(
      nlohmann::json::parse(merged).contains("FIntRenderForceVideoMemorySize"));
}

TEST(TextureMemoryPolicyTest, ClampsToTheFastVariableRange) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeTextureMemoryClientSettingsOverrides(
      8ULL * 1024U * kMebibyte, "{}", &merged, &error))
      << error;
  EXPECT_EQ(nlohmann::json::parse(merged).at("FIntRenderForceVideoMemorySize"),
            std::to_string(std::numeric_limits<std::int32_t>::max()));
}

TEST(TextureMemoryPolicyTest, RejectsMalformedInput) {
  std::string merged;
  std::string error;
  EXPECT_FALSE(MergeTextureMemoryClientSettingsOverrides(kMebibyte, "[]",
                                                         &merged, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(MergeTextureMemoryClientSettingsOverrides(kMebibyte, "{}",
                                                         nullptr, &error));
}

TEST(TextureMemoryPolicyTest, ReadsHostMemory) {
  // /proc/meminfo is always present on the supported hosts.
  EXPECT_GT(DetectHostMemoryBytes(), 0U);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
