#include "runtime/roblox_haptics.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <nlohmann/json.hpp>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::size_t kMaximumJsonBytes = 256 * 1024;
constexpr std::size_t kMaximumSamples = 16384;
constexpr double kMaximumSampleMs = 60000;

bool HasBoundedDepth(const std::string& json) {
  int depth = 0;
  bool quoted = false;
  bool escaped = false;
  for (const char ch : json) {
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (ch == '\\')
        escaped = true;
      else if (ch == '"')
        quoted = false;
    } else if (ch == '"') {
      quoted = true;
    } else if (ch == '{' || ch == '[') {
      if (++depth > 8)
        return false;
    } else if (ch == '}' || ch == ']') {
      --depth;
    }
  }
  return true;
}

bool ReadIntensity(const nlohmann::json& value, float* output) {
  double f;
  if (value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    char* end = nullptr;
    f = std::strtod(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size())
      return false;
  } else if (value.is_number()) {
    f = value.get<double>();
  } else {
    return false;
  }
  if (!std::isfinite(f))
    return false;
  *output = static_cast<float>(std::clamp(f, 0.0, 1.0));
  return true;
}

}  // namespace

bool RobloxHaptics::Submit(const std::string& method, const std::string& json) {
  if (json.size() > kMaximumJsonBytes || !HasBoundedDepth(json))
    return false;
  const auto body = nlohmann::json::parse(json, nullptr, false);
  if (!body.is_object())
    return false;
  std::vector<float> samples;
  double interval = 0;
  if (method == "UpdateSingletonVibration") {
    float value = 0;
    if (!body.contains("intensity") || !ReadIntensity(body["intensity"], &value))
      return false;
    samples.push_back(value);
  } else if (method == "SetMotorState") {
    if (!body.empty()) {
      if (body.size() != 1 || !body.contains("defaultMotor"))
        return false;
      const auto& motor = body["defaultMotor"];
      if (!motor.is_object() || !motor.contains("intensities") ||
          !motor.contains("millisecondsPerSample"))
        return false;
      const auto& values = motor["intensities"];
      if (!values.is_array() || values.size() > kMaximumSamples ||
          !motor["millisecondsPerSample"].is_number())
        return false;
      interval = motor["millisecondsPerSample"].get<double>();
      if (!std::isfinite(interval) || interval <= 0 ||
          interval > kMaximumSampleMs)
        return false;
      for (const auto& entry : values) {
        float value = 0;
        if (!ReadIntensity(entry, &value))
          return false;
        samples.push_back(value);
      }
    }
  } else {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_ || !available_ || !focused_)
    return true;
  samples_ = std::move(samples);
  sample_ms_ = interval;
  pending_ = true;
  return true;
}

void RobloxHaptics::SetEnabled(bool value) {
  std::lock_guard<std::mutex> lock(mutex_);
  enabled_ = value;
  if (!value) {
    samples_.clear();
    pending_ = false;
  }
}

void RobloxHaptics::SetAvailable(bool value) {
  std::lock_guard<std::mutex> lock(mutex_);
  available_ = value;
  if (!value) {
    samples_.clear();
    pending_ = false;
  }
}

bool RobloxHaptics::available() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return enabled_ && available_;
}

void RobloxHaptics::SetFocused(bool value) {
  std::lock_guard<std::mutex> lock(mutex_);
  focused_ = value;
  if (!value) {
    samples_.clear();
    pending_ = false;
  }
}

void RobloxHaptics::Cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  samples_.clear();
  pending_ = false;
}

float RobloxHaptics::Sample(uint64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_ || !available_ || !focused_ || samples_.empty())
    return 0;
  if (pending_) {
    start_ns_ = now;
    pending_ = false;
  }
  if (sample_ms_ == 0)
    return samples_.front();
  const double index =
      static_cast<double>(now - start_ns_) / (sample_ms_ * 1000000.0);
  if (index >= samples_.size()) {
    samples_.clear();
    return 0;
  }
  return samples_[static_cast<std::size_t>(index)];
}

}  // namespace runtime
}  // namespace mocktail
