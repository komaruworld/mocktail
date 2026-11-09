#include "window/window_pointer_capture_owner.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>

#include <SDL3/SDL.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

namespace mocktail::window {
namespace {

class WindowPointerCaptureSdlTest : public testing::Test {
 protected:
  void SetUp() override {
    const char* isolated = std::getenv("MOCKTAIL_POINTER_TEST_XVFB");
    if (isolated == nullptr || std::string_view(isolated) != "1") {
      GTEST_SKIP() << "Run through run_xvfb_pointer_test.py";
    }
    ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO)) << SDL_GetError();
    initialized_ = true;
    window_ = SDL_CreateWindow("Mocktail pointer regression", 800, 600, 0);
    ASSERT_NE(window_, nullptr) << SDL_GetError();
    ASSERT_TRUE(SDL_SetWindowPosition(window_, 200, 100)) << SDL_GetError();
    display_ = XOpenDisplay(nullptr);
    ASSERT_NE(display_, nullptr);
    window_id_ = static_cast<Window>(SDL_GetNumberProperty(
        SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    ASSERT_NE(window_id_, 0UL);
    int event_base = 0, error_base = 0, major = 0, minor = 0;
    ASSERT_TRUE(XTestQueryExtension(display_, &event_base, &error_base,
                                    &major, &minor));
    XSetInputFocus(display_, window_id_, RevertToParent, CurrentTime);
    QueueMotion(300, 200);
    XSync(display_, False);
    for (int attempt = 0; attempt < 8; ++attempt) {
      SDL_Delay(10);
      SDL_PumpEvents();
    }
    ASSERT_EQ(SDL_GetKeyboardFocus(), window_);
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    backend_ = std::make_unique<SdlPointerCaptureBackend>(window_);
    owner_ = std::make_unique<WindowPointerCaptureOwner>(backend_.get());
    ASSERT_TRUE(owner_->RegisterQuery(Query, this));
    ASSERT_TRUE(owner_->Pump(false));
    ExpectPosition(300, 200);
  }

  void TearDown() override {
    owner_.reset();
    backend_.reset();
    if (display_ != nullptr) {
      XTestFakeButtonEvent(display_, 3, False, 0);
      if (foreign_window_ != 0) XDestroyWindow(display_, foreign_window_);
      XSync(display_, False);
      XCloseDisplay(display_);
    }
    if (window_ != nullptr) SDL_DestroyWindow(window_);
    if (initialized_) SDL_Quit();
  }

  static bool Query(void* context, bool* locked) {
    *locked = static_cast<WindowPointerCaptureSdlTest*>(context)->native_lock_;
    return true;
  }

  void QueueMotion(int x, int y) {
    ASSERT_NE(XTestFakeMotionEvent(display_, 0, x, y, 0), 0);
  }

  void QueueRightButton(bool down) {
    ASSERT_NE(XTestFakeButtonEvent(display_, 3, down ? True : False, 0), 0);
  }

  void Pump(bool dispatch = true) {
    XSync(display_, False);
    SDL_Delay(30);
    SDL_PumpEvents();
    if (dispatch) Dispatch();
  }

  void Dispatch() {
    SDL_Event event;
    const SDL_WindowID id = SDL_GetWindowID(window_);
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT,
                          SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
      if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST && event.window.windowID == id) {
        EXPECT_TRUE(owner_->OnFocusLost());
      }
      if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED && event.window.windowID == id) {
        EXPECT_TRUE(owner_->OnFocusGained(false));
      }
      if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
           event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
          event.button.windowID == id && event.button.button == SDL_BUTTON_RIGHT) {
        std::optional<PointerPosition> position;
        if (event.button.down) {
          position = PointerPosition{event.button.x, event.button.y};
          last_press_ = position;
          SDL_GetMouseState(&cached_at_press_.x, &cached_at_press_.y);
          ++presses_;
        } else {
          ++releases_;
        }
        EXPECT_TRUE(owner_->OnRightButton(event.button.down, false, position));
      }
    }
  }

  void ExpectPosition(float x, float y) {
    XSync(display_, False);
    SDL_Delay(20);
    float actual_x = 0, actual_y = 0;
    SDL_GetGlobalMouseState(&actual_x, &actual_y);
    EXPECT_NEAR(actual_x, x, 1.0F);
    EXPECT_NEAR(actual_y, y, 1.0F);
  }

  void BeginRightDrag() {
    QueueRightButton(true);
    Pump();
    ASSERT_EQ(presses_, 1);
    ASSERT_TRUE(owner_->captured());
    ASSERT_TRUE(SDL_GetWindowRelativeMouseMode(window_));
  }

  void ReleaseRightDrag() {
    QueueRightButton(false);
    Pump();
    ASSERT_EQ(releases_, 1);
  }

  SDL_Window* window_ = nullptr;
  Display* display_ = nullptr;
  Window window_id_ = 0;
  Window foreign_window_ = 0;
  bool initialized_ = false;
  bool native_lock_ = false;
  int presses_ = 0;
  int releases_ = 0;
  std::optional<PointerPosition> last_press_;
  PointerPosition cached_at_press_;
  std::unique_ptr<SdlPointerCaptureBackend> backend_;
  std::unique_ptr<WindowPointerCaptureOwner> owner_;
};

