#ifndef MOCKTAIL_RUNTIME_OWNED_PTHREAD_H_
#define MOCKTAIL_RUNTIME_OWNED_PTHREAD_H_

#include <pthread.h>

#include <cstddef>

namespace mocktail {
namespace runtime {

using PthreadEntryPoint = void* (*)(void* context);
using PthreadWaitPump = void (*)(void* context);

enum class OwnedPthreadWaitStatus {
  kJoined,
  kTimedOut,
  kPlatformError,
};

struct OwnedPthreadWaitResult {
  OwnedPthreadWaitStatus status = OwnedPthreadWaitStatus::kPlatformError;
  int platform_error = 0;

  bool joined() const { return status == OwnedPthreadWaitStatus::kJoined; }
};

struct OwnedPthreadCancelResult {
  int cancel_error = 0;
  OwnedPthreadWaitResult wait;
};

// Owns a joinable Linux pthread. There is intentionally no detach operation:
// context and every dependency reachable by a worker must outlive a physical
// join. Destroying a still-joinable instance terminates the process without
// unwinding, preventing a live worker from observing released stack or RAII
// state.
class OwnedPthread final {
 public:
  OwnedPthread() = default;
  ~OwnedPthread();

  OwnedPthread(const OwnedPthread&) = delete;
  OwnedPthread& operator=(const OwnedPthread&) = delete;

  int Start(PthreadEntryPoint entry_point, void* context,
            std::size_t stack_size);
  OwnedPthreadWaitResult WaitFor(int timeout_ms, int poll_interval_ms,
                                 PthreadWaitPump pump = nullptr,
                                 void* pump_context = nullptr);
  OwnedPthreadCancelResult CancelAndJoinFor(int timeout_ms,
                                            int poll_interval_ms,
                                            PthreadWaitPump pump = nullptr,
                                            void* pump_context = nullptr);
  int Signal(int signal_number) const;

  bool joinable() const { return joinable_; }

 private:
  pthread_t thread_{};
  bool joinable_ = false;
};

const char* OwnedPthreadWaitStatusName(OwnedPthreadWaitStatus status);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_OWNED_PTHREAD_H_
