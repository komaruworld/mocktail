#include "runtime/roblox_web_view_bridge.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "jnivm/jnivm.h"

namespace mocktail {
namespace runtime {
namespace {

struct WebViewBridgeProbe {
  std::vector<jobject> subscribed_callbacks;
  std::vector<jobject> cleared_callbacks;
  std::vector<jlong> deleted_handles;
  int dispatches = 0;
  int mutation_dispatches = 0;
  int close_dispatches = 0;
  int cookie_dispatches = 0;
  int synchronous_subscribe_index = -1;
  int fail_subscribe_index = -1;
  std::string synchronous_payload;
  std::vector<std::string> subscription_ids;
  std::vector<std::string> message_id_protocols;
  std::vector<std::string> message_id_methods;
  std::string availability_protocol;
  std::string availability_method;
  std::string published_message_id;
  std::string published_payload;
  std::string hybrid_callback_payload;
  std::string cookie_header;
  std::vector<std::string> data_model_focus_states;
  std::vector<std::string> initialization_order;
  int native_initializations = 0;
  int close_publications = 0;
  int cookie_handler_registrations = 0;
  int cookie_handler_clears = 0;
  jobject cookie_set_handler = nullptr;
  WebViewHelperExitObserver exit_observer;
  RobloxWebViewOpenRequest request;
  RobloxWebViewMutationRequest mutation;
};

struct JavaVmAcquireProbe {
  JavaVM* delegate = nullptr;
  int get_env_calls = 0;
  int successful_get_env_calls = 1;
};

WebViewBridgeProbe *g_web_view_probe = nullptr;
JavaVmAcquireProbe* g_java_vm_acquire_probe = nullptr;

jint JNICALL ControlledGetEnv(JavaVM*, void** env, jint version) {
  if (g_java_vm_acquire_probe == nullptr ||
      g_java_vm_acquire_probe->delegate == nullptr) {
    if (env != nullptr) {
      *env = nullptr;
    }
    return JNI_ERR;
  }
  ++g_java_vm_acquire_probe->get_env_calls;
  if (g_java_vm_acquire_probe->get_env_calls >
      g_java_vm_acquire_probe->successful_get_env_calls) {
    if (env != nullptr) {
      *env = nullptr;
    }
    return JNI_ERR;
  }
  return g_java_vm_acquire_probe->delegate->GetEnv(env, version);
}

std::string ReadJavaString(JNIEnv *env, jstring value) {
  if (env == nullptr || value == nullptr) {
    return {};
  }
  const char *chars = env->GetStringUTFChars(value, nullptr);
  if (chars == nullptr) {
    return {};
  }
  const std::string result(chars);
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

jstring GetOpenWindowId(JNIEnv *env, jclass) {
  return env->NewStringUTF("openWindow");
}

jstring GetMutateWindowId(JNIEnv* env, jclass) {
  return env->NewStringUTF("mutateWindow");
}

jstring GetCloseWindowId(JNIEnv* env, jclass) {
  return env->NewStringUTF("closeWindow");
}

jstring GetHandleWindowCloseId(JNIEnv *env, jclass) {
  return env->NewStringUTF("handleWindowClose");
}

jstring GetProtocolName(JNIEnv *env, jclass) {
  return env->NewStringUTF("WebView");
}

jstring GetIsAvailableId(JNIEnv *env, jclass) {
  return env->NewStringUTF("isAvailable");
}

jstring GetMessageId(JNIEnv *env, jclass, jstring protocol_name,
                     jstring method_name) {
  const char *protocol = env->GetStringUTFChars(protocol_name, nullptr);
  const char *method = env->GetStringUTFChars(method_name, nullptr);
  const std::string protocol_value = protocol != nullptr ? protocol : "";
  const std::string method_value = method != nullptr ? method : "";
  if (method_value != "handleWindowClose") {
    g_web_view_probe->message_id_protocols.push_back(protocol_value);
    g_web_view_probe->message_id_methods.push_back(method_value);
    g_web_view_probe->initialization_order.push_back("message_id:" +
                                                     method_value);
  }
  if (protocol != nullptr) {
    env->ReleaseStringUTFChars(protocol_name, protocol);
  }
  if (method != nullptr) {
    env->ReleaseStringUTFChars(method_name, method);
  }
  return env->NewStringUTF((protocol_value + "." + method_value).c_str());
}

void InitializeAndroidWebViewProtocol(JNIEnv *, jclass) {
  ++g_web_view_probe->native_initializations;
  g_web_view_probe->initialization_order.push_back("initialize");
}

void SignalJavascriptCallback(JNIEnv* env, jclass, jstring payload) {
  g_web_view_probe->hybrid_callback_payload = ReadJavaString(env, payload);
}

void UpdateCookieSetHandler(JNIEnv*, jobject, jobject handler) {
  if (handler == nullptr) {
    ++g_web_view_probe->cookie_handler_clears;
    g_web_view_probe->cookie_set_handler = nullptr;
    return;
  }
  ++g_web_view_probe->cookie_handler_registrations;
  g_web_view_probe->cookie_set_handler = handler;
}

void SetRequestHandler(JNIEnv *env, jobject, jstring protocol_name,
                       jstring method_name, jobject) {
  const char *protocol = env->GetStringUTFChars(protocol_name, nullptr);
  const char *method = env->GetStringUTFChars(method_name, nullptr);
  g_web_view_probe->availability_protocol = protocol != nullptr ? protocol : "";
  g_web_view_probe->availability_method = method != nullptr ? method : "";
  if (protocol != nullptr) {
    env->ReleaseStringUTFChars(protocol_name, protocol);
  }
  if (method != nullptr) {
    env->ReleaseStringUTFChars(method_name, method);
  }
  g_web_view_probe->initialization_order.push_back("availability");
}
void ClearRequestHandler(JNIEnv *, jobject, jstring, jstring) {}

void PublishRaw(JNIEnv *env, jobject, jstring message_id, jstring payload) {
  g_web_view_probe->published_message_id =
      message_id != nullptr ? ReadJavaString(env, message_id) : "";
  g_web_view_probe->published_payload =
      payload != nullptr ? ReadJavaString(env, payload) : "";
  ++g_web_view_probe->close_publications;
}

void BroadcastDataModelFocus(JNIEnv *env, jclass, jstring event_namespace,
                             jstring event_detail, jstring state) {
  EXPECT_EQ(ReadJavaString(env, event_namespace), "AppInput");
  EXPECT_EQ(ReadJavaString(env, event_detail), "");
  g_web_view_probe->data_model_focus_states.push_back(
      ReadJavaString(env, state));
}

jobject SubscribeRaw(JNIEnv *env, jobject, jstring subscription_id,
                     jobject callback, jboolean) {
  const int subscribe_index =
      static_cast<int>(g_web_view_probe->subscription_ids.size());
  const char *id = env->GetStringUTFChars(subscription_id, nullptr);
  g_web_view_probe->subscription_ids.push_back(id != nullptr ? id : "");
  if (id != nullptr) {
    env->ReleaseStringUTFChars(subscription_id, id);
  }
  g_web_view_probe->subscribed_callbacks.push_back(callback);
  g_web_view_probe->initialization_order.push_back("subscribe");
  if (g_web_view_probe->synchronous_subscribe_index == subscribe_index ||
      g_web_view_probe->synchronous_subscribe_index == -2) {
    jclass callback_class = env->GetObjectClass(callback);
    jmethodID run =
        env->GetMethodID(callback_class, "run", "(Ljava/lang/String;)V");
    std::string payload = g_web_view_probe->synchronous_payload;
    if (payload.empty()) {
      if (subscribe_index == 0) {
        payload = R"({"url":"https://www.roblox.com/games/1/servers"})";
      } else if (subscribe_index == 1) {
        payload = R"({"title":"Synchronous mutation","isVisible":false})";
      } else {
        payload = "payload intentionally ignored";
      }
    }
    jstring json = env->NewStringUTF(payload.c_str());
    env->CallVoidMethod(callback, run, json);
    env->DeleteLocalRef(json);
    env->DeleteLocalRef(callback_class);
  }
  if (g_web_view_probe->fail_subscribe_index == subscribe_index) {
    return nullptr;
  }
  jclass connection =
      env->FindClass("com/roblox/universalapp/messagebus/Connection");
  jmethodID constructor = env->GetMethodID(connection, "<init>", "(J)V");
  return env->NewObject(connection, constructor,
                        static_cast<jlong>(91 + subscribe_index));
}

void DeleteConnection(JNIEnv *, jobject, jlong handle) {
  g_web_view_probe->deleted_handles.push_back(handle);
}

jobject CreateRawCallback(void *context, std::shared_ptr<void> callback_context,
                          void (*run)(void *, JNIEnv *, jstring)) {
  auto *vm = static_cast<jnivm::VM *>(context);
  return vm->CreateMessageBusRawCallback(std::move(callback_context),
                                         jnivm::MessageBusRawCallbacks{run});
}

void ClearRawCallback(void *context, jobject callback) {
  g_web_view_probe->cleared_callbacks.push_back(callback);
  static_cast<jnivm::VM *>(context)->ClearMessageBusRawCallback(callback);
}

jobject CreateRequestHandler(void *context,
                             std::shared_ptr<void> callback_context,
                             std::string (*run)(void *, JNIEnv *, jstring)) {
  return static_cast<jnivm::VM *>(context)->CreateMessageBusRequestHandler(
      std::move(callback_context),
      jnivm::MessageBusRequestHandlerCallbacks{run});
}

void ClearRequestHandlerObject(void *context, jobject handler) {
  static_cast<jnivm::VM *>(context)->ClearMessageBusRequestHandler(handler);
}

bool SetPlatformWebCallbacks(
    void* context, std::shared_ptr<void> callback_context,
    void (*on_data_model_notification)(void*, JNIEnv*, jstring, jstring),
    void (*on_app_bridge_notification)(void*, JNIEnv*, jstring, jstring),
    void (*on_native_overlay)(void*, JNIEnv*, jstring, jstring),
    void (*on_open_web_activity)(void*, JNIEnv*, jstring, jstring),
    void (*on_sync_cookies)(void*, JNIEnv*, jstring),
    void (*on_set_cookie)(void*, JNIEnv*, jstring, jstring)) {
  auto *vm = static_cast<jnivm::VM *>(context);
  if (vm == nullptr || callback_context == nullptr ||
      on_data_model_notification == nullptr ||
      on_app_bridge_notification == nullptr || on_native_overlay == nullptr ||
      on_open_web_activity == nullptr || on_sync_cookies == nullptr ||
      on_set_cookie == nullptr) {
    return false;
  }
  vm->SetRobloxDataModelNotificationCallbacks(
      std::move(callback_context),
      jnivm::RobloxDataModelNotificationCallbacks{
          on_data_model_notification, on_app_bridge_notification,
          on_native_overlay, on_open_web_activity, on_sync_cookies,
          on_set_cookie});
  return true;
}

void ClearPlatformWebCallbacks(void* context) {
  static_cast<jnivm::VM*>(context)->ClearRobloxDataModelNotificationCallbacks();
}

Status DispatchOpen(void *context, const RobloxWebViewOpenRequest &request,
                    WebViewHelperExitObserver exit_observer) {
  auto *probe = static_cast<WebViewBridgeProbe *>(context);
  ++probe->dispatches;
  probe->request = request;
  probe->exit_observer = std::move(exit_observer);
  return Status::Ok();
}

Status DispatchMutate(void* context,
                      const RobloxWebViewMutationRequest& request) {
  auto* probe = static_cast<WebViewBridgeProbe*>(context);
  ++probe->mutation_dispatches;
  probe->mutation = request;
  return Status::Ok();
}

Status DispatchClose(void* context) {
  ++static_cast<WebViewBridgeProbe*>(context)->close_dispatches;
  return Status::Ok();
}

Status DispatchCookie(void* context, std::string_view header) {
  auto* probe = static_cast<WebViewBridgeProbe*>(context);
  ++probe->cookie_dispatches;
  probe->cookie_header.assign(header);
  return Status::Ok();
}

RobloxWebViewBridge MakeBridge(jnivm::VM* vm, jobject bus,
                               WebViewBridgeProbe* probe) {
  return RobloxWebViewBridge(
      {vm->GetJavaVM(), nullptr, nullptr},
      {&GetOpenWindowId, &GetHandleWindowCloseId, &GetProtocolName,
       &GetIsAvailableId, &GetMessageId, &InitializeAndroidWebViewProtocol,
       &SubscribeRaw, &DeleteConnection, &SetRequestHandler,
       &ClearRequestHandler, &PublishRaw, &BroadcastDataModelFocus,
       &GetMutateWindowId, &GetCloseWindowId, &SignalJavascriptCallback,
       &UpdateCookieSetHandler},
      {bus, vm, &CreateRawCallback, &ClearRawCallback, &CreateRequestHandler,
       &ClearRequestHandlerObject, &SetPlatformWebCallbacks,
       &ClearPlatformWebCallbacks},
      {probe, &DispatchOpen, &DispatchMutate, &DispatchClose, &DispatchCookie});
}

TEST(RobloxWebViewParserTest, PreservesCompleteTypedOpenPayload) {
  RobloxWebViewOpenRequest request;
  const Status status = ParseRobloxWebViewOpenJson(
      R"({"url":"https://www.roblox.com/games/1/servers-section/2",)"
      R"("title":"Servers","searchParams":{"searchType":"games"},)"
      R"("isVisible":false,"windowType":"modal",)"
      R"("showDomainAsTitle":true,"backButtonVisible":false,)"
      R"("hideHeader":true,"future":{"nested":true}})",
      &request);

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(request.url, "https://www.roblox.com/games/1/servers-section/2");
  EXPECT_EQ(request.title, "Servers");
  ASSERT_TRUE(request.search_type.has_value());
  EXPECT_EQ(*request.search_type, "games");
  ASSERT_TRUE(request.is_visible.has_value());
  EXPECT_FALSE(*request.is_visible);
  ASSERT_TRUE(request.window_type.has_value());
  EXPECT_EQ(*request.window_type, "modal");
  ASSERT_TRUE(request.show_domain_as_title.has_value());
  EXPECT_TRUE(*request.show_domain_as_title);
  ASSERT_TRUE(request.back_button_visible.has_value());
  EXPECT_FALSE(*request.back_button_visible);
  ASSERT_TRUE(request.hide_header.has_value());
  EXPECT_TRUE(*request.hide_header);
}

TEST(RobloxWebViewParserTest, PreservesAbsentOptionalOpenFields) {
  RobloxWebViewOpenRequest request;
  ASSERT_TRUE(ParseRobloxWebViewOpenJson(
                  R"({"url":"https://www.roblox.com/login"})", &request)
                  .ok());
  EXPECT_FALSE(request.search_type.has_value());
  EXPECT_FALSE(request.is_visible.has_value());
  EXPECT_FALSE(request.window_type.has_value());
  EXPECT_FALSE(request.show_domain_as_title.has_value());
  EXPECT_FALSE(request.back_button_visible.has_value());
  EXPECT_FALSE(request.hide_header.has_value());
}

TEST(RobloxWebViewParserTest,
     ParsesTypedPartialMutationAndPreservesFieldAbsence) {
  RobloxWebViewMutationRequest request;
  Status status = ParseRobloxWebViewMutationJson(
      R"({"url":"https://www.roblox.com/discover","title":"Discover",)"
      R"("searchParams":{"searchType":"games"},"isVisible":false,)"
      R"("windowType":"modal","showDomainAsTitle":true,"future":1})",
      &request);
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_TRUE(request.url.has_value());
  EXPECT_EQ(*request.url, "https://www.roblox.com/discover");
  ASSERT_TRUE(request.title.has_value());
  EXPECT_EQ(*request.title, "Discover");
  ASSERT_TRUE(request.search_type.has_value());
  EXPECT_EQ(*request.search_type, "games");
  ASSERT_TRUE(request.is_visible.has_value());
  EXPECT_FALSE(*request.is_visible);
  ASSERT_TRUE(request.window_type.has_value());
  EXPECT_EQ(*request.window_type, "modal");
  ASSERT_TRUE(request.show_domain_as_title.has_value());
  EXPECT_TRUE(*request.show_domain_as_title);

  status = ParseRobloxWebViewMutationJson(R"({"title":"","searchParams":{}})",
                                          &request);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_FALSE(request.url.has_value());
  ASSERT_TRUE(request.title.has_value());
  EXPECT_TRUE(request.title->empty());
  EXPECT_FALSE(request.search_type.has_value());
  EXPECT_FALSE(request.is_visible.has_value());
  EXPECT_FALSE(request.window_type.has_value());
  EXPECT_FALSE(request.show_domain_as_title.has_value());
}

TEST(RobloxWebViewParserTest, RejectsInvalidMutationFieldTypesAndBounds) {
  RobloxWebViewMutationRequest request;
  EXPECT_FALSE(ParseRobloxWebViewMutationJson("", &request).ok());
  EXPECT_FALSE(ParseRobloxWebViewMutationJson("[]", &request).ok());
  EXPECT_FALSE(
      ParseRobloxWebViewMutationJson(R"({"isVisible":"false"})", &request)
          .ok());
  EXPECT_FALSE(
      ParseRobloxWebViewMutationJson(R"({"searchParams":"games"})", &request)
          .ok());
  EXPECT_FALSE(ParseRobloxWebViewMutationJson(
                   R"({"searchParams":{"searchType":false}})", &request)
                   .ok());
  EXPECT_FALSE(ParseRobloxWebViewMutationJson(
                   std::string("{\"url\":\"") +
                       std::string(kMaximumRobloxWebViewUrlBytes + 1, 'x') +
                       "\"}",
                   &request)
                   .ok());
}

TEST(RobloxWebViewParserTest, BuildsExactCaptchaRoutesWithoutGuessingSuccess) {
  RobloxWebViewOpenRequest request;
  Status status = ParseRobloxCaptchaOpenJson(
      R"({"captchaType":"login","cvalueType":"phone_number","cvalue":"+1 555"})",
      &request);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(request.title, "Roblox verification");
  EXPECT_EQ(request.url,
            "https://www.roblox.com/captcha/app/login?credentialsType="
            "phonenumber&credentialsValue=%2B1%20555&hybrid-return-token=1");

  status = ParseRobloxCaptchaOpenJson(R"({"captchaType":"signup"})", &request);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(request.url,
            "https://www.roblox.com/captcha/app/signup?hybrid-return-token=1");

  EXPECT_FALSE(
      ParseRobloxCaptchaOpenJson(
          R"({"captchaType":"login","cvalueType":"unknown","cvalue":"x"})",
          &request)
          .ok());
  EXPECT_FALSE(
      ParseRobloxCaptchaOpenJson(
          R"({"captchaType":"login","cvalueType":"username"})", &request)
          .ok());
}

TEST(RobloxWebViewParserTest, AcceptsOnlyCanonicalProfileRoutes) {
  RobloxWebViewOpenRequest request;
  Status status = ParseRobloxProfileViewUrl(
      "https://www.roblox.com/users/4902309364/profile", &request);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(request.url, "https://www.roblox.com/users/4902309364/profile");
  EXPECT_EQ(request.title, "Profile");

  status = ParseRobloxProfileViewUrl(
      "https://www.roblox.com/users/4902309364/profile/", &request);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_FALSE(ParseRobloxProfileViewUrl(
                   "http://www.roblox.com/users/4902309364/profile", &request)
                   .ok());
  EXPECT_FALSE(ParseRobloxProfileViewUrl(
                   "https://roblox.com/users/4902309364/profile", &request)
                   .ok());
  EXPECT_FALSE(ParseRobloxProfileViewUrl(
                   "https://www.roblox.com/users/0/profile", &request)
                   .ok());
  EXPECT_FALSE(ParseRobloxProfileViewUrl(
                   "https://www.roblox.com/users/not-a-user/profile", &request)
                   .ok());
  EXPECT_FALSE(ParseRobloxProfileViewUrl(
                   "https://www.roblox.com/users/1/profile?from=home", &request)
                   .ok());
  EXPECT_FALSE(
      ParseRobloxProfileViewUrl(
          "https://www.roblox.com@evil.example/users/1/profile", &request)
          .ok());
  EXPECT_FALSE(
      ParseRobloxProfileViewUrl(
          std::string("https://www.roblox.com/users/1/profile\n"), &request)
          .ok());
}

TEST(RobloxWebViewParserTest, BuildsCanonicalRobuxRoute) {
  RobloxWebViewOpenRequest request;
  Status status =
      ParseRobloxPurchaseRobuxNotification(R"({"animated":true})", &request);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(request.url,
            "https://www.roblox.com/upgrades/robux?showHeader=true");
  EXPECT_EQ(request.title, "Robux");
  status = ParseRobloxPurchaseRobuxNotification(
      R"({"animated":false,"url":"https://evil.example/steal"})", &request);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(request.url,
            "https://www.roblox.com/upgrades/robux?showHeader=true");
  EXPECT_TRUE(ParseRobloxPurchaseRobuxNotification(
                  R"({"animated":"future-value"})", &request)
                  .ok());
  EXPECT_TRUE(ParseRobloxPurchaseRobuxNotification("not-json", &request).ok());
  EXPECT_TRUE(ParseRobloxPurchaseRobuxNotification("", &request).ok());
  EXPECT_FALSE(
      ParseRobloxPurchaseRobuxNotification(
          std::string(kMaximumRobloxWebViewJsonBytes + 1, 'x'), &request)
          .ok());
}

TEST(RobloxWebViewParserTest, RejectsMalformedMissingAndUnboundedPayloads) {
  RobloxWebViewOpenRequest request;
  EXPECT_FALSE(ParseRobloxWebViewOpenJson("{", &request).ok());
  EXPECT_FALSE(ParseRobloxWebViewOpenJson("[]", &request).ok());
  EXPECT_FALSE(ParseRobloxWebViewOpenJson("{}", &request).ok());
  EXPECT_FALSE(ParseRobloxWebViewOpenJson(R"({"url":12})", &request).ok());
  EXPECT_FALSE(ParseRobloxWebViewOpenJson(R"({"url":""})", &request).ok());
  EXPECT_FALSE(
      ParseRobloxWebViewOpenJson(
          std::string(kMaximumRobloxWebViewJsonBytes + 1, 'x'), &request)
          .ok());
  EXPECT_FALSE(ParseRobloxWebViewOpenJson(
                   std::string("{\"url\":\"") +
                       std::string(kMaximumRobloxWebViewUrlBytes + 1, 'x') +
                       "\"}",
                   &request)
                   .ok());
}

TEST(RobloxWebViewBridgeTest,
     SubscribesDispatchesAndDeletesExactNativeConnection) {
  jnivm::VM vm;
  JNIEnv *env = vm.GetJNIEnv();
  jclass bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  jobject bus = env->AllocObject(bus_class);
  WebViewBridgeProbe probe;
  g_web_view_probe = &probe;
  RobloxWebViewBridge bridge = MakeBridge(&vm, bus, &probe);

  ASSERT_TRUE(bridge.Initialize().ok());
  EXPECT_EQ(probe.cookie_handler_registrations, 1);
  ASSERT_NE(probe.cookie_set_handler, nullptr);
  const jobject registered_cookie_set_handler = probe.cookie_set_handler;
  ASSERT_TRUE(vm.DispatchRobloxCookieSync(
      env, env->NewStringUTF(".ROBLOSECURITY=_|typed-cookie")));
  EXPECT_EQ(probe.cookie_dispatches, 1);
  EXPECT_EQ(probe.cookie_header, ".ROBLOSECURITY=_|typed-cookie");
  jclass cookie_handler_class =
      env->GetObjectClass(registered_cookie_set_handler);
  jmethodID on_set_cookie = env->GetMethodID(
      cookie_handler_class, "onSetCookie",
      "([Ljava/lang/String;Ljava/lang/String;)V");
  jclass string_class = env->FindClass("java/lang/String");
  jobjectArray cookies = env->NewObjectArray(1, string_class, nullptr);
  env->SetObjectArrayElement(
      cookies, 0,
      env->NewStringUTF(
          ".ROBLOSECURITY=_|challenge-cookie; Domain=.roblox.com; Path=/"));
  env->CallVoidMethod(registered_cookie_set_handler, on_set_cookie, cookies,
                      env->NewStringUTF("https://www.roblox.com/"));
  EXPECT_EQ(probe.cookie_dispatches, 2);
  EXPECT_EQ(probe.cookie_header, ".ROBLOSECURITY=_|challenge-cookie");
  EXPECT_EQ(probe.message_id_protocols,
            (std::vector<std::string>{"WebView", "WebView", "WebView"}));
  EXPECT_EQ(
      probe.message_id_methods,
      (std::vector<std::string>{"openWindow", "mutateWindow", "closeWindow"}));
  EXPECT_EQ(
      probe.subscription_ids,
      (std::vector<std::string>{"WebView.openWindow", "WebView.mutateWindow",
                                "WebView.closeWindow"}));
  EXPECT_EQ(probe.availability_protocol, "WebView");
  EXPECT_EQ(probe.availability_method, "isAvailable");
  EXPECT_EQ(probe.native_initializations, 1);
  EXPECT_EQ(probe.initialization_order,
            (std::vector<std::string>{
                "message_id:openWindow", "message_id:mutateWindow",
                "message_id:closeWindow", "availability", "subscribe",
                "subscribe", "subscribe", "initialize"}));
  ASSERT_EQ(probe.subscribed_callbacks.size(), 3u);
  jclass callback_class = env->GetObjectClass(probe.subscribed_callbacks[0]);
  jmethodID run =
      env->GetMethodID(callback_class, "run", "(Ljava/lang/String;)V");

  jstring mutation_json = env->NewStringUTF(
      R"({"title":"Servers","searchParams":{"searchType":"places"},"isVisible":false,"showDomainAsTitle":true})");
  env->CallVoidMethod(probe.subscribed_callbacks[1], run, mutation_json);
  ASSERT_EQ(probe.mutation_dispatches, 1);
  ASSERT_TRUE(probe.mutation.title.has_value());
  EXPECT_EQ(*probe.mutation.title, "Servers");
  ASSERT_TRUE(probe.mutation.search_type.has_value());
  EXPECT_EQ(*probe.mutation.search_type, "places");
  ASSERT_TRUE(probe.mutation.is_visible.has_value());
  EXPECT_FALSE(*probe.mutation.is_visible);
  ASSERT_TRUE(probe.mutation.show_domain_as_title.has_value());
  EXPECT_TRUE(*probe.mutation.show_domain_as_title);

  jstring ignored_close_payload = env->NewStringUTF("not-json");
  env->CallVoidMethod(probe.subscribed_callbacks[2], run,
                      ignored_close_payload);
  EXPECT_EQ(probe.close_dispatches, 1);

  jstring json =
      env->NewStringUTF(R"({"url":"https://www.roblox.com/games/1/servers"})");
  env->CallVoidMethod(probe.subscribed_callbacks[0], run, json);
  ASSERT_EQ(probe.dispatches, 1);
  EXPECT_EQ(probe.request.url, "https://www.roblox.com/games/1/servers");
  ASSERT_TRUE(probe.exit_observer.valid());
  probe.exit_observer.on_exit(probe.exit_observer.context.get());
  EXPECT_EQ(probe.close_publications, 0);
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.close_publications, 1);
  EXPECT_EQ(probe.published_message_id, "WebView.handleWindowClose");
  EXPECT_EQ(probe.published_payload, "{}");
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.close_publications, 1);

  jstring unrelated_type = env->NewStringUTF("APP_READY");
  jstring unrelated_data = env->NewStringUTF(R"({"ready":true})");
  EXPECT_TRUE(vm.DispatchRobloxDataModelNotification(env, unrelated_type,
                                                     unrelated_data));
  EXPECT_EQ(probe.dispatches, 1);
  jstring custom_web_view_type =
      env->NewStringUTF(kRobloxOpenCustomWebViewNotification);
  jstring custom_web_view_data = env->NewStringUTF(
      R"({"animated":true,"title":"Servers","url":"https://www.roblox.com/games/servers-section/10148749921","transitionAnimation":"slideInFromRight"})");
  EXPECT_TRUE(vm.DispatchRobloxDataModelNotification(env, custom_web_view_type,
                                                     custom_web_view_data));
  ASSERT_EQ(probe.dispatches, 2);
  EXPECT_EQ(probe.request.url,
            "https://www.roblox.com/games/servers-section/10148749921");
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.data_model_focus_states,
            (std::vector<std::string>{"Unfocused"}));
  const WebViewHelperExitObserver data_model_exit_observer =
      probe.exit_observer;
  ASSERT_TRUE(data_model_exit_observer.valid());
  data_model_exit_observer.on_exit(data_model_exit_observer.context.get());
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.data_model_focus_states,
            (std::vector<std::string>{"Unfocused", "Focused"}));
  EXPECT_EQ(probe.close_publications, 1);
  const WebViewHelperExitObserver stale_observer = probe.exit_observer;

  jstring captcha_notification = env->NewStringUTF(
      R"({"captchaType":"login","cvalueType":"username","cvalue":"test user"})");
  EXPECT_TRUE(vm.DispatchRobloxAppBridgeNotification(
      env, env->NewStringUTF(kRobloxOpenCaptchaViewNotification),
      captcha_notification));
  ASSERT_EQ(probe.dispatches, 3);
  EXPECT_EQ(probe.request.url,
            "https://www.roblox.com/captcha/app/login?credentialsType="
            "username&credentialsValue=test%20user&hybrid-return-token=1");
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.data_model_focus_states,
            (std::vector<std::string>{"Unfocused", "Focused", "Unfocused"}));
  probe.exit_observer.on_exit(probe.exit_observer.context.get());
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.data_model_focus_states,
            (std::vector<std::string>{"Unfocused", "Focused", "Unfocused",
                                      "Focused"}));

