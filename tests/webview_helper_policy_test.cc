#include "webview/webview_helper_policy.h"

#include <gtest/gtest.h>
#include <jsc/jsc.h>

#include <cstdlib>
#include <string>
#include <string_view>

namespace mocktail {
namespace webview {
namespace {

class ScopedEnvironment final {
 public:
  ScopedEnvironment(const char* name, const char* value) : name_(name) {
    const char* current = std::getenv(name);
    if (current != nullptr) {
      had_value_ = true;
      old_value_ = current;
    }
    setenv(name, value, 1);
  }

  ~ScopedEnvironment() {
    if (had_value_) {
      setenv(name_.c_str(), old_value_.c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

 private:
  std::string name_;
  std::string old_value_;
  bool had_value_ = false;
};

class JavaScriptContext final {
 public:
  JavaScriptContext() : context_(jsc_context_new()) {}
  ~JavaScriptContext() { g_object_unref(context_); }

  JavaScriptContext(const JavaScriptContext&) = delete;
  JavaScriptContext& operator=(const JavaScriptContext&) = delete;

  JSCContext* get() const { return context_; }

 private:
  JSCContext* context_ = nullptr;
};

JSCValue* Evaluate(JSCContext* context, std::string_view script) {
  return jsc_context_evaluate(context, script.data(),
                              static_cast<gssize>(script.size()));
}

bool EvaluateBoolean(JSCContext* context, std::string_view script,
                     bool* result) {
  JSCValue* value = Evaluate(context, script);
  if (value == nullptr || !jsc_value_is_boolean(value)) {
    if (value != nullptr) {
      g_object_unref(value);
    }
    return false;
  }
  *result = jsc_value_to_boolean(value);
  g_object_unref(value);
  return true;
}

bool EvaluateString(JSCContext* context, std::string_view script,
                    std::string* result) {
  JSCValue* value = Evaluate(context, script);
  if (value == nullptr || !jsc_value_is_string(value)) {
    if (value != nullptr) {
      g_object_unref(value);
    }
    return false;
  }
  char* text = jsc_value_to_string(value);
  *result = text == nullptr ? std::string() : std::string(text);
  g_free(text);
  g_object_unref(value);
  return true;
}

void EvaluateForSideEffects(JSCContext* context, std::string_view script) {
  JSCValue* value = Evaluate(context, script);
  ASSERT_NE(value, nullptr);
  g_object_unref(value);
}

TEST(WebViewHelperPolicyTest, AllowsOnlyTlsTopLevelNavigation) {
  const UriPolicyResult roblox =
      EvaluateNavigationUri("https://www.roblox.com/login");
  EXPECT_TRUE(roblox.allowed);
  EXPECT_TRUE(roblox.privileged_bridge_allowed);
  const UriPolicyResult captcha =
      EvaluateNavigationUri("HTTPS://client-api.arkoselabs.com:443/");
  EXPECT_TRUE(captcha.allowed);
  EXPECT_FALSE(captcha.privileged_bridge_allowed);
  EXPECT_TRUE(EvaluateNavigationUri("about:blank").allowed);
  EXPECT_FALSE(EvaluateNavigationUri("about:blank").privileged_bridge_allowed);
  EXPECT_TRUE(EvaluateNavigationUri("https://apis.roblox.com/")
                  .privileged_bridge_allowed);
  EXPECT_FALSE(EvaluateNavigationUri("https://evilroblox.com/")
                   .privileged_bridge_allowed);

  for (const char* rejected : {
           "http://www.roblox.com/login",
           "file:///etc/passwd",
           "data:text/html,test",
           "javascript:alert(1)",
           "https://user@www.roblox.com/login",
           "https://www.roblox.com:444/login",
           "about:blank#fragment",
           "https:///missing-host",
       }) {
    EXPECT_FALSE(EvaluateNavigationUri(rejected).allowed) << rejected;
  }
  EXPECT_FALSE(EvaluateNavigationUri(nullptr).allowed);
}

TEST(WebViewHelperPolicyTest, DiagnosticsNeverRetainPathOrQuery) {
  const UriPolicyResult result = EvaluateNavigationUri(
      "https://www.roblox.com/login?state=SECRET_CALLBACK_TOKEN");
  EXPECT_TRUE(result.allowed);
  EXPECT_EQ(result.scheme, "https");
  EXPECT_EQ(result.host, "www.roblox.com");
  EXPECT_EQ(result.host.find("SECRET_CALLBACK_TOKEN"), std::string::npos);
}

TEST(WebViewHelperPolicyTest, ParsesOnlyTypedCaptchaNavigationEvents) {
  CaptchaEvent event;
  EXPECT_TRUE(ParseCaptchaEvent(
      R"({"moduleID":"Navigation","functionName":"navigateToFeature","params":{"params":{"feature":"CaptchaShown"}},"callbackID":"shown-1"})",
      &event));
  EXPECT_EQ(event.type, CaptchaEventType::kShown);
  EXPECT_EQ(event.callback_id, "shown-1");

  EXPECT_TRUE(ParseCaptchaEvent(
      R"({"moduleID":"Navigation","functionName":"navigateToFeature","params":{"params":{"feature":"CaptchaSuccess"}}})",
      &event));
  EXPECT_EQ(event.type, CaptchaEventType::kSuccess);
  EXPECT_TRUE(event.callback_id.empty());

  for (
      std::string_view rejected : {
          R"({"moduleID":"Browser","functionName":"navigateToFeature","params":{"params":{"feature":"CaptchaSuccess"}}})",
          R"({"moduleID":"Navigation","functionName":"openUserProfile","params":{"params":{"feature":"CaptchaSuccess"}}})",
          R"({"moduleID":"Navigation","functionName":"navigateToFeature","params":{"params":{"feature":"Settings"}}})",
          R"({"moduleID":"Navigation","functionName":"navigateToFeature","params":{"feature":"CaptchaSuccess"}})",
          R"({not-json})",
      }) {
    EXPECT_FALSE(ParseCaptchaEvent(rejected, &event)) << rejected;
  }
}

TEST(WebViewHelperPolicyTest, AcceptsBothSoberHybridMessageShapes) {
  JavaScriptContext context;
  JSCValue* execute_roblox = Evaluate(context.get(), R"JS(({
    moduleID: "Navigation",
    functionName: "navigateToFeature",
    params: {params: {feature: "CaptchaShown"}},
    callbackID: "execute-callback"
  }))JS");
  ASSERT_NE(execute_roblox, nullptr);
  std::string command;
  EXPECT_TRUE(ExtractExecuteRobloxCommand(execute_roblox, &command));
  g_object_unref(execute_roblox);
  CaptchaEvent event;
  EXPECT_TRUE(ParseCaptchaEvent(command, &event));
  EXPECT_EQ(event.type, CaptchaEventType::kShown);
  EXPECT_EQ(event.callback_id, "execute-callback");

  JSCValue* wk_hybrid = Evaluate(context.get(), R"JS(({
    command: JSON.stringify({
      moduleID: "Navigation",
      functionName: "navigateToFeature",
      params: {params: {feature: "CaptchaSuccess"}},
      callbackID: "wk-callback"
    })
  }))JS");
  ASSERT_NE(wk_hybrid, nullptr);
  command.clear();
  EXPECT_TRUE(ExtractRobloxWkHybridCommand(wk_hybrid, &command));
  g_object_unref(wk_hybrid);
  EXPECT_TRUE(ParseCaptchaEvent(command, &event));
  EXPECT_EQ(event.type, CaptchaEventType::kSuccess);
  EXPECT_EQ(event.callback_id, "wk-callback");

