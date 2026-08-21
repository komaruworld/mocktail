#include "runtime/game_mode.h"

#include <gtest/gtest.h>

#include <utility>

namespace mocktail {
namespace runtime {
namespace {

struct ClientProbe {
  int query_result = 0;
  int start_result = 0;
  int end_result = 0;
  int query_calls = 0;
  int start_calls = 0;
  int end_calls = 0;
  const char* error = "fixture failure";
};

ClientProbe* g_probe = nullptr;

int Query() {
  ++g_probe->query_calls;
  return g_probe->query_result;
}

int Start() {
  ++g_probe->start_calls;
  return g_probe->start_result;
}

int End() {
  ++g_probe->end_calls;
  return g_probe->end_result;
}

const char* Error() { return g_probe->error; }

GameModeClientApi Api() {
  return GameModeClientApi{&Start, &End, &Query, &Error};
}

TEST(GameModePolicyTest, ParsesCanonicalValuesAndBooleanAliases) {
  GameModePolicy policy = GameModePolicy::kOff;
  EXPECT_TRUE(ParseGameModePolicy("auto", &policy));
  EXPECT_EQ(policy, GameModePolicy::kAuto);
  EXPECT_TRUE(ParseGameModePolicy("on", &policy));
  EXPECT_EQ(policy, GameModePolicy::kOn);
  EXPECT_TRUE(ParseGameModePolicy("true", &policy));
  EXPECT_EQ(policy, GameModePolicy::kOn);
  EXPECT_TRUE(ParseGameModePolicy("off", &policy));
  EXPECT_EQ(policy, GameModePolicy::kOff);
  EXPECT_TRUE(ParseGameModePolicy("0", &policy));
  EXPECT_EQ(policy, GameModePolicy::kOff);
  EXPECT_FALSE(ParseGameModePolicy("required", &policy));
  EXPECT_FALSE(ParseGameModePolicy("auto", nullptr));
}

TEST(GameModeSessionTest, DisabledPolicyMakesNoClientCalls) {
  ClientProbe probe;
  g_probe = &probe;
  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kOff, Api());

  EXPECT_EQ(session.state(), GameModeSessionState::kDisabled);
  EXPECT_TRUE(session.Stop().ok());
  EXPECT_EQ(probe.query_calls, 0);
  EXPECT_EQ(probe.start_calls, 0);
  EXPECT_EQ(probe.end_calls, 0);
}

TEST(GameModeSessionTest, OwnsExactlyOneSuccessfulRequest) {
  ClientProbe probe;
  g_probe = &probe;
  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api());

  EXPECT_EQ(session.state(), GameModeSessionState::kActive);
  EXPECT_TRUE(session.owns_request());
  EXPECT_EQ(probe.query_calls, 1);
  EXPECT_EQ(probe.start_calls, 1);
  EXPECT_TRUE(session.Stop().ok());
  EXPECT_EQ(session.state(), GameModeSessionState::kStopped);
  EXPECT_EQ(probe.end_calls, 1);
  EXPECT_TRUE(session.Stop().ok());
  EXPECT_EQ(probe.end_calls, 1);
}

TEST(GameModeSessionTest, DestructorReleasesOwnedRequestOnEarlyReturn) {
  ClientProbe probe;
  g_probe = &probe;
  {
    GameModeSession session = GameModeSession::StartWithClientForTesting(
        GameModePolicy::kAuto, Api());
    ASSERT_TRUE(session.owns_request());
    EXPECT_EQ(probe.end_calls, 0);
  }
  EXPECT_EQ(probe.end_calls, 1);
}

TEST(GameModeSessionTest, AlreadyRegisteredProcessIsNotRequestedAgain) {
  ClientProbe probe;
  probe.query_result = 2;
  g_probe = &probe;
  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api());

  EXPECT_EQ(session.state(), GameModeSessionState::kAlreadyActive);
  EXPECT_TRUE(session.active());
  EXPECT_FALSE(session.owns_request());
  EXPECT_EQ(probe.query_calls, 1);
  EXPECT_EQ(probe.start_calls, 0);
  EXPECT_TRUE(session.Stop().ok());
  EXPECT_EQ(probe.end_calls, 0);
}

TEST(GameModeSessionTest, FailedQueryStillAttemptsTheOptimization) {
  ClientProbe probe;
  probe.query_result = -1;
  g_probe = &probe;
  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kOn, Api());

  EXPECT_EQ(session.state(), GameModeSessionState::kActive);
  EXPECT_EQ(probe.start_calls, 1);
  EXPECT_TRUE(session.Stop().ok());
  EXPECT_EQ(probe.end_calls, 1);
}

TEST(GameModeSessionTest, RejectedRequestIsFailOpenAndNeverEnded) {
  ClientProbe probe;
  probe.start_result = -1;
  probe.error = "daemon rejected fixture";
  g_probe = &probe;
  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kOn, Api());

  EXPECT_EQ(session.state(), GameModeSessionState::kRequestFailed);
  EXPECT_EQ(session.detail(), "daemon rejected fixture");
  EXPECT_FALSE(session.active());
  EXPECT_TRUE(session.Stop().ok());
  EXPECT_EQ(probe.end_calls, 0);
}

TEST(GameModeSessionTest, MoveTransfersTheOwnedRequest) {
  ClientProbe probe;
  g_probe = &probe;
  GameModeSession first = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api());
  GameModeSession second = std::move(first);

  EXPECT_FALSE(first.owns_request());
  EXPECT_TRUE(second.owns_request());
  EXPECT_TRUE(second.Stop().ok());
  EXPECT_EQ(probe.end_calls, 1);
}

TEST(GameModeSessionTest, StopFailureIsReportedOnlyOnce) {
  ClientProbe probe;
  probe.end_result = -1;
  probe.error = "daemon disappeared";
  g_probe = &probe;
  GameModeSession session = GameModeSession::StartWithClientForTesting(
      GameModePolicy::kAuto, Api());

  const Status stopped = session.Stop();
  EXPECT_FALSE(stopped.ok());
  EXPECT_EQ(stopped.message(), "daemon disappeared");
  EXPECT_EQ(session.state(), GameModeSessionState::kStopFailed);
  EXPECT_EQ(probe.end_calls, 1);
  EXPECT_TRUE(session.Stop().ok());
  EXPECT_EQ(probe.end_calls, 1);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