  EXPECT_TRUE(vm.DispatchRobloxNativeOverlay(
      env, env->NewStringUTF("Account"),
      env->NewStringUTF("https://www.roblox.com/login")));
  ASSERT_EQ(probe.dispatches, 4);
  EXPECT_EQ(probe.request.title, "Account");
  EXPECT_EQ(probe.request.url, "https://www.roblox.com/login");
  const int close_publications_before_overlay = probe.close_publications;
  probe.exit_observer.on_exit(probe.exit_observer.context.get());
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.close_publications, close_publications_before_overlay);

  ASSERT_TRUE(bridge.Shutdown().ok());
  EXPECT_EQ(probe.cookie_handler_clears, 1);
  EXPECT_EQ(probe.cookie_set_handler, nullptr);
  EXPECT_EQ(probe.deleted_handles, (std::vector<jlong>{91, 92, 93}));
  EXPECT_EQ(probe.cleared_callbacks.size(), 3u);
  env->CallVoidMethod(probe.subscribed_callbacks[0], run, json);
  env->CallVoidMethod(probe.subscribed_callbacks[1], run, mutation_json);
  env->CallVoidMethod(probe.subscribed_callbacks[2], run,
                      ignored_close_payload);
  EXPECT_FALSE(vm.DispatchRobloxDataModelNotification(env, custom_web_view_type,
                                                      custom_web_view_data));
  env->CallVoidMethod(registered_cookie_set_handler, on_set_cookie, cookies,
                      env->NewStringUTF("https://www.roblox.com/"));
  ASSERT_TRUE(stale_observer.valid());
  stale_observer.on_exit(stale_observer.context.get());
  EXPECT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.close_publications, 1);
  EXPECT_EQ(probe.dispatches, 4);
  EXPECT_EQ(probe.mutation_dispatches, 1);
  EXPECT_EQ(probe.close_dispatches, 1);
  EXPECT_EQ(probe.cookie_dispatches, 2);
  g_web_view_probe = nullptr;
}

