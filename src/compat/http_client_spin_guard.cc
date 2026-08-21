#include "compat/http_client_spin_guard.h"

#include <pthread.h>
#include <time.h>

#include <cerrno>
#include <cstring>

namespace mocktail {
namespace compat {
namespace {

constexpr std::uint32_t kThreadNameRecheckInterval = 4096;

struct ThreadSpinState {
  HttpClientSpinRateLimiter limiter;
  std::uint32_t operations_until_name_check = 0;
  bool is_http_client = false;
};

thread_local ThreadSpinState g_thread_spin_state;

bool IsHttpClientThread(ThreadSpinState* state) {
  if (state->is_http_client) {
    return true;
  }
  if (state->operations_until_name_check != 0) {
    --state->operations_until_name_check;
    return false;
  }
  state->operations_until_name_check = kThreadNameRecheckInterval;

  char thread_name[16]{};
  if (pthread_getname_np(pthread_self(), thread_name, sizeof(thread_name)) !=
      0) {
    return false;
  }
  state->is_http_client = std::strcmp(thread_name, "HttpClient") == 0;
  return state->is_http_client;
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
