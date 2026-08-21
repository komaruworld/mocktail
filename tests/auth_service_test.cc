#include "services/auth_service.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace mocktail {
namespace services {
namespace {

class FakeHttpClient final : public HttpClient {
 public:
  HttpResponse response;
  HttpRequest last_request;
  int requests = 0;

  HttpResponse Get(const HttpRequest& request) override {
    ++requests;
    last_request = request;
    return response;
  }
};

constexpr char kTestCookieValue[] = "_|test-security-token";
constexpr char kTestCookieHeader[] = ".ROBLOSECURITY=_|test-security-token";
constexpr char kValidIdentityJson[] =
    R"({"id":123456789,"name":"TestUser","displayName":"Test Display"})";

TEST(AuthServiceTest, ExtractsCookieFromSupportedForms) {
  EXPECT_EQ(AuthService::ExtractRoblosecurityValue(
                ".ROBLOSECURITY=_|WARNING:token; Domain=.roblox.com"),
            "_|WARNING:token");
  EXPECT_EQ(AuthService::ExtractRoblosecurityValue("_|raw-token"),
            "_|raw-token");
  EXPECT_EQ(AuthService::ExtractRoblosecurityValue(
                "other=value; .ROBLOSECURITY = account-token; another=x"),
            "account-token");
  EXPECT_TRUE(AuthService::ExtractRoblosecurityValue("other=value").empty());
  EXPECT_TRUE(
      AuthService::ExtractRoblosecurityValue("not.ROBLOSECURITY=lookalike")
          .empty());
  EXPECT_TRUE(
      AuthService::ExtractRoblosecurityValue(".ROBLOSECURITY_SUFFIX=lookalike")
          .empty());
}

TEST(AuthServiceTest, RedactsOnlyExactRejectedSecurityCookieSegments) {
  const std::string cookies =
      "OtherCookie=keep\n"
      ".ROBLOSECURITY=_|test-security-token\n"
      "RBXEventTrackerV2=browserid=123456\n"
      ".ROBLOSECURITY=duplicate-value\n"
      "not.ROBLOSECURITY=keep-lookalike\n"
      ".ROBLOSECURITY_SUFFIX=keep-suffix\n";
  std::string redacted;

  ASSERT_TRUE(AuthService::RedactRejectedRoblosecurity(
      cookies, kTestCookieValue, &redacted));

  const std::string expected =
      "OtherCookie=keep\n" +
      std::string(
          std::string_view(".ROBLOSECURITY=_|test-security-token").size(),
          ' ') +
      "\nRBXEventTrackerV2=browserid=123456\n" +
      std::string(std::string_view(".ROBLOSECURITY=duplicate-value").size(),
                  ' ') +
      "\nnot.ROBLOSECURITY=keep-lookalike\n"
      ".ROBLOSECURITY_SUFFIX=keep-suffix\n";
  EXPECT_EQ(redacted, expected);
  EXPECT_EQ(redacted.size(), cookies.size());
  EXPECT_FALSE(AuthService::HasRoblosecurityCookie(redacted));
}

TEST(AuthServiceTest, RejectsCleanupWhenEffectiveCredentialChanged) {
  std::string redacted = "must be cleared";

  EXPECT_FALSE(AuthService::RedactRejectedRoblosecurity(
      ".ROBLOSECURITY=_|new-token\nOtherCookie=keep\n", kTestCookieValue,
      &redacted));

  EXPECT_TRUE(redacted.empty());
}

TEST(AuthServiceTest, RedactsRawRejectedCredentialAndNamedDuplicates) {
  const std::string cookies =
      "_|test-security-token\r\nOtherCookie=keep;"
      ".ROBLOSECURITY=duplicate-value\tTail=keep";
  std::string redacted;

  ASSERT_TRUE(AuthService::RedactRejectedRoblosecurity(
      cookies, kTestCookieValue, &redacted));

  const std::string expected =
      std::string(std::string_view("_|test-security-token").size(), ' ') +
      "\r\nOtherCookie=keep;" +
      std::string(std::string_view(".ROBLOSECURITY=duplicate-value").size(),
                  ' ') +
      "\tTail=keep";
  EXPECT_EQ(redacted, expected);
  EXPECT_EQ(redacted.size(), cookies.size());
}

TEST(AuthServiceTest, ResolvesTypedIdentityWithSafeRequest) {
  FakeHttpClient http;
  http.response = {true, 200, kValidIdentityJson, {}};
  AuthService auth(http);

  const AuthSession session = auth.ResolveSession(
      std::string(kTestCookieHeader) + "\r\nInjected: header");

  EXPECT_EQ(session.status, AuthSessionStatus::kAuthenticated);
  EXPECT_EQ(session.identity.user_id, 123456789);
  EXPECT_EQ(session.identity.username, "TestUser");
  EXPECT_EQ(session.identity.display_name, "Test Display");
  EXPECT_EQ(session.http_status, 200);
  EXPECT_TRUE(session.error.empty());
  ASSERT_EQ(http.requests, 1);
  EXPECT_EQ(http.last_request.url,
            "https://users.roblox.com/v1/users/authenticated");
  EXPECT_EQ(http.last_request.timeout_ms, 15000);
  EXPECT_EQ(http.last_request.maximum_body_bytes, 1024U * 1024U);
  EXPECT_FALSE(http.last_request.follow_redirects);
  ASSERT_EQ(http.last_request.headers.size(), 2U);
  EXPECT_EQ(http.last_request.headers[0], "Accept: application/json");
  EXPECT_EQ(http.last_request.headers[1],
            std::string("Cookie: .ROBLOSECURITY=") + kTestCookieValue);
  EXPECT_EQ(http.last_request.url.find(kTestCookieValue), std::string::npos);
  EXPECT_EQ(http.last_request.headers[1].find("Injected"), std::string::npos);
}

TEST(AuthServiceTest, AcceptsLargestSignedUserId) {
  FakeHttpClient http;
  http.response = {
      true,
      200,
      R"({"id":9223372036854775807,"name":"Maximum","displayName":"Max"})",
      {}};
  AuthService auth(http);

  const AuthSession session = auth.ResolveSession(kTestCookieHeader);

  EXPECT_EQ(session.status, AuthSessionStatus::kAuthenticated);
  EXPECT_EQ(session.identity.user_id, std::numeric_limits<int64_t>::max());
}

TEST(AuthServiceTest, RejectsMalformedOrWrongIdentitySchema) {
  const std::vector<std::string> invalid_bodies = {
      "",
      "not-json",
      "[]",
      "{}",
      R"({"id":1,"name":"User"})",
      R"({"id":1,"displayName":"Display"})",
      R"({"name":"User","displayName":"Display"})",
      R"({"id":"1","name":"User","displayName":"Display"})",
      R"({"id":true,"name":"User","displayName":"Display"})",
      R"({"id":null,"name":"User","displayName":"Display"})",
      R"({"id":1,"name":"","displayName":"Display"})",
      R"({"id":1,"name":7,"displayName":"Display"})",
      R"({"id":1,"name":"User","displayName":7})",
      R"({"id":1,"name":"User","displayName":"Display"} trailing)",
  };

  for (const std::string& body : invalid_bodies) {
    SCOPED_TRACE(body);
    FakeHttpClient http;
    http.response = {true, 200, body, {}};
    AuthService auth(http);

    const AuthSession session = auth.ResolveSession(kTestCookieHeader);

    EXPECT_EQ(session.status, AuthSessionStatus::kUnavailable);
    EXPECT_EQ(session.identity.user_id, -1);
    EXPECT_TRUE(session.identity.username.empty());
    EXPECT_TRUE(session.identity.display_name.empty());
    EXPECT_EQ(session.http_status, 200);
    EXPECT_EQ(session.error, "invalid authenticated-user response");
  }
}

TEST(AuthServiceTest, RejectsNonPositiveFractionalAndOverflowingUserIds) {
  const std::vector<std::string> invalid_ids = {
      "0",
      "-1",
      "1.0",
      "9223372036854775808",
      "18446744073709551615",
      "18446744073709551616",
      "-9223372036854775809",
  };

  for (const std::string& id : invalid_ids) {
    SCOPED_TRACE(id);
    FakeHttpClient http;
    http.response = {
        true,
        200,
        "{\"id\":" + id + ",\"name\":\"User\",\"displayName\":\"Display\"}",
        {}};
    AuthService auth(http);

    EXPECT_EQ(auth.ResolveSession(kTestCookieHeader).status,
              AuthSessionStatus::kUnavailable);
  }
}

TEST(AuthServiceTest, MapsRejectedAndUnavailableStatuses) {
  FakeHttpClient http;
  AuthService auth(http);

  for (const long status : {401L, 403L}) {
    SCOPED_TRACE(status);
    http.response = {true, status, "", {}};
    const AuthSession session = auth.ResolveSession(kTestCookieHeader);
    EXPECT_EQ(session.status, AuthSessionStatus::kInvalid);
    EXPECT_EQ(session.http_status, status);
    EXPECT_EQ(session.error, "authentication rejected");
  }

  for (const long status : {302L, 404L, 500L, 503L}) {
    SCOPED_TRACE(status);
    http.response = {true, status, "", {}};
    const AuthSession session = auth.ResolveSession(kTestCookieHeader);
    EXPECT_EQ(session.status, AuthSessionStatus::kUnavailable);
    EXPECT_EQ(session.http_status, status);
    EXPECT_EQ(session.error, "unexpected authentication HTTP status");
  }

  http.response = {false, 0, "", "network unavailable"};
  const AuthSession unavailable = auth.ResolveSession(kTestCookieHeader);
  EXPECT_EQ(unavailable.status, AuthSessionStatus::kUnavailable);
  EXPECT_EQ(unavailable.http_status, 0);
  EXPECT_EQ(unavailable.error, "authentication service unavailable");
}

TEST(AuthServiceTest, MissingCookieRequiresExplicitGuestPolicy) {
  FakeHttpClient http;
  AuthService auth(http);

  const AuthSession invalid = auth.ResolveSession("other=value", false);
  EXPECT_EQ(invalid.status, AuthSessionStatus::kInvalid);
  EXPECT_EQ(invalid.identity.user_id, -1);
  EXPECT_FALSE(invalid.error.empty());

  const AuthSession guest = auth.ResolveSession("other=value", true);
  EXPECT_EQ(guest.status, AuthSessionStatus::kGuest);
  EXPECT_EQ(guest.identity.user_id, -1);
  EXPECT_TRUE(guest.error.empty());
  EXPECT_EQ(http.requests, 0);
}

TEST(AuthServiceTest, UnsafeCookieIsInvalidEvenWhenGuestsAreAllowed) {
  FakeHttpClient http;
  AuthService auth(http);

  const AuthSession session =
      auth.ResolveSession(".ROBLOSECURITY=unsafe cookie", true);

  EXPECT_EQ(session.status, AuthSessionStatus::kInvalid);
  EXPECT_EQ(session.error, "invalid .ROBLOSECURITY cookie");
  EXPECT_EQ(http.requests, 0);
}

TEST(AuthServiceTest, EmptyCookieIsNotMistakenForMissingGuestCredentials) {
  FakeHttpClient http;
  AuthService auth(http);

  for (const std::string& header : {".ROBLOSECURITY=", ".ROBLOSECURITY"}) {
    SCOPED_TRACE(header);
    const AuthSession session = auth.ResolveSession(header, true);
    EXPECT_EQ(session.status, AuthSessionStatus::kInvalid);
    EXPECT_EQ(session.error, "invalid .ROBLOSECURITY cookie");
  }
  EXPECT_EQ(http.requests, 0);
}

TEST(AuthServiceTest, NeverCopiesCredentialsIntoErrors) {
  FakeHttpClient http;
  const std::string credential = "distinct-secret-credential";
  http.response = {false, 0, "", "failed request with " + credential};
  AuthService auth(http);

  const AuthSession session =
      auth.ResolveSession(".ROBLOSECURITY=" + credential);
  const AuthCookieValidation validation =
      auth.ValidateCookie(".ROBLOSECURITY=" + credential);

  EXPECT_EQ(session.error.find(credential), std::string::npos);
  EXPECT_EQ(validation.error.find(credential), std::string::npos);
  EXPECT_EQ(session.identity.username.find(credential), std::string::npos);
  EXPECT_EQ(session.identity.display_name.find(credential), std::string::npos);
}

TEST(AuthServiceTest, ValidateCookieRemainsACompatibilityProjection) {
  FakeHttpClient http;
  AuthService auth(http);

  http.response = {true, 200, kValidIdentityJson, {}};
  const AuthCookieValidation valid = auth.ValidateCookie(kTestCookieHeader);
  EXPECT_EQ(valid.status, AuthCookieStatus::kValid);
  EXPECT_EQ(valid.http_status, 200);

  http.response = {true, 401, "", {}};
  EXPECT_EQ(auth.ValidateCookie(kTestCookieHeader).status,
            AuthCookieStatus::kInvalid);

  // A successful HTTP status without a valid identity fails closed.
  http.response = {true, 200, "{}", {}};
  EXPECT_EQ(auth.ValidateCookie(kTestCookieHeader).status,
            AuthCookieStatus::kUnavailable);

  EXPECT_EQ(auth.ValidateCookie("other=value").status,
            AuthCookieStatus::kInvalid);
}

}  // namespace
}  // namespace services
}  // namespace mocktail
