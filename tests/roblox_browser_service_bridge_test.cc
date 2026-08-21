#include "runtime/roblox_browser_service_bridge.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "jnivm/jnivm.h"

namespace mocktail {
namespace runtime {
namespace {

struct BrowserCallbackBinding {
  jobject object = nullptr;
  std::shared_ptr<void> context;
  void (*on_item_set)(void *, JNIEnv *, jstring) = nullptr;
  bool cleared = false;
};

struct FiredBrowserServiceEvent {
  std::string key;
  std::string payload;

  bool operator==(const FiredBrowserServiceEvent& other) const {
    return key == other.key && payload == other.payload;
  }
};

struct BrowserServiceProbe {
  JNIEnv *env = nullptr;
  std::vector<std::string> bound_keys;
  std::vector<BrowserCallbackBinding> callbacks;
  std::vector<jobject> connections;
  std::vector<jobject> disconnected;
  std::vector<jobject> released;
  std::vector<RobloxBrowserServiceOpenRequest> opened_requests;
  std::vector<WebViewHelperExitObserver> exit_observers;
  std::vector<RobloxBrowserServiceConfigRequest> config_requests;
  std::vector<RobloxBrowserServiceExecuteRequest> execute_requests;
  std::vector<FiredBrowserServiceEvent> fired_events;
  int close_dispatches = 0;
  int fail_bind_index = -1;
  int fail_open_dispatch_index = -1;
  int synchronous_bind_index = -1;
  int synchronous_exit_open_index = -1;
  std::string synchronous_payload;
};

struct JavaVmAcquireProbe {
  JavaVM *delegate = nullptr;
  int get_env_calls = 0;
  int successful_get_env_calls = 1;
};

struct JniStringLengthProbe {
  JNIEnv *delegate = nullptr;
  int calls = 0;
};

BrowserServiceProbe *g_browser_service_probe = nullptr;
JavaVmAcquireProbe *g_java_vm_acquire_probe = nullptr;
JniStringLengthProbe *g_jni_string_length_probe = nullptr;

jint JNICALL ControlledGetEnv(JavaVM *, void **env, jint version) {
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

jsize JNICALL CountGetStringUtfLength(JNIEnv *, jstring value) {
  if (g_jni_string_length_probe == nullptr ||
      g_jni_string_length_probe->delegate == nullptr) {
    return 0;
  }
  ++g_jni_string_length_probe->calls;
  return g_jni_string_length_probe->delegate->GetStringUTFLength(value);
}

std::string CopyJniString(JNIEnv *env, jstring value) {
  if (env == nullptr || value == nullptr) {
    return {};
  }
  const jsize size = env->GetStringUTFLength(value);
  const char *chars = env->GetStringUTFChars(value, nullptr);
  if (chars == nullptr) {
    return {};
  }
  std::string result(chars, static_cast<std::size_t>(size));
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

BrowserCallbackBinding *FindCallback(jobject object) {
  if (g_browser_service_probe == nullptr) {
    return nullptr;
  }
  for (BrowserCallbackBinding &binding : g_browser_service_probe->callbacks) {
    if (binding.object == object) {
      return &binding;
    }
  }
  return nullptr;
}

void DispatchCallback(const BrowserCallbackBinding &binding,
                      const std::string &payload) {
  ASSERT_NE(g_browser_service_probe, nullptr);
  ASSERT_NE(binding.context, nullptr);
  ASSERT_NE(binding.on_item_set, nullptr);
  jstring value = g_browser_service_probe->env->NewStringUTF(payload.c_str());
  binding.on_item_set(binding.context.get(), g_browser_service_probe->env,
                      value);
  g_browser_service_probe->env->DeleteLocalRef(value);
}

jobject CreateCallback(void *context, std::shared_ptr<void> callback_context,
                       void (*on_item_set)(void *, JNIEnv *, jstring)) {
  auto *probe = static_cast<BrowserServiceProbe *>(context);
  jclass callback_class =
      probe->env->FindClass("com/roblox/engine/jni/memstorage/Callback");
  jobject callback = probe->env->AllocObject(callback_class);
  probe->env->DeleteLocalRef(callback_class);
  probe->callbacks.push_back(
      {callback, std::move(callback_context), on_item_set, false});
  return callback;
}

void ClearCallback(void *, jobject callback) {
  BrowserCallbackBinding *binding = FindCallback(callback);
  if (binding != nullptr) {
    binding->cleared = true;
  }
}

jobject Bind(JNIEnv *env, jclass, jstring key, jobject callback) {
  const int bind_index =
      static_cast<int>(g_browser_service_probe->bound_keys.size());
  g_browser_service_probe->bound_keys.push_back(CopyJniString(env, key));
  if (bind_index == g_browser_service_probe->synchronous_bind_index) {
    BrowserCallbackBinding *binding = FindCallback(callback);
    if (binding == nullptr) {
      ADD_FAILURE() << "synchronous BrowserService callback is unavailable";
      return nullptr;
    }
    DispatchCallback(*binding, g_browser_service_probe->synchronous_payload);
  }
  if (bind_index == g_browser_service_probe->fail_bind_index) {
    return nullptr;
  }
  jclass connection_class =
      env->FindClass("com/roblox/engine/jni/memstorage/Connection");
  jmethodID constructor = env->GetMethodID(connection_class, "<init>", "(J)V");
  jobject connection = env->NewObject(connection_class, constructor,
                                      static_cast<jlong>(bind_index + 1));
  env->DeleteLocalRef(connection_class);
  g_browser_service_probe->connections.push_back(connection);
  return connection;
}

void Disconnect(JNIEnv *, jclass, jobject connection) {
  g_browser_service_probe->disconnected.push_back(connection);
}

void ReleaseConnection(JNIEnv *, jclass, jobject connection) {
  g_browser_service_probe->released.push_back(connection);
}

void Fire(JNIEnv* env, jclass, jstring key, jstring payload) {
  g_browser_service_probe->fired_events.push_back(
      {CopyJniString(env, key), CopyJniString(env, payload)});
}

Status DispatchOpen(void* context,
                    const RobloxBrowserServiceOpenRequest& request,
                    WebViewHelperExitObserver exit_observer) {
  auto* probe = static_cast<BrowserServiceProbe*>(context);
  const int dispatch_index = static_cast<int>(probe->opened_requests.size());
  probe->opened_requests.push_back(request);
  probe->exit_observers.push_back(exit_observer);
  if (dispatch_index == probe->fail_open_dispatch_index) {
    return Status::Error(StatusCode::kUnavailable,
                         "injected BrowserService open failure");
  }
  if (dispatch_index == probe->synchronous_exit_open_index) {
    exit_observer.on_exit(exit_observer.context.get());
  }
  return Status::Ok();
}

Status DispatchConfig(void* context,
                      const RobloxBrowserServiceConfigRequest& request) {
  static_cast<BrowserServiceProbe*>(context)->config_requests.push_back(
      request);
  return Status::Ok();
}

Status DispatchClose(void *context) {
  ++static_cast<BrowserServiceProbe *>(context)->close_dispatches;
  return Status::Ok();
}

Status DispatchExecute(void* context,
                       const RobloxBrowserServiceExecuteRequest& request) {
  static_cast<BrowserServiceProbe*>(context)->execute_requests.push_back(
      request);
  return Status::Ok();
}

RobloxBrowserServiceBridge MakeBridge(jnivm::VM *vm,
                                      BrowserServiceProbe *probe) {
  return RobloxBrowserServiceBridge(
      {vm->GetJavaVM(), nullptr, nullptr},
      {&Bind, &Disconnect, &ReleaseConnection, &Fire},
      {probe, &CreateCallback, &ClearCallback},
      {probe, &DispatchOpen, &DispatchClose, &DispatchConfig,
       &DispatchExecute});
}

TEST(RobloxBrowserServiceParserTest,
     ParsesRawAndSendCommandOpenWithExactDefaultsAndFields) {
  RobloxBrowserServiceOpenRequest request;
  Status status = ParseRobloxBrowserServiceOpenWindow(
      "www:games/servers-section/123", &request);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(request.url, "www:games/servers-section/123");
  EXPECT_FALSE(request.title.has_value());
  EXPECT_FALSE(request.visible.has_value());
  EXPECT_FALSE(request.modal.has_value());
  EXPECT_FALSE(request.back_navigation_disabled.has_value());
  EXPECT_FALSE(request.show_domain_as_title.has_value());

  RobloxBrowserServiceCommand command;
  status = ParseRobloxBrowserServiceSendCommand(
      R"({"command":"open","url":"https://www.roblox.com/games/1",)"
      R"("title":"Servers","visible":false,"modal":true,)"
      R"("backNavigationDisabled":true,"showDomainAsTitle":true,)"
      R"("future":{"value":true}})",
      &command);
  ASSERT_TRUE(status.ok()) << status.message();
  const auto* open = std::get_if<RobloxBrowserServiceOpenRequest>(&command);
  ASSERT_NE(open, nullptr);
  EXPECT_EQ(open->url, "https://www.roblox.com/games/1");
  ASSERT_TRUE(open->title.has_value());
  EXPECT_EQ(*open->title, "Servers");
  ASSERT_TRUE(open->visible.has_value());
  EXPECT_FALSE(*open->visible);
  ASSERT_TRUE(open->modal.has_value());
  EXPECT_TRUE(*open->modal);
  ASSERT_TRUE(open->back_navigation_disabled.has_value());
  EXPECT_TRUE(*open->back_navigation_disabled);
  ASSERT_TRUE(open->show_domain_as_title.has_value());
  EXPECT_TRUE(*open->show_domain_as_title);

  status = ParseRobloxBrowserServiceSendCommand(
      R"({"command":"open","url":"https://www.roblox.com/home"})", &command);
  ASSERT_TRUE(status.ok()) << status.message();
  open = std::get_if<RobloxBrowserServiceOpenRequest>(&command);
  ASSERT_NE(open, nullptr);
  ASSERT_TRUE(open->title.has_value());
  EXPECT_TRUE(open->title->empty());
  ASSERT_TRUE(open->visible.has_value());
  EXPECT_TRUE(*open->visible);
  ASSERT_TRUE(open->modal.has_value());
  EXPECT_FALSE(*open->modal);
  ASSERT_TRUE(open->back_navigation_disabled.has_value());
  EXPECT_FALSE(*open->back_navigation_disabled);
  ASSERT_TRUE(open->show_domain_as_title.has_value());
  EXPECT_FALSE(*open->show_domain_as_title);
}

TEST(RobloxBrowserServiceParserTest,
     ParsesConfigAndPreservesOptionalFieldAbsence) {
  RobloxBrowserServiceCommand command;
  Status status = ParseRobloxBrowserServiceSendCommand(
      R"({"command":"config","title":"Challenge","visible":false})", &command);
  ASSERT_TRUE(status.ok()) << status.message();
  const auto* config = std::get_if<RobloxBrowserServiceConfigRequest>(&command);
  ASSERT_NE(config, nullptr);
  ASSERT_TRUE(config->title.has_value());
  EXPECT_EQ(*config->title, "Challenge");
  ASSERT_TRUE(config->visible.has_value());
  EXPECT_FALSE(*config->visible);
  EXPECT_FALSE(config->back_navigation_disabled.has_value());

  status = ParseRobloxBrowserServiceSendCommand(
      R"({"command":"config","backNavigationDisabled":true})", &command);
  ASSERT_TRUE(status.ok()) << status.message();
  config = std::get_if<RobloxBrowserServiceConfigRequest>(&command);
  ASSERT_NE(config, nullptr);
  EXPECT_FALSE(config->title.has_value());
  EXPECT_FALSE(config->visible.has_value());
  ASSERT_TRUE(config->back_navigation_disabled.has_value());
  EXPECT_TRUE(*config->back_navigation_disabled);
}

TEST(RobloxBrowserServiceParserTest, RejectsMalformedAndUnboundedInput) {
  RobloxBrowserServiceOpenRequest request;
  RobloxBrowserServiceCommand command;
  EXPECT_FALSE(ParseRobloxBrowserServiceOpenWindow("", &request).ok());
  EXPECT_FALSE(ParseRobloxBrowserServiceOpenWindow(
                   "https://www.roblox.com/\nunsafe", &request)
                   .ok());
  EXPECT_FALSE(
      ParseRobloxBrowserServiceOpenWindow(
          std::string(kMaximumRobloxBrowserServiceUrlBytes + 1, 'x'), &request)
          .ok());
  EXPECT_FALSE(ParseRobloxBrowserServiceSendCommand("{", &command).ok());
  EXPECT_FALSE(
      ParseRobloxBrowserServiceSendCommand(R"({"command":"open"})", &command)
          .ok());
  const Status unsupported =
      ParseRobloxBrowserServiceSendCommand(R"({"command":"reload"})", &command);
  EXPECT_EQ(unsupported.code(), StatusCode::kUnsupported);
  EXPECT_FALSE(ParseRobloxBrowserServiceSendCommand(
                   R"({"command":"open","url":"https://www.roblox.com/",)"
                   R"("visible":"false"})",
                   &command)
                   .ok());
  EXPECT_FALSE(ParseRobloxBrowserServiceSendCommand(
                   R"({"command":"config","title":false})", &command)
                   .ok());
  EXPECT_FALSE(ParseRobloxBrowserServiceSendCommand(
                   R"({"command":"config","title":"line\nbreak"})", &command)
                   .ok());
  EXPECT_FALSE(
      ParseRobloxBrowserServiceSendCommand(
          R"({"command":"config","title":")" + std::string(513, 'x') + R"("})",
          &command)
          .ok());
  EXPECT_FALSE(
      ParseRobloxBrowserServiceSendCommand(
          std::string(kMaximumRobloxBrowserServicePayloadBytes + 1, 'x'),
          &command)
          .ok());
}

TEST(RobloxBrowserServiceBridgeTest,
     BindsExactKeysDispatchesOwnedEventsAndReleasesConnections) {
  jnivm::VM vm;
  BrowserServiceProbe probe;
  probe.env = vm.GetJNIEnv();
  g_browser_service_probe = &probe;
  RobloxBrowserServiceBridge bridge = MakeBridge(&vm, &probe);

  ASSERT_TRUE(bridge.Initialize().ok());
  EXPECT_TRUE(bridge.initialized());
  EXPECT_EQ(probe.bound_keys, (std::vector<std::string>{
                                  kRobloxBrowserServiceOpenWindowKey,
                                  kRobloxBrowserServiceCloseWindowKey,
                                  kRobloxBrowserServiceSendCommandKey,
                                  kRobloxBrowserServiceExecuteJavaScriptKey,
                              }));
  ASSERT_EQ(probe.callbacks.size(), 4u);

  DispatchCallback(probe.callbacks[0], "www:games/servers-section/987654321");
  DispatchCallback(probe.callbacks[2],
                   R"({"command":"open","url":"https://www.roblox.com/home"})");
  DispatchCallback(probe.callbacks[2],
                   R"({"command":"config","title":"Captcha","visible":false,)"
                   R"("backNavigationDisabled":true})");
  DispatchCallback(probe.callbacks[1], "");
  DispatchCallback(probe.callbacks[3], "window.challenge && challenge.run()");
  ASSERT_EQ(probe.opened_requests.size(), 2u);
  EXPECT_EQ(probe.opened_requests[0].url,
            "www:games/servers-section/987654321");
  EXPECT_EQ(probe.opened_requests[1].url, "https://www.roblox.com/home");
  ASSERT_EQ(probe.config_requests.size(), 1u);
  ASSERT_TRUE(probe.config_requests[0].title.has_value());
  EXPECT_EQ(*probe.config_requests[0].title, "Captcha");
  ASSERT_TRUE(probe.config_requests[0].visible.has_value());
  EXPECT_FALSE(*probe.config_requests[0].visible);
  ASSERT_TRUE(probe.config_requests[0].back_navigation_disabled.has_value());
  EXPECT_TRUE(*probe.config_requests[0].back_navigation_disabled);
  ASSERT_EQ(probe.execute_requests.size(), 1u);
  EXPECT_EQ(probe.execute_requests[0].source,
            "window.challenge && challenge.run()");
  EXPECT_EQ(probe.close_dispatches, 1);
  ASSERT_EQ(probe.exit_observers.size(), 2u);
  const WebViewHelperExitObserver retained_exit_observer =
      probe.exit_observers.back();
  ASSERT_TRUE(retained_exit_observer.valid());

  const BrowserCallbackBinding retained_callback = probe.callbacks[0];
  ASSERT_TRUE(bridge.Shutdown().ok());
  EXPECT_FALSE(bridge.initialized());
  EXPECT_EQ(probe.disconnected, probe.connections);
  EXPECT_EQ(probe.released, probe.connections);
  for (const BrowserCallbackBinding &binding : probe.callbacks) {
    EXPECT_TRUE(binding.cleared);
  }

  DispatchCallback(retained_callback, "https://www.roblox.com/after-shutdown");
  retained_exit_observer.on_exit(retained_exit_observer.context.get());
  EXPECT_EQ(probe.opened_requests.size(), 2u);
  EXPECT_TRUE(probe.fired_events.empty());
  g_browser_service_probe = nullptr;
}

TEST(RobloxBrowserServiceBridgeTest,
     AcceptsCallbacksInvokedSynchronouslyDuringBindAndOpen) {
  jnivm::VM vm;
  BrowserServiceProbe probe;
  probe.env = vm.GetJNIEnv();
  probe.synchronous_bind_index = 0;
  probe.synchronous_exit_open_index = 0;
  probe.synchronous_payload = "www:games/servers-section/42";
  g_browser_service_probe = &probe;
  RobloxBrowserServiceBridge bridge = MakeBridge(&vm, &probe);

  const Status initialize = bridge.Initialize();
  ASSERT_TRUE(initialize.ok()) << initialize.message();
  ASSERT_EQ(probe.opened_requests.size(), 1u);
  EXPECT_EQ(probe.opened_requests[0].url, "www:games/servers-section/42");
  EXPECT_TRUE(probe.fired_events.empty());
  ASSERT_TRUE(bridge.DrainOutgoingEvents().ok());
  EXPECT_EQ(probe.fired_events,
            (std::vector<FiredBrowserServiceEvent>{
                {kRobloxBrowserServiceBrowserWindowClosedKey, ""},
            }));

  ASSERT_TRUE(bridge.Shutdown().ok());
  g_browser_service_probe = nullptr;
}

TEST(RobloxBrowserServiceBridgeTest,
     ExplicitCloseQueuesAsynchronousExitExactlyOnceForMainThreadDrain) {
  jnivm::VM vm;
  BrowserServiceProbe probe;
  probe.env = vm.GetJNIEnv();
  g_browser_service_probe = &probe;
  RobloxBrowserServiceBridge bridge = MakeBridge(&vm, &probe);

  ASSERT_TRUE(bridge.Initialize().ok());
  ASSERT_TRUE(
      bridge.HandleOwnedOpenWindow("https://www.roblox.com/login").ok());
  ASSERT_EQ(probe.exit_observers.size(), 1u);
  const WebViewHelperExitObserver exit_observer = probe.exit_observers[0];
  ASSERT_TRUE(exit_observer.valid());
  ASSERT_TRUE(bridge.HandleCloseWindow().ok());
  EXPECT_EQ(probe.close_dispatches, 1);

  std::thread reaper([exit_observer] {
    exit_observer.on_exit(exit_observer.context.get());
    exit_observer.on_exit(exit_observer.context.get());
  });
  reaper.join();
  EXPECT_TRUE(probe.fired_events.empty());

  const Status drain = bridge.DrainOutgoingEvents();
  ASSERT_TRUE(drain.ok()) << drain.message();
  EXPECT_EQ(probe.fired_events,
            (std::vector<FiredBrowserServiceEvent>{
                {kRobloxBrowserServiceBrowserWindowClosedKey, ""},
            }));
  ASSERT_TRUE(bridge.DrainOutgoingEvents().ok());
  EXPECT_EQ(probe.fired_events.size(), 1u);

  ASSERT_TRUE(bridge.Shutdown().ok());
  g_browser_service_probe = nullptr;
}

TEST(RobloxBrowserServiceBridgeTest,
     ReopenSuppressesStaleExitAndFailedReopenRestoresCurrentGeneration) {
  jnivm::VM vm;
  BrowserServiceProbe probe;
  probe.env = vm.GetJNIEnv();
  g_browser_service_probe = &probe;
  RobloxBrowserServiceBridge bridge = MakeBridge(&vm, &probe);

  ASSERT_TRUE(bridge.Initialize().ok());
  ASSERT_TRUE(
      bridge.HandleOwnedOpenWindow("https://www.roblox.com/first").ok());
  ASSERT_TRUE(
      bridge.HandleOwnedOpenWindow("https://www.roblox.com/second").ok());
  ASSERT_EQ(probe.exit_observers.size(), 2u);
  const WebViewHelperExitObserver first_observer = probe.exit_observers[0];
  const WebViewHelperExitObserver second_observer = probe.exit_observers[1];
  ASSERT_TRUE(first_observer.valid());
  ASSERT_TRUE(second_observer.valid());

  first_observer.on_exit(first_observer.context.get());
  ASSERT_TRUE(bridge.DrainOutgoingEvents().ok());
  EXPECT_TRUE(probe.fired_events.empty());

  probe.fail_open_dispatch_index = 2;
  const Status failed_reopen =
      bridge.HandleOwnedOpenWindow("https://www.roblox.com/failed");
  EXPECT_EQ(failed_reopen.code(), StatusCode::kUnavailable);
  ASSERT_EQ(probe.exit_observers.size(), 3u);
  const WebViewHelperExitObserver failed_observer = probe.exit_observers[2];
  ASSERT_TRUE(failed_observer.valid());
  failed_observer.on_exit(failed_observer.context.get());
  second_observer.on_exit(second_observer.context.get());
  second_observer.on_exit(second_observer.context.get());
  EXPECT_TRUE(probe.fired_events.empty());

  ASSERT_TRUE(bridge.DrainOutgoingEvents().ok());
  EXPECT_EQ(probe.fired_events,
            (std::vector<FiredBrowserServiceEvent>{
                {kRobloxBrowserServiceBrowserWindowClosedKey, ""},
            }));

  ASSERT_TRUE(bridge.Shutdown().ok());
  g_browser_service_probe = nullptr;
}

TEST(RobloxBrowserServiceBridgeTest,
     QueuesAndDrainsExactOutgoingEventsInOrder) {
  jnivm::VM vm;
  BrowserServiceProbe probe;
  probe.env = vm.GetJNIEnv();
  g_browser_service_probe = &probe;
  RobloxBrowserServiceBridge bridge = MakeBridge(&vm, &probe);

  ASSERT_TRUE(bridge.Initialize().ok());
  ASSERT_TRUE(bridge
                  .QueueJavaScriptCallback(
                      R"({"moduleID":"Navigation","functionName":"done"})")
                  .ok());
  ASSERT_TRUE(bridge.QueueBrowserWindowClosed().ok());
  EXPECT_TRUE(probe.fired_events.empty());

  const Status drain = bridge.DrainOutgoingEvents();
  ASSERT_TRUE(drain.ok()) << drain.message();
  EXPECT_EQ(probe.fired_events,
            (std::vector<FiredBrowserServiceEvent>{
                {kRobloxBrowserServiceJavaScriptCallbackKey,
                 R"({"moduleID":"Navigation","functionName":"done"})"},
                {kRobloxBrowserServiceBrowserWindowClosedKey, ""},
            }));
  EXPECT_TRUE(bridge.DrainOutgoingEvents().ok());
  EXPECT_EQ(probe.fired_events.size(), 2u);

  ASSERT_TRUE(bridge.QueueBrowserWindowClosed().ok());
  ASSERT_TRUE(bridge.Shutdown().ok());
  EXPECT_EQ(probe.fired_events.size(), 2u);
  EXPECT_FALSE(bridge.DrainOutgoingEvents().ok());
  EXPECT_FALSE(bridge.QueueBrowserWindowClosed().ok());
  g_browser_service_probe = nullptr;
}

TEST(RobloxBrowserServiceBridgeTest, BoundsOutgoingEventQueue) {
  jnivm::VM vm;
  BrowserServiceProbe probe;
  probe.env = vm.GetJNIEnv();
  g_browser_service_probe = &probe;
  RobloxBrowserServiceBridge bridge = MakeBridge(&vm, &probe);

  ASSERT_TRUE(bridge.Initialize().ok());
  for (std::size_t index = 0; index < kMaximumRobloxBrowserServiceQueuedEvents;
       ++index) {
    ASSERT_TRUE(bridge.QueueBrowserWindowClosed().ok());
  }
  EXPECT_EQ(bridge.QueueBrowserWindowClosed().code(), StatusCode::kUnavailable);
  EXPECT_FALSE(bridge
                   .QueueJavaScriptCallback(std::string(
                       kMaximumRobloxBrowserServicePayloadBytes + 1, 'x'))
                   .ok());

  ASSERT_TRUE(bridge.Shutdown().ok());
  g_browser_service_probe = nullptr;
}

TEST(RobloxBrowserServiceBridgeTest,
     RollsBackEarlierBindingsWhenLaterBindFails) {
  jnivm::VM vm;
  BrowserServiceProbe probe;
  probe.env = vm.GetJNIEnv();
  probe.fail_bind_index = 1;
  g_browser_service_probe = &probe;
  RobloxBrowserServiceBridge bridge = MakeBridge(&vm, &probe);

  const Status initialize = bridge.Initialize();
  EXPECT_FALSE(initialize.ok());
  EXPECT_FALSE(bridge.initialized());
  ASSERT_EQ(probe.connections.size(), 1u);
  EXPECT_EQ(probe.disconnected, probe.connections);
  EXPECT_EQ(probe.released, probe.connections);
  ASSERT_EQ(probe.callbacks.size(), 2u);
  EXPECT_TRUE(probe.callbacks[0].cleared);
  EXPECT_TRUE(probe.callbacks[1].cleared);
  EXPECT_TRUE(bridge.Shutdown().ok());
  g_browser_service_probe = nullptr;
}

TEST(RobloxBrowserServiceBridgeTest,
     MakesRetainedCallbacksInertBeforeJniCleanupCanFail) {
  jnivm::VM vm;
  BrowserServiceProbe probe;
  probe.env = vm.GetJNIEnv();
  g_browser_service_probe = &probe;

  JavaVmAcquireProbe acquire_probe{vm.GetJavaVM()};
  g_java_vm_acquire_probe = &acquire_probe;
  JNIInvokeInterface_ invoke_interface = *vm.GetJavaVM()->functions;
  invoke_interface.GetEnv = &ControlledGetEnv;
  JavaVM controlled_java_vm{&invoke_interface};

  RobloxBrowserServiceBridge bridge(
      {&controlled_java_vm, nullptr, nullptr},
      {&Bind, &Disconnect, &ReleaseConnection, &Fire},
      {&probe, &CreateCallback, &ClearCallback},
      {&probe, &DispatchOpen, &DispatchClose, &DispatchConfig,
       &DispatchExecute});
  ASSERT_TRUE(bridge.Initialize().ok());
  ASSERT_EQ(probe.callbacks.size(), 4u);
  const BrowserCallbackBinding retained_callback = probe.callbacks[0];

  const Status shutdown = bridge.Shutdown();
  EXPECT_FALSE(shutdown.ok());
  EXPECT_TRUE(bridge.initialized());
  EXPECT_TRUE(probe.disconnected.empty());

  JniStringLengthProbe string_length_probe{probe.env};
  g_jni_string_length_probe = &string_length_probe;
  JNINativeInterface_ native_interface = *probe.env->functions;
  native_interface.GetStringUTFLength = &CountGetStringUtfLength;
  JNIEnv counting_env{&native_interface};
  jstring payload = probe.env->NewStringUTF("https://www.roblox.com/ignored");
  retained_callback.on_item_set(retained_callback.context.get(), &counting_env,
                                payload);
  probe.env->DeleteLocalRef(payload);

  EXPECT_EQ(string_length_probe.calls, 0);
  EXPECT_TRUE(probe.opened_requests.empty());
  g_jni_string_length_probe = nullptr;
  g_java_vm_acquire_probe = nullptr;
  g_browser_service_probe = nullptr;
}

TEST(RobloxBrowserServiceBridgeTest, RejectsIncompletePrerequisites) {
  jnivm::VM vm;
  BrowserServiceProbe probe;
  probe.env = vm.GetJNIEnv();
  g_browser_service_probe = &probe;
  RobloxBrowserServiceBridge bridge({vm.GetJavaVM(), nullptr, nullptr},
                                    {&Bind, nullptr, &ReleaseConnection, &Fire},
                                    {&probe, &CreateCallback, &ClearCallback},
                                    {&probe, &DispatchOpen, &DispatchClose,
                                     &DispatchConfig, &DispatchExecute});

  EXPECT_FALSE(bridge.Initialize().ok());
  EXPECT_TRUE(probe.bound_keys.empty());
  g_browser_service_probe = nullptr;
}

} // namespace
} // namespace runtime
} // namespace mocktail
