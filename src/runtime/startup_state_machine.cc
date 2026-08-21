#include "runtime/startup_state_machine.h"

#include <utility>

namespace mocktail {
namespace runtime {
namespace {

constexpr bool IsCoreEvidence(StartupEvidence evidence) {
  return evidence >= StartupEvidence::kConfigured &&
         evidence <= StartupEvidence::kLuaAppReady;
}

constexpr bool IsKnownEvidence(StartupEvidence evidence) {
  switch (evidence) {
    case StartupEvidence::kConfigured:
    case StartupEvidence::kJniRuntimeReady:
    case StartupEvidence::kLibrariesLoaded:
    case StartupEvidence::kJniOnLoadComplete:
    case StartupEvidence::kEngineStarted:
    case StartupEvidence::kDataModelLoading:
    case StartupEvidence::kLuaAppReady:
    case StartupEvidence::kRealFramePresented:
    case StartupEvidence::kInputReady:
    case StartupEvidence::kAudioReady:
      return true;
  }
  return false;
}

constexpr uint32_t EvidenceBit(StartupEvidence evidence) {
  return uint32_t{1} << static_cast<uint32_t>(evidence);
}

StartupState StateForCoreEvidence(StartupEvidence evidence) {
  switch (evidence) {
    case StartupEvidence::kConfigured:
      return StartupState::kConfigured;
    case StartupEvidence::kJniRuntimeReady:
      return StartupState::kJniRuntimeReady;
    case StartupEvidence::kLibrariesLoaded:
      return StartupState::kLibrariesLoaded;
    case StartupEvidence::kJniOnLoadComplete:
      return StartupState::kJniOnLoadComplete;
    case StartupEvidence::kEngineStarted:
      return StartupState::kEngineStarted;
    case StartupEvidence::kDataModelLoading:
      return StartupState::kDataModelLoading;
    case StartupEvidence::kLuaAppReady:
      return StartupState::kLuaAppReady;
    case StartupEvidence::kRealFramePresented:
    case StartupEvidence::kInputReady:
    case StartupEvidence::kAudioReady:
      return StartupState::kFailure;
  }
  return StartupState::kFailure;
}

StartupEvidence ExpectedEvidenceForState(StartupState state) {
  switch (state) {
    case StartupState::kCreated:
      return StartupEvidence::kConfigured;
    case StartupState::kConfigured:
      return StartupEvidence::kJniRuntimeReady;
    case StartupState::kJniRuntimeReady:
      return StartupEvidence::kLibrariesLoaded;
    case StartupState::kLibrariesLoaded:
      return StartupEvidence::kJniOnLoadComplete;
    case StartupState::kJniOnLoadComplete:
      return StartupEvidence::kEngineStarted;
    case StartupState::kEngineStarted:
      return StartupEvidence::kDataModelLoading;
    case StartupState::kDataModelLoading:
      return StartupEvidence::kLuaAppReady;
    case StartupState::kLuaAppReady:
    case StartupState::kReady:
    case StartupState::kFailure:
    case StartupState::kStopping:
    case StartupState::kStopped:
      return StartupEvidence::kLuaAppReady;
  }
  return StartupEvidence::kConfigured;
}

}  // namespace

StartupStateMachine::StartupStateMachine(StartupRequirements requirements)
    : requirements_(requirements) {}

StartupUpdateResult StartupStateMachine::RecordEvidence(
    StartupEvidence evidence) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsKnownEvidence(evidence)) {
    return RejectInvalidOrderLocked(
        evidence, StartupEvidenceName(ExpectedEvidenceForState(state_)));
  }
  if (HasEvidenceLocked(evidence)) {
    return {StartupUpdateStatus::kDuplicate, state_,
            std::string("duplicate startup evidence: ") +
                StartupEvidenceName(evidence)};
  }
  if (state_ == StartupState::kFailure || state_ == StartupState::kStopping ||
      state_ == StartupState::kStopped) {
    return {StartupUpdateStatus::kRejected, state_,
            std::string("startup evidence rejected in state ") +
                StartupStateName(state_)};
  }
  return IsCoreEvidence(evidence) ? RecordCoreEvidenceLocked(evidence)
                                  : RecordAuxiliaryEvidenceLocked(evidence);
}

