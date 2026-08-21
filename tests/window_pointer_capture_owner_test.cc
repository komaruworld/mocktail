#include "window/window_pointer_capture_owner.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace mocktail {
namespace window {
namespace {

class FakeBackend final : public PointerCaptureBackend {
 public:
  bool Apply(bool relative_mode, bool cursor_visible) override {
    calls.push_back({relative_mode, cursor_visible});
    return succeeds;
  }

  struct Call {
    bool relative_mode;
    bool cursor_visible;
  };
  std::vector<Call> calls;
  bool succeeds = true;
};

struct QueryState {
  bool succeeds = true;
  bool locked_center = false;
};

bool Query(void* context, bool* locked_center) {
  auto* state = static_cast<QueryState*>(context);
  if (state == nullptr || locked_center == nullptr || !state->succeeds) {
    return false;
  }
  *locked_center = state->locked_center;
  return true;
}

TEST(WindowPointerCaptureOwnerTest, FollowsNativeMouseLockState) {
  FakeBackend backend;
  QueryState query;
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  query.locked_center = true;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  query.locked_center = false;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
}

TEST(WindowPointerCaptureOwnerTest, TextAndFocusAlwaysReleaseCapture) {
  FakeBackend backend;
  QueryState query{true, true};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  EXPECT_TRUE(owner.Pump(true));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
  EXPECT_TRUE(owner.OnFocusLost());
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.OnFocusGained(false));
  EXPECT_TRUE(owner.captured());
}

TEST(WindowPointerCaptureOwnerTest, QueryOrCaptureFailureStaysReleased) {
  FakeBackend backend;
  backend.succeeds = false;
  QueryState query{true, true};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  EXPECT_FALSE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());
  EXPECT_EQ(backend.calls.size(), 2U);
  query.succeeds = false;
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
}

TEST(WindowPointerCaptureOwnerTest, RightDragCapturesUntilButtonRelease) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());

  EXPECT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
}

TEST(WindowPointerCaptureOwnerTest,
     LongRightReleaseRejectsStaleNativeLockUntilUnlockObserved) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  query.locked_center = true;
  ASSERT_TRUE(owner.OnRightButton(true, false));
  constexpr int kTenSecondsAt240Hz = 2400;
  for (int pump = 0; pump < kTenSecondsAt240Hz; ++pump) {
    ASSERT_TRUE(owner.Pump(false));
  }
  ASSERT_TRUE(owner.captured());

  ASSERT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.ShouldDispatchMouseMotion());
  ASSERT_TRUE(owner.Pump(false));
  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.ShouldDispatchMouseMotion());

  query.locked_center = false;
  ASSERT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.ShouldDispatchMouseMotion());
  query.locked_center = true;
  ASSERT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
}

TEST(WindowPointerCaptureOwnerTest,
     RightReleasePreservesNativeLockThatPredatedDrag) {
  FakeBackend backend;
  QueryState query{true, true};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  ASSERT_TRUE(owner.OnRightButton(true, false));
  ASSERT_TRUE(owner.OnRightButton(false, false));

  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  EXPECT_TRUE(owner.ShouldDispatchMouseMotion());
}

TEST(WindowPointerCaptureOwnerTest,
     ShiftLockActivatedDuringRightDragSurvivesButtonRelease) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  ASSERT_TRUE(owner.OnRightButton(true, false));
  query.locked_center = true;
  ASSERT_TRUE(owner.OnShiftKeyPressed(false));
  ASSERT_TRUE(owner.OnRightButton(false, false));

  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  EXPECT_TRUE(owner.ShouldDispatchMouseMotion());
}

TEST(WindowPointerCaptureOwnerTest,
     ShiftDuringRightDragDoesNotInventNativeMouseLock) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));

  ASSERT_TRUE(owner.OnRightButton(true, false));
  ASSERT_TRUE(owner.OnShiftKeyPressed(false));
  ASSERT_TRUE(owner.OnRightButton(false, false));

  EXPECT_FALSE(owner.captured());
  EXPECT_FALSE(owner.ShouldDispatchMouseMotion());
  EXPECT_TRUE(owner.ShouldDispatchMouseMotion());
}

TEST(WindowPointerCaptureOwnerTest,
     MotionButtonStateRecoversMissingRightRelease) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));
  ASSERT_TRUE(owner.Pump(false));
  ASSERT_TRUE(owner.OnRightButton(true, false));

  EXPECT_FALSE(owner.NeedsRightButtonReleaseRecovery(true));
  EXPECT_TRUE(owner.NeedsRightButtonReleaseRecovery(false));
  ASSERT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.NeedsRightButtonReleaseRecovery(false));
  EXPECT_FALSE(owner.ShouldDispatchMouseMotion());
  EXPECT_TRUE(owner.ShouldDispatchMouseMotion());
}

TEST(WindowPointerCaptureOwnerTest, RightDragDoesNotDependOnNativeLockQuery) {
  FakeBackend backend;
  QueryState query{false, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  EXPECT_TRUE(owner.OnRightButton(true, false));
  EXPECT_TRUE(owner.captured());
  EXPECT_FALSE(owner.cursor_visible());
  EXPECT_TRUE(owner.OnRightButton(false, false));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());
}

TEST(WindowPointerCaptureOwnerTest, TextAndFocusCancelRightDragCapture) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  EXPECT_TRUE(owner.OnRightButton(true, true));
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());
  EXPECT_TRUE(owner.Pump(false));
  EXPECT_TRUE(owner.captured());
  EXPECT_TRUE(owner.OnFocusLost());
  EXPECT_FALSE(owner.captured());
  EXPECT_TRUE(owner.cursor_visible());
  EXPECT_TRUE(owner.OnFocusGained(false));
  EXPECT_FALSE(owner.captured());
}

// Roblox draws its own cursor while it owns the pointer. Focus loss reveals the
// system cursor; regaining focus has to hide it again in the same call, because
// Pump() runs before SDL_PollEvent delivers the focus event and would otherwise
// leave the desktop arrow on screen for a whole frame.
TEST(WindowPointerCaptureOwnerTest, FocusGainHidesTheSystemCursorImmediately) {
  FakeBackend backend;
  QueryState query{true, false};
  WindowPointerCaptureOwner owner(&backend);
  ASSERT_TRUE(owner.RegisterQuery(Query, &query));

  ASSERT_TRUE(owner.Pump(false));
  ASSERT_FALSE(owner.cursor_visible());

  EXPECT_TRUE(owner.OnFocusLost());
  EXPECT_TRUE(owner.cursor_visible());

  const std::size_t calls_before = backend.calls.size();
  EXPECT_TRUE(owner.OnFocusGained(false));
  EXPECT_FALSE(owner.cursor_visible());
  ASSERT_GT(backend.calls.size(), calls_before);
  EXPECT_FALSE(backend.calls.back().cursor_visible);

  // An active text field keeps the system cursor, focus or not.
  EXPECT_TRUE(owner.OnFocusLost());
  EXPECT_TRUE(owner.OnFocusGained(true));
  EXPECT_TRUE(owner.cursor_visible());
}

}  // namespace
}  // namespace window
}  // namespace mocktail
