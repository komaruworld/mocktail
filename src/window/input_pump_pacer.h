#ifndef MOCKTAIL_WINDOW_INPUT_PUMP_PACER_H_
#define MOCKTAIL_WINDOW_INPUT_PUMP_PACER_H_

#include <cstdint>
#include <limits>

namespace mocktail {
namespace window {

inline constexpr uint64_t kProductionInputPumpHz = 240;
inline constexpr uint64_t kProductionInputPumpIntervalNs =
    1000000000ULL / kProductionInputPumpHz;

// Keeps SDL input polling aligned to Roblox's maximum supported render
// cadence. A missed deadline is rebased instead of issuing catch-up pumps,
// which prevents motion events from being delivered in artificial bursts.
class InputPumpPacer final {
 public:
  explicit InputPumpPacer(
      uint64_t interval_ns = kProductionInputPumpIntervalNs)
      : interval_ns_(interval_ns) {}

  uint64_t DelayBeforeNextPump(uint64_t now_ns) {
    if (interval_ns_ == 0) {
      return 0;
    }
    if (next_deadline_ns_ == 0 || now_ns > next_deadline_ns_) {
      next_deadline_ns_ = AddSaturating(now_ns, interval_ns_);
    }
    const uint64_t delay_ns = next_deadline_ns_ - now_ns;
    next_deadline_ns_ = AddSaturating(next_deadline_ns_, interval_ns_);
    return delay_ns;
  }

  void Reset() { next_deadline_ns_ = 0; }

 private:
  static uint64_t AddSaturating(uint64_t value, uint64_t increment) {
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    return value > maximum - increment ? maximum : value + increment;
  }

  const uint64_t interval_ns_;
  uint64_t next_deadline_ns_ = 0;
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_INPUT_PUMP_PACER_H_