TEST(RobloxWebViewBridgeTest, MainGameActivityOpensThroughHostSink) {
  jnivm::VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  jclass bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  jobject bus = env->AllocObject(bus_class);
  WebViewBridgeProbe probe;
  g_web_view_probe = &probe;
  RobloxWebViewBridge bridge = MakeBridge(&vm, bus, &probe);
  ASSERT_TRUE(bridge.Initialize().ok());

  jclass activity_class =
      env->FindClass("com/roblox/client/startup/MainGameActivity");
  jobject activity = env->AllocObject(activity_class);
  jmethodID open = env->GetMethodID(activity_class, "openWebActivity",
                                    "(Ljava/lang/String;Ljava/lang/String;)V");
  ASSERT_NE(open, nullptr);
  jvalue args[2] = {};
  args[0].l = env->NewStringUTF("https://www.roblox.com/login");
  args[1].l = env->NewStringUTF("Log in");
  env->CallVoidMethodA(activity, open, args);

  EXPECT_EQ(probe.dispatches, 1);
  EXPECT_EQ(probe.request.url, "https://www.roblox.com/login");
  EXPECT_EQ(probe.request.title, "Log in");
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.data_model_focus_states,
            (std::vector<std::string>{"Unfocused"}));
  ASSERT_TRUE(probe.exit_observer.valid());
  probe.exit_observer.on_exit(probe.exit_observer.context.get());
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.data_model_focus_states,
            (std::vector<std::string>{"Unfocused", "Focused"}));
  EXPECT_TRUE(bridge.Shutdown().ok());
}

