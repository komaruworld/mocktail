#include "runtime/crash_report_policy.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "runtime/frame_rate_policy.h"
#include "runtime/performance_policy.h"

namespace mocktail {
namespace runtime {
namespace {

struct ExpectedSetting {
  std::string_view name;
  std::string_view value;
};

constexpr ExpectedSetting kCriticalSettings[] = {
    {"DFFlagUseCrashpad", "False"},
    {"UseCrashpad", "False"},
    {"UseAppCrashpadReporterAndroid", "False"},
    {"FFlagUseAppCrashpadReporterForAndroidHangMonitor", "False"},
    {"FFlagUseCrashpadDirectlyForAndroidHangMonitor", "False"},
    {"EnableAppHangMonitor", "False"},
    {"CrashpadUploadToBacktraceUrl", ""},
    {"CrashpadUploadToBacktraceAndroidPlayerToken", ""},
    {"CrashpadUploadToBacktraceAndroidPlayerToken_PlaceFilter", ""},
    {"DFStringCrashpadUploadToBacktraceAndroidPlayerToken_PlaceFilter", ""},
    {"DFStringCrashUploadToBacktraceBaseUrl", ""},
    {"DFIntCrashReportingHundredthsPercentage", "0"},
    {"DFIntCrashUploadToBacktracePercentage", "0"},
    {"DFIntCrashUploadToBacktracePercentage_DataCenterFilter", ""},
    {"DFIntCrashUploadToBacktracePercentage_PlaceFilter", ""},
    {"DFFlagCrashUploadToBacktraceRolloutMinimumEnabled", "False"},
    {"DFIntCrashUploadToBacktraceRolloutMinimumPercentage", "0"},
    {"DFStringErrorUploadToBacktraceBaseUrl", ""},
    {"JavaCrashUploadToBacktracePercentage", "0"},
    {"JavaCrashUploadToBacktraceUrl", ""},
    {"JavaCrashUploadToBacktraceToken", ""},
    {"JavaANRUploadToBacktracePercentage", "0"},
    {"DFFlagReportCrashReasonWithTelemetry", "False"},
    {"DFFlagCrashMetricStorageEnableBufferConstantReporting", "False"},
    {"DFIntCrashMetricStorageHundredthPercentageSent", "0"},
    {"DFIntSessionReportCrashErrorEventIngestThrottleHundredthsPercent", "0"},
    {"DFIntSessionReportCrashErrorPointsThrottleHundredthsPercent", "0"},
    {"FIntCoreScriptBacktraceErrorReportPercentage", "0"},
    {"FIntLuaAppBacktraceErrorReportPercentage", "0"},
    {"FFlagBacktraceReporterSourceCodeFieldAndEndpoint2", "False"},
    {"FFlagEnableBacktraceRobloxChannel2", "False"},
    {"DFFlagInferredCrashReportToBacktrace2", "False"},
    {"DFFlagInferredCrashReportToBacktraceRegister", "False"},
    {"DFIntInferredCrashReportToBacktraceThrottleHundredthsPercentage", "0"},
    {"DFFlagObtainAndroidInferredCrashExitLog", "False"},
    {"FFlagLuaAppInferredCrashReportOnStateChange", "False"},
    {"FFlagVideoSessionTrackingOnInferredCrash", "False"},
    {"SkipReportingIsolatedProcessAsInferredCrash", "True"},
    {"FFlagEnableIsolatedAdsBacktrace", "False"},
    {"FFlagEnableIsolatedAdsBacktraceAnrDetection", "False"},
    {"FIntIsolatedAdsBacktraceCrashUploadPercent", "0"},
    {"FIntIsolatedAdsBacktraceAnrUploadPercent", "0"},
};

void ExpectCriticalSettings(const nlohmann::json& settings) {
  for (const ExpectedSetting& expected : kCriticalSettings) {
    SCOPED_TRACE(expected.name);
    ASSERT_TRUE(settings.contains(expected.name));
    EXPECT_EQ(settings.at(expected.name), expected.value);
  }
}

TEST(CrashReportPolicyTest, ForcesEveryUploadPathOffAndPreservesOtherValues) {
  const std::string unsafe = R"({
    "DFFlagUseCrashpad":"True",
    "UseAppCrashpadReporterAndroid":"True",
    "CrashpadUploadToBacktraceUrl":
        "https://upload.crashes.rbxinfra.com/post",
    "DFIntCrashUploadToBacktracePercentage":"100",
    "DFFlagInferredCrashReportToBacktrace2":"True",
    "JavaCrashUploadToBacktracePercentage":100,
    "FStringGraphicsVulkanBlacklist":"keep-me"
  })";
  std::string merged;
  std::string error;

