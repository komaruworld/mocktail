#ifndef MOCKTAIL_WINDOW_VULKAN_PRESENT_PROGRESS_GATE_H_
#define MOCKTAIL_WINDOW_VULKAN_PRESENT_PROGRESS_GATE_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace mocktail {
namespace window {

enum class VulkanPresentStallStage {
  kBeforeHostPresent,
  kInsideVulkanCall,
  kInsideHostPresent,
};

struct VulkanPresentStallReport {
  VulkanPresentStallStage stage = VulkanPresentStallStage::kBeforeHostPresent;
  const char* call_name = nullptr;
  uint64_t call_sequence = 0;
  uint64_t thread_id = 0;
  uint64_t elapsed_ns = 0;
  uint64_t frame_count = 0;
  uint64_t host_present_count = 0;
  uint64_t last_present_thread_id = 0;
  bool last_completed_call_snapshot_coherent = false;
  const char* last_completed_call_name = nullptr;
  uint64_t last_completed_call_sequence = 0;
  uint64_t last_completed_call_thread_id = 0;
  uint64_t last_completed_call_started_ns = 0;
  uint64_t last_completed_call_completed_ns = 0;
  int32_t last_completed_call_result = 0;
  const char* oldest_active_call_name = nullptr;
  uint64_t oldest_active_call_sequence = 0;
  uint64_t oldest_active_call_thread_id = 0;
  uint64_t oldest_active_call_elapsed_ns = 0;
};

// Transfers host-present progress from the Vulkan render thread to the SDL
// event thread. This gate is diagnostic only: a blocked vkQueuePresentKHR
// cannot be cancelled safely while the guest still owns the same VkQueue.
class VulkanPresentProgressGate final {
 public:
  static constexpr uint64_t kStallThresholdNs = 5000000000ULL;

  VulkanPresentProgressGate() = default;
  VulkanPresentProgressGate(const VulkanPresentProgressGate&) = delete;
  VulkanPresentProgressGate& operator=(const VulkanPresentProgressGate&) =
      delete;