TEST(RobloxWebViewBridgeTest,
     RoutesProfileAndRobuxDataModelNotificationsThroughHostWebView) {
  jnivm::VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  jclass bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  jobject bus = env->AllocObject(bus_class);
  WebViewBridgeProbe probe;
  g_web_view_probe = &probe;
  RobloxWebViewBridge bridge = MakeBridge(&vm, bus, &probe);
  ASSERT_TRUE(bridge.Initialize().ok());

  EXPECT_TRUE(vm.DispatchRobloxDataModelNotification(
      env, env->NewStringUTF(kRobloxViewProfileNotification),
      env->NewStringUTF("https://www.roblox.com/users/4902309364/profile")));
  ASSERT_EQ(probe.dispatches, 1);
  EXPECT_EQ(probe.request.url,
            "https://www.roblox.com/users/4902309364/profile");
  EXPECT_EQ(probe.request.title, "Profile");
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.data_model_focus_states,
            (std::vector<std::string>{"Unfocused"}));
  ASSERT_TRUE(probe.exit_observer.valid());
  probe.exit_observer.on_exit(probe.exit_observer.context.get());
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.data_model_focus_states,
            (std::vector<std::string>{"Unfocused", "Focused"}));

  EXPECT_TRUE(vm.DispatchRobloxDataModelNotification(
      env, env->NewStringUTF(kRobloxPurchaseRobuxNotification),
      env->NewStringUTF(R"({"animated":true})")));
  ASSERT_EQ(probe.dispatches, 2);
  EXPECT_EQ(probe.request.url,
            "https://www.roblox.com/upgrades/robux?showHeader=true");
  EXPECT_EQ(probe.request.title, "Robux");
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.data_model_focus_states,
            (std::vector<std::string>{"Unfocused", "Focused", "Unfocused"}));
  const WebViewHelperExitObserver robux_exit_observer = probe.exit_observer;
  ASSERT_TRUE(robux_exit_observer.valid());
  robux_exit_observer.on_exit(robux_exit_observer.context.get());
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.data_model_focus_states,
            (std::vector<std::string>{"Unfocused", "Focused", "Unfocused",
                                      "Focused"}));
  robux_exit_observer.on_exit(robux_exit_observer.context.get());
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.data_model_focus_states,
            (std::vector<std::string>{"Unfocused", "Focused", "Unfocused",
                                      "Focused"}));

  EXPECT_TRUE(vm.DispatchRobloxAppBridgeNotification(
      env, env->NewStringUTF(kRobloxViewProfileNotification),
      env->NewStringUTF("https://www.roblox.com/users/4902309364/profile")));
  EXPECT_EQ(probe.dispatches, 2);
  EXPECT_FALSE(bridge
                   .HandleDataModelNotification(
                       env, env->NewStringUTF(kRobloxViewProfileNotification),
                       env->NewStringUTF("javascript:alert(1)"))
                   .ok());

  ASSERT_TRUE(bridge.Shutdown().ok());
  g_web_view_probe = nullptr;
}

