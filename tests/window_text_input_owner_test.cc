#include "window/window_text_input_owner.h"
#include "window/main_thread_command_gate.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace mocktail {
namespace window {
namespace {

class RecordingBackend final : public TextInputBackend {
 public:
  bool SetArea(const TextInputArea& area) override {
    calls.push_back('A');
    areas.push_back(area);
    return set_area_result;
  }
  bool ClearArea() override {
    calls.push_back('C');
    return true;
  }
  bool Start(const TextInputOptions& options) override {
    calls.push_back('S');
    start_options.push_back(options);
    return start_result;
  }
  bool Stop() override {
    calls.push_back('T');
    return true;
  }
  void FlushTextEvents() override { calls.push_back('F'); }

  bool set_area_result = true;
  bool start_result = true;
  std::vector<char> calls;
  std::vector<TextInputArea> areas;
  std::vector<TextInputOptions> start_options;
};

constexpr TextInputArea kArea{10, 20, 300, 40, 25};
constexpr TextInputOptions kOptions{3, false};

TEST(WindowTextInputOwnerTest, DisabledRequestsHaveNoBackendCost) {
  RecordingBackend backend;
  WindowTextInputOwner owner(&backend);

  EXPECT_FALSE(owner.RequestShowTextInput(1, kArea, kOptions));
  EXPECT_FALSE(owner.RequestHideTextInput(1));
  EXPECT_TRUE(owner.Pump());
  EXPECT_TRUE(backend.calls.empty());
}

TEST(WindowTextInputOwnerTest, ShowAndHideRunOnlyWhenPumped) {
  RecordingBackend backend;
  WindowTextInputOwner owner(&backend);
  owner.SetEnabled(true);

  EXPECT_TRUE(owner.RequestShowTextInput(3, kArea, kOptions));
  EXPECT_TRUE(backend.calls.empty());
  EXPECT_TRUE(owner.Pump());
  EXPECT_EQ(backend.calls, (std::vector<char>{'F', 'A', 'S'}));
  EXPECT_TRUE(owner.active());
  EXPECT_EQ(owner.active_generation(), 3u);

  EXPECT_TRUE(owner.RequestHideTextInput(3));
  EXPECT_TRUE(owner.Pump());
  EXPECT_EQ(backend.calls,
            (std::vector<char>{'F', 'A', 'S', 'T', 'C', 'F'}));
  EXPECT_FALSE(owner.active());
}

TEST(WindowTextInputOwnerTest, RejectsStaleFocusCommands) {
  RecordingBackend backend;
  WindowTextInputOwner owner(&backend);
  owner.SetEnabled(true);

  EXPECT_TRUE(owner.RequestShowTextInput(5, kArea, kOptions));
  EXPECT_FALSE(owner.RequestShowTextInput(4, kArea, kOptions));
  EXPECT_FALSE(owner.RequestHideTextInput(4));
  EXPECT_TRUE(owner.Pump());
  EXPECT_TRUE(owner.active());
}

TEST(WindowTextInputOwnerTest, CoalescesCommandsToLatestDesiredState) {
  RecordingBackend backend;
  WindowTextInputOwner owner(&backend);
  owner.SetEnabled(true);

  EXPECT_TRUE(owner.RequestShowTextInput(7, kArea, kOptions));
  EXPECT_TRUE(owner.RequestHideTextInput(7));
  EXPECT_TRUE(owner.Pump());
  EXPECT_TRUE(backend.calls.empty());
}

TEST(WindowTextInputOwnerTest, ReappliesAreaAfterResizeWithoutRestart) {
  RecordingBackend backend;
  WindowTextInputOwner owner(&backend);
  owner.SetEnabled(true);
  ASSERT_TRUE(owner.RequestShowTextInput(2, kArea, kOptions));
  ASSERT_TRUE(owner.Pump());

  EXPECT_TRUE(owner.OnViewportChanged());
  EXPECT_EQ(backend.calls, (std::vector<char>{'F', 'A', 'S', 'A'}));
  EXPECT_EQ(backend.areas.size(), 2u);
}

TEST(WindowTextInputOwnerTest, FocusLossAndShutdownClearInputState) {
  RecordingBackend backend;
  WindowTextInputOwner owner(&backend);
  owner.SetEnabled(true);
  ASSERT_TRUE(owner.RequestShowTextInput(1, kArea, kOptions));
  ASSERT_TRUE(owner.Pump());

  owner.OnFocusLost();
  EXPECT_FALSE(owner.active());
  EXPECT_EQ(backend.calls,
            (std::vector<char>{'F', 'A', 'S', 'T', 'C', 'F'}));

  EXPECT_TRUE(owner.RequestShowTextInput(1, kArea, kOptions));
  EXPECT_TRUE(owner.Pump());
  EXPECT_FALSE(owner.active());
  owner.OnFocusGained();
  EXPECT_TRUE(owner.RequestShowTextInput(1, kArea, kOptions));
  ASSERT_TRUE(owner.Pump());
  owner.Shutdown();
  EXPECT_FALSE(owner.active());
  EXPECT_FALSE(owner.RequestShowTextInput(1, kArea, kOptions));
}

TEST(WindowTextInputOwnerTest, FailedStartDoesNotPublishActiveState) {
  RecordingBackend backend;
  backend.start_result = false;
  WindowTextInputOwner owner(&backend);
  owner.SetEnabled(true);

  ASSERT_TRUE(owner.RequestShowTextInput(1, kArea, kOptions));
  EXPECT_FALSE(owner.Pump());
  EXPECT_FALSE(owner.active());
  EXPECT_EQ(backend.calls, (std::vector<char>{'F', 'A', 'S', 'C'}));
}

TEST(WindowTextInputOwnerTest, GuestRequestsAreThreadSafeAndBounded) {
  RecordingBackend backend;
  WindowTextInputOwner owner(&backend);
  owner.SetEnabled(true);
  std::atomic<bool> start{false};
  std::vector<std::thread> threads;
  for (int thread_index = 0; thread_index < 4; ++thread_index) {
    threads.emplace_back([&owner, &start, thread_index]() {
      while (!start.load(std::memory_order_acquire)) {
      }
      for (int index = 0; index < 1000; ++index) {
        TextInputArea area = kArea;
        area.x = thread_index;
        owner.RequestShowTextInput(9, area, kOptions);
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (std::thread& thread : threads) {
    thread.join();
  }

  ASSERT_TRUE(owner.Pump());
  EXPECT_EQ(backend.calls, (std::vector<char>{'F', 'A', 'S'}));
  EXPECT_EQ(backend.areas.size(), 1u);
}

TEST(WindowTextInputOwnerTest, UsesSafeAreaAndPassesInputOptions) {
  RecordingBackend backend;
  WindowTextInputOwner owner(&backend);
  owner.SetEnabled(true);
  const TextInputOptions options{5, true};

  ASSERT_TRUE(owner.RequestShowTextInput(8, {}, options));
  ASSERT_TRUE(owner.Pump());
  ASSERT_EQ(backend.areas.size(), 1u);
  EXPECT_TRUE(backend.areas[0].valid());
  ASSERT_EQ(backend.start_options.size(), 1u);
  EXPECT_EQ(backend.start_options[0].roblox_text_input_type, 5);
  EXPECT_TRUE(backend.start_options[0].multiline);
}

TEST(WindowTextInputOwnerTest, OlderHideCannotReplaceNewerPendingShow) {
  RecordingBackend backend;
  WindowTextInputOwner owner(&backend);
  owner.SetEnabled(true);

  ASSERT_TRUE(owner.RequestShowTextInput(11, kArea, kOptions));
  EXPECT_FALSE(owner.RequestHideTextInput(10));
  EXPECT_TRUE(owner.Pump());
  EXPECT_TRUE(owner.active());
  EXPECT_EQ(owner.active_generation(), 11u);
}

bool Increment(void* context) {
  ++*static_cast<int*>(context);
  return true;
}

TEST(MainThreadCommandGateTest, DefaultPathAndRegistrationAreDeterministic) {
  MainThreadCommandGate gate;
  int calls = 0;

  EXPECT_TRUE(gate.Invoke());
  EXPECT_FALSE(gate.Register(&Increment, &calls));
  gate.Activate();
  ASSERT_TRUE(gate.Register(&Increment, &calls));
  EXPECT_FALSE(gate.Register(&Increment, &calls));
  EXPECT_TRUE(gate.Invoke());
  EXPECT_EQ(calls, 1);
  gate.Clear();
  EXPECT_TRUE(gate.Invoke());
  EXPECT_EQ(calls, 1);
  gate.Deactivate();
}

}  // namespace
}  // namespace window
}  // namespace mocktail
