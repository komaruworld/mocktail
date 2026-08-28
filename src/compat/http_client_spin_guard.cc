#include "compat/http_client_spin_guard.h"

#include <pthread.h>
#include <time.h>

#include <cerrno>
#include <cstring>

namespace mocktail {
namespace compat {
namespace {

struct ThreadSpinState {
  HttpClientSpinRateLimiter limiter;
  uint8_t checked_state = 0;  // 0 = unchecked, 1 = not_http, 2 = is_http
  uint8_t retries_remaining = 8;
};

thread_local ThreadSpinState g_thread_spin_state;

bool IsHttpClientThread(ThreadSpinState* state) {
  if (__builtin_expect(state->checked_state == 1, 1)) {
    return false;
  }
  if (__builtin_expect(state->checked_state == 2, 0)) {
    return true;
  }

  char thread_name[16]{};
  if (pthread_getname_np(pthread_self(), thread_name, sizeof(thread_name)) == 0) {
    if (std::strcmp(thread_name, "HttpClient") == 0) {
      state->checked_state = 2;
      return true;
    }
    if (thread_name[0] != '\0') {
      state->checked_state = 1;
      return false;
    }
  }

  if (state->retries_remaining > 0) {
    --state->retries_remaining;
  } else {
    state->checked_state = 1;
  }
  return false;
}

std::uint64_t MonotonicNowNs() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(now.tv_nsec);
}

void SleepForNs(std::uint64_t pause_ns) {
  timespec request{};
  request.tv_sec = static_cast<time_t>(pause_ns / 1000000000ULL);
  request.tv_nsec = static_cast<long>(pause_ns % 1000000000ULL);
  while (nanosleep(&request, &request) != 0 && errno == EINTR) {
  }
}

}  // namespace

std::uint64_t HttpClientSpinRateLimiter::Observe(std::uint64_t now_ns) {
  ++operation_count_;
  if (operation_count_ < kHttpClientSpinBatchSize) {
    return 0;
  }
  operation_count_ = 0;

  if (now_ns == 0 || checkpoint_ns_ == 0 || now_ns < checkpoint_ns_) {
    checkpoint_ns_ = now_ns;
    return 0;
  }
  const std::uint64_t elapsed_ns = now_ns - checkpoint_ns_;
  if (elapsed_ns >= kHttpClientSpinBatchIntervalNs) {
    checkpoint_ns_ = now_ns;
    return 0;
  }
  const std::uint64_t pause_ns =
      kHttpClientSpinBatchIntervalNs - elapsed_ns;
  checkpoint_ns_ = now_ns + pause_ns;
  return pause_ns;
}

void ApplyHttpClientSpinGuard() {
  ThreadSpinState* state = &g_thread_spin_state;
  if (!IsHttpClientThread(state)) {
    return;
  }
  const std::uint64_t pause_ns = state->limiter.Observe(MonotonicNowNs());
  if (pause_ns != 0) {
    SleepForNs(pause_ns);
  }
}

}  // namespace compat
}  // namespace mocktail
