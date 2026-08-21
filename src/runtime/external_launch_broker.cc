#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runtime/external_launch_broker.h"

#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "runtime/owned_pthread.h"
#include "runtime/single_instance_lock.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr std::string_view kWirePrefix = "MOCKTAIL-EXTERNAL-LAUNCH/1\n";
constexpr std::array<unsigned char, 4> kReplyPrefix = {'M', 'T', 'L', '1'};
constexpr std::size_t kReplySize = kReplyPrefix.size() + 1;
// Mocktail links the Android guest loader into the process. Its static-TLS
// image makes one-megabyte pthread stacks fail with EINVAL on the real runtime
// even before the payload compatibility gate runs. Match the proven guest
// worker floor used by the ExperienceProtocol launch path.
constexpr std::size_t kWorkerStackSize = 64ULL * 1024 * 1024;
constexpr int kWorkerJoinTimeoutMs = 5000;
constexpr int kPeerIoTimeoutSeconds = 2;
constexpr std::size_t kMaximumConfiguredPendingLaunches = 64;

enum class WireReply : unsigned char {
  kAccepted = 0,
  kInvalid = 1,
  kQueueFull = 2,
  kDenied = 3,
  kStopping = 4,
};

std::mutex g_active_broker_mutex;
std::weak_ptr<ExternalLaunchBroker> g_active_broker;

Status Invalid(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status Failed(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

Status Unavailable(std::string message) {
  return Status::Error(StatusCode::kUnavailable, std::move(message));
}

Status Platform(std::string message) {
  return Status::Error(StatusCode::kPlatformError, std::move(message));
}

std::string ErrnoMessage(std::string_view operation, int error_number) {
  return std::string(operation) + ": " + std::strerror(error_number);
}

bool IsPrivateOwnedDirectory(const std::filesystem::path& path) {
  struct stat status{};
  return !path.empty() && path.is_absolute() &&
         lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
         status.st_uid == getuid() && (status.st_mode & 0077) == 0;
}

Status EnsureFallbackDirectory(const std::filesystem::path& path, bool owner) {
  struct stat status{};
  if (lstat(path.c_str(), &status) != 0) {
    const int error_number = errno;
    if (error_number != ENOENT) {
      return Platform(ErrnoMessage("cannot inspect external-launch directory",
                                   error_number));
    }
    if (!owner) {
      return Unavailable("external-launch owner endpoint is unavailable");
    }
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
      return Platform(
          ErrnoMessage("cannot create external-launch directory", errno));
    }
  }
  if (!IsPrivateOwnedDirectory(path)) {
    return Failed(
        "external-launch directory must be private and owned by this user");
  }
  return Status::Ok();
}

Status ValidateSocketPath(const std::filesystem::path& path) {
  if (path.empty() || !path.is_absolute() || path.filename().empty()) {
    return Invalid("external-launch socket path must be absolute");
  }
  const std::string encoded = path.string();
  sockaddr_un address{};
  if (encoded.size() >= sizeof(address.sun_path)) {
    return Invalid("external-launch socket path is too long");
  }
  if (!IsPrivateOwnedDirectory(path.parent_path())) {
    return Failed(
        "external-launch socket parent must be private and user-owned");
  }
  return Status::Ok();
}

Status NormalizeRequest(const RobloxExperienceLaunchRequest& request,
                        RobloxExperienceLaunchRequest* normalized) {
  if (normalized == nullptr) {
    return Invalid("external-launch normalized output is null");
  }
  if (request.canonical_json.empty()) {
    return Invalid("external-launch canonical JSON is empty");
  }
  Status status =
      ParseRobloxExperienceLaunchJson(request.canonical_json, normalized);
  if (!status.ok()) {
    return Invalid("external-launch canonical JSON is invalid");
  }
  return Status::Ok();
}

sockaddr_un SocketAddress(const std::filesystem::path& path,
                          socklen_t* length) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string encoded = path.string();
  std::memcpy(address.sun_path, encoded.c_str(), encoded.size() + 1);
  *length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                   encoded.size() + 1);
  return address;
}

void CloseDescriptor(int* descriptor) {
  if (descriptor != nullptr && *descriptor >= 0) {
    close(*descriptor);
    *descriptor = -1;
  }
}

