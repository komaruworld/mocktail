#include "runtime/roblox_launch_uri.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "runtime/roblox_experience_launch_bridge.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr char kModernPublicLaunchUri[] =
    "roblox://experiences/start?placeId=17580461965";

void ExpectCanonicalRequestMatches(
    const RobloxExperienceLaunchRequest& request) {
  RobloxExperienceLaunchRequest canonical;
  const Status status =
      ParseRobloxExperienceLaunchJson(request.canonical_json, &canonical);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(canonical.place_id, request.place_id);
  EXPECT_EQ(canonical.user_id, request.user_id);
  EXPECT_EQ(canonical.conversation_id, request.conversation_id);
  EXPECT_EQ(canonical.referred_by_player_id, request.referred_by_player_id);
  EXPECT_EQ(canonical.game_instance_id, request.game_instance_id);
  EXPECT_EQ(canonical.reserved_server_access_code,
            request.reserved_server_access_code);
  EXPECT_EQ(canonical.call_id, request.call_id);
  EXPECT_EQ(canonical.referral_page, request.referral_page);
  EXPECT_EQ(canonical.access_code, request.access_code);
  EXPECT_EQ(canonical.link_code, request.link_code);
  EXPECT_EQ(canonical.launch_data, request.launch_data);
  EXPECT_EQ(canonical.event_id, request.event_id);
  EXPECT_EQ(canonical.game_join_context, request.game_join_context);
  EXPECT_EQ(canonical.join_attempt_id, request.join_attempt_id);
  EXPECT_EQ(canonical.join_attempt_origin, request.join_attempt_origin);
  EXPECT_EQ(canonical.iso_context, request.iso_context);
}

void ExpectRejected(std::string_view uri) {
  RobloxExperienceLaunchRequest request;
  request.place_id = 123;
  request.game_instance_id = "stale-instance";
  request.canonical_json = "stale-canonical-json";

  const Status status = ParseRobloxLaunchUri(uri, &request);

  EXPECT_FALSE(status.ok()) << uri;
  EXPECT_EQ(request.place_id, 0) << uri;
  EXPECT_TRUE(request.game_instance_id.empty()) << uri;
  EXPECT_TRUE(request.canonical_json.empty()) << uri;
}

TEST(RobloxLaunchUriTest, ParsesModernPublicExperienceLaunch) {
  RobloxExperienceLaunchRequest request;

  const Status status = ParseRobloxLaunchUri(kModernPublicLaunchUri, &request);

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(request.place_id, 17580461965);
  EXPECT_TRUE(request.game_instance_id.empty());
  EXPECT_TRUE(request.access_code.empty());
  EXPECT_TRUE(request.link_code.empty());
  ExpectCanonicalRequestMatches(request);
}

TEST(RobloxLaunchUriTest, ParsesOfficialDirectAppLaunch) {
  RobloxExperienceLaunchRequest request;

  const Status status = ParseRobloxLaunchUri(
      "roblox://placeId=17580461965&gameInstanceId=server%2Bjob&"
      "launchData=%7B%22source%22%3A%22share%2520link%22%7D",
      &request);

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(request.place_id, 17580461965);
  EXPECT_EQ(request.game_instance_id, "server+job");
  // The documented direct-app form URL-decodes launchData once. The current
  // website /experiences/start form intentionally has its extra decode layer.
  EXPECT_EQ(request.launch_data, R"({"source":"share%20link"})");
  ExpectCanonicalRequestMatches(request);
}

TEST(RobloxLaunchUriTest, ParsesModernInstanceAndDecodesOnlyLaunchDataTwice) {
  RobloxExperienceLaunchRequest request;
  const Status status = ParseRobloxLaunchUri(
      "roblox://experiences/start?gameInstanceId=instance%252Fencoded&"
      "placeId=1962086868&referredByPlayerId=42&"
      "launchData=%257B%2522source%2522%253A%2522invite%252Bcard%2522%257D",
      &request);

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(request.place_id, 1962086868);
  EXPECT_EQ(request.game_instance_id, "instance%2Fencoded");
  EXPECT_EQ(request.referred_by_player_id, 42);
  EXPECT_EQ(request.launch_data, R"({"source":"invite+card"})");
  ExpectCanonicalRequestMatches(request);
}