TEST_F(WindowPointerCaptureSdlTest, RestoresTheRightButtonPressPosition) {
  BeginRightDrag();
  QueueMotion(500, 400);
  Pump();
  ReleaseRightDrag();
  EXPECT_FALSE(SDL_GetWindowRelativeMouseMode(window_));
  ExpectPosition(300, 200);
}

TEST_F(WindowPointerCaptureSdlTest, IgnoresLaterMotionInTheSameEventBatch) {
  QueueRightButton(true);
  QueueMotion(2800, 300);
  Pump();
  ASSERT_EQ(presses_, 1);
  ASSERT_TRUE(last_press_.has_value());
  EXPECT_FLOAT_EQ(last_press_->x, 100);
  EXPECT_FLOAT_EQ(last_press_->y, 100);
  ASSERT_GT(cached_at_press_.x, 800);
  ASSERT_TRUE(owner_->captured());
  QueueMotion(500, 400);
  Pump();
  ReleaseRightDrag();
  ExpectPosition(300, 200);
}

TEST_F(WindowPointerCaptureSdlTest, FocusLossDoesNotWarpToTheOldWindow) {
  BeginRightDrag();
  foreign_window_ = XCreateSimpleWindow(
      display_, DefaultRootWindow(display_), 2100, 100, 800, 600, 0, 0, 0);
  ASSERT_NE(foreign_window_, 0UL);
  XMapWindow(display_, foreign_window_);
  XSetInputFocus(display_, foreign_window_, RevertToParent, CurrentTime);
  Pump(false);
  ASSERT_NE(SDL_GetKeyboardFocus(), window_);
  QueueMotion(2300, 400);
  Pump(false);
  ExpectPosition(2300, 400);
  Dispatch();
  EXPECT_FALSE(owner_->captured());
  EXPECT_FALSE(SDL_GetWindowRelativeMouseMode(window_));
  ExpectPosition(2300, 400);
}

TEST_F(WindowPointerCaptureSdlTest, RightDragPreservesTheExistingNativeLockAnchor) {
  native_lock_ = true;
  ASSERT_TRUE(owner_->Pump(false));
  ASSERT_TRUE(owner_->captured());
  QueueMotion(500, 400);
  Pump();
  BeginRightDrag();
  ReleaseRightDrag();
  EXPECT_TRUE(owner_->captured());
  EXPECT_TRUE(SDL_GetWindowRelativeMouseMode(window_));
  native_lock_ = false;
  ASSERT_TRUE(owner_->Pump(false));
  EXPECT_FALSE(owner_->captured());
  ExpectPosition(300, 200);
}

TEST_F(WindowPointerCaptureSdlTest, ResizeKeepsTheRestoredPointerInsideTheWindow) {
  QueueMotion(800, 600);
  Pump();
  BeginRightDrag();
  ASSERT_TRUE(last_press_.has_value());
  EXPECT_FLOAT_EQ(last_press_->x, 600);
  EXPECT_FLOAT_EQ(last_press_->y, 500);
  ASSERT_TRUE(SDL_SetWindowSize(window_, 300, 200));
  Pump();
  ReleaseRightDrag();
  ExpectPosition(499, 299);
}

}  // namespace
}  // namespace mocktail::window