  JSCValue* oversized =
      Evaluate(context.get(), "({command: 'a'.repeat(65537)})");
  ASSERT_NE(oversized, nullptr);
  EXPECT_FALSE(ExtractRobloxWkHybridCommand(oversized, &command));
  g_object_unref(oversized);
}

TEST(WebViewHelperPolicyTest, InstallsExactAndroidBridgeAtDocumentStart) {
  JavaScriptContext context;
  EvaluateForSideEffects(context.get(), R"JS(
    var captured = null;
    var window = {webkit: {messageHandlers: {executeRoblox: {
      postMessage: (value) => { captured = value; }
    }}}};
  )JS");
  EvaluateForSideEffects(context.get(), AndroidBridgeSource());
  EvaluateForSideEffects(
      context.get(),
      R"JS(window.__globalRobloxAndroidBridge__.executeRoblox(JSON.stringify({ok: true}));)JS");

  std::string captured;
  EXPECT_TRUE(
      EvaluateString(context.get(), "JSON.stringify(captured)", &captured));
  EXPECT_EQ(captured, R"({"ok":true})");
  bool exact_name_and_immutable = false;
  EXPECT_TRUE(EvaluateBoolean(
      context.get(),
      R"JS(typeof window.__globalRobloxAndroidBridge__.executeRoblox === "function" && Object.getOwnPropertyDescriptor(window, "__globalRobloxAndroidBridge__").writable === false)JS",
      &exact_name_and_immutable));
  EXPECT_TRUE(exact_name_and_immutable);
}

TEST(WebViewHelperPolicyTest, CallbackScriptEscapesIdAndConfirmsDelivery) {
  JavaScriptContext context;
  EvaluateForSideEffects(context.get(), R"JS(
    var compromised = false;
    var callbackArgs = null;
    var window = {Roblox: {Hybrid: {Bridge: {
      nativeCallback: (id, success, payload) => {
        callbackArgs = [id, success, payload];
      }
    }}}};
  )JS");
  const std::string callback_id =
      "callback-'); globalThis.compromised = true; //";
  const std::string callback_script = BuildCallbackScript(callback_id);

  bool confirmed = false;
  EXPECT_TRUE(EvaluateBoolean(context.get(), callback_script, &confirmed));
  EXPECT_TRUE(confirmed);
  std::string received_id;
  EXPECT_TRUE(EvaluateString(context.get(), "callbackArgs[0]", &received_id));
  EXPECT_EQ(received_id, callback_id);
  bool arguments_are_exact = false;
  EXPECT_TRUE(EvaluateBoolean(
      context.get(),
      "callbackArgs[1] === true && Object.keys(callbackArgs[2]).length === 0 "
      "&& compromised === false",
      &arguments_are_exact));
  EXPECT_TRUE(arguments_are_exact);
}

TEST(WebViewHelperPolicyTest, CallbackRequiresNativeCallbackFunction) {
  JavaScriptContext context;
  EvaluateForSideEffects(context.get(),
                         "var window = {Roblox: {Hybrid: {Bridge: {}}}};");
  bool confirmed = true;
  EXPECT_TRUE(
      EvaluateBoolean(context.get(), BuildCallbackScript("id"), &confirmed));
  EXPECT_FALSE(confirmed);
}

TEST(WebViewHelperPolicyTest, UserAgentTracksValidatedPayloadVersion) {
  ScopedEnvironment valid_version("MOCKTAIL_ROBLOX_VERSION", "2.999.42");
  const std::string user_agent = BuildRobloxAndroidUserAgent();
  EXPECT_EQ(
      user_agent,
      "Mozilla/5.0 AppleWebKit/605.1.15 (KHTML, like Gecko)  ROBLOX Android "
      "App 2.999.42 Tablet Hybrid()  GooglePlayStore RobloxApp/2.999.42"
      "(GlobalDist; GooglePlayStore)");
}

}  // namespace
}  // namespace webview
}  // namespace mocktail
