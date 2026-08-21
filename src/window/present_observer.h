#ifndef MOCKTAIL_WINDOW_PRESENT_OBSERVER_H_
#define MOCKTAIL_WINDOW_PRESENT_OBSERVER_H_

#include <cstdint>
#include <mutex>
#include <utility>

namespace mocktail {
namespace window {

using PresentObserver = void (*)(void* context, uint64_t frame_serial);

// Serializes successful-present notifications with observer replacement.
// Callbacks run synchronously and must not mutate their own registration.
class PresentObserverRegistry final {
 public:
  PresentObserverRegistry() = default;
  PresentObserverRegistry(const PresentObserverRegistry&) = delete;
  PresentObserverRegistry& operator=(const PresentObserverRegistry&) = delete;

  bool Register(PresentObserver observer, void* context) {
    if (observer == nullptr || context == nullptr) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (observer_ != nullptr) {
      return false;
    }
    observer_ = observer;
    context_ = context;
    return true;
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    observer_ = nullptr;
    context_ = nullptr;
  }

  void Notify(uint64_t frame_serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (observer_ != nullptr) {
      observer_(context_, frame_serial);
    }
  }

 private:
  std::mutex mutex_;
  PresentObserver observer_ = nullptr;
  void* context_ = nullptr;
};

// Owns the synchronization boundary between a successful host presentation
// and window teardown for both Vulkan and EGL/OpenGL ES. The lifecycle mutex
// is always acquired before the observer registry mutex. A PresentScope keeps
// the window alive through the callback and post-present SDL access performed
// by its caller. Callbacks must not re-enter this gate or trigger teardown.
class PresentLifecycleGate final {
 public:
  class PresentScope final {
   public:
    PresentScope(PresentScope&&) noexcept = default;
    PresentScope& operator=(PresentScope&&) noexcept = delete;

    PresentScope(const PresentScope&) = delete;
    PresentScope& operator=(const PresentScope&) = delete;

    explicit operator bool() const {
      return lifecycle_lock_.owns_lock() && observers_ != nullptr;
    }

    void NotifyObserver(uint64_t frame_serial) {
      if (*this) {
        observers_->Notify(frame_serial);
      }
    }

   private:
    friend class PresentLifecycleGate;

    PresentScope() = default;
    PresentScope(std::unique_lock<std::mutex> lifecycle_lock,
                 PresentObserverRegistry* observers)
        : lifecycle_lock_(std::move(lifecycle_lock)), observers_(observers) {}

    std::unique_lock<std::mutex> lifecycle_lock_;
    PresentObserverRegistry* observers_ = nullptr;
  };

  PresentLifecycleGate() = default;
  PresentLifecycleGate(const PresentLifecycleGate&) = delete;
  PresentLifecycleGate& operator=(const PresentLifecycleGate&) = delete;

  bool Activate() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (active_) {
      return false;
    }
    observers_.Clear();
    active_ = true;
    return true;
  }

  void Deactivate() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    active_ = false;
    observers_.Clear();
  }

  bool Register(PresentObserver observer, void* context) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!active_) {
      return false;
    }
    return observers_.Register(observer, context);
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    observers_.Clear();
  }

  PresentScope EnterPresent() {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    if (!active_) {
      lock.unlock();
      return PresentScope();
    }
    return PresentScope(std::move(lock), &observers_);
  }

 private:
  std::mutex lifecycle_mutex_;
  PresentObserverRegistry observers_;
  bool active_ = false;
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_PRESENT_OBSERVER_H_