StartupUpdateResult StartupStateMachine::RecordCoreEvidenceLocked(
    StartupEvidence evidence) {
  const StartupEvidence expected = ExpectedEvidenceForState(state_);
  if (evidence != expected) {
    return RejectInvalidOrderLocked(evidence, StartupEvidenceName(expected));
  }

  AddEvidenceLocked(evidence);
  state_ = StateForCoreEvidence(evidence);
  ++revision_;
  EvaluateReadyLocked();
  return {StartupUpdateStatus::kApplied, state_,
          std::string("accepted startup evidence: ") +
              StartupEvidenceName(evidence)};
}

StartupUpdateResult StartupStateMachine::RecordAuxiliaryEvidenceLocked(
    StartupEvidence evidence) {
  if (!HasEvidenceLocked(StartupEvidence::kConfigured)) {
    return RejectInvalidOrderLocked(evidence, "Configured");
  }
  if (evidence == StartupEvidence::kRealFramePresented &&
      !HasEvidenceLocked(StartupEvidence::kEngineStarted)) {
    return RejectInvalidOrderLocked(evidence, "EngineStarted");
  }

  AddEvidenceLocked(evidence);
  ++revision_;
  EvaluateReadyLocked();
  return {StartupUpdateStatus::kApplied, state_,
          std::string("accepted startup evidence: ") +
              StartupEvidenceName(evidence)};
}

StartupUpdateResult StartupStateMachine::RejectInvalidOrderLocked(
    StartupEvidence evidence, const char* expected) {
  failure_reason_ = std::string("invalid startup evidence order in state ") +
                    StartupStateName(state_) + ": expected " +
                    (expected != nullptr ? expected : "no further evidence") +
                    ", received " + StartupEvidenceName(evidence);
  state_ = StartupState::kFailure;
  ++revision_;
  return {StartupUpdateStatus::kRejected, state_, failure_reason_};
}

void StartupStateMachine::EvaluateReadyLocked() {
  if (!HasEvidenceLocked(StartupEvidence::kLuaAppReady)) {
    return;
  }
  if (requirements_.windowed &&
      !HasEvidenceLocked(StartupEvidence::kRealFramePresented)) {
    state_ = StartupState::kLuaAppReady;
    return;
  }
  if (requirements_.require_input_ready &&
      !HasEvidenceLocked(StartupEvidence::kInputReady)) {
    state_ = StartupState::kLuaAppReady;
    return;
  }
  if (requirements_.require_audio_ready &&
      !HasEvidenceLocked(StartupEvidence::kAudioReady)) {
    state_ = StartupState::kLuaAppReady;
    return;
  }
  state_ = StartupState::kReady;
}

StartupUpdateResult StartupStateMachine::Fail(std::string reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == StartupState::kFailure) {
    return {StartupUpdateStatus::kDuplicate, state_, failure_reason_};
  }
  if (state_ == StartupState::kStopped) {
    return {StartupUpdateStatus::kRejected, state_,
            "cannot fail an already stopped runtime"};
  }
  failure_reason_ = reason.empty() ? "startup failed" : std::move(reason);
  state_ = StartupState::kFailure;
  ++revision_;
  return {StartupUpdateStatus::kApplied, state_, failure_reason_};
}

StartupUpdateResult StartupStateMachine::BeginStopping() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == StartupState::kStopping || state_ == StartupState::kStopped) {
    return {StartupUpdateStatus::kDuplicate, state_,
            std::string("runtime is already ") + StartupStateName(state_)};
  }
  state_ = StartupState::kStopping;
  ++revision_;
  return {StartupUpdateStatus::kApplied, state_, "runtime stopping"};
}