TEST(RobloxLaunchUriTest, ParsesModernPrivateServerSelectors) {
  RobloxExperienceLaunchRequest link_request;
  Status status = ParseRobloxLaunchUri(
      "roblox://experiences/start?placeId=17580461965&"
      "linkCode=abc%2Bdef%2Fghi",
      &link_request);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(link_request.place_id, 17580461965);
  EXPECT_EQ(link_request.link_code, "abc+def/ghi");
  EXPECT_TRUE(link_request.access_code.empty());
  ExpectCanonicalRequestMatches(link_request);

  RobloxExperienceLaunchRequest access_request;
  status = ParseRobloxLaunchUri(
      "roblox://experiences/start?placeId=17580461965&"
      "accessCode=reserved%2Bserver%2Fcode",
      &access_request);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(access_request.place_id, 17580461965);
  EXPECT_EQ(access_request.access_code, "reserved+server/code");
  EXPECT_TRUE(access_request.link_code.empty());
  ExpectCanonicalRequestMatches(access_request);
}

TEST(RobloxLaunchUriTest, ParsesClassicWebsiteRequestGameLaunch) {
  constexpr char kAuthenticationTicket[] = "RBX-AUTH-TICKET-DO-NOT-RETAIN";
  constexpr char kUri[] =
      "roblox-player:1+launchmode:play+"
      "gameinfo:RBX-AUTH-TICKET-DO-NOT-RETAIN+launchtime:1780000000000+"
      "placelauncherurl:https%3A%2F%2Fwww.roblox.com%2FGame%2F"
      "PlaceLauncher.ashx%3Frequest%3DRequestGame%26browserTrackerId%3D123%"
      "26placeId%3D17580461965%26isPlayTogetherGame%3Dfalse%26launchData%3D%"
      "257B%2522source%2522%253A%2522website%2522%257D%26eventId%3Dsummer%"
      "26referredByPlayerId%3D42+browsertrackerid:123+robloxLocale:en_us+"
      "gameLocale:en_us+channel:+LaunchExp:InApp";
  RobloxExperienceLaunchRequest request;

  const Status status = ParseRobloxLaunchUri(kUri, &request);

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(request.place_id, 17580461965);
  EXPECT_EQ(request.referred_by_player_id, 42);
  EXPECT_EQ(request.launch_data, R"({"source":"website"})");
  EXPECT_EQ(request.event_id, "summer");
  EXPECT_EQ(request.canonical_json.find(kAuthenticationTicket),
            std::string::npos);
  EXPECT_EQ(request.canonical_json.find("browserTrackerId"), std::string::npos);
  ExpectCanonicalRequestMatches(request);
}

TEST(RobloxLaunchUriTest, ParsesClassicWebsiteRequestGameJobLaunch) {
  constexpr char kUri[] =
      "roblox-player:1+launchmode:play+gameinfo:ignored+launchtime:123+"
      "placelauncherurl:https%3A%2F%2Fwww.roblox.com%2FGame%2F"
      "PlaceLauncher.ashx%3Frequest%3DRequestGameJob%26browserTrackerId%3D7%"
      "26placeId%3D1962086868%26gameId%3D97ea-49e4%26referredByPlayerId%3D9%"
      "26joinAttemptId%3Dattempt%252B1%26joinAttemptOrigin%3DPlayButton";
  RobloxExperienceLaunchRequest request;

  const Status status = ParseRobloxLaunchUri(kUri, &request);

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(request.place_id, 1962086868);
  EXPECT_EQ(request.game_instance_id, "97ea-49e4");
  EXPECT_EQ(request.referred_by_player_id, 9);
  EXPECT_EQ(request.join_attempt_id, "attempt+1");
  EXPECT_EQ(request.join_attempt_origin, "PlayButton");
  ExpectCanonicalRequestMatches(request);
}

