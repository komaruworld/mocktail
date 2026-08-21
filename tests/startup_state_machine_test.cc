#include "runtime/startup_state_machine.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <thread>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::array<StartupEvidence, 7> CoreSequence() {
  return {
      StartupEvidence::kConfigured,      StartupEvidence::kJniRuntimeReady,
      StartupEvidence::kLibrariesLoaded, StartupEvidence::kJniOnLoadComplete,
      StartupEvidence::kEngineStarted,   StartupEvidence::kDataModelLoading,
      StartupEvidence::kLuaAppReady,
  };
}

void RecordCoreSequence(StartupStateMachine* machine) {
  ASSERT_NE(machine, nullptr);
  for (StartupEvidence evidence : CoreSequence()) {
    const StartupUpdateResult result = machine->RecordEvidence(evidence);
    ASSERT_EQ(result.status, StartupUpdateStatus::kApplied) << result.message;
  }
}

TEST(StartupStateMachineTest, HeadlessCoreSequenceReachesReady) {
  StartupStateMachine machine;

  RecordCoreSequence(&machine);

  const StartupSnapshot snapshot = machine.Snapshot();
  EXPECT_EQ(snapshot.state, StartupState::kReady);
  EXPECT_TRUE(snapshot.ready());
  EXPECT_TRUE(snapshot.lua_app_ready);
  EXPECT_FALSE(snapshot.real_frame_presented);
  EXPECT_EQ(snapshot.revision, 7U);
}

TEST(StartupStateMachineTest, WindowedRuntimeWaitsForRealPresentEvidence) {
  StartupStateMachine machine({true, false, false});
  RecordCoreSequence(&machine);

  EXPECT_EQ(machine.Snapshot().state, StartupState::kLuaAppReady);
  const StartupUpdateResult present =
      machine.RecordEvidence(StartupEvidence::kRealFramePresented);

  EXPECT_EQ(present.status, StartupUpdateStatus::kApplied);
  EXPECT_EQ(present.state, StartupState::kReady);
  EXPECT_TRUE(machine.Snapshot().real_frame_presented);
}

TEST(StartupStateMachineTest, OptionalInputAndAudioDoNotBlockReadiness) {
  StartupStateMachine machine;
  RecordCoreSequence(&machine);

  EXPECT_EQ(machine.Snapshot().state, StartupState::kReady);
  EXPECT_FALSE(machine.Snapshot().input_ready);
  EXPECT_FALSE(machine.Snapshot().audio_ready);
}

TEST(StartupStateMachineTest, RequiredInputAndAudioBothGateReadiness) {
  StartupStateMachine machine({false, true, true});
  ASSERT_TRUE(machine.RecordEvidence(StartupEvidence::kConfigured).ok());
  ASSERT_TRUE(machine.RecordEvidence(StartupEvidence::kInputReady).ok());
  const auto core_sequence = CoreSequence();
  for (size_t index = 1; index < core_sequence.size(); ++index) {
    ASSERT_TRUE(machine.RecordEvidence(core_sequence[index]).ok());
  }

  EXPECT_EQ(machine.Snapshot().state, StartupState::kLuaAppReady);
  EXPECT_TRUE(machine.Snapshot().input_ready);
  EXPECT_FALSE(machine.Snapshot().audio_ready);

  const StartupUpdateResult audio =
      machine.RecordEvidence(StartupEvidence::kAudioReady);
  EXPECT_EQ(audio.state, StartupState::kReady);
}

TEST(StartupStateMachineTest, ReadinessEvidenceCanPrecedeLuaAppReady) {
  StartupStateMachine machine({true, true, false});
  ASSERT_TRUE(machine.RecordEvidence(StartupEvidence::kConfigured).ok());
  ASSERT_TRUE(machine.RecordEvidence(StartupEvidence::kInputReady).ok());
  ASSERT_TRUE(machine.RecordEvidence(StartupEvidence::kJniRuntimeReady).ok());
  ASSERT_TRUE(machine.RecordEvidence(StartupEvidence::kLibrariesLoaded).ok());
  ASSERT_TRUE(machine.RecordEvidence(StartupEvidence::kJniOnLoadComplete).ok());
  ASSERT_TRUE(machine.RecordEvidence(StartupEvidence::kEngineStarted).ok());
  ASSERT_TRUE(
      machine.RecordEvidence(StartupEvidence::kRealFramePresented).ok());
  ASSERT_TRUE(machine.RecordEvidence(StartupEvidence::kDataModelLoading).ok());

  const StartupUpdateResult lua =
      machine.RecordEvidence(StartupEvidence::kLuaAppReady);
  EXPECT_EQ(lua.state, StartupState::kReady);
}

TEST(StartupStateMachineTest, InvalidCoreOrderFailsClosed) {
  StartupStateMachine machine;
  ASSERT_TRUE(machine.RecordEvidence(StartupEvidence::kConfigured).ok());

  const StartupUpdateResult invalid =
      machine.RecordEvidence(StartupEvidence::kLibrariesLoaded);

  EXPECT_EQ(invalid.status, StartupUpdateStatus::kRejected);
  EXPECT_EQ(invalid.state, StartupState::kFailure);
  EXPECT_NE(invalid.message.find("expected JniRuntimeReady"),
            std::string::npos);
  EXPECT_FALSE(machine.RecordEvidence(StartupEvidence::kJniRuntimeReady).ok());
}