TEST(RobloxWebViewBridgeTest,
     PreservesFocusOrderWhenAClosedSurfaceIsReopenedBeforeDrain) {
  jnivm::VM vm;
  JNIEnv *env = vm.GetJNIEnv();
  jclass bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  jobject bus = env->AllocObject(bus_class);
  WebViewBridgeProbe probe;
  g_web_view_probe = &probe;
  RobloxWebViewBridge bridge = MakeBridge(&vm, bus, &probe);
  ASSERT_TRUE(bridge.Initialize().ok());

  EXPECT_TRUE(vm.DispatchRobloxDataModelNotification(
      env, env->NewStringUTF(kRobloxViewProfileNotification),
      env->NewStringUTF("https://www.roblox.com/users/1/profile")));
  const WebViewHelperExitObserver first_exit = probe.exit_observer;
  ASSERT_TRUE(first_exit.valid());
  first_exit.on_exit(first_exit.context.get());
  EXPECT_TRUE(vm.DispatchRobloxDataModelNotification(
      env, env->NewStringUTF(kRobloxViewProfileNotification),
      env->NewStringUTF("https://www.roblox.com/users/2/profile")));
  const WebViewHelperExitObserver second_exit = probe.exit_observer;
  ASSERT_TRUE(second_exit.valid());

  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.data_model_focus_states,
            (std::vector<std::string>{"Unfocused", "Focused", "Unfocused"}));
  first_exit.on_exit(first_exit.context.get());
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.data_model_focus_states,
            (std::vector<std::string>{"Unfocused", "Focused", "Unfocused"}));
  second_exit.on_exit(second_exit.context.get());
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.data_model_focus_states,
            (std::vector<std::string>{"Unfocused", "Focused", "Unfocused",
                                      "Focused"}));

  EXPECT_TRUE(bridge.Shutdown().ok());
  g_web_view_probe = nullptr;
}