  ASSERT_TRUE(MergeCrashReportClientSettingsOverrides(unsafe, &merged, &error))
      << error;

  const nlohmann::json parsed =
      nlohmann::json::parse(merged, nullptr, false, true);
  ASSERT_TRUE(parsed.is_object());
  ExpectCriticalSettings(parsed);
  EXPECT_EQ(parsed.at("FStringGraphicsVulkanBlacklist"), "keep-me");
}

TEST(CrashReportPolicyTest, IsMandatoryInComposedRuntimePolicy) {
  std::string merged;
  std::string error;

  ASSERT_TRUE(MergeRuntimeClientSettingsOverrides(
      ParseFrameRatePolicy("144"), {true, 8},
      R"({"DFFlagUseCrashpad":"True",)"
      R"("CrashpadUploadToBacktraceUrl":"https://example.invalid/upload"})",
      &merged, &error))
      << error;

  const nlohmann::json parsed =
      nlohmann::json::parse(merged, nullptr, false, true);
  ASSERT_TRUE(parsed.is_object());
  ExpectCriticalSettings(parsed);
  EXPECT_EQ(parsed.at("DFIntTaskSchedulerTargetFps"), "144");
  EXPECT_EQ(parsed.at("FIntTaskSchedulerThreadMin"), "8");
}

TEST(CrashReportPolicyTest, IsIdempotent) {
  std::string first;
  std::string second;
  std::string error;

  ASSERT_TRUE(MergeCrashReportClientSettingsOverrides(
      R"({"Unrelated":"value"})", &first, &error));
  ASSERT_TRUE(MergeCrashReportClientSettingsOverrides(first, &second, &error));
  EXPECT_EQ(first, second);
}

TEST(CrashReportPolicyTest, SanitizesSeparateFastFlagsChannel) {
  std::string merged;
  std::string error;

  ASSERT_TRUE(MergeCrashReportFastFlagsOverrides(
      R"({"DFFlagUseCrashpad":"True",)"
      R"("DFIntCrashUploadToBacktracePercentage":"100",)"
      R"("FFlagUnrelated":"True","JavaCrashUploadToBacktraceUrl":"keep"})",
      &merged, &error))
      << error;

  const nlohmann::json parsed =
      nlohmann::json::parse(merged, nullptr, false, true);
  ASSERT_TRUE(parsed.is_object());
  EXPECT_EQ(parsed.at("DFFlagUseCrashpad"), "False");
  EXPECT_EQ(parsed.at("DFIntCrashUploadToBacktracePercentage"), "0");
  EXPECT_EQ(parsed.at("DFFlagInferredCrashReportToBacktrace2"), "False");
  EXPECT_EQ(parsed.at("FFlagUnrelated"), "True");
  EXPECT_EQ(parsed.at("JavaCrashUploadToBacktraceUrl"), "keep");
  EXPECT_FALSE(parsed.contains("CrashpadUploadToBacktraceUrl"));
}

TEST(CrashReportPolicyTest, RejectsMalformedInputAndMissingOutput) {
  std::string output = "unchanged";
  std::string error;

  EXPECT_FALSE(MergeCrashReportClientSettingsOverrides("[]", &output, &error));
  EXPECT_EQ(output, "unchanged");
  EXPECT_NE(error.find("JSON object"), std::string::npos);

  error.clear();
  EXPECT_FALSE(
      MergeCrashReportClientSettingsOverrides("not-json", &output, &error));
  EXPECT_EQ(output, "unchanged");
  EXPECT_NE(error.find("JSON object"), std::string::npos);

  error.clear();
  EXPECT_FALSE(MergeCrashReportClientSettingsOverrides("{}", nullptr, &error));
  EXPECT_NE(error.find("output"), std::string::npos);

  error.clear();
  EXPECT_FALSE(MergeCrashReportFastFlagsOverrides("[]", &output, &error));
  EXPECT_NE(error.find("JSON object"), std::string::npos);

  error.clear();
  EXPECT_FALSE(MergeCrashReportFastFlagsOverrides("{}", nullptr, &error));
  EXPECT_NE(error.find("output"), std::string::npos);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