TEST(StartupStateMachineTest, PresentBeforeEngineStartFailsClosed) {
  StartupStateMachine machine({true, false, false});
  ASSERT_TRUE(machine.RecordEvidence(StartupEvidence::kConfigured).ok());

  const StartupUpdateResult invalid =
      machine.RecordEvidence(StartupEvidence::kRealFramePresented);

  EXPECT_EQ(invalid.state, StartupState::kFailure);
  EXPECT_NE(invalid.message.find("expected EngineStarted"), std::string::npos);
}

TEST(StartupStateMachineTest, AuxiliaryEvidenceBeforeConfigurationFailsClosed) {
  StartupStateMachine machine;

  EXPECT_EQ(machine.RecordEvidence(StartupEvidence::kInputReady).state,
            StartupState::kFailure);
}

TEST(StartupStateMachineTest, UnknownEvidenceFailsClosed) {
  StartupStateMachine machine;

  const StartupUpdateResult invalid =
      machine.RecordEvidence(static_cast<StartupEvidence>(255));

  EXPECT_EQ(invalid.status, StartupUpdateStatus::kRejected);
  EXPECT_EQ(invalid.state, StartupState::kFailure);
  EXPECT_NE(invalid.message.find("received Unknown"), std::string::npos);
}

TEST(StartupStateMachineTest, DuplicateEvidenceIsIdempotent) {
  StartupStateMachine machine;
  const StartupUpdateResult first =
      machine.RecordEvidence(StartupEvidence::kConfigured);
  const uint64_t revision = machine.Snapshot().revision;
  const StartupUpdateResult duplicate =
      machine.RecordEvidence(StartupEvidence::kConfigured);

  EXPECT_EQ(first.status, StartupUpdateStatus::kApplied);
  EXPECT_EQ(duplicate.status, StartupUpdateStatus::kDuplicate);
  EXPECT_EQ(machine.Snapshot().state, StartupState::kConfigured);
  EXPECT_EQ(machine.Snapshot().revision, revision);
}

TEST(StartupStateMachineTest, ConcurrentDuplicateEvidenceIsSafe) {
  StartupStateMachine machine;
  std::atomic<int> applied{0};
  std::atomic<int> duplicates{0};
  std::atomic<int> rejected{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < 16; ++index) {
    threads.emplace_back([&]() {
      const StartupUpdateResult result =
          machine.RecordEvidence(StartupEvidence::kConfigured);
      if (result.status == StartupUpdateStatus::kApplied) {
        ++applied;
      } else if (result.status == StartupUpdateStatus::kDuplicate) {
        ++duplicates;
      } else {
        ++rejected;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(applied.load(), 1);
  EXPECT_EQ(duplicates.load(), 15);
  EXPECT_EQ(rejected.load(), 0);
  EXPECT_EQ(machine.Snapshot().state, StartupState::kConfigured);
  EXPECT_EQ(machine.Snapshot().revision, 1U);
}

TEST(StartupStateMachineTest, ExplicitFailureCanProceedThroughShutdown) {
  StartupStateMachine machine;
  ASSERT_TRUE(machine.RecordEvidence(StartupEvidence::kConfigured).ok());

  const StartupUpdateResult failure = machine.Fail("JNI runtime failed");
  EXPECT_EQ(failure.state, StartupState::kFailure);
  EXPECT_EQ(machine.Snapshot().failure_reason, "JNI runtime failed");
  EXPECT_FALSE(machine.RecordEvidence(StartupEvidence::kJniRuntimeReady).ok());

  EXPECT_EQ(machine.BeginStopping().state, StartupState::kStopping);
  EXPECT_EQ(machine.MarkStopped().state, StartupState::kStopped);
  EXPECT_TRUE(machine.Snapshot().terminal());
}

TEST(StartupStateMachineTest, FailureAndShutdownTransitionsAreIdempotent) {
  StartupStateMachine machine;
  EXPECT_EQ(machine.Fail("first").status, StartupUpdateStatus::kApplied);
  EXPECT_EQ(machine.Fail("second").status, StartupUpdateStatus::kDuplicate);
  EXPECT_EQ(machine.Snapshot().failure_reason, "first");
  EXPECT_EQ(machine.BeginStopping().status, StartupUpdateStatus::kApplied);
  EXPECT_EQ(machine.BeginStopping().status, StartupUpdateStatus::kDuplicate);
  EXPECT_EQ(machine.MarkStopped().status, StartupUpdateStatus::kApplied);
  EXPECT_EQ(machine.MarkStopped().status, StartupUpdateStatus::kDuplicate);
}

TEST(StartupStateMachineTest, StoppedWithoutStoppingFailsClosed) {
  StartupStateMachine machine;
  ASSERT_TRUE(machine.RecordEvidence(StartupEvidence::kConfigured).ok());

  const StartupUpdateResult stopped = machine.MarkStopped();

  EXPECT_EQ(stopped.status, StartupUpdateStatus::kRejected);
  EXPECT_EQ(stopped.state, StartupState::kFailure);
  EXPECT_NE(stopped.message.find("Stopped requires Stopping"),
            std::string::npos);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