StartupUpdateResult StartupStateMachine::MarkStopped() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == StartupState::kStopped) {
    return {StartupUpdateStatus::kDuplicate, state_,
            "runtime is already Stopped"};
  }
  if (state_ != StartupState::kStopping) {
    failure_reason_ =
        std::string(
            "invalid lifecycle order: Stopped requires Stopping, was ") +
        StartupStateName(state_);
    state_ = StartupState::kFailure;
    ++revision_;
    return {StartupUpdateStatus::kRejected, state_, failure_reason_};
  }
  state_ = StartupState::kStopped;
  ++revision_;
  return {StartupUpdateStatus::kApplied, state_, "runtime stopped"};
}

StartupSnapshot StartupStateMachine::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return SnapshotLocked();
}

bool StartupStateMachine::HasEvidenceLocked(StartupEvidence evidence) const {
  return (evidence_bits_ & EvidenceBit(evidence)) != 0;
}

void StartupStateMachine::AddEvidenceLocked(StartupEvidence evidence) {
  evidence_bits_ |= EvidenceBit(evidence);
}

StartupSnapshot StartupStateMachine::SnapshotLocked() const {
  StartupSnapshot snapshot;
  snapshot.state = state_;
  snapshot.requirements = requirements_;
  snapshot.configured = HasEvidenceLocked(StartupEvidence::kConfigured);
  snapshot.jni_runtime_ready =
      HasEvidenceLocked(StartupEvidence::kJniRuntimeReady);
  snapshot.libraries_loaded =
      HasEvidenceLocked(StartupEvidence::kLibrariesLoaded);
  snapshot.jni_on_load_complete =
      HasEvidenceLocked(StartupEvidence::kJniOnLoadComplete);
  snapshot.engine_started = HasEvidenceLocked(StartupEvidence::kEngineStarted);
  snapshot.data_model_loading =
      HasEvidenceLocked(StartupEvidence::kDataModelLoading);
  snapshot.lua_app_ready = HasEvidenceLocked(StartupEvidence::kLuaAppReady);
  snapshot.real_frame_presented =
      HasEvidenceLocked(StartupEvidence::kRealFramePresented);
  snapshot.input_ready = HasEvidenceLocked(StartupEvidence::kInputReady);
  snapshot.audio_ready = HasEvidenceLocked(StartupEvidence::kAudioReady);
  snapshot.revision = revision_;
  snapshot.failure_reason = failure_reason_;
  return snapshot;
}

const char* StartupStateName(StartupState state) {
  switch (state) {
    case StartupState::kCreated:
      return "Created";
    case StartupState::kConfigured:
      return "Configured";
    case StartupState::kJniRuntimeReady:
      return "JniRuntimeReady";
    case StartupState::kLibrariesLoaded:
      return "LibrariesLoaded";
    case StartupState::kJniOnLoadComplete:
      return "JniOnLoadComplete";
    case StartupState::kEngineStarted:
      return "EngineStarted";
    case StartupState::kDataModelLoading:
      return "DataModelLoading";
    case StartupState::kLuaAppReady:
      return "LuaAppReady";
    case StartupState::kReady:
      return "Ready";
    case StartupState::kFailure:
      return "Failure";
    case StartupState::kStopping:
      return "Stopping";
    case StartupState::kStopped:
      return "Stopped";
  }
  return "Unknown";
}

const char* StartupEvidenceName(StartupEvidence evidence) {
  switch (evidence) {
    case StartupEvidence::kConfigured:
      return "Configured";
    case StartupEvidence::kJniRuntimeReady:
      return "JniRuntimeReady";
    case StartupEvidence::kLibrariesLoaded:
      return "LibrariesLoaded";
    case StartupEvidence::kJniOnLoadComplete:
      return "JniOnLoadComplete";
    case StartupEvidence::kEngineStarted:
      return "EngineStarted";
    case StartupEvidence::kDataModelLoading:
      return "DataModelLoading";
    case StartupEvidence::kLuaAppReady:
      return "LuaAppReady";
    case StartupEvidence::kRealFramePresented:
      return "RealFramePresented";
    case StartupEvidence::kInputReady:
      return "InputReady";
    case StartupEvidence::kAudioReady:
      return "AudioReady";
  }
  return "Unknown";
}

}  // namespace runtime
}  // namespace mocktail