bool SetPeerTimeouts(int descriptor) {
  timeval timeout{};
  timeout.tv_sec = kPeerIoTimeoutSeconds;
  return setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                    sizeof(timeout)) == 0 &&
         setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                    sizeof(timeout)) == 0;
}

bool SendReply(int descriptor, WireReply reply) {
  std::array<unsigned char, kReplySize> bytes{};
  std::copy(kReplyPrefix.begin(), kReplyPrefix.end(), bytes.begin());
  bytes.back() = static_cast<unsigned char>(reply);
  return send(descriptor, bytes.data(), bytes.size(), MSG_NOSIGNAL) ==
         static_cast<ssize_t>(bytes.size());
}

int RemainingMilliseconds(
    const std::chrono::steady_clock::time_point& deadline) {
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (remaining.count() <= 0) return 0;
  return static_cast<int>(
      std::min<int64_t>(remaining.count(), std::numeric_limits<int>::max()));
}

int ConnectOnce(const std::filesystem::path& path, int timeout_ms,
                int* error_number) {
  *error_number = 0;
  int descriptor =
      socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (descriptor < 0) {
    *error_number = errno;
    return -1;
  }
  socklen_t address_length = 0;
  const sockaddr_un address = SocketAddress(path, &address_length);
  int result = connect(descriptor, reinterpret_cast<const sockaddr*>(&address),
                       address_length);
  if (result != 0 && errno != EINPROGRESS && errno != EAGAIN) {
    *error_number = errno;
    close(descriptor);
    return -1;
  }
  if (result != 0) {
    pollfd wait{descriptor, POLLOUT, 0};
    do {
      result = poll(&wait, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result <= 0) {
      *error_number = result == 0 ? ETIMEDOUT : errno;
      close(descriptor);
      return -1;
    }
    socklen_t error_size = sizeof(*error_number);
    if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, error_number,
                   &error_size) != 0) {
      *error_number = errno;
    }
    if (*error_number != 0) {
      close(descriptor);
      return -1;
    }
  }
  return descriptor;
}

Status SendForwardPacket(
    int descriptor, std::string_view packet,
    const std::chrono::steady_clock::time_point& deadline) {
  ucred peer{};
  socklen_t peer_size = sizeof(peer);
  if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 ||
      peer_size != sizeof(peer) || peer.uid != getuid()) {
    return Failed("external-launch owner credential check failed");
  }

  while (true) {
    const ssize_t sent =
        send(descriptor, packet.data(), packet.size(), MSG_NOSIGNAL);
    if (sent == static_cast<ssize_t>(packet.size())) break;
    if (sent >= 0) {
      return Platform("external-launch request was only partially sent");
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      return Platform(
          ErrnoMessage("cannot send external-launch request", errno));
    }
    const int remaining = RemainingMilliseconds(deadline);
    if (remaining == 0) {
      return Unavailable("external-launch forwarding timed out");
    }
    pollfd wait{descriptor, POLLOUT, 0};
    int poll_result = -1;
    do {
      poll_result = poll(&wait, 1, remaining);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result <= 0) {
      return poll_result == 0
                 ? Unavailable("external-launch forwarding timed out")
                 : Platform(
                       ErrnoMessage("cannot wait to forward launch", errno));
    }
  }

  const int remaining = RemainingMilliseconds(deadline);
  if (remaining == 0) {
    return Unavailable("external-launch owner ACK timed out");
  }
  pollfd wait{descriptor, POLLIN, 0};
  int poll_result = -1;
  do {
    poll_result = poll(&wait, 1, remaining);
  } while (poll_result < 0 && errno == EINTR);
  if (poll_result <= 0) {
    return poll_result == 0
               ? Unavailable("external-launch owner ACK timed out")
               : Platform(ErrnoMessage("cannot wait for launch ACK", errno));
  }
  std::array<unsigned char, kReplySize> reply{};
  const ssize_t received = recv(descriptor, reply.data(), reply.size(), 0);
  if (received != static_cast<ssize_t>(reply.size()) ||
      !std::equal(kReplyPrefix.begin(), kReplyPrefix.end(), reply.begin())) {
    return Platform("external-launch owner returned an invalid ACK");
  }
  switch (static_cast<WireReply>(reply.back())) {
    case WireReply::kAccepted:
      return Status::Ok();
    case WireReply::kInvalid:
      return Invalid("external-launch owner rejected the normalized request");
    case WireReply::kQueueFull:
      return Unavailable("external-launch owner queue is full");
    case WireReply::kDenied:
      return Failed("external-launch owner denied the forwarding peer");
    case WireReply::kStopping:
      return Unavailable("external-launch owner is stopping");
  }
  return Platform("external-launch owner returned an unknown ACK");
}

}  // namespace

