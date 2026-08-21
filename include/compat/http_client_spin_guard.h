#ifndef MOCKTAIL_COMPAT_HTTP_CLIENT_SPIN_GUARD_H_
#define MOCKTAIL_COMPAT_HTTP_CLIENT_SPIN_GUARD_H_

#include <cstdint>

namespace mocktail {
namespace compat {

constexpr std::uint32_t kHttpClientSpinBatchSize = 256;
constexpr std::uint64_t kHttpClientSpinBatchIntervalNs = 1000000;

// Pure rate-limiter state used by the process-local HttpClient guard. A normal
// request that does not complete a full mutex batch is never delayed.
class HttpClientSpinRateLimiter {
 public:
  std::uint64_t Observe(std::uint64_t now_ns);

 private:
  std::uint32_t operation_count_ = 0;
  std::uint64_t checkpoint_ns_ = 0;
};

// Called after a successful Bionic mutex unlock. Only a thread named exactly
// "HttpClient" is rate-limited, and only when it completes mutex batches
// faster than the cooperative one-millisecond interval.
void ApplyHttpClientSpinGuard();

}  // namespace compat
}  // namespace mocktail

#endif  // MOCKTAIL_COMPAT_HTTP_CLIENT_SPIN_GUARD_H_
