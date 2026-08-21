#include "runtime/http_client_policy.h"

#include <gtest/gtest.h>

#define JSON_NOEXCEPTION 1
#include <nlohmann/json.hpp>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

TEST(HttpClientPolicyTest, AppliesCompatibilityModeAndPreservesOtherValues) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeHttpClientSettingsOverrides(
      R"({"DFIntHttpExponetialRetryBaseInterval":"3"})", &merged, &error))
      << error;

  const nlohmann::json parsed =
      nlohmann::json::parse(merged, nullptr, false, true);
  ASSERT_TRUE(parsed.is_object());
  EXPECT_EQ(parsed.at("FFlagUseRuntimeMutexRvHttpClient"), "False");
  EXPECT_EQ(parsed.at("DFFlagHttpClientSkipRetryForStreamingRequests"), "True");
  EXPECT_EQ(parsed.at("FFlagLuaAppDefaultHttpRetry"), "False");
  EXPECT_EQ(parsed.at("DFIntHttpExponetialRetryBaseInterval"), "3");
}

TEST(HttpClientPolicyTest, ReplacesUnsafeServerOrCallerValues) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeHttpClientSettingsOverrides(
      R"({"FFlagUseRuntimeMutexRvHttpClient":"True","DFFlagHttpClientSkipRetryForStreamingRequests":"False","FFlagLuaAppDefaultHttpRetry":"True"})",
      &merged, &error))
      << error;

  const nlohmann::json parsed =
      nlohmann::json::parse(merged, nullptr, false, true);
  EXPECT_EQ(parsed.at("FFlagUseRuntimeMutexRvHttpClient"), "False");
  EXPECT_EQ(parsed.at("DFFlagHttpClientSkipRetryForStreamingRequests"), "True");
  EXPECT_EQ(parsed.at("FFlagLuaAppDefaultHttpRetry"), "False");
}

TEST(HttpClientPolicyTest, RejectsMalformedInputAndMissingOutput) {
  std::string merged = "unchanged";
  std::string error;
  EXPECT_FALSE(MergeHttpClientSettingsOverrides("[]", &merged, &error));
  EXPECT_EQ(merged, "unchanged");
  EXPECT_NE(error.find("JSON object"), std::string::npos);

  error.clear();
  EXPECT_FALSE(MergeHttpClientSettingsOverrides("{}", nullptr, &error));
  EXPECT_NE(error.find("output"), std::string::npos);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