Status ResolveExternalLaunchSocketPath(bool owner,
                                       std::filesystem::path* socket_path) {
  if (socket_path == nullptr) {
    return Invalid("external-launch socket output is null");
  }
  socket_path->clear();
  const uid_t user_id = getuid();
  const std::string instance_key = CurrentExecutableInstanceKey();
  // Keep this endpoint coupled to SingleInstanceLock's process-independent
  // current-user path. XDG_RUNTIME_DIR may legitimately differ between a
  // browser sandbox and an already-running Mocktail process; consulting it
  // here would make both processes agree on the lock but disagree on IPC.
  const std::filesystem::path runtime_directory =
      std::filesystem::path("/run/user") / std::to_string(user_id);
  if (IsPrivateOwnedDirectory(runtime_directory)) {
    *socket_path = runtime_directory /
                   ("mocktail." + instance_key +
                    ".external-launch-v1.sock");
    return ValidateSocketPath(*socket_path);
  }

  const std::filesystem::path fallback =
      std::filesystem::path("/tmp") / ("mocktail-" + std::to_string(user_id));
  Status status = EnsureFallbackDirectory(fallback, owner);
  if (!status.ok()) return status;
  *socket_path =
      fallback / (instance_key + ".external-launch-v1.sock");
  return ValidateSocketPath(*socket_path);
}

class ExternalLaunchBroker::Impl final {
 public:
  explicit Impl(ExternalLaunchBrokerOptions options)
      : options_(std::move(options)) {}

  ~Impl() { (void)Shutdown(); }

  Status Start(const RobloxExperienceLaunchRequest* initial_request) {
    if (options_.maximum_pending_launches == 0 ||
        options_.maximum_pending_launches > kMaximumConfiguredPendingLaunches ||
        options_.forwarding_timeout_ms <= 0 ||
        options_.forwarding_timeout_ms > 30000) {
      return Invalid("external-launch broker options are invalid");
    }
    if (initial_request != nullptr) {
      RobloxExperienceLaunchRequest normalized;
      Status initial_status = NormalizeRequest(*initial_request, &normalized);
      if (!initial_status.ok()) return initial_status;
      pending_.push_back(std::move(normalized));
    }
    socket_path_ = options_.socket_path;
    Status status = socket_path_.empty()
                        ? ResolveExternalLaunchSocketPath(true, &socket_path_)
                        : ValidateSocketPath(socket_path_);
    if (!status.ok()) return status;

    struct stat stale{};
    if (lstat(socket_path_.c_str(), &stale) == 0) {
      if (!S_ISSOCK(stale.st_mode) || stale.st_uid != getuid()) {
        return Failed("external-launch endpoint is not an owned stale socket");
      }
      if (unlink(socket_path_.c_str()) != 0) {
        return Platform(
            ErrnoMessage("cannot remove stale external-launch socket", errno));
      }
    } else if (errno != ENOENT) {
      return Platform(
          ErrnoMessage("cannot inspect external-launch socket", errno));
    }

    listener_ =
        socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener_ < 0) {
      return Platform(
          ErrnoMessage("cannot create external-launch socket", errno));
    }
    socklen_t address_length = 0;
    const sockaddr_un address = SocketAddress(socket_path_, &address_length);
    if (bind(listener_, reinterpret_cast<const sockaddr*>(&address),
             address_length) != 0 ||
        chmod(socket_path_.c_str(), 0600) != 0 ||
        listen(listener_,
               static_cast<int>(options_.maximum_pending_launches)) != 0) {
      const int error_number = errno;
      CleanupEndpoint();
      return Platform(
          ErrnoMessage("cannot activate external-launch socket", error_number));
    }
    struct stat bound{};
    if (lstat(socket_path_.c_str(), &bound) != 0 || !S_ISSOCK(bound.st_mode) ||
        bound.st_uid != getuid() || (bound.st_mode & 0077) != 0) {
      CleanupEndpoint();
      return Failed("external-launch socket ownership is invalid");
    }
    socket_device_ = bound.st_dev;
    socket_inode_ = bound.st_ino;