TEST(RobloxWebViewBridgeTest, ForwardsWkHybridPayloadToNativeProtocol) {
  jnivm::VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  jclass bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  jobject bus = env->AllocObject(bus_class);
  WebViewBridgeProbe probe;
  g_web_view_probe = &probe;
  RobloxWebViewBridge bridge = MakeBridge(&vm, bus, &probe);
  ASSERT_TRUE(bridge.Initialize().ok());

  EXPECT_TRUE(bridge
                  .SignalJavascriptCallback(
                      R"({"feature":"CaptchaSuccess","callbackID":"7"})")
                  .ok());
  EXPECT_EQ(probe.hybrid_callback_payload,
            R"({"feature":"CaptchaSuccess","callbackID":"7"})");
  EXPECT_FALSE(bridge.SignalJavascriptCallback("").ok());
  EXPECT_TRUE(bridge.Shutdown().ok());
}

TEST(RobloxWebViewBridgeTest,
     AcceptsAllCallbacksInvokedSynchronouslyBySubscribe) {
  jnivm::VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  jclass bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  jobject bus = env->AllocObject(bus_class);
  WebViewBridgeProbe probe;
  probe.synchronous_subscribe_index = -2;
  g_web_view_probe = &probe;
  RobloxWebViewBridge bridge = MakeBridge(&vm, bus, &probe);

  const Status initialize = bridge.Initialize();
  ASSERT_TRUE(initialize.ok()) << initialize.message();
  EXPECT_TRUE(bridge.initialized());
  EXPECT_EQ(probe.native_initializations, 1);
  ASSERT_EQ(probe.dispatches, 1);
  EXPECT_EQ(probe.request.url, "https://www.roblox.com/games/1/servers");
  ASSERT_EQ(probe.mutation_dispatches, 1);
  ASSERT_TRUE(probe.mutation.title.has_value());
  EXPECT_EQ(*probe.mutation.title, "Synchronous mutation");
  EXPECT_EQ(probe.close_dispatches, 1);

  ASSERT_TRUE(bridge.Shutdown().ok());
  EXPECT_EQ(probe.deleted_handles, (std::vector<jlong>{91, 92, 93}));
  g_web_view_probe = nullptr;
}

