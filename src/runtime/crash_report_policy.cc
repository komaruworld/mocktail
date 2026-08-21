#include "runtime/crash_report_policy.h"

#define JSON_NOEXCEPTION 1
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace mocktail {
namespace runtime {
namespace {

struct ClientSetting {
  std::string_view name;
  std::string_view value;
};

// The supported Android payloads contain both native FastVariables and
// Java-side client settings for Backtrace. Keep both families fail-closed:
// Java bytecode is not normally executed by the pseudo-JVM, but an APK update
// must not turn that implementation detail into an upload path.
constexpr ClientSetting kCrashReportSettings[] = {
    {"DFFlagUseCrashpad", "False"},
    {"FFlagUseCrashpad", "False"},
    {"UseCrashpad", "False"},
    {"UseAppCrashpadReporterAndroid", "False"},
    {"FFlagUseAppCrashpadReporterForAndroidHangMonitor", "False"},
    {"FFlagUseCrashpadDirectlyForAndroidHangMonitor", "False"},
    {"EnableAppHangMonitor", "False"},
    {"CrashpadUploadToBacktraceUrl", ""},
    {"CrashpadUploadToBacktraceAndroidPlayerToken", ""},
    {"CrashpadUploadToBacktraceAndroidPlayerToken_PlaceFilter", ""},
    {"DFStringCrashpadUploadToBacktraceAndroidPlayerToken", ""},
    {"DFStringCrashpadUploadToBacktraceAndroidPlayerToken_PlaceFilter", ""},
    {"CrashpadUploadAttachmentKiloByteLimit", "0"},
    {"DFIntCrashUploadAttachmentKiloByteLimit", "0"},
    {"DFIntCrashReportingHundredthsPercentage", "0"},
    {"CrashUploadToBacktraceBaseUrl", ""},
    {"DFStringCrashUploadToBacktraceBaseUrl", ""},
    {"CrashUploadToBacktracePercentage", "0"},
    {"DFIntCrashUploadToBacktracePercentage", "0"},
    {"DFIntCrashUploadToBacktracePercentage_DataCenterFilter", ""},
    {"DFIntCrashUploadToBacktracePercentage_PlaceFilter", ""},
    {"CrashUploadToBacktraceRolloutMinimumEnabled", "False"},
    {"DFFlagCrashUploadToBacktraceRolloutMinimumEnabled", "False"},
    {"CrashUploadToBacktraceRolloutMinimumPercentage", "0"},
    {"DFIntCrashUploadToBacktraceRolloutMinimumPercentage", "0"},
    {"ErrorUploadToBacktraceBaseUrl", ""},
    {"DFStringErrorUploadToBacktraceBaseUrl", ""},
    {"ErrorUploadToBacktraceLuaAppToken", ""},
    {"DFStringErrorUploadToBacktraceLuaAppToken", ""},
    {"JavaCrashUploadToBacktracePercentage", "0"},
    {"JavaCrashUploadToBacktraceUrl", ""},
    {"JavaCrashUploadToBacktraceToken", ""},
    {"JavaANRUploadToBacktracePercentage", "0"},
    {"DFFlagReportCrashReasonWithTelemetry", "False"},
    {"DFFlagUseCrashReportLogGroup", "False"},
    {"DFFlagEnableCrashHandlerSessionDurationAndroid", "False"},
    {"DFFlagCrashMetricStorageEnableBufferConstantReporting", "False"},
    {"DFIntCrashMetricStorageHundredthPercentageSent", "0"},
    {"DFIntSessionReportCrashErrorEventIngestThrottleHundredthsPercent", "0"},
    {"DFIntSessionReportCrashErrorPointsThrottleHundredthsPercent", "0"},
    {"FIntCoreScriptBacktraceErrorReportPercentage", "0"},
    {"FIntLuaAppBacktraceErrorReportPercentage", "0"},
    {"FStringCoreScriptBacktraceErrorUploadToken", ""},
    {"FFlagBacktraceReporterSourceCodeFieldAndEndpoint2", "False"},
    {"FFlagUserAgentInBacktraceReports", "False"},
    {"FFlagEnableBacktraceRobloxChannel2", "False"},
    {"FStringBacktraceLogTags", ""},
    {"BacktraceLogSize", "0"},
    {"DFIntBacktraceMetricKitUploadHundrethsPercent", "0"},
    {"FFlagEnableBacktraceMetricKitReporter2", "False"},
    {"FFlagEnableBacktraceMetricKitReporter3", "False"},
    {"FFlagenableBacktraceMetricKitReporterMinidump", "False"},
    {"FFlagEnablePLCrashReporterApi", "False"},
    {"DFFlagInferredCrashReportToBacktrace", "False"},
    {"DFFlagInferredCrashReportToBacktrace2", "False"},
    {"InferredCrashReportToBacktrace", "False"},
    {"InferredCrashReportToBacktraceRegister", "False"},
    {"DFFlagInferredCrashReportToBacktraceRegister", "False"},
    {"InferredCrashReportSynchronous", "False"},
    {"DFFlagInferredCrashReportSynchronous", "False"},
    {"FFlagInferredCrashReportSynchronous", "False"},
    {"InferredCrashReportToBacktraceHundredthPercent", "0"},
    {"DFIntInferredCrashReportToBacktraceHundredthPercent", "0"},
    {"InferredCrashReportToBacktraceThrottleHundredthsPercentage", "0"},
    {"DFIntInferredCrashReportToBacktraceThrottleHundredthsPercentage", "0"},
    {"InferredCrashReportBacktraceToken", ""},
    {"DFStringInferredCrashReportBacktraceToken", ""},
    {"InferredCrashReportBacktraceTokenSync", ""},
    {"DFStringInferredCrashReportBacktraceTokenSync", ""},
    {"DFFlagObtainAndroidInferredCrashExitLog", "False"},
    {"DFFlagStoreUniqueIdInferredCrashSystem", "False"},
    {"FFlagLuaAppInferredCrashReportOnStateChange", "False"},
    {"FFlagVideoSessionTrackingOnInferredCrash", "False"},
    {"SkipReportingIsolatedProcessAsInferredCrash", "True"},
    {"FFlagEnableIsolatedAdsBacktrace", "False"},
    {"FFlagEnableIsolatedAdsBacktraceAnrDetection", "False"},
    {"FIntIsolatedAdsBacktraceCrashUploadPercent", "0"},
    {"FIntIsolatedAdsBacktraceAnrUploadPercent", "0"},
};

bool HasPrefix(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

bool IsFastVariable(std::string_view name) {
  constexpr std::string_view kPrefixes[] = {
      "DFFlag", "DFInt", "DFString", "FFlag", "FInt", "FString",
  };
  for (const std::string_view prefix : kPrefixes) {
    if (HasPrefix(name, prefix)) {
      return true;
    }
  }
  return false;
}

bool ParseOverrideObject(std::string_view base_json, std::string_view label,
                         nlohmann::json* overrides, std::string* error) {
  *overrides = nlohmann::json::parse(base_json.empty() ? "{}" : base_json,
                                     nullptr, false, true);
  if (!overrides->is_discarded() && overrides->is_object()) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string(label) + " must be a JSON object";
  }
  return false;
}

}  // namespace

bool MergeCrashReportClientSettingsOverrides(std::string_view base_json,
                                             std::string* merged_json,
                                             std::string* error) {
  if (merged_json == nullptr) {
    if (error != nullptr) {
      *error = "crash-report client-settings output is required";
    }
    return false;
  }

  nlohmann::json overrides;
  if (!ParseOverrideObject(base_json, "client-settings overrides", &overrides,
                           error)) {
    return false;
  }

  for (const ClientSetting& setting : kCrashReportSettings) {
    overrides[std::string(setting.name)] = std::string(setting.value);
  }
  *merged_json = overrides.dump();
  return true;
}

bool MergeCrashReportFastFlagsOverrides(std::string_view base_json,
                                        std::string* merged_json,
                                        std::string* error) {
  if (merged_json == nullptr) {
    if (error != nullptr) {
      *error = "crash-report fast-flags output is required";
    }
    return false;
  }

  nlohmann::json overrides;
  if (!ParseOverrideObject(base_json, "fast-flags overrides", &overrides,
                           error)) {
    return false;
  }
  for (const ClientSetting& setting : kCrashReportSettings) {
    if (IsFastVariable(setting.name)) {
      overrides[std::string(setting.name)] = std::string(setting.value);
    }
  }
  *merged_json = overrides.dump();
  return true;
}

}  // namespace runtime
}  // namespace mocktail