  bool Activate() {
    uint8_t expected = kLifecycleInactive;
    if (!lifecycle_state_.compare_exchange_strong(
            expected, kLifecycleInitializing, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return false;
    }
    lifecycle_epoch_.fetch_add(1, std::memory_order_acq_rel);
    ResetProgress();
    lifecycle_state_.store(kLifecycleActive, std::memory_order_release);
    return true;
  }

  void Deactivate() {
    lifecycle_state_.store(kLifecycleInactive, std::memory_order_release);
    lifecycle_epoch_.fetch_add(1, std::memory_order_acq_rel);
    ResetProgress();
  }

  uint64_t NotifyHostPresentBegin(uint64_t now_ticks_ns,
                                  uint64_t thread_id = 0) {
    if (!IsActive()) {
      return 0;
    }
    const uint64_t lifecycle_epoch =
        lifecycle_epoch_.load(std::memory_order_acquire);
    host_present_started_ns_.store(now_ticks_ns, std::memory_order_relaxed);
    const uint64_t sequence =
        host_present_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    host_present_thread_id_.store(thread_id, std::memory_order_relaxed);
    host_present_lifecycle_epoch_.store(lifecycle_epoch,
                                        std::memory_order_relaxed);
    if (!IsActive() ||
        lifecycle_epoch_.load(std::memory_order_acquire) != lifecycle_epoch) {
      return 0;
    }
    host_present_in_flight_.store(true, std::memory_order_release);
    return sequence;
  }

  void NotifyHostPresentEnd(uint64_t sequence) {
    if (sequence == 0 || !IsActive() ||
        host_present_count_.load(std::memory_order_acquire) != sequence) {
      return;
    }
    host_present_in_flight_.store(false, std::memory_order_release);
  }

  // Records a potentially blocking host Vulkan call without serializing the
  // caller. The returned sequence must be passed to NotifyCallEnd. A fixed
  // slot table is sufficient because only threads currently inside a tracked
  // call occupy a slot; exhaustion drops diagnostics but never changes Vulkan
  // behavior.
  uint64_t NotifyCallBegin(const char* call_name, uint64_t now_ticks_ns,
                           uint64_t thread_id) {
    if (call_name == nullptr || !IsActive()) {
      return 0;
    }
    const uint64_t lifecycle_epoch =
        lifecycle_epoch_.load(std::memory_order_acquire);
    if (!IsActive()) {
      return 0;
    }
    uint64_t sequence =
        next_call_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence == 0 || sequence == kReservedCallSequence) {
      sequence =
          next_call_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    for (CallSlot& slot : call_slots_) {
      uint64_t expected = 0;
      if (!slot.sequence.compare_exchange_strong(
              expected, kReservedCallSequence, std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        continue;
      }
      slot.call_name.store(call_name, std::memory_order_relaxed);
      slot.started_ns.store(now_ticks_ns, std::memory_order_relaxed);
      slot.thread_id.store(thread_id, std::memory_order_relaxed);
      slot.lifecycle_epoch.store(lifecycle_epoch, std::memory_order_relaxed);
      if (!IsActive() ||
          lifecycle_epoch_.load(std::memory_order_acquire) !=
              lifecycle_epoch) {
        slot.sequence.store(0, std::memory_order_release);
        return 0;
      }
      slot.sequence.store(sequence, std::memory_order_release);
      return sequence;
    }
    return 0;
  }

  void NotifyCallEnd(uint64_t sequence, int32_t result,
                     uint64_t now_ticks_ns = 0, uint64_t thread_id = 0) {
    if (sequence == 0 || sequence == kReservedCallSequence) {
      return;
    }
    for (CallSlot& slot : call_slots_) {
      uint64_t expected = sequence;
      if (!slot.sequence.compare_exchange_strong(
              expected, kReservedCallSequence, std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        continue;
      }
      const uint64_t completed_thread_id =
          thread_id != 0 ? thread_id
                         : slot.thread_id.load(std::memory_order_relaxed);
      const uint64_t call_lifecycle_epoch =
          slot.lifecycle_epoch.load(std::memory_order_relaxed);
      const uint64_t render_thread_id =
          last_present_thread_id_.load(std::memory_order_acquire);
      const uint64_t current_lifecycle_epoch =
          lifecycle_epoch_.load(std::memory_order_acquire);
      // The last-completed record is diagnostic only. A bounded seqlock write
      // never delays a Vulkan caller. Once a frame identifies the render
      // thread, unrelated worker completions cannot replace its evidence.
      uint64_t record_version =
          last_call_record_version_.load(std::memory_order_relaxed);
      if (IsActive() &&
          call_lifecycle_epoch == current_lifecycle_epoch &&
          (render_thread_id == 0 ||
           completed_thread_id == render_thread_id) &&
          (record_version & 1U) == 0U &&
          last_call_record_version_.compare_exchange_strong(
              record_version, record_version + 1, std::memory_order_acquire,
              std::memory_order_relaxed)) {
        last_call_name_.store(slot.call_name.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
        last_call_sequence_.store(sequence, std::memory_order_relaxed);
        last_call_thread_id_.store(completed_thread_id,
                                   std::memory_order_relaxed);
        last_call_started_ns_.store(
            slot.started_ns.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        last_call_completed_ns_.store(now_ticks_ns,
                                      std::memory_order_relaxed);
        last_call_result_.store(result, std::memory_order_relaxed);
        last_call_lifecycle_epoch_.store(call_lifecycle_epoch,
                                         std::memory_order_relaxed);
        last_call_record_version_.store(record_version + 2,
                                        std::memory_order_release);
      }
      slot.sequence.store(0, std::memory_order_release);
      return;
    }
  }

  void NotifyFramePresented(uint64_t now_ticks_ns, uint64_t frame_count,
                            uint64_t thread_id = 0) {
    if (!IsActive()) {
      return;
    }
    const uint64_t lifecycle_epoch =
        lifecycle_epoch_.load(std::memory_order_acquire);
    last_progress_ns_.store(now_ticks_ns, std::memory_order_relaxed);
    frame_count_.store(frame_count, std::memory_order_relaxed);
    last_present_thread_id_.store(thread_id, std::memory_order_relaxed);
    last_present_lifecycle_epoch_.store(lifecycle_epoch,
                                        std::memory_order_relaxed);
    if (!IsActive() ||
        lifecycle_epoch_.load(std::memory_order_acquire) != lifecycle_epoch) {
      return;
    }
    present_generation_.fetch_add(1, std::memory_order_release);
    has_presented_frame_.store(true, std::memory_order_release);
  }

  bool Poll(uint64_t now_ticks_ns, bool window_can_present,
            VulkanPresentStallReport* report) {
    if (report == nullptr || !window_can_present ||
        !IsActive() ||
        !has_presented_frame_.load(std::memory_order_acquire)) {
      return false;
    }

    const uint64_t lifecycle_epoch =
        lifecycle_epoch_.load(std::memory_order_acquire);
    const uint64_t present_generation =
        present_generation_.load(std::memory_order_acquire);
    if (last_present_lifecycle_epoch_.load(std::memory_order_acquire) !=
        lifecycle_epoch) {
      return false;
    }
    const uint64_t last_progress_ns =
        last_progress_ns_.load(std::memory_order_acquire);
    if (present_generation == 0 || last_progress_ns == 0 ||
        now_ticks_ns < last_progress_ns ||
        now_ticks_ns - last_progress_ns < kStallThresholdNs) {
      return false;
    }
    const uint64_t render_thread_id =
        last_present_thread_id_.load(std::memory_order_acquire);
    const uint64_t frame_count =
        frame_count_.load(std::memory_order_acquire);

    const bool host_present_in_flight =
        host_present_in_flight_.load(std::memory_order_acquire);
    uint64_t host_present_started_ns = 0;
    uint64_t host_present_count = 0;
    uint64_t host_present_thread_id = 0;
    bool stalled_host_present = false;
    bool blocked_host_present = false;
    if (host_present_in_flight) {
      host_present_started_ns =
          host_present_started_ns_.load(std::memory_order_acquire);
      host_present_count =
          host_present_count_.load(std::memory_order_acquire);
      host_present_thread_id =
          host_present_thread_id_.load(std::memory_order_acquire);
      stalled_host_present =
          host_present_lifecycle_epoch_.load(std::memory_order_acquire) ==
              lifecycle_epoch &&
          host_present_started_ns >= last_progress_ns &&
          now_ticks_ns >= host_present_started_ns &&
          now_ticks_ns - host_present_started_ns >= kStallThresholdNs;
      blocked_host_present =
          stalled_host_present &&
          (render_thread_id == 0 ||
           host_present_thread_id == render_thread_id);
    }
    const char* call_name = nullptr;
    uint64_t call_sequence = 0;
    uint64_t call_thread_id = 0;
    uint64_t baseline_ns = last_progress_ns;
    const char* oldest_active_call_name = nullptr;
    uint64_t oldest_active_call_sequence = 0;
    uint64_t oldest_active_call_thread_id = 0;
    uint64_t oldest_active_call_started_ns = now_ticks_ns;
    if (stalled_host_present && !blocked_host_present) {
      oldest_active_call_name = "vkQueuePresentKHR(host)";
      oldest_active_call_sequence = host_present_count;
      oldest_active_call_thread_id = host_present_thread_id;
      oldest_active_call_started_ns = host_present_started_ns;
    }
    for (const CallSlot& slot : call_slots_) {
      const uint64_t sequence =
          slot.sequence.load(std::memory_order_acquire);
      if (sequence == 0 || sequence == kReservedCallSequence) {
        continue;
      }
      const uint64_t started_ns =
          slot.started_ns.load(std::memory_order_relaxed);
      const char* candidate_name =
          slot.call_name.load(std::memory_order_relaxed);
      const uint64_t candidate_thread_id =
          slot.thread_id.load(std::memory_order_relaxed);
      const uint64_t candidate_lifecycle_epoch =
          slot.lifecycle_epoch.load(std::memory_order_relaxed);
      if (slot.sequence.load(std::memory_order_acquire) == sequence &&
          candidate_lifecycle_epoch == lifecycle_epoch &&
          candidate_name != nullptr && now_ticks_ns >= started_ns &&
          now_ticks_ns - started_ns >= kStallThresholdNs &&
          (oldest_active_call_sequence == 0 ||
           started_ns < oldest_active_call_started_ns)) {
        oldest_active_call_name = candidate_name;
        oldest_active_call_sequence = sequence;
        oldest_active_call_thread_id = candidate_thread_id;
        oldest_active_call_started_ns = started_ns;
      }
      if (blocked_host_present ||
          slot.sequence.load(std::memory_order_acquire) != sequence ||
          candidate_lifecycle_epoch != lifecycle_epoch ||
          started_ns < last_progress_ns || now_ticks_ns < started_ns ||
          now_ticks_ns - started_ns < kStallThresholdNs ||
          candidate_name == nullptr ||
          (render_thread_id != 0 &&
           candidate_thread_id != render_thread_id) ||
          (call_sequence != 0 && started_ns >= baseline_ns)) {
        continue;
      }
      baseline_ns = started_ns;
      call_name = candidate_name;
      call_sequence = sequence;
      call_thread_id = candidate_thread_id;
    }
    *report = VulkanPresentStallReport{};
    report->stage =
        blocked_host_present
            ? VulkanPresentStallStage::kInsideHostPresent
            : (call_sequence != 0
                   ? VulkanPresentStallStage::kInsideVulkanCall
                   : VulkanPresentStallStage::kBeforeHostPresent);
    report->call_name =
        blocked_host_present ? "vkQueuePresentKHR(host)" : call_name;
    report->call_sequence =
        blocked_host_present ? host_present_count : call_sequence;
    report->thread_id =
        blocked_host_present ? host_present_thread_id : call_thread_id;
    report->elapsed_ns =
        now_ticks_ns -
        (blocked_host_present ? host_present_started_ns : baseline_ns);
    report->frame_count = frame_count;
    report->host_present_count =
        host_present_count_.load(std::memory_order_acquire);
    report->last_present_thread_id = render_thread_id;
    for (int attempt = 0; attempt < 2; ++attempt) {
      const uint64_t record_version_before =
          last_call_record_version_.load(std::memory_order_acquire);
      if ((record_version_before & 1U) != 0U) {
        continue;
      }
      const char* completed_name =
          last_call_name_.load(std::memory_order_relaxed);
      const uint64_t completed_sequence =
          last_call_sequence_.load(std::memory_order_relaxed);
      const uint64_t completed_thread_id =
          last_call_thread_id_.load(std::memory_order_relaxed);
      const uint64_t completed_started_ns =
          last_call_started_ns_.load(std::memory_order_relaxed);
      const uint64_t completed_ns =
          last_call_completed_ns_.load(std::memory_order_relaxed);
      const int32_t completed_result =
          last_call_result_.load(std::memory_order_relaxed);
      const uint64_t completed_lifecycle_epoch =
          last_call_lifecycle_epoch_.load(std::memory_order_relaxed);
      const uint64_t record_version_after =
          last_call_record_version_.load(std::memory_order_acquire);
      if (record_version_before != record_version_after ||
          (record_version_after & 1U) != 0U) {
        continue;
      }
      report->last_completed_call_snapshot_coherent = true;
      if (completed_lifecycle_epoch == lifecycle_epoch &&
          (render_thread_id == 0 ||
           completed_thread_id == render_thread_id)) {
        report->last_completed_call_name = completed_name;
        report->last_completed_call_sequence = completed_sequence;
        report->last_completed_call_thread_id = completed_thread_id;
        report->last_completed_call_started_ns = completed_started_ns;
        report->last_completed_call_completed_ns = completed_ns;
        report->last_completed_call_result = completed_result;
      }
      break;
    }
    report->oldest_active_call_name = oldest_active_call_name;
    report->oldest_active_call_sequence = oldest_active_call_sequence;
    report->oldest_active_call_thread_id = oldest_active_call_thread_id;
    report->oldest_active_call_elapsed_ns =
        oldest_active_call_sequence != 0
            ? now_ticks_ns - oldest_active_call_started_ns
            : 0;

    // A successful present can race the watchdog after the initial snapshot.
    // Reject stale evidence before committing any deduplication key.
    if (!IsActive() ||
        lifecycle_epoch_.load(std::memory_order_acquire) != lifecycle_epoch ||
        last_present_lifecycle_epoch_.load(std::memory_order_acquire) !=
            lifecycle_epoch ||
        present_generation_.load(std::memory_order_acquire) !=
            present_generation ||
        last_progress_ns_.load(std::memory_order_acquire) != last_progress_ns) {
      return false;
    }
    if (blocked_host_present &&
        (!host_present_in_flight_.load(std::memory_order_acquire) ||
         host_present_lifecycle_epoch_.load(std::memory_order_acquire) !=
             lifecycle_epoch ||
         host_present_count_.load(std::memory_order_acquire) !=
             host_present_count)) {
      return false;
    }
    if (call_sequence != 0) {
      bool call_still_active = false;
      for (const CallSlot& slot : call_slots_) {
        if (slot.sequence.load(std::memory_order_acquire) == call_sequence &&
            slot.lifecycle_epoch.load(std::memory_order_relaxed) ==
                lifecycle_epoch) {
          call_still_active = true;
          break;
        }
      }
      if (!call_still_active) {
        return false;
      }
    }
    if (blocked_host_present) {
      if (reported_host_present_count_.exchange(
              host_present_count, std::memory_order_acq_rel) ==
          host_present_count) {
        return false;
      }
      reported_stall_generation_.store(present_generation,
                                       std::memory_order_release);
    } else if (call_sequence != 0) {
      if (reported_call_sequence_.exchange(call_sequence,
                                            std::memory_order_acq_rel) ==
          call_sequence) {
        return false;
      }
      reported_stall_generation_.store(present_generation,
                                       std::memory_order_release);
    } else if (reported_stall_generation_.exchange(
                   present_generation, std::memory_order_acq_rel) ==
               present_generation) {
      return false;
    }
    return true;
  }

 private:
  static constexpr std::size_t kCallSlotCount = 32;
  static constexpr uint64_t kReservedCallSequence =
      std::numeric_limits<uint64_t>::max();
  static constexpr uint8_t kLifecycleInactive = 0;
  static constexpr uint8_t kLifecycleInitializing = 1;
  static constexpr uint8_t kLifecycleActive = 2;

  struct CallSlot {
    std::atomic<uint64_t> sequence{0};
    std::atomic<const char*> call_name{nullptr};
    std::atomic<uint64_t> started_ns{0};
    std::atomic<uint64_t> thread_id{0};
    std::atomic<uint64_t> lifecycle_epoch{0};
  };

  bool IsActive() const {
    return lifecycle_state_.load(std::memory_order_acquire) ==
           kLifecycleActive;
  }

  void ResetProgress() {
    has_presented_frame_.store(false, std::memory_order_relaxed);
    host_present_in_flight_.store(false, std::memory_order_relaxed);
    host_present_started_ns_.store(0, std::memory_order_relaxed);
    last_progress_ns_.store(0, std::memory_order_relaxed);
    frame_count_.store(0, std::memory_order_relaxed);
    last_present_thread_id_.store(0, std::memory_order_relaxed);
  }

  std::atomic<uint8_t> lifecycle_state_{kLifecycleInactive};
  std::atomic<uint64_t> lifecycle_epoch_{0};
  std::atomic<bool> has_presented_frame_{false};
  std::atomic<bool> host_present_in_flight_{false};
  std::atomic<uint64_t> present_generation_{0};
  std::atomic<uint64_t> reported_stall_generation_{0};
  std::atomic<uint64_t> reported_call_sequence_{0};
  std::atomic<uint64_t> reported_host_present_count_{0};
  std::atomic<uint64_t> host_present_started_ns_{0};
  std::atomic<uint64_t> host_present_thread_id_{0};
  std::atomic<uint64_t> host_present_lifecycle_epoch_{0};
  std::atomic<uint64_t> last_progress_ns_{0};
  std::atomic<uint64_t> frame_count_{0};
  std::atomic<uint64_t> host_present_count_{0};
  std::atomic<uint64_t> last_present_thread_id_{0};
  std::atomic<uint64_t> last_present_lifecycle_epoch_{0};
  std::atomic<uint64_t> next_call_sequence_{0};
  std::atomic<uint64_t> last_call_record_version_{0};
  std::atomic<uint64_t> last_call_lifecycle_epoch_{0};
  std::atomic<const char*> last_call_name_{nullptr};
  std::atomic<uint64_t> last_call_sequence_{0};
  std::atomic<uint64_t> last_call_thread_id_{0};
  std::atomic<uint64_t> last_call_started_ns_{0};
  std::atomic<uint64_t> last_call_completed_ns_{0};
  std::atomic<int32_t> last_call_result_{0};
  std::array<CallSlot, kCallSlotCount> call_slots_{};
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_VULKAN_PRESENT_PROGRESS_GATE_H_