TEST(RobloxLaunchUriTest, ParsesClassicWebsiteRequestPrivateGameLaunch) {
  constexpr char kUri[] =
      "roblox-player:1+launchmode:play+gameinfo:ignored+launchtime:123+"
      "placelauncherurl:https%3A%2F%2Fwww.roblox.com%2FGame%2F"
      "PlaceLauncher.ashx%3Frequest%3DRequestPrivateGame%"
      "26browserTrackerId%3D7%26placeId%3D17580461965%"
      "26accessCode%3Dreserved%252Bcode%26linkCode%3Dlink%252Fcode";
  RobloxExperienceLaunchRequest request;

  const Status status = ParseRobloxLaunchUri(kUri, &request);

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(request.place_id, 17580461965);
  EXPECT_EQ(request.access_code, "reserved+code");
  EXPECT_EQ(request.link_code, "link/code");
  ExpectCanonicalRequestMatches(request);
}

TEST(RobloxLaunchUriTest, RejectsInvalidOrAmbiguousModernIdentifiers) {
  for (const char* uri : {
           "roblox://experiences/start",
           "roblox://experiences/start?placeId=",
           "roblox://experiences/start?placeId=0",
           "roblox://experiences/start?placeId=-1",
           "roblox://experiences/start?placeId=1.5",
           "roblox://experiences/start?placeId=18446744073709551615",
           "roblox://experiences/start?placeId=1&placeId=2",
           "roblox://experiences/start?placeId=1&gameInstanceId=instance&"
           "linkCode=private",
           "roblox://experiences/start?placeId=1&gameInstanceId=instance&"
           "accessCode=private",
       }) {
    ExpectRejected(uri);
  }
}

TEST(RobloxLaunchUriTest, RejectsMalformedEncodingAndControlBytes) {
  for (const char* uri : {
           "roblox://experiences/start?placeId=%",
           "roblox://experiences/start?placeId=%GG",
           "roblox://experiences/start?placeId=1%00",
           "roblox://experiences/start?placeId=1%0A",
           "roblox://experiences/start?placeId=1&launchData=%C3%28",
           "roblox://experiences/start?placeId=1&launchData=%C0%AF",
           "roblox://experiences/start?placeId=1\n&gameInstanceId=instance",
           "roblox://experiences/start?placeId=1%26gameInstanceId%3Devil",
           "roblox-player:1+launchmode:play+placelauncherurl:%",
           "roblox-player:1+launchmode:play+placelauncherurl:%GG",
       }) {
    ExpectRejected(uri);
  }
}

TEST(RobloxLaunchUriTest, RejectsUnsupportedSchemesRoutesAndLaunchModes) {
  for (const char* uri : {
           "https://www.roblox.com/experiences/start?placeId=1",
           "file:///experiences/start?placeId=1",
           "roblox://navigation?placeId=1",
           "roblox://experiences/other?placeId=1",
           "roblox://user@experiences/start?placeId=1",
           "roblox://experiences/start?placeId=1#fragment",
           "roblox-player:1+launchmode:edit+placelauncherurl:"
           "https%3A%2F%2Fwww.roblox.com%2FGame%2FPlaceLauncher.ashx%"
           "3Frequest%3DRequestGame%26placeId%3D1",
           "roblox-player:1+launchmode:play+placelauncherurl:"
           "https%3A%2F%2Fwww.roblox.com%2FGame%2FPlaceLauncher.ashx%"
           "3Frequest%3DRequestFollowUser%26userId%3D42",
           "roblox-player:1+launchmode:play+placelauncherurl:"
           "https%3A%2F%2Fwww.roblox.com%2FGame%2FPlaceLauncher.ashx%"
           "3FplaceId%3D1",
       }) {
    ExpectRejected(uri);
  }
}

