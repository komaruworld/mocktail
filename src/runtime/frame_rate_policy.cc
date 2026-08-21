#include "runtime/frame_rate_policy.h"

#define JSON_NOEXCEPTION 1
#include <nlohmann/json.hpp>

#include <array>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

bool SetCompatibleValue(nlohmann::json *object, const char *key,
                        const std::string &value, std::string *error) {
  const auto existing = object->find(key);
  if (existing != object->end()) {
    if (!existing->is_string() || existing->get<std::string>() != value) {
      if (error != nullptr) {
        *error = std::string("frame-rate policy conflicts with ") + key;
      }
      return false;
    }
    return true;
  }
  (*object)[key] = value;
  return true;
}

} // namespace

FrameRatePolicy ParseFrameRatePolicy(std::string_view value) {
  if (value.empty() || value == "display") {
    return {};
  }
  if (value == "unlimited") {
    return {FrameRateLimitMode::kUnlimited, 0};
  }
  constexpr std::array<int, 5> kSupportedFixedRates = {30, 60, 120, 144, 240};
  for (const int rate : kSupportedFixedRates) {
    if (value == std::to_string(rate)) {
      return {FrameRateLimitMode::kFixed, rate};
    }
  }
  return {FrameRateLimitMode::kInvalid, 0};
}

bool MergeFrameRateClientSettingsOverrides(const FrameRatePolicy &policy,
                                           std::string_view base_json,
                                           std::string *merged_json,
                                           std::string *error) {
  if (merged_json == nullptr || !policy.valid()) {
    if (error != nullptr) {
      *error = "invalid frame-rate policy";
    }
    return false;
  }
  nlohmann::json overrides = nlohmann::json::parse(
      base_json.empty() ? "{}" : base_json, nullptr, false, true);
  if (overrides.is_discarded() || !overrides.is_object()) {
    if (error != nullptr) {
      *error = "client-settings overrides must be a JSON object";
    }
    return false;
  }
  if (policy.mode == FrameRateLimitMode::kDisplay) {
    *merged_json = overrides.dump();
    return true;
  }
  const std::string target =
      policy.mode == FrameRateLimitMode::kUnlimited
          ? std::to_string(kMaximumSupportedRobloxSchedulerFps)
          : std::to_string(policy.fixed_fps);
  if (!SetCompatibleValue(&overrides, "DFIntTaskSchedulerTargetFps", target,
                          error)) {
    return false;
  }
  *merged_json = overrides.dump();
  return true;
}

} // namespace runtime
} // namespace mocktail
