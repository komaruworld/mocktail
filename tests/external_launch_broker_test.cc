#include "runtime/external_launch_broker.h"

#include "runtime/single_instance_lock.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

RobloxExperienceLaunchRequest Request(int64_t place_id,
                                      std::string game_instance_id = {}) {
  const std::string json =
      game_instance_id.empty()
          ? "{\"placeId\":" + std::to_string(place_id) + "}"
          : "{\"gameInstanceId\":\"" + game_instance_id +
                "\",\"placeId\":" + std::to_string(place_id) + "}";
  RobloxExperienceLaunchRequest request;
  EXPECT_TRUE(ParseRobloxExperienceLaunchJson(json, &request).ok());
  return request;
}

struct CaptureProbe {
  std::vector<RobloxExperienceLaunchRequest> requests;
  bool reject_next = false;
};

Status Capture(void* context, const RobloxExperienceLaunchRequest& request) {
  auto* probe = static_cast<CaptureProbe*>(context);
  if (probe == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument, "capture probe is null");
  }
  if (probe->reject_next) {
    probe->reject_next = false;
    return Status::Error(StatusCode::kUnavailable,
                         "capture sink is temporarily full");
  }
  probe->requests.push_back(request);
  return Status::Ok();
}

class ExternalLaunchBrokerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/mocktail-external-launch-test-XXXXXX";
    char* created = mkdtemp(pattern);
    ASSERT_NE(created, nullptr);
    directory_ = created;
    ASSERT_EQ(chmod(directory_.c_str(), 0700), 0);
    options_.socket_path = directory_ / "broker.sock";
    options_.forwarding_timeout_ms = 1000;
  }

  void TearDown() override {
    if (broker_ != nullptr) {
      ClearActiveExternalLaunchBroker(broker_.get());
      EXPECT_TRUE(broker_->Shutdown().ok());
      broker_.reset();
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  void Start(
      std::size_t maximum_pending_launches = kMaximumPendingExternalLaunches) {
    options_.maximum_pending_launches = maximum_pending_launches;
    const Status status =
        ExternalLaunchBroker::StartOwnerAfterLockAcquired(options_, &broker_);
    ASSERT_TRUE(status.ok()) << status.message();
  }

  std::filesystem::path directory_;
  ExternalLaunchBrokerOptions options_;
  std::shared_ptr<ExternalLaunchBroker> broker_;
};

TEST_F(ExternalLaunchBrokerTest, QueuesAndDrainsInitialRequestsInFifoOrder) {
  Start();
  ASSERT_TRUE(broker_->QueueInitialRequest(Request(1001)).ok());
  ASSERT_TRUE(broker_->QueueInitialRequest(Request(1002, "server-two")).ok());

  CaptureProbe probe;
  const Status status =
      broker_->Drain({&probe, Capture}, kMaximumPendingExternalLaunches);
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_EQ(probe.requests.size(), 2u);
  EXPECT_EQ(probe.requests[0].place_id, 1001);
  EXPECT_EQ(probe.requests[1].place_id, 1002);
  EXPECT_EQ(probe.requests[1].game_instance_id, "server-two");
  EXPECT_EQ(broker_->pending_launch_count(), 0u);
}

TEST_F(ExternalLaunchBrokerTest, QueuesControlledStartBeforeListenerAccepts) {
  const RobloxExperienceLaunchRequest initial = Request(1501, "first-click");
  const Status start_status = ExternalLaunchBroker::StartOwnerAfterLockAcquired(
      options_, &broker_, &initial);
  ASSERT_TRUE(start_status.ok()) << start_status.message();
  ASSERT_TRUE(ExternalLaunchBroker::ForwardToOwner(
                  options_, Request(1502, "second-click"))
                  .ok());

  CaptureProbe probe;
  ASSERT_TRUE(broker_->Drain({&probe, Capture}, 2).ok());
  ASSERT_EQ(probe.requests.size(), 2u);
  EXPECT_EQ(probe.requests[0].game_instance_id, "first-click");
  EXPECT_EQ(probe.requests[1].game_instance_id, "second-click");
}

TEST_F(ExternalLaunchBrokerTest, IgnoresProcessLocalXdgRuntimeDirectory) {
  const char* previous = std::getenv("XDG_RUNTIME_DIR");
  const bool had_previous = previous != nullptr;
  const std::string previous_value = had_previous ? previous : "";
  ASSERT_EQ(setenv("XDG_RUNTIME_DIR", directory_.c_str(), 1), 0);

  std::filesystem::path resolved;
  const Status status = ResolveExternalLaunchSocketPath(true, &resolved);

  if (had_previous) {
    EXPECT_EQ(setenv("XDG_RUNTIME_DIR", previous_value.c_str(), 1), 0);
  } else {
    EXPECT_EQ(unsetenv("XDG_RUNTIME_DIR"), 0);
  }
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_NE(resolved.parent_path(), directory_);
  EXPECT_NE(resolved.filename().string().find(
                CurrentExecutableInstanceKey()),
            std::string::npos);
}

TEST_F(ExternalLaunchBrokerTest, ForwardsNormalizedRequestAndWaitsForOwnerAck) {
  Start();
  const Status status = ExternalLaunchBroker::ForwardToOwner(
      options_, Request(2001, "private-instance"));
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_EQ(broker_->pending_launch_count(), 1u);

  CaptureProbe probe;
  ASSERT_TRUE(broker_->Drain({&probe, Capture}, 1).ok());
  ASSERT_EQ(probe.requests.size(), 1u);
  EXPECT_EQ(probe.requests[0].place_id, 2001);
  EXPECT_EQ(probe.requests[0].game_instance_id, "private-instance");
}

TEST_F(ExternalLaunchBrokerTest, RetriesWhileOwnerCompletesControlledStart) {
  EXPECT_EQ(ExternalLaunchBrokerOptions{}.forwarding_timeout_ms, 30000);
  Status forwarded = Status::Error(StatusCode::kUnavailable, "not attempted");
  const RobloxExperienceLaunchRequest request = Request(2501);
  const ExternalLaunchBrokerOptions client_options = options_;
  std::thread client([&forwarded, &request, &client_options]() {
    forwarded = ExternalLaunchBroker::ForwardToOwner(client_options, request);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  Start();
  client.join();

  ASSERT_TRUE(forwarded.ok()) << forwarded.message();
  EXPECT_EQ(broker_->pending_launch_count(), 1u);
}

TEST_F(ExternalLaunchBrokerTest, FailsClosedWhenOwnerQueueIsFull) {
  Start(1);
  ASSERT_TRUE(broker_->QueueInitialRequest(Request(3001)).ok());

  const Status local_status = broker_->QueueInitialRequest(Request(3002));
  EXPECT_FALSE(local_status.ok());
  EXPECT_EQ(local_status.code(), StatusCode::kUnavailable);

  const Status forwarded_status =
      ExternalLaunchBroker::ForwardToOwner(options_, Request(3003));
  EXPECT_FALSE(forwarded_status.ok());
  EXPECT_EQ(forwarded_status.code(), StatusCode::kUnavailable);
  EXPECT_EQ(broker_->pending_launch_count(), 1u);
}

TEST_F(ExternalLaunchBrokerTest, RequeuesAtFrontWhenDownstreamIsBusy) {
  Start();
  ASSERT_TRUE(broker_->QueueInitialRequest(Request(4001)).ok());
  ASSERT_TRUE(broker_->QueueInitialRequest(Request(4002)).ok());

  CaptureProbe probe;
  probe.reject_next = true;
  const Status first = broker_->Drain({&probe, Capture}, 2);
  EXPECT_FALSE(first.ok());
  EXPECT_EQ(first.code(), StatusCode::kUnavailable);
  EXPECT_EQ(broker_->pending_launch_count(), 2u);

  ASSERT_TRUE(broker_->Drain({&probe, Capture}, 2).ok());
  ASSERT_EQ(probe.requests.size(), 2u);
  EXPECT_EQ(probe.requests[0].place_id, 4001);
  EXPECT_EQ(probe.requests[1].place_id, 4002);
}

TEST_F(ExternalLaunchBrokerTest, RejectsMalformedWirePacketWithoutQueueing) {
  Start();
  const int peer = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  ASSERT_GE(peer, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string path = options_.socket_path.string();
  ASSERT_LT(path.size(), sizeof(address.sun_path));
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const socklen_t address_length =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
  ASSERT_EQ(connect(peer, reinterpret_cast<const sockaddr*>(&address),
                    address_length),
            0);
  constexpr std::array<char, 5> kMalformed = {'w', 'r', 'o', 'n', 'g'};
  ASSERT_EQ(send(peer, kMalformed.data(), kMalformed.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(kMalformed.size()));
  std::array<unsigned char, 5> reply{};
  ASSERT_EQ(recv(peer, reply.data(), reply.size(), 0),
            static_cast<ssize_t>(reply.size()));
  close(peer);

  EXPECT_EQ(reply[0], 'M');
  EXPECT_EQ(reply[1], 'T');
  EXPECT_EQ(reply[2], 'L');
  EXPECT_EQ(reply[3], '1');
  EXPECT_EQ(reply[4], 1);
  EXPECT_EQ(broker_->pending_launch_count(), 0u);
}

TEST_F(ExternalLaunchBrokerTest, ReplacesOnlyAnOwnedStaleSocket) {
  const int stale = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  ASSERT_GE(stale, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string path = options_.socket_path.string();
  ASSERT_LT(path.size(), sizeof(address.sun_path));
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const socklen_t address_length =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
  ASSERT_EQ(
      bind(stale, reinterpret_cast<const sockaddr*>(&address), address_length),
      0);
  close(stale);

  Start();
  EXPECT_TRUE(std::filesystem::is_socket(options_.socket_path));
  ASSERT_TRUE(broker_->Shutdown().ok());
  EXPECT_FALSE(std::filesystem::exists(options_.socket_path));
}

TEST_F(ExternalLaunchBrokerTest, PreservesNonSocketEndpointAndRefusesStart) {
  std::ofstream endpoint(options_.socket_path);
  endpoint << "not a socket";
  endpoint.close();

  const Status status =
      ExternalLaunchBroker::StartOwnerAfterLockAcquired(options_, &broker_);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kFailedPrecondition);
  EXPECT_TRUE(std::filesystem::is_regular_file(options_.socket_path));
}

TEST_F(ExternalLaunchBrokerTest, GlobalAttachmentDrainsAndRequiresOneOwner) {
  Start();
  ASSERT_TRUE(InstallActiveExternalLaunchBroker(broker_).ok());
  const Status duplicate = InstallActiveExternalLaunchBroker(broker_);
  EXPECT_FALSE(duplicate.ok());
  EXPECT_EQ(duplicate.code(), StatusCode::kFailedPrecondition);
  ASSERT_TRUE(broker_->QueueInitialRequest(Request(5001)).ok());

  CaptureProbe probe;
  ASSERT_TRUE(DrainActiveExternalLaunchRequests({&probe, Capture}, 1).ok());
  ASSERT_EQ(probe.requests.size(), 1u);
  EXPECT_EQ(probe.requests[0].place_id, 5001);

  ClearActiveExternalLaunchBroker(broker_.get());
  EXPECT_EQ(GetActiveExternalLaunchBroker(), nullptr);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