TEST(RobloxLaunchUriTest, RejectsUntrustedClassicPlaceLauncherUrl) {
  for (const char* nested_url : {
           "http%3A%2F%2Fwww.roblox.com%2FGame%2FPlaceLauncher.ashx%"
           "3Frequest%3DRequestGame%26placeId%3D1",
           "https%3A%2F%2Fevilroblox.com%2FGame%2FPlaceLauncher.ashx%"
           "3Frequest%3DRequestGame%26placeId%3D1",
           "https%3A%2F%2Froblox.com.example.org%2FGame%2F"
           "PlaceLauncher.ashx%3Frequest%3DRequestGame%26placeId%3D1",
           "https%3A%2F%2Fuser%40www.roblox.com%2FGame%2F"
           "PlaceLauncher.ashx%3Frequest%3DRequestGame%26placeId%3D1",
           "https%3A%2F%2Fwww.roblox.com%3A444%2FGame%2F"
           "PlaceLauncher.ashx%3Frequest%3DRequestGame%26placeId%3D1",
           "https%3A%2F%2Fwww.roblox.com%2FGame%2FOther.ashx%"
           "3Frequest%3DRequestGame%26placeId%3D1",
           "https%3A%2F%2Fwww.roblox.com%2FGame%2FPlaceLauncher.ashx%"
           "3Frequest%3DRequestGame%26placeId%3D1%23fragment",
       }) {
    const std::string uri =
        std::string("roblox-player:1+launchmode:play+gameinfo:ignored+") +
        "placelauncherurl:" + nested_url;
    ExpectRejected(uri);
  }
}

TEST(RobloxLaunchUriTest, RejectsDuplicateClassicFieldsAndNestedInjection) {
  ExpectRejected(
      "roblox-player:1+launchmode:play+launchmode:play+"
      "placelauncherurl:https%3A%2F%2Fwww.roblox.com%2FGame%2F"
      "PlaceLauncher.ashx%3Frequest%3DRequestGame%26placeId%3D1");
  ExpectRejected(
      "roblox-player:1+launchmode:play+"
      "placelauncherurl:https%3A%2F%2Fwww.roblox.com%2FGame%2F"
      "PlaceLauncher.ashx%3Frequest%3DRequestGame%26placeId%3D1+"
      "placelauncherurl:https%3A%2F%2Fwww.roblox.com%2FGame%2F"
      "PlaceLauncher.ashx%3Frequest%3DRequestGame%26placeId%3D2");
  ExpectRejected(
      "roblox-player:1+launchmode:play+"
      "placelauncherurl:https%3A%2F%2Fwww.roblox.com%2FGame%2F"
      "PlaceLauncher.ashx%3Frequest%3DRequestGame%26placeId%3D1%"
      "2526gameId%253Devil");
}

TEST(RobloxLaunchUriTest, RejectsUnboundedUriAndDecodedLaunchField) {
  std::string oversized_launch_data =
      "roblox://experiences/start?placeId=1&launchData=";
  oversized_launch_data.append(16 * 1024 + 1, 'x');
  ExpectRejected(oversized_launch_data);

  std::string oversized_uri =
      "roblox://experiences/start?placeId=1&futureField=";
  oversized_uri.append(1024 * 1024, 'x');
  ExpectRejected(oversized_uri);

  std::string excessive_parameters = "roblox://experiences/start?placeId=1";
  for (int index = 0; index < 64; ++index) {
    excessive_parameters += "&future" + std::to_string(index) + "=value";
  }
  ExpectRejected(excessive_parameters);
}

TEST(RobloxLaunchUriTest, ClearsOutputAndRedactsTicketOnFailure) {
  constexpr char kAuthenticationTicket[] = "SECRET-TICKET-MUST-NOT-ESCAPE";
  RobloxExperienceLaunchRequest request;
  request.place_id = 123;
  request.canonical_json = "stale";
  const Status status = ParseRobloxLaunchUri(
      "roblox-player:1+launchmode:edit+"
      "gameinfo:SECRET-TICKET-MUST-NOT-ESCAPE+launchtime:123",
      &request);

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.message().find(kAuthenticationTicket), std::string::npos);
  EXPECT_EQ(request.place_id, 0);
  EXPECT_TRUE(request.canonical_json.empty());
  EXPECT_FALSE(ParseRobloxLaunchUri(kModernPublicLaunchUri, nullptr).ok());
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
