#include "runtime/webview_roblox_cookie.h"

#include <gtest/gtest.h>

#include <type_traits>

namespace mocktail::runtime {
namespace {

static_assert(!std::is_copy_constructible_v<SecureWebViewRobloxCookie>);
static_assert(!std::is_copy_assignable_v<SecureWebViewRobloxCookie>);
static_assert(std::is_nothrow_move_constructible_v<SecureWebViewRobloxCookie>);

TEST(WebViewRobloxCookieTest, DerivesValueWithoutHeaderOrAttributes) {
  SecureRobloxCredential credential(".ROBLOSECURITY=_|signed-token");

  WebViewRobloxCookieResult result = PrepareWebViewRobloxCookie(credential);

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.cookie.value(), "_|signed-token");
  EXPECT_EQ(kWebViewRobloxCookieName, ".ROBLOSECURITY");
  EXPECT_EQ(kWebViewRobloxCookieDomain, ".roblox.com");
  EXPECT_EQ(kWebViewRobloxCookiePath, "/");
  EXPECT_EQ(credential.view(), ".ROBLOSECURITY=_|signed-token");
}

TEST(WebViewRobloxCookieTest, GuestCredentialProducesNoCookie) {
  SecureRobloxCredential credential;

  WebViewRobloxCookieResult result = PrepareWebViewRobloxCookie(credential);

  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.cookie.empty());
}

TEST(WebViewRobloxCookieTest, RejectsNonCanonicalAndUnsafeValues) {
  SecureRobloxCredential wrong_name("OTHER=value");
  SecureRobloxCredential empty_value(".ROBLOSECURITY=");
  SecureRobloxCredential injected(".ROBLOSECURITY=value; Domain=evil.test");

  EXPECT_FALSE(PrepareWebViewRobloxCookie(wrong_name));
  EXPECT_FALSE(PrepareWebViewRobloxCookie(empty_value));
  EXPECT_FALSE(PrepareWebViewRobloxCookie(injected));
}

TEST(WebViewRobloxCookieTest, MoveTransfersOwnershipAndClearIsExplicit) {
  SecureRobloxCredential credential(".ROBLOSECURITY=secret");
  WebViewRobloxCookieResult result = PrepareWebViewRobloxCookie(credential);
  ASSERT_TRUE(result);

  SecureWebViewRobloxCookie moved = std::move(result.cookie);
  EXPECT_EQ(moved.value(), "secret");
  moved.Clear();
  EXPECT_TRUE(moved.empty());
}

TEST(WebViewRobloxCookieTest, EnforcesBoundedTransportValue) {
  std::string header = ".ROBLOSECURITY=";
  header.append(kMaximumWebViewRobloxCookieValueBytes + 1, 'a');
  SecureRobloxCredential credential(std::move(header));

  EXPECT_FALSE(PrepareWebViewRobloxCookie(credential));
}

}  // namespace
}  // namespace mocktail::runtime
