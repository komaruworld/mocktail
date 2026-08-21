#include "runtime/discord_rpc.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

RobloxExperienceLaunchRequest PublicServer() {
  RobloxExperienceLaunchRequest request;
  request.place_id = 189707;
  request.game_instance_id = "job/a+b";
  return request;
}

void Encode32(uint32_t value, unsigned char* output) {
  output[0] = static_cast<unsigned char>(value & 0xffU);
  output[1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
  output[2] = static_cast<unsigned char>((value >> 16U) & 0xffU);
  output[3] = static_cast<unsigned char>((value >> 24U) & 0xffU);
}

uint32_t Decode32(const unsigned char* input) {
  return static_cast<uint32_t>(input[0]) |
         (static_cast<uint32_t>(input[1]) << 8U) |
         (static_cast<uint32_t>(input[2]) << 16U) |
         (static_cast<uint32_t>(input[3]) << 24U);
}

bool ReadBytes(int descriptor, void* output, std::size_t size) {
  auto* next = static_cast<unsigned char*>(output);
  while (size != 0) {
    const ssize_t received = recv(descriptor, next, size, 0);
    if (received <= 0) return false;
    next += received;
    size -= static_cast<std::size_t>(received);
  }
  return true;
}

bool ReadTestFrame(int descriptor, uint32_t* opcode, std::string* payload) {
  std::array<unsigned char, 8> header{};
  if (!ReadBytes(descriptor, header.data(), header.size())) return false;
  *opcode = Decode32(header.data());
  const uint32_t size = Decode32(header.data() + 4);
  if (size > 1024 * 1024) return false;
  payload->resize(size);
  return size == 0 || ReadBytes(descriptor, payload->data(), payload->size());
}

bool WriteTestFrame(int descriptor, uint32_t opcode, std::string_view payload) {
  std::array<unsigned char, 8> header{};
  Encode32(opcode, header.data());
  Encode32(static_cast<uint32_t>(payload.size()), header.data() + 4);
  return send(descriptor, header.data(), header.size(), MSG_NOSIGNAL) ==
             static_cast<ssize_t>(header.size()) &&
         send(descriptor, payload.data(), payload.size(), MSG_NOSIGNAL) ==
             static_cast<ssize_t>(payload.size());
}

TEST(DiscordRpcTest, BuildsEnglishBrowsingAndJoiningActivities) {
  const DiscordRpcConfig config;

  const DiscordRpcActivity browsing = BuildDiscordRpcActivity(
      config, RobloxExperiencePresencePhase::kBrowsing, nullptr, {}, 0);
  EXPECT_EQ(browsing.details, "Browsing experiences");
  EXPECT_EQ(browsing.state, "Playing Roblox");
  EXPECT_FALSE(browsing.start_timestamp.has_value());
  EXPECT_TRUE(browsing.button_url.empty());

  const RobloxExperienceLaunchRequest request = PublicServer();
  const DiscordRpcActivity joining = BuildDiscordRpcActivity(
      config, RobloxExperiencePresencePhase::kJoining, &request, {}, 0);
  EXPECT_EQ(joining.details, "Joining an experience");
  EXPECT_EQ(joining.state, "Playing Roblox");
  EXPECT_TRUE(joining.button_url.empty());
}

TEST(DiscordRpcTest, RequiresAReleaseApplicationIdWhenEnabled) {
  DiscordRpcConfig config;
  config.enabled = true;
  DiscordRpcSession session(std::move(config));
  std::string detail;
  EXPECT_FALSE(session.Start(&detail));
  EXPECT_NE(detail.find("application ID"), std::string::npos);
}

TEST(DiscordRpcTest, ShowsPlaceTimerAndPublicJoinWhenPlaying) {
  const DiscordRpcConfig config;
  const RobloxExperienceLaunchRequest request = PublicServer();

  const DiscordRpcActivity activity = BuildDiscordRpcActivity(
      config, RobloxExperiencePresencePhase::kPlaying, &request,
      "Natural Disaster Survival", 1770000000);

  EXPECT_EQ(activity.details, "Natural Disaster Survival");
  EXPECT_EQ(activity.state, "Playing Roblox");
  ASSERT_TRUE(activity.start_timestamp.has_value());
  EXPECT_EQ(*activity.start_timestamp, 1770000000);
  EXPECT_EQ(activity.button_label, "Join Server");
  EXPECT_EQ(activity.button_url,
            "https://komaruworld.github.io/mocktail/join.html#placeId="
            "189707&gameInstanceId=job%2Fa%2Bb");
}

TEST(DiscordRpcTest, ShowsExternalPlaceThumbnailWhenAvailable) {
  const DiscordRpcConfig config;
  const RobloxExperienceLaunchRequest request = PublicServer();
  const DiscordRpcActivity activity = BuildDiscordRpcActivity(
      config, RobloxExperiencePresencePhase::kPlaying, &request,
      "Natural Disaster Survival", 1,
      "https://tr.rbxcdn.com/example/512/512/Image/Png");

  EXPECT_EQ(activity.large_image,
            "https://tr.rbxcdn.com/example/512/512/Image/Png");
  EXPECT_EQ(activity.large_text, "Natural Disaster Survival");
}

TEST(DiscordRpcTest, ShowsPlaceJoinWhenServerIdIsUnavailable) {
  const DiscordRpcConfig config;
  RobloxExperienceLaunchRequest request = PublicServer();
  request.game_instance_id.clear();

  const DiscordRpcActivity activity = BuildDiscordRpcActivity(
      config, RobloxExperiencePresencePhase::kPlaying, &request,
      "Natural Disaster Survival", 1);

  EXPECT_EQ(activity.button_label, "Join Server");
  EXPECT_EQ(activity.button_url,
            "https://komaruworld.github.io/mocktail/join.html#placeId="
            "189707");
}

TEST(DiscordRpcTest, RejectsUnsafePlaceThumbnailUrl) {
  const DiscordRpcConfig config;
  const RobloxExperienceLaunchRequest request = PublicServer();
  const DiscordRpcActivity activity = BuildDiscordRpcActivity(
      config, RobloxExperiencePresencePhase::kPlaying, &request,
      "Natural Disaster Survival", 1, "file:///tmp/not-an-image");

  EXPECT_TRUE(activity.large_image.empty());
  EXPECT_TRUE(activity.large_text.empty());
}

TEST(DiscordRpcTest, DoesNotPublishPrivateJoinMaterial) {
  for (const auto private_field : {0, 1, 2}) {
    RobloxExperienceLaunchRequest request = PublicServer();
    if (private_field == 0) request.reserved_server_access_code = "secret";
    if (private_field == 1) request.access_code = "secret";
    if (private_field == 2) request.link_code = "secret";
    EXPECT_FALSE(IsPublicDiscordJoin(request));
    EXPECT_EQ(BuildDiscordJoinUrl(request).find("secret"), std::string::npos);
    const DiscordRpcActivity activity = BuildDiscordRpcActivity(
        DiscordRpcConfig(), RobloxExperiencePresencePhase::kPlaying, &request,
        "Private experience", 1);
    EXPECT_TRUE(activity.button_url.empty());
  }

  DiscordRpcConfig opted_in;
  opted_in.public_servers_only = false;
  RobloxExperienceLaunchRequest private_request = PublicServer();
  private_request.reserved_server_access_code = "never-publish-this";
  const DiscordRpcActivity opted_in_activity =
      BuildDiscordRpcActivity(opted_in, RobloxExperiencePresencePhase::kPlaying,
                              &private_request, "Private experience", 1);
  EXPECT_FALSE(opted_in_activity.button_url.empty());
  EXPECT_EQ(opted_in_activity.button_url.find("never-publish-this"),
            std::string::npos);
}

TEST(DiscordRpcTest, HidesPlaceNameAndElapsedTimeByPolicy) {
  DiscordRpcConfig config;
  config.show_place_name = false;
  config.show_elapsed_time = false;
  config.join_enabled = false;

  const RobloxExperienceLaunchRequest request = PublicServer();
  const DiscordRpcActivity activity =
      BuildDiscordRpcActivity(config, RobloxExperiencePresencePhase::kPlaying,
                              &request, "Secret place name", 1770000000);

  EXPECT_EQ(activity.details, "Playing Roblox");
  EXPECT_TRUE(activity.state.empty());
  EXPECT_FALSE(activity.start_timestamp.has_value());
  EXPECT_TRUE(activity.button_url.empty());
}

TEST(DiscordRpcTest, ExpandsCustomPlaceTemplateAndUsesFallback) {
  DiscordRpcConfig config;
  config.text.playing = "Playing {place_name} with Mocktail";
  config.text.unknown_place = "Unknown world";

  const RobloxExperienceLaunchRequest request = PublicServer();
  EXPECT_EQ(
      BuildDiscordRpcActivity(config, RobloxExperiencePresencePhase::kPlaying,
                              &request, "Natural Disaster Survival", 1)
          .details,
      "Playing Natural Disaster Survival with Mocktail");
  EXPECT_EQ(
      BuildDiscordRpcActivity(config, RobloxExperiencePresencePhase::kPlaying,
                              &request, {}, 1)
          .details,
      "Playing Unknown world with Mocktail");
}

TEST(DiscordRpcTest, PublishesLifecycleActivitiesOverDiscordIpc) {
  char directory_pattern[] = "/tmp/mocktail_discord_rpc_XXXXXX";
  char* directory = mkdtemp(directory_pattern);
  ASSERT_NE(directory, nullptr);
  const std::filesystem::path socket_path =
      std::filesystem::path(directory) / "discord-ipc-0";

  const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(listener, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  ASSERT_LT(socket_path.string().size(), sizeof(address.sun_path));
  std::memcpy(address.sun_path, socket_path.c_str(),
              socket_path.string().size() + 1);
  ASSERT_EQ(bind(listener, reinterpret_cast<const sockaddr*>(&address),
                 sizeof(address)),
            0);
  ASSERT_EQ(listen(listener, 1), 0);

  const char* previous_runtime = std::getenv("XDG_RUNTIME_DIR");
  const std::optional<std::string> saved_runtime =
      previous_runtime != nullptr ? std::optional<std::string>(previous_runtime)
                                  : std::nullopt;
  ASSERT_EQ(setenv("XDG_RUNTIME_DIR", directory, 1), 0);

  std::mutex mutex;
  std::condition_variable condition;
  std::string handshake_payload;
  std::vector<std::string> activity_payloads;
  std::thread server([&]() {
    const int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (client >= 0) {
      timeval timeout{4, 0};
      (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout));
      uint32_t opcode = 0;
      if (ReadTestFrame(client, &opcode, &handshake_payload) && opcode == 0 &&
          WriteTestFrame(client, 1,
                         R"({"cmd":"DISPATCH","data":{},"evt":"READY"})")) {
        for (int index = 0; index < 3; ++index) {
          std::string activity_payload;
          if (!ReadTestFrame(client, &opcode, &activity_payload) ||
              opcode != 1) {
            break;
          }
          std::lock_guard<std::mutex> lock(mutex);
          activity_payloads.push_back(std::move(activity_payload));
          condition.notify_all();
        }
        std::string clear_payload;
        (void)ReadTestFrame(client, &opcode, &clear_payload);
      }
      (void)close(client);
    }
  });

  DiscordRpcConfig config;
  config.enabled = true;
  config.application_id = "123456789012345678";
  config.show_place_name = false;
  config.join_enabled = false;
  DiscordRpcSession session(std::move(config));
  std::string detail;
  ASSERT_TRUE(session.Start(&detail)) << detail;
  {
    std::unique_lock<std::mutex> lock(mutex);
    EXPECT_TRUE(condition.wait_for(lock, std::chrono::seconds(4),
                                   [&]() {
                                     return activity_payloads.size() >= 1;
                                   }));
  }
  RobloxExperienceLaunchRequest request = PublicServer();
  // This IPC framing test intentionally avoids real Roblox metadata traffic.
  request.place_id = 0;
  const RobloxExperiencePresenceObserver observer = session.observer();
  observer.notify(observer.context, RobloxExperiencePresencePhase::kJoining,
                  &request);
  {
    std::unique_lock<std::mutex> lock(mutex);
    EXPECT_TRUE(condition.wait_for(lock, std::chrono::seconds(4),
                                   [&]() {
                                     return activity_payloads.size() >= 2;
                                   }));
  }
  observer.notify(observer.context, RobloxExperiencePresencePhase::kPlaying,
                  &request);
  {
    std::unique_lock<std::mutex> lock(mutex);
    EXPECT_TRUE(condition.wait_for(lock, std::chrono::seconds(4),
                                   [&]() {
                                     return activity_payloads.size() >= 3;
                                   }));
  }
  session.Stop();
  server.join();

  EXPECT_NE(handshake_payload.find("123456789012345678"), std::string::npos);
  ASSERT_EQ(activity_payloads.size(), 3U);
  EXPECT_NE(activity_payloads[0].find("SET_ACTIVITY"), std::string::npos);
  EXPECT_NE(activity_payloads[0].find("Browsing experiences"),
            std::string::npos);
  EXPECT_NE(activity_payloads[1].find("Joining an experience"),
            std::string::npos);
  EXPECT_NE(activity_payloads[2].find("Playing Roblox"), std::string::npos);
  EXPECT_NE(activity_payloads[2].find("timestamps"), std::string::npos);

  if (saved_runtime.has_value()) {
    EXPECT_EQ(setenv("XDG_RUNTIME_DIR", saved_runtime->c_str(), 1), 0);
  } else {
    EXPECT_EQ(unsetenv("XDG_RUNTIME_DIR"), 0);
  }
  (void)close(listener);
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
