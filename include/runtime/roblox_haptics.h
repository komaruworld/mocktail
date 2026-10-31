#ifndef MOCKTAIL_RUNTIME_ROBLOX_HAPTICS_H_
#define MOCKTAIL_RUNTIME_ROBLOX_HAPTICS_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace mocktail {
namespace runtime {

class RobloxHaptics final {
 public:
  bool Submit(const std::string& method, const std::string& json);
  void SetEnabled(bool enabled);
  void SetAvailable(bool available);
  bool available() const;
  void SetFocused(bool focused);
  void Cancel();
  float Sample(uint64_t now_ns);

 private:
  mutable std::mutex mutex_;
  bool available_ = false;
  bool enabled_ = true;
  bool focused_ = true;
  bool pending_ = false;
  uint64_t start_ns_ = 0;
  double sample_ms_ = 0;
  std::vector<float> samples_;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_HAPTICS_H_
