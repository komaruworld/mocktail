#ifndef MOCKTAIL_WINDOW_WINDOW_FULLSCREEN_REQUEST_GATE_H_
#define MOCKTAIL_WINDOW_WINDOW_FULLSCREEN_REQUEST_GATE_H_

#include <cstdint>
#include <mutex>

namespace mocktail {
namespace window {

// Android WindowManager.LayoutParams flags used by GameActivity.setWindowFlags.
inline constexpr std::uint32_t kAndroidWindowFlagFullscreen = 0x00000400U;
inline constexpr std::uint32_t kAndroidWindowFlagForceNotFullscreen =
    0x00000800U;

// Coalesces worker-thread Android window flag changes into the latest desired
// host fullscreen state. SDL consumes the request only from its main thread.
class WindowFullscreenRequestGate final {
 public:
  bool RequestFromAndroidFlags(int flags, int mask);
  bool Take(bool* fullscreen);
  void Reset();

 private:
  std::mutex mutex_;
  std::uint32_t android_flags_ = 0;
  // 0 = no request, 1 = windowed, 2 = fullscreen.
  int pending_state_ = 0;
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_WINDOW_FULLSCREEN_REQUEST_GATE_H_