    stop_event_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (stop_event_ < 0) {
      const int error_number = errno;
      CleanupEndpoint();
      return Platform(ErrnoMessage("cannot create external-launch stop event",
                                   error_number));
    }
    stopping_.store(false, std::memory_order_release);
    const int start_error =
        worker_.Start(&Impl::WorkerEntry, this, kWorkerStackSize);
    if (start_error != 0) {
      CleanupEndpoint();
      return Platform("cannot start external-launch broker worker: " +
                      std::to_string(start_error));
    }
    started_ = true;
    return Status::Ok();
  }

  Status QueueInitial(RobloxExperienceLaunchRequest request) {
    RobloxExperienceLaunchRequest normalized;
    Status status = NormalizeRequest(request, &normalized);
    return status.ok() ? QueueNormalized(std::move(normalized)) : status;
  }

  Status Drain(const ExternalLaunchSink& sink, std::size_t maximum_requests) {
    if (!sink.valid()) {
      return Invalid("external-launch sink is incomplete");
    }
    std::lock_guard<std::mutex> drain_lock(drain_mutex_);
    if (stopping_.load(std::memory_order_acquire)) {
      return Failed("external-launch broker is stopping");
    }
    for (std::size_t count = 0; count < maximum_requests; ++count) {
      RobloxExperienceLaunchRequest request;
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (pending_.empty()) break;
        request = std::move(pending_.front());
        pending_.pop_front();
      }
      Status status = sink.dispatch(sink.context, request);
      if (!status.ok()) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pending_.push_front(std::move(request));
        return status;
      }
    }
    return Status::Ok();
  }

  Status Shutdown() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!started_) return Status::Ok();
    std::lock_guard<std::mutex> drain_lock(drain_mutex_);
    stopping_.store(true, std::memory_order_release);
    uint64_t wake = 1;
    while (write(stop_event_, &wake, sizeof(wake)) < 0 && errno == EINTR) {
    }
    OwnedPthreadWaitResult wait = worker_.WaitFor(kWorkerJoinTimeoutMs, 5);
    if (!wait.joined()) {
      // Closing the listener makes a missed wake observable to poll while the
      // stop flag prevents any newly accepted request from being queued.
      CloseDescriptor(&listener_);
      wait = worker_.WaitFor(kPeerIoTimeoutSeconds * 1000 + 1000, 5);
    }
    int cancel_error = 0;
    if (!wait.joined()) {
      const OwnedPthreadCancelResult cancelled =
          worker_.CancelAndJoinFor(kWorkerJoinTimeoutMs, 5);
      cancel_error = cancelled.cancel_error;
      wait = cancelled.wait;
    }
    if (!wait.joined()) {
      return Platform("external-launch worker did not stop: wait=" +
                      std::string(OwnedPthreadWaitStatusName(wait.status)) +
                      " platform_error=" + std::to_string(wait.platform_error) +
                      " cancel_error=" + std::to_string(cancel_error));
    }
    CloseDescriptor(&stop_event_);
    CloseDescriptor(&listener_);
    RemoveOwnedSocket();
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      pending_.clear();
    }
    started_ = false;
    return Status::Ok();
  }

  std::size_t pending_launch_count() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return pending_.size();
  }

  const std::filesystem::path& socket_path() const { return socket_path_; }

 private:
  static void* WorkerEntry(void* context) {
    if (context != nullptr) static_cast<Impl*>(context)->WorkerLoop();
    return nullptr;
  }

  void WorkerLoop() {
    while (!stopping_.load(std::memory_order_acquire)) {
      pollfd descriptors[2] = {{listener_, POLLIN, 0},
                               {stop_event_, POLLIN, 0}};
      int poll_result = -1;
      do {
        poll_result = poll(descriptors, 2, -1);
      } while (poll_result < 0 && errno == EINTR);
      if (poll_result <= 0 || (descriptors[1].revents & POLLIN) != 0 ||
          stopping_.load(std::memory_order_acquire)) {
        break;
      }
      if ((descriptors[0].revents & POLLIN) == 0) continue;
      while (!stopping_.load(std::memory_order_acquire)) {
        const int peer = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
        if (peer < 0) {
          if (errno == EINTR) continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;
          break;
        }
        HandlePeer(peer);
        close(peer);
      }
    }
  }

  void HandlePeer(int peer) {
    if (!SetPeerTimeouts(peer)) return;
    ucred credentials{};
    socklen_t credentials_size = sizeof(credentials);
    if (getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credentials,
                   &credentials_size) != 0 ||
        credentials_size != sizeof(credentials) ||
        credentials.uid != getuid()) {
      (void)SendReply(peer, WireReply::kDenied);
      return;
    }
    std::vector<char> packet(kWirePrefix.size() +
                             kMaximumExperienceLaunchJsonBytes);
    const ssize_t received =
        recv(peer, packet.data(), packet.size(), MSG_TRUNC);
    if (received <= static_cast<ssize_t>(kWirePrefix.size()) ||
        received > static_cast<ssize_t>(packet.size()) ||
        !std::equal(kWirePrefix.begin(), kWirePrefix.end(), packet.begin())) {
      (void)SendReply(peer, WireReply::kInvalid);
      return;
    }
    const std::string canonical_json(
        packet.data() + kWirePrefix.size(),
        static_cast<std::size_t>(received) - kWirePrefix.size());
    RobloxExperienceLaunchRequest request;
    Status status = ParseRobloxExperienceLaunchJson(canonical_json, &request);
    if (!status.ok()) {
      (void)SendReply(peer, WireReply::kInvalid);
      return;
    }
    status = QueueNormalized(std::move(request));
    WireReply reply = WireReply::kAccepted;
    if (!status.ok()) {
      reply = status.code() == StatusCode::kUnavailable ? WireReply::kQueueFull
                                                        : WireReply::kStopping;
    }
    (void)SendReply(peer, reply);
  }

  Status QueueNormalized(RobloxExperienceLaunchRequest request) {
    if (stopping_.load(std::memory_order_acquire)) {
      return Failed("external-launch broker is stopping");
    }
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (stopping_.load(std::memory_order_relaxed)) {
      return Failed("external-launch broker is stopping");
    }
    if (pending_.size() >= options_.maximum_pending_launches) {
      return Unavailable("external-launch queue is full");
    }
    pending_.push_back(std::move(request));
    return Status::Ok();
  }

  void CleanupEndpoint() {
    CloseDescriptor(&stop_event_);
    CloseDescriptor(&listener_);
    struct stat status{};
    if (!socket_path_.empty() && lstat(socket_path_.c_str(), &status) == 0 &&
        S_ISSOCK(status.st_mode) && status.st_uid == getuid()) {
      (void)unlink(socket_path_.c_str());
    }
  }

  void RemoveOwnedSocket() {
    struct stat status{};
    if (!socket_path_.empty() && lstat(socket_path_.c_str(), &status) == 0 &&
        S_ISSOCK(status.st_mode) && status.st_uid == getuid() &&
        status.st_dev == socket_device_ && status.st_ino == socket_inode_) {
      (void)unlink(socket_path_.c_str());
    }
    socket_device_ = 0;
    socket_inode_ = 0;
  }

  const ExternalLaunchBrokerOptions options_;
  std::filesystem::path socket_path_;
  mutable std::mutex queue_mutex_;
  std::mutex drain_mutex_;
  std::mutex lifecycle_mutex_;
  std::deque<RobloxExperienceLaunchRequest> pending_;
  OwnedPthread worker_;
  std::atomic<bool> stopping_{true};
  int listener_ = -1;
  int stop_event_ = -1;
  dev_t socket_device_ = 0;
  ino_t socket_inode_ = 0;
  bool started_ = false;
};

