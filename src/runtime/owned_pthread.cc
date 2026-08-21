#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runtime/owned_pthread.h"

#include <signal.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace mocktail {
namespace runtime {

OwnedPthread::~OwnedPthread() {
  if (joinable_) {
    std::_Exit(EXIT_FAILURE);
  }
}

int OwnedPthread::Start(PthreadEntryPoint entry_point, void* context,
                        std::size_t stack_size) {
  if (entry_point == nullptr) {
    return EINVAL;
  }
  if (joinable_) {
    return EBUSY;
  }

  pthread_attr_t attributes;
  int result = pthread_attr_init(&attributes);
  if (result != 0) {
    return result;
  }
  if (stack_size != 0) {
    result = pthread_attr_setstacksize(&attributes, stack_size);
  }
  if (result == 0) {
    result = pthread_create(&thread_, &attributes, entry_point, context);
  }
  pthread_attr_destroy(&attributes);
  if (result == 0) {
    joinable_ = true;
  }
  return result;
}

OwnedPthreadWaitResult OwnedPthread::WaitFor(int timeout_ms,
                                             int poll_interval_ms,
                                             PthreadWaitPump pump,
                                             void* pump_context) {
  if (!joinable_) {
    return {OwnedPthreadWaitStatus::kPlatformError, EINVAL};
  }

  const bool wait_forever = timeout_ms < 0;
  const int effective_poll_ms = poll_interval_ms > 0 ? poll_interval_ms : 1;
  const auto started_at = std::chrono::steady_clock::now();
  while (true) {
    const int join_result = pthread_tryjoin_np(thread_, nullptr);
    if (join_result == 0) {
      joinable_ = false;
      return {OwnedPthreadWaitStatus::kJoined, 0};
    }
    if (join_result != EBUSY) {
      return {OwnedPthreadWaitStatus::kPlatformError, join_result};
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    if (!wait_forever && elapsed.count() >= timeout_ms) {
      return {OwnedPthreadWaitStatus::kTimedOut, 0};
    }
    if (pump != nullptr) {
      pump(pump_context);
    }

    int sleep_ms = effective_poll_ms;
    if (!wait_forever) {
      const int remaining_ms = timeout_ms - static_cast<int>(elapsed.count());
      if (remaining_ms < sleep_ms) {
        sleep_ms = remaining_ms;
      }
    }
    if (sleep_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
  }
}

OwnedPthreadCancelResult OwnedPthread::CancelAndJoinFor(int timeout_ms,
                                                        int poll_interval_ms,
                                                        PthreadWaitPump pump,
                                                        void* pump_context) {
  OwnedPthreadCancelResult result;
  if (!joinable_) {
    result.cancel_error = EINVAL;
    result.wait = {OwnedPthreadWaitStatus::kPlatformError, EINVAL};
    return result;
  }
  result.cancel_error = pthread_cancel(thread_);
  result.wait = WaitFor(timeout_ms, poll_interval_ms, pump, pump_context);
  return result;
}

int OwnedPthread::Signal(int signal_number) const {
  return joinable_ ? pthread_kill(thread_, signal_number) : ESRCH;
}

const char* OwnedPthreadWaitStatusName(OwnedPthreadWaitStatus status) {
  switch (status) {
    case OwnedPthreadWaitStatus::kJoined:
      return "Joined";
    case OwnedPthreadWaitStatus::kTimedOut:
      return "TimedOut";
    case OwnedPthreadWaitStatus::kPlatformError:
      return "PlatformError";
  }
  return "Unknown";
}

}  // namespace runtime
}  // namespace mocktail
