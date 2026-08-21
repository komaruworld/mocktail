#ifndef MOCKTAIL_WINDOW_MAIN_THREAD_COMMAND_GATE_H_
#define MOCKTAIL_WINDOW_MAIN_THREAD_COMMAND_GATE_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace mocktail {
namespace window {

using MainThreadCommandCallback = bool (*)(void* context);

// Synchronously invokes one registered command-drain callback from the SDL
// event owner. The atomic fast path avoids taking a mutex when no callback is
// installed. Clear waits for callbacks that already crossed that fast path.
class MainThreadCommandGate final {
 public:
  void Activate() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = true;
  }

  bool Register(MainThreadCommandCallback callback, void* context) {
    if (callback == nullptr || context == nullptr) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || callback_ != nullptr) {
      return false;
    }
    callback_ = callback;
    context_ = context;
    registered_.store(true, std::memory_order_release);
    return true;
  }

  bool Invoke() {
    if (!registered_.load(std::memory_order_acquire)) {
      return true;
    }
    MainThreadCommandCallback callback = nullptr;
    void* context = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!active_ || callback_ == nullptr) {
        return true;
      }
      callback = callback_;
      context = context_;
      ++in_flight_;
    }
    const bool success = callback(context);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --in_flight_;
    }
    condition_.notify_all();
    return success;
  }

  void Clear() {
    std::unique_lock<std::mutex> lock(mutex_);
    registered_.store(false, std::memory_order_release);
    callback_ = nullptr;
    context_ = nullptr;
    condition_.wait(lock, [this] { return in_flight_ == 0; });
  }

  void Deactivate() {
    Clear();
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
  }

 private:
  std::atomic<bool> registered_{false};
  std::mutex mutex_;
  std::condition_variable condition_;
  MainThreadCommandCallback callback_ = nullptr;
  void* context_ = nullptr;
  std::size_t in_flight_ = 0;
  bool active_ = false;
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_MAIN_THREAD_COMMAND_GATE_H_