ExternalLaunchBroker::ExternalLaunchBroker(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ExternalLaunchBroker::~ExternalLaunchBroker() { (void)impl_->Shutdown(); }

Status ExternalLaunchBroker::StartOwnerAfterLockAcquired(
    ExternalLaunchBrokerOptions options,
    std::shared_ptr<ExternalLaunchBroker>* broker,
    const RobloxExperienceLaunchRequest* initial_request) {
  if (broker == nullptr) {
    return Invalid("external-launch broker output is null");
  }
  broker->reset();
  auto impl = std::make_unique<Impl>(std::move(options));
  Status status = impl->Start(initial_request);
  if (!status.ok()) return status;
  *broker = std::shared_ptr<ExternalLaunchBroker>(
      new ExternalLaunchBroker(std::move(impl)));
  return Status::Ok();
}

Status ExternalLaunchBroker::ForwardToOwner(
    const ExternalLaunchBrokerOptions& options,
    const RobloxExperienceLaunchRequest& request) {
  if (options.forwarding_timeout_ms <= 0 ||
      options.forwarding_timeout_ms > 30000) {
    return Invalid("external-launch forwarding timeout is invalid");
  }
  RobloxExperienceLaunchRequest normalized;
  Status status = NormalizeRequest(request, &normalized);
  if (!status.ok()) return status;
  const std::string packet =
      std::string(kWirePrefix) + normalized.canonical_json;
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(options.forwarding_timeout_ms);
  int last_error = ENOENT;
  while (RemainingMilliseconds(deadline) > 0) {
    std::filesystem::path socket_path = options.socket_path;
    status = socket_path.empty()
                 ? ResolveExternalLaunchSocketPath(false, &socket_path)
                 : ValidateSocketPath(socket_path);
    if (!status.ok()) {
      if (status.code() != StatusCode::kUnavailable) return status;
    } else {
      int connect_error = 0;
      const int descriptor = ConnectOnce(
          socket_path, RemainingMilliseconds(deadline), &connect_error);
      if (descriptor >= 0) {
        status = SendForwardPacket(descriptor, packet, deadline);
        close(descriptor);
        return status;
      }
      last_error = connect_error;
      if (last_error != ENOENT && last_error != ECONNREFUSED &&
          last_error != EAGAIN && last_error != ETIMEDOUT) {
        return Platform(ErrnoMessage("cannot connect to external-launch owner",
                                     last_error));
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return Unavailable(
      ErrnoMessage("external-launch owner is unavailable", last_error));
}

Status ExternalLaunchBroker::QueueInitialRequest(
    RobloxExperienceLaunchRequest request) {
  return impl_->QueueInitial(std::move(request));
}

Status ExternalLaunchBroker::Drain(const ExternalLaunchSink& sink,
                                   std::size_t maximum_requests) {
  return impl_->Drain(sink, maximum_requests);
}

Status ExternalLaunchBroker::Shutdown() { return impl_->Shutdown(); }

std::size_t ExternalLaunchBroker::pending_launch_count() const {
  return impl_->pending_launch_count();
}

const std::filesystem::path& ExternalLaunchBroker::socket_path() const {
  return impl_->socket_path();
}

Status InstallActiveExternalLaunchBroker(
    const std::shared_ptr<ExternalLaunchBroker>& broker) {
  if (broker == nullptr)
    return Invalid("active external-launch broker is null");
  std::lock_guard<std::mutex> lock(g_active_broker_mutex);
  if (!g_active_broker.expired()) {
    return Failed("an active external-launch broker is already installed");
  }
  g_active_broker = broker;
  return Status::Ok();
}

void ClearActiveExternalLaunchBroker(const ExternalLaunchBroker* broker) {
  std::lock_guard<std::mutex> lock(g_active_broker_mutex);
  const std::shared_ptr<ExternalLaunchBroker> active = g_active_broker.lock();
  if (active.get() == broker) g_active_broker.reset();
}

std::shared_ptr<ExternalLaunchBroker> GetActiveExternalLaunchBroker() {
  std::lock_guard<std::mutex> lock(g_active_broker_mutex);
  return g_active_broker.lock();
}

Status DrainActiveExternalLaunchRequests(const ExternalLaunchSink& sink,
                                         std::size_t maximum_requests) {
  const std::shared_ptr<ExternalLaunchBroker> broker =
      GetActiveExternalLaunchBroker();
  return broker != nullptr ? broker->Drain(sink, maximum_requests)
                           : Status::Ok();
}

}  // namespace runtime
}  // namespace mocktail