TEST(RobloxWebViewBridgeTest,
     ReopenSupersedesThePreviousWindowAndIgnoresItsStaleExit) {
  jnivm::VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  jclass bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  jobject bus = env->AllocObject(bus_class);
  WebViewBridgeProbe probe;
  g_web_view_probe = &probe;
  RobloxWebViewBridge bridge = MakeBridge(&vm, bus, &probe);

  ASSERT_TRUE(bridge.Initialize().ok());
  ASSERT_TRUE(
      bridge.HandleOwnedMessage(R"({"url":"https://www.roblox.com/first"})")
          .ok());
  const WebViewHelperExitObserver first = probe.exit_observer;
  ASSERT_TRUE(first.valid());
  ASSERT_TRUE(
      bridge.HandleOwnedMessage(R"({"url":"https://www.roblox.com/second"})")
          .ok());
  const WebViewHelperExitObserver second = probe.exit_observer;
  ASSERT_TRUE(second.valid());

  first.on_exit(first.context.get());
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.close_publications, 0);

  second.on_exit(second.context.get());
  second.on_exit(second.context.get());
  ASSERT_TRUE(bridge.DrainHostWindowEvents().ok());
  EXPECT_EQ(probe.close_publications, 1);

  ASSERT_TRUE(bridge.Shutdown().ok());
  g_web_view_probe = nullptr;
}

TEST(RobloxWebViewBridgeTest,
     RollsBackEarlierSubscriptionsWhenLaterSubscriptionFails) {
  jnivm::VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  jclass bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  jobject bus = env->AllocObject(bus_class);
  WebViewBridgeProbe probe;
  probe.fail_subscribe_index = 2;
  g_web_view_probe = &probe;
  RobloxWebViewBridge bridge = MakeBridge(&vm, bus, &probe);

  const Status initialize = bridge.Initialize();
  EXPECT_FALSE(initialize.ok());
  EXPECT_FALSE(bridge.initialized());
  EXPECT_EQ(
      probe.subscription_ids,
      (std::vector<std::string>{"WebView.openWindow", "WebView.mutateWindow",
                                "WebView.closeWindow"}));
  EXPECT_EQ(probe.deleted_handles, (std::vector<jlong>{91, 92}));
  EXPECT_EQ(probe.cleared_callbacks.size(), 3u);
  EXPECT_EQ(probe.native_initializations, 0);

  ASSERT_EQ(probe.subscribed_callbacks.size(), 3u);
  jclass callback_class = env->GetObjectClass(probe.subscribed_callbacks[0]);
  jmethodID run =
      env->GetMethodID(callback_class, "run", "(Ljava/lang/String;)V");
  jstring open_json =
      env->NewStringUTF(R"({"url":"https://www.roblox.com/home"})");
  jstring mutation_json = env->NewStringUTF(R"({"isVisible":false})");
  env->CallVoidMethod(probe.subscribed_callbacks[0], run, open_json);
  env->CallVoidMethod(probe.subscribed_callbacks[1], run, mutation_json);
  env->CallVoidMethod(probe.subscribed_callbacks[2], run, nullptr);
  EXPECT_EQ(probe.dispatches, 0);
  EXPECT_EQ(probe.mutation_dispatches, 0);
  EXPECT_EQ(probe.close_dispatches, 0);
  EXPECT_TRUE(bridge.Shutdown().ok());
  g_web_view_probe = nullptr;
}

TEST(RobloxWebViewBridgeTest,
     KeepsCallbacksInertAndRetriesConnectionCleanupAfterJniFailure) {
  jnivm::VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  jclass bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  jobject bus = env->AllocObject(bus_class);
  WebViewBridgeProbe probe;
  g_web_view_probe = &probe;

  JavaVmAcquireProbe acquire_probe{vm.GetJavaVM()};
  g_java_vm_acquire_probe = &acquire_probe;
  JNIInvokeInterface_ invoke_interface = *vm.GetJavaVM()->functions;
  invoke_interface.GetEnv = &ControlledGetEnv;
  JavaVM controlled_java_vm{&invoke_interface};
  RobloxWebViewBridge bridge(
      {&controlled_java_vm, nullptr, nullptr},
      {&GetOpenWindowId, &GetHandleWindowCloseId, &GetProtocolName,
       &GetIsAvailableId, &GetMessageId, &InitializeAndroidWebViewProtocol,
       &SubscribeRaw, &DeleteConnection, &SetRequestHandler,
       &ClearRequestHandler, &PublishRaw, &BroadcastDataModelFocus,
       &GetMutateWindowId, &GetCloseWindowId, &SignalJavascriptCallback,
       &UpdateCookieSetHandler},
      {bus, &vm, &CreateRawCallback, &ClearRawCallback, &CreateRequestHandler,
       &ClearRequestHandlerObject, &SetPlatformWebCallbacks,
       &ClearPlatformWebCallbacks},
      {&probe, &DispatchOpen, &DispatchMutate, &DispatchClose,
       &DispatchCookie});

  ASSERT_TRUE(bridge.Initialize().ok());
  ASSERT_EQ(probe.subscribed_callbacks.size(), 3u);
  EXPECT_FALSE(bridge.Shutdown().ok());
  EXPECT_TRUE(bridge.initialized());
  EXPECT_TRUE(probe.deleted_handles.empty());

  jclass callback_class = env->GetObjectClass(probe.subscribed_callbacks[0]);
  jmethodID run =
      env->GetMethodID(callback_class, "run", "(Ljava/lang/String;)V");
  jstring open_json =
      env->NewStringUTF(R"({"url":"https://www.roblox.com/ignored"})");
  env->CallVoidMethod(probe.subscribed_callbacks[0], run, open_json);
  env->CallVoidMethod(probe.subscribed_callbacks[1], run, open_json);
  env->CallVoidMethod(probe.subscribed_callbacks[2], run, open_json);
  EXPECT_EQ(probe.dispatches, 0);
  EXPECT_EQ(probe.mutation_dispatches, 0);
  EXPECT_EQ(probe.close_dispatches, 0);

  acquire_probe.successful_get_env_calls = 3;
  ASSERT_TRUE(bridge.Shutdown().ok());
  EXPECT_FALSE(bridge.initialized());
  EXPECT_EQ(probe.deleted_handles, (std::vector<jlong>{91, 92, 93}));
  EXPECT_EQ(probe.cleared_callbacks.size(), 3u);
  g_java_vm_acquire_probe = nullptr;
  g_web_view_probe = nullptr;
}

TEST(RobloxWebViewBridgeTest,
     RejectsMissingMandatoryAndroidProtocolInitializer) {
  jnivm::VM vm;
  JNIEnv *env = vm.GetJNIEnv();
  jclass bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  jobject bus = env->AllocObject(bus_class);
  WebViewBridgeProbe probe;
  g_web_view_probe = &probe;
  RobloxWebViewBridge bridge(
      {vm.GetJavaVM(), nullptr, nullptr},
      {&GetOpenWindowId, &GetHandleWindowCloseId, &GetProtocolName,
       &GetIsAvailableId, &GetMessageId, nullptr, &SubscribeRaw,
       &DeleteConnection, &SetRequestHandler, &ClearRequestHandler, &PublishRaw,
       &BroadcastDataModelFocus, &GetMutateWindowId, &GetCloseWindowId,
       &SignalJavascriptCallback, &UpdateCookieSetHandler},
      {bus, &vm, &CreateRawCallback, &ClearRawCallback, &CreateRequestHandler,
       &ClearRequestHandlerObject, &SetPlatformWebCallbacks,
       &ClearPlatformWebCallbacks},
      {&probe, &DispatchOpen, &DispatchMutate, &DispatchClose,
       &DispatchCookie});

  const Status initialize = bridge.Initialize();
  EXPECT_FALSE(initialize.ok());
  EXPECT_FALSE(bridge.initialized());
  EXPECT_TRUE(probe.initialization_order.empty());
  EXPECT_EQ(probe.native_initializations, 0);
  g_web_view_probe = nullptr;
}

} // namespace
} // namespace runtime
} // namespace mocktail
