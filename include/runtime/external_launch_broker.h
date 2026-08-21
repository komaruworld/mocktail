#ifndef MOCKTAIL_RUNTIME_EXTERNAL_LAUNCH_BROKER_H_
#define MOCKTAIL_RUNTIME_EXTERNAL_LAUNCH_BROKER_H_

#include <cstddef>
#include <filesystem>
#include <memory>

#include "mocktail/status.h"
#include "runtime/roblox_experience_launch_bridge.h"

namespace mocktail {
namespace runtime {

inline constexpr std::size_t kMaximumPendingExternalLaunches = 8;

struct ExternalLaunchBrokerOptions {
  // Empty selects the current user's protected endpoint.
  std::filesystem::path socket_path;
  std::size_t maximum_pending_launches = kMaximumPendingExternalLaunches;
  // Covers bootstrap and a possible cgroup re-exec.
  int forwarding_timeout_ms = 30000;
};

using DispatchExternalLaunchFn =
    Status (*)(void* context, const RobloxExperienceLaunchRequest& request);

struct ExternalLaunchSink {
  void* context = nullptr;
  DispatchExternalLaunchFn dispatch = nullptr;

  bool valid() const { return dispatch != nullptr; }
};

// Resolution never removes stale sockets; only a lock-owning broker may do so.
Status ResolveExternalLaunchSocketPath(bool owner,
                                       std::filesystem::path* socket_path);

// The wire format contains normalized requests, never raw browser tickets.
class ExternalLaunchBroker final {
 public:
  ~ExternalLaunchBroker();

  ExternalLaunchBroker(const ExternalLaunchBroker&) = delete;
  ExternalLaunchBroker& operator=(const ExternalLaunchBroker&) = delete;
  ExternalLaunchBroker(ExternalLaunchBroker&&) = delete;
  ExternalLaunchBroker& operator=(ExternalLaunchBroker&&) = delete;

  // Requires the single-instance lock. The initial request is queued before
  // the listener starts, preserving click order.
  static Status StartOwnerAfterLockAcquired(
      ExternalLaunchBrokerOptions options,
      std::shared_ptr<ExternalLaunchBroker>* broker,
      const RobloxExperienceLaunchRequest* initial_request = nullptr);

  // Waits for an owner ACK and never logs the request.
  static Status ForwardToOwner(const ExternalLaunchBrokerOptions& options,
                               const RobloxExperienceLaunchRequest& request);

  Status QueueInitialRequest(RobloxExperienceLaunchRequest request);

  // A failed dispatch returns to the front of the FIFO queue.
  Status Drain(const ExternalLaunchSink& sink, std::size_t maximum_requests);

  Status Shutdown();
  std::size_t pending_launch_count() const;
  const std::filesystem::path& socket_path() const;

 private:
  class Impl;
  explicit ExternalLaunchBroker(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

// shared_ptr acquisition keeps the active broker alive during a drain.
Status InstallActiveExternalLaunchBroker(
    const std::shared_ptr<ExternalLaunchBroker>& broker);
void ClearActiveExternalLaunchBroker(const ExternalLaunchBroker* broker);
std::shared_ptr<ExternalLaunchBroker> GetActiveExternalLaunchBroker();
Status DrainActiveExternalLaunchRequests(const ExternalLaunchSink& sink,
                                         std::size_t maximum_requests);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_EXTERNAL_LAUNCH_BROKER_H_
