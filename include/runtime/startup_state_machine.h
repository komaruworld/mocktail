#ifndef MOCKTAIL_RUNTIME_STARTUP_STATE_MACHINE_H_
#define MOCKTAIL_RUNTIME_STARTUP_STATE_MACHINE_H_

#include <cstdint>
#include <mutex>
#include <string>

namespace mocktail {
namespace runtime {

enum class StartupState {
  kCreated,
  kConfigured,
  kJniRuntimeReady,
  kLibrariesLoaded,
  kJniOnLoadComplete,
  kEngineStarted,
  kDataModelLoading,
  kLuaAppReady,
  kReady,
  kFailure,
  kStopping,
  kStopped,
};

enum class StartupEvidence {
  kConfigured,
  kJniRuntimeReady,
  kLibrariesLoaded,
  kJniOnLoadComplete,
  kEngineStarted,
  kDataModelLoading,
  kLuaAppReady,
  kRealFramePresented,
  kInputReady,
  kAudioReady,
};

struct StartupRequirements {
  bool windowed = false;
  bool require_input_ready = false;
  bool require_audio_ready = false;
};

enum class StartupUpdateStatus {
  kApplied,
  kDuplicate,
  kRejected,
};

struct StartupUpdateResult {
  StartupUpdateStatus status = StartupUpdateStatus::kRejected;
  StartupState state = StartupState::kCreated;
  std::string message;

  bool ok() const { return status != StartupUpdateStatus::kRejected; }
  bool changed() const { return status == StartupUpdateStatus::kApplied; }
};

struct StartupSnapshot {
  StartupState state = StartupState::kCreated;
  StartupRequirements requirements;
  bool configured = false;
  bool jni_runtime_ready = false;
  bool libraries_loaded = false;
  bool jni_on_load_complete = false;
  bool engine_started = false;
  bool data_model_loading = false;
  bool lua_app_ready = false;
  bool real_frame_presented = false;
  bool input_ready = false;
  bool audio_ready = false;
  uint64_t revision = 0;
  std::string failure_reason;

  bool ready() const { return state == StartupState::kReady; }
  bool terminal() const {
    return state == StartupState::kFailure || state == StartupState::kStopped;
  }
};

// Thread-safe startup evidence collector and lifecycle state machine.
//
// Core startup evidence is strictly ordered. Auxiliary readiness evidence may
// arrive early, but only after configuration; it cannot make the runtime Ready
// before the complete core sequence. Invalid new evidence transitions an active
// startup to Failure. Repeated evidence that was already accepted is
// idempotent.
class StartupStateMachine {
 public:
  explicit StartupStateMachine(StartupRequirements requirements = {});

  StartupStateMachine(const StartupStateMachine&) = delete;
  StartupStateMachine& operator=(const StartupStateMachine&) = delete;
  StartupStateMachine(StartupStateMachine&&) = delete;
  StartupStateMachine& operator=(StartupStateMachine&&) = delete;

  StartupUpdateResult RecordEvidence(StartupEvidence evidence);
  StartupUpdateResult Fail(std::string reason);
  StartupUpdateResult BeginStopping();
  StartupUpdateResult MarkStopped();

  StartupSnapshot Snapshot() const;

 private:
  StartupUpdateResult RecordCoreEvidenceLocked(StartupEvidence evidence);
  StartupUpdateResult RecordAuxiliaryEvidenceLocked(StartupEvidence evidence);
  StartupUpdateResult RejectInvalidOrderLocked(StartupEvidence evidence,
                                               const char* expected);
  void EvaluateReadyLocked();
  bool HasEvidenceLocked(StartupEvidence evidence) const;
  void AddEvidenceLocked(StartupEvidence evidence);
  StartupSnapshot SnapshotLocked() const;

  mutable std::mutex mutex_;
  const StartupRequirements requirements_;
  StartupState state_ = StartupState::kCreated;
  uint32_t evidence_bits_ = 0;
  uint64_t revision_ = 0;
  std::string failure_reason_;
};

const char* StartupStateName(StartupState state);
const char* StartupEvidenceName(StartupEvidence evidence);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_STARTUP_STATE_MACHINE_H_
