#ifndef MOCKTAIL_AUDIO_FMOD_THREAD_FLOATING_POINT_H_
#define MOCKTAIL_AUDIO_FMOD_THREAD_FLOATING_POINT_H_

#include <string_view>

#if defined(__x86_64__)
#include <xmmintrin.h>
#endif
#if defined(__aarch64__)
#include <cstdint>
#endif

namespace mocktail::audio {

// FMOD's Android threads identify themselves through JavaVMAttachArgs before
// entering their mix loop. Voice DSP filters can accumulate subnormal values
// and spend an entire x86 core on floating-point assists (issue #128).
// Limit FTZ/DAZ to these audio threads; retain the caller's other FP controls.
class FmodThreadFloatingPointMode final {
 public:
  FmodThreadFloatingPointMode() = default;
  ~FmodThreadFloatingPointMode() { Restore(); }
  FmodThreadFloatingPointMode(const FmodThreadFloatingPointMode&) = delete;
  FmodThreadFloatingPointMode& operator=(const FmodThreadFloatingPointMode&) =
      delete;

  bool Enable(const char* thread_name) noexcept {
#if defined(__x86_64__) || defined(__aarch64__)
    if (active_ || thread_name == nullptr) return false;
    const std::string_view name(thread_name);
    if (name != "FMOD mixer thread" && name != "FMOD feeder thread" &&
        name != "FMOD Convolution thread" && name != "FMOD Worker Thread") {
      return false;
    }
#if defined(__x86_64__)
    const unsigned int control = _mm_getcsr();
    original_mode_ = control & kModeMask;
    _mm_setcsr(control | kModeMask);
#else
    std::uint64_t control;
    __asm__ volatile("mrs %0, fpcr" : "=r"(control));
    original_mode_ = control & kModeMask;
    __asm__ volatile("msr fpcr, %0" ::"r"(control | kModeMask));
#endif
    active_ = true;
    return true;
#else
    (void)thread_name;
    return false;
#endif
  }

  void Restore() noexcept {
#if defined(__x86_64__) || defined(__aarch64__)
    if (active_) {
      // Restore only our two bits, preserving rounding, exception masks and
      // status flags set by the guest while the thread was attached.
#if defined(__x86_64__)
      _mm_setcsr((_mm_getcsr() & ~kModeMask) | original_mode_);
#else
      std::uint64_t control;
      __asm__ volatile("mrs %0, fpcr" : "=r"(control));
      __asm__ volatile("msr fpcr, %0" ::"r"((control & ~kModeMask) |
                                          original_mode_));
#endif
      active_ = false;
    }
#endif
  }

 private:
#if defined(__x86_64__)
  static constexpr unsigned int kModeMask = (1U << 15) | (1U << 6);
  unsigned int original_mode_ = 0;
  bool active_ = false;
#elif defined(__aarch64__)
  // FPCR FZ flushes single/double subnormals; FZ16 covers the half-precision
  // path used by some voice DSP filters.
  static constexpr std::uint64_t kModeMask =
      (std::uint64_t{1} << 24) | (std::uint64_t{1} << 19);
  std::uint64_t original_mode_ = 0;
  bool active_ = false;
#endif
};

}  // namespace mocktail::audio

#endif  // MOCKTAIL_AUDIO_FMOD_THREAD_FLOATING_POINT_H_
