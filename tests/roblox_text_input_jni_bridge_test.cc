#include "runtime/roblox_text_input_jni_bridge.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "jnivm/jnivm.h"

namespace mocktail {
namespace runtime {
namespace {

class FakeBackend final : public RobloxTextInputJniBridgeBackend {
 public:
  Status BeginTextFocusSession(RobloxTextFocusSession session) override {
    calls.push_back("begin:" + std::to_string(session.generation));
    active_handle = session.textbox_handle;
    active_generation = session.generation;
    last_initial_text = session.initial_text;
    last_area_x = session.area_x;
    last_area_y = session.area_y;
    last_area_width = session.area_width;
    last_area_height = session.area_height;
    last_text_input_type = session.text_input_type;
    last_multiline = session.multiline;
    last_manual_focus_release = session.manual_focus_release;
    active = true;
    std::fill(session.initial_text.begin(), session.initial_text.end(), '\0');
    session.initial_text.clear();
    return begin_status;
  }

  Status EndTextFocusSession(int64_t textbox_handle, uint64_t generation,
                             bool notify_native) override {
    calls.push_back("end:" + std::to_string(generation) + ":" +
                    (notify_native ? "notify" : "quiet"));
    if (textbox_handle == active_handle && generation == active_generation) {
      active = false;
    }
    return end_status;
  }

  Status ReplaceFocusedTextFromEngine(uint64_t generation,
                                      std::string authoritative_utf8) override {
    calls.push_back("replace:" + std::to_string(generation));
    replaced_texts.push_back(authoritative_utf8);
    std::fill(authoritative_utf8.begin(), authoritative_utf8.end(), '\0');
    authoritative_utf8.clear();
    return replace_status;
  }

  Status QueryCurrentTextBoxInfo(
      RobloxNativeTextBoxInfoQueryResult* result) override {
    calls.push_back("query:" + std::to_string(active_generation));
    ++query_calls;
    if (!query_status.ok()) {
      return query_status;
    }
    *result = query_result;
    return Status::Ok();
  }

  Status UpdateTextFocusProperties(
      uint64_t generation,
      const RobloxTextFocusProperties& properties) override {
    calls.push_back("properties:" + std::to_string(generation));
    last_properties = properties;
    return properties_status;
  }

  bool RegisterMainThreadPump(window::PreTextInputPumpCallback callback,
                              void* context) override {
    ++register_calls;
    if (!allow_registration) {
      return false;
    }
    pump = callback;
    pump_context = context;
    return true;
  }

  void ClearMainThreadPump() override {
    ++clear_calls;
    pump = nullptr;
    pump_context = nullptr;
  }

  void SetTextInputOwnerEnabled(bool enabled) override {
    owner_enabled = enabled;
    owner_transitions.push_back(enabled);
  }

  bool RequestShowTextInput(uint64_t generation,
                            const window::TextInputArea& area,
                            const window::TextInputOptions& options) override {
    calls.push_back("show:" + std::to_string(generation));
    last_requested_area = area;
    last_requested_options = options;
    return allow_show;
  }

  bool RequestHideTextInput(uint64_t generation) override {
    calls.push_back("hide:" + std::to_string(generation));
    return allow_hide;
  }

  bool Pump() { return pump == nullptr || pump(pump_context); }

  bool allow_registration = true;
  bool allow_show = true;
  bool allow_hide = true;
  bool owner_enabled = false;
  bool active = false;
  int register_calls = 0;
  int clear_calls = 0;
  int query_calls = 0;
  int64_t active_handle = 0;
  uint64_t active_generation = 0;
  int32_t last_area_x = 0;
  int32_t last_area_y = 0;
  int32_t last_area_width = 0;
  int32_t last_area_height = 0;
  int32_t last_text_input_type = 0;
  bool last_multiline = false;
  bool last_manual_focus_release = false;
  std::string last_initial_text;
  std::vector<std::string> replaced_texts;
  RobloxNativeTextBoxInfoQueryResult query_result;
  RobloxTextFocusProperties last_properties;
  window::TextInputArea last_requested_area;
  window::TextInputOptions last_requested_options;
  Status begin_status = Status::Ok();
  Status end_status = Status::Ok();
  Status replace_status = Status::Ok();
  Status query_status = Status::Ok();
  Status properties_status = Status::Ok();
  window::PreTextInputPumpCallback pump = nullptr;
  void* pump_context = nullptr;
  std::vector<bool> owner_transitions;
  std::vector<std::string> calls;
};

jnivm::RobloxTextInputShowRequest ShowRequest(int64_t handle,
                                              std::string text) {
  jnivm::RobloxTextInputShowRequest request;
  request.text_box = handle;
  request.show_native_input = true;
  request.text = std::move(text);
  request.info.width = 200.0F;
  request.info.height = 30.0F;
  request.info.text_input_type = 1;
  return request;
}

struct ExistingBindingProbe {
  int shows = 0;
  int shutdowns = 0;
};

void ExistingShow(void* context,
                  const jnivm::RobloxTextInputShowRequest&) {
  ++static_cast<ExistingBindingProbe*>(context)->shows;
}

void ExistingShutdown(void* context) {
  ++static_cast<ExistingBindingProbe*>(context)->shutdowns;
}

TEST(RobloxTextInputJniBridgeTest,
     FailedPumpReservationDoesNotMutateExistingOwners) {
  jnivm::VM vm;
  auto existing = std::make_shared<ExistingBindingProbe>();
  jnivm::RobloxTextInputCallbacks callbacks;
  callbacks.show = &ExistingShow;
  callbacks.shutdown = &ExistingShutdown;
  vm.SetRobloxTextInputCallbacks(existing, callbacks);

  auto backend = std::make_shared<FakeBackend>();
  backend->allow_registration = false;
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  const Status status =
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge);

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(bridge, nullptr);
  EXPECT_EQ(backend->register_calls, 1);
  EXPECT_EQ(backend->clear_calls, 0);
  EXPECT_TRUE(backend->owner_transitions.empty());
  EXPECT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(11, "secret")));
  EXPECT_EQ(existing->shows, 1);
  EXPECT_EQ(existing->shutdowns, 0);
  vm.ClearRobloxTextInputCallbacks();
  EXPECT_EQ(existing->shutdowns, 1);
}

TEST(RobloxTextInputJniBridgeTest, CommandsDrainInGenerationOrder) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());

  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(42, "initial")));
  ASSERT_TRUE(vm.DispatchRobloxTextInputReplaceText("replacement"));
  ASSERT_TRUE(vm.DispatchRobloxTextInputHide());
  EXPECT_TRUE(backend->Pump());
  EXPECT_EQ(backend->calls,
            (std::vector<std::string>{"begin:1", "show:1", "replace:1",
                                      "end:1:quiet", "hide:1"}));
  EXPECT_FALSE(backend->active);
  EXPECT_EQ(backend->last_area_width, 200);
  EXPECT_EQ(backend->last_area_height, 30);
  EXPECT_EQ(backend->last_text_input_type, 1);
  EXPECT_TRUE(bridge->Shutdown().ok());
  ASSERT_EQ(backend->owner_transitions.size(), 2u);
  EXPECT_TRUE(backend->owner_transitions.front());
  EXPECT_FALSE(backend->owner_transitions.back());
}

TEST(RobloxTextInputJniBridgeTest,
     PendingReplaceCallbacksCollapseToLatestTextInCurrentGeneration) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(42, "initial")));
  ASSERT_TRUE(backend->Pump());
  backend->calls.clear();

  ASSERT_TRUE(vm.DispatchRobloxTextInputReplaceText("stale-engine-echo"));
  ASSERT_TRUE(vm.DispatchRobloxTextInputReplaceText("latest-engine-text"));
  EXPECT_TRUE(backend->Pump());

  EXPECT_EQ(backend->calls, (std::vector<std::string>{"replace:1"}));
  ASSERT_EQ(backend->replaced_texts.size(), 1u);
  EXPECT_EQ(backend->replaced_texts.front(), "latest-engine-text");
  EXPECT_TRUE(bridge->Shutdown().ok());
}

TEST(RobloxTextInputJniBridgeTest,
     CoalescedReplaceAccountsOnlyForRetainedSensitiveText) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(42, "initial")));
  ASSERT_TRUE(backend->Pump());

  constexpr std::size_t kLargeReplacementBytes = 3U * 1024U * 1024U;
  ASSERT_TRUE(vm.DispatchRobloxTextInputReplaceText(
      std::string(kLargeReplacementBytes, 'a')));
  ASSERT_TRUE(vm.DispatchRobloxTextInputReplaceText(
      std::string(kLargeReplacementBytes, 'b')));
  EXPECT_TRUE(backend->Pump());

  ASSERT_EQ(backend->replaced_texts.size(), 1u);
  EXPECT_EQ(backend->replaced_texts.front().size(), kLargeReplacementBytes);
  EXPECT_EQ(backend->replaced_texts.front().front(), 'b');
  EXPECT_EQ(backend->replaced_texts.front().back(), 'b');
  EXPECT_TRUE(bridge->Shutdown().ok());
}

TEST(RobloxTextInputJniBridgeTest,
     ReplaceCoalescingDoesNotCrossFocusGenerations) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(42, "first")));
  ASSERT_TRUE(backend->Pump());
  backend->calls.clear();

  ASSERT_TRUE(vm.DispatchRobloxTextInputReplaceText("first-generation"));
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(84, "second")));
  ASSERT_TRUE(vm.DispatchRobloxTextInputReplaceText("stale-second"));
  ASSERT_TRUE(vm.DispatchRobloxTextInputReplaceText("latest-second"));
  EXPECT_TRUE(backend->Pump());

  EXPECT_EQ(backend->calls, (std::vector<std::string>{"replace:1", "begin:2",
                                                      "show:2", "replace:2"}));
  ASSERT_EQ(backend->replaced_texts.size(), 2u);
  EXPECT_EQ(backend->replaced_texts[0], "first-generation");
  EXPECT_EQ(backend->replaced_texts[1], "latest-second");
  EXPECT_TRUE(bridge->Shutdown().ok());
}

TEST(RobloxTextInputJniBridgeTest,
     PendingSameHandleShowKeepsGenerationAndLastValidGeometry) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());
  jnivm::RobloxTextInputShowRequest first = ShowRequest(42, "old");
  first.info.x = 12.0F;
  first.info.y = 34.0F;
  first.info.manual_focus_release = true;
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(first));
  ASSERT_TRUE(vm.DispatchRobloxTextInputReplaceText("superseded"));

  jnivm::RobloxTextInputShowRequest latest = ShowRequest(42, "latest");
  latest.info.width = 0.0F;
  latest.info.height = 0.0F;
  latest.info.multiline = true;
  latest.info.text_input_type = 7;
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(latest));
  EXPECT_TRUE(backend->Pump());

  EXPECT_EQ(backend->calls, (std::vector<std::string>{"begin:1", "show:1"}));
  EXPECT_EQ(backend->active_generation, 1u);
  EXPECT_EQ(backend->last_initial_text, "latest");
  EXPECT_EQ(backend->last_area_x, 12);
  EXPECT_EQ(backend->last_area_y, 34);
  EXPECT_EQ(backend->last_area_width, 200);
  EXPECT_EQ(backend->last_area_height, 30);
  EXPECT_EQ(backend->last_text_input_type, 7);
  EXPECT_TRUE(backend->last_multiline);
  EXPECT_FALSE(backend->last_manual_focus_release);
  EXPECT_TRUE(backend->replaced_texts.empty());
  EXPECT_TRUE(bridge->Shutdown().ok());
}

TEST(RobloxTextInputJniBridgeTest,
     AppliedSameHandleShowReusesGeometryButRefreshesPropertiesAndText) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());
  jnivm::RobloxTextInputShowRequest first = ShowRequest(42, "old");
  first.info.x = 25.0F;
  first.info.y = 50.0F;
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(first));
  ASSERT_TRUE(backend->Pump());
  backend->calls.clear();

  jnivm::RobloxTextInputShowRequest latest = ShowRequest(42, "new");
  latest.info.width = 0.0F;
  latest.info.height = 0.0F;
  latest.info.text_input_type = 9;
  latest.info.multiline = true;
  latest.info.manual_focus_release = true;
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(latest));
  EXPECT_TRUE(backend->Pump());

  EXPECT_EQ(backend->calls, (std::vector<std::string>{"begin:2", "show:2"}));
  EXPECT_EQ(backend->active_generation, 2u);
  EXPECT_EQ(backend->last_initial_text, "new");
  EXPECT_EQ(backend->last_area_x, 25);
  EXPECT_EQ(backend->last_area_y, 50);
  EXPECT_EQ(backend->last_area_width, 200);
  EXPECT_EQ(backend->last_area_height, 30);
  EXPECT_EQ(backend->last_text_input_type, 9);
  EXPECT_TRUE(backend->last_multiline);
  EXPECT_TRUE(backend->last_manual_focus_release);
  EXPECT_TRUE(bridge->Shutdown().ok());
}

TEST(RobloxTextInputJniBridgeTest,
     NonNativeFocusRefreshKeepsGeometryAcrossJavaWrapperHandles) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(42, "first")));
  ASSERT_TRUE(backend->Pump());

  jnivm::RobloxTextInputShowRequest next = ShowRequest(84, "second");
  next.show_native_input = false;
  next.info.width = 0.0F;
  next.info.height = 0.0F;
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(next));
  EXPECT_TRUE(backend->Pump());

  EXPECT_EQ(backend->active_handle, 84);
  EXPECT_EQ(backend->active_generation, 2u);
  EXPECT_EQ(backend->last_area_width, 200);
  EXPECT_EQ(backend->last_area_height, 30);
  EXPECT_TRUE(bridge->Shutdown().ok());
}

TEST(RobloxTextInputJniBridgeTest,
     NewNativeHandleWithMissingGeometryDoesNotBorrowPreviousTextBox) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(42, "first")));
  ASSERT_TRUE(backend->Pump());

  jnivm::RobloxTextInputShowRequest next = ShowRequest(84, "second");
  next.show_native_input = true;
  next.info.width = 0.0F;
  next.info.height = 0.0F;
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(next));
  EXPECT_TRUE(backend->Pump());

  EXPECT_EQ(backend->active_handle, 84);
  EXPECT_EQ(backend->last_area_width, 0);
  EXPECT_EQ(backend->last_area_height, 0);
  EXPECT_TRUE(bridge->Shutdown().ok());
}

TEST(RobloxTextInputJniBridgeTest,
     ExplicitHideClearsGeometryProvenanceBeforeNextHandle) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(42, "first")));
  ASSERT_TRUE(backend->Pump());
  ASSERT_TRUE(vm.DispatchRobloxTextInputHide());
  ASSERT_TRUE(backend->Pump());

  jnivm::RobloxTextInputShowRequest next = ShowRequest(84, "second");
  next.show_native_input = false;
  next.info.width = 0.0F;
  next.info.height = 0.0F;
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(next));
  EXPECT_TRUE(backend->Pump());

  EXPECT_EQ(backend->active_handle, 84);
  EXPECT_EQ(backend->last_area_width, 0);
  EXPECT_EQ(backend->last_area_height, 0);
  EXPECT_TRUE(bridge->Shutdown().ok());
}

TEST(RobloxTextInputJniBridgeTest,
     InstalledEngineCallbackDrivesFocusReplaceAndKeyboardHide) {
  jnivm::VM vm;
  vm.RegisterClass("com/roblox/engine/jni/EngineJavaCallback2");
  vm.RegisterClass("com/roblox/engine/jni/model/NativeTextBoxInfo");
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());

  JNIEnv* env = vm.GetJNIEnv();
  jclass callback_class =
      env->FindClass("com/roblox/engine/jni/EngineJavaCallback2");
  jclass info_class =
      env->FindClass("com/roblox/engine/jni/model/NativeTextBoxInfo");
  ASSERT_NE(callback_class, nullptr);
  ASSERT_NE(info_class, nullptr);
  jobject callback = env->AllocObject(callback_class);
  jobject info = env->AllocObject(info_class);
  ASSERT_NE(callback, nullptr);
  ASSERT_NE(info, nullptr);
  env->SetFloatField(info, env->GetFieldID(info_class, "width", "F"), 320.0F);
  env->SetFloatField(info, env->GetFieldID(info_class, "height", "F"), 48.0F);
  env->SetIntField(info, env->GetFieldID(info_class, "textInputType", "I"), 1);
  jbyteArray text = env->NewByteArray(4);
  const jbyte initial[] = {'u', 's', 'e', 'r'};
  env->SetByteArrayRegion(text, 0, 4, initial);

  const jmethodID show = env->GetMethodID(
      callback_class, "q",
      "(JZ[BLcom/roblox/engine/jni/model/NativeTextBoxInfo;)V");
  const jmethodID replace =
      env->GetMethodID(callback_class, "g", "(Ljava/lang/String;)V");
  const jmethodID hide = env->GetMethodID(callback_class, "d", "()V");
  ASSERT_NE(show, nullptr);
  ASSERT_NE(replace, nullptr);
  ASSERT_NE(hide, nullptr);

  env->CallVoidMethod(callback, show, static_cast<jlong>(901), JNI_TRUE, text,
                      info);
  ASSERT_TRUE(backend->Pump());
  EXPECT_TRUE(backend->active);
  EXPECT_EQ(backend->active_handle, 901);
  EXPECT_EQ(backend->last_area_width, 320);
  EXPECT_EQ(backend->last_area_height, 48);

  env->CallVoidMethod(callback, replace, env->NewStringUTF("updated"));
  ASSERT_TRUE(backend->Pump());
  EXPECT_NE(
      std::find(backend->calls.begin(), backend->calls.end(), "replace:1"),
      backend->calls.end());

  env->CallVoidMethod(callback, hide);
  ASSERT_TRUE(backend->Pump());
  EXPECT_FALSE(backend->active);
  EXPECT_NE(
      std::find(backend->calls.begin(), backend->calls.end(), "end:1:quiet"),
      backend->calls.end());
  EXPECT_NE(std::find(backend->calls.begin(), backend->calls.end(), "hide:1"),
            backend->calls.end());
}

TEST(RobloxTextInputJniBridgeTest,
     PropertyCallbackQueriesAndUpdatesCurrentGenerationOnMainThread) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());

  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(42, "old")));
  ASSERT_TRUE(backend->Pump());
  jnivm::RobloxTextInputShowRequest rebound = ShowRequest(84, "query");
  rebound.info.width = 0.0F;
  rebound.info.height = 0.0F;
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(rebound));
  ASSERT_TRUE(backend->Pump());
  backend->calls.clear();

  backend->query_result.available = true;
  backend->query_result.info.x = 69.0F;
  backend->query_result.info.y = 36.0F;
  backend->query_result.info.width = 633.0F;
  backend->query_result.info.height = 36.0F;
  backend->query_result.info.font_size = 16.0F;
  backend->query_result.info.x_alignment = 2;
  backend->query_result.info.y_alignment = 1;
  backend->query_result.info.text_color = static_cast<int32_t>(0xffd4d8e1U);
  backend->query_result.info.text_input_type = 3;

  ASSERT_TRUE(vm.DispatchRobloxTextInputPropertiesChanged());
  EXPECT_EQ(backend->query_calls, 0);
  ASSERT_TRUE(backend->Pump());

  EXPECT_EQ(backend->calls,
            (std::vector<std::string>{"query:2", "properties:2", "show:2"}));
  EXPECT_EQ(backend->query_calls, 1);
  EXPECT_EQ(backend->last_properties.area_x, 69);
  EXPECT_EQ(backend->last_properties.area_y, 36);
  EXPECT_EQ(backend->last_properties.area_width, 633);
  EXPECT_EQ(backend->last_properties.area_height, 36);
  EXPECT_FLOAT_EQ(backend->last_properties.font_size, 16.0F);
  EXPECT_EQ(backend->last_properties.x_alignment, 2);
  EXPECT_EQ(backend->last_properties.text_input_type, 3);
  EXPECT_EQ(backend->last_requested_area.x, 69);
  EXPECT_EQ(backend->last_requested_area.y, 36);
  EXPECT_EQ(backend->last_requested_area.width, 633);
  EXPECT_EQ(backend->last_requested_area.height, 36);
  EXPECT_TRUE(bridge->Shutdown().ok());
}

TEST(RobloxTextInputJniBridgeTest,
     StalePropertyCallbackCannotQueryOrOverwriteReboundTextBox) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(42, "first")));
  ASSERT_TRUE(backend->Pump());
  backend->calls.clear();

  ASSERT_TRUE(vm.DispatchRobloxTextInputPropertiesChanged());
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(84, "second")));
  ASSERT_TRUE(backend->Pump());

  EXPECT_EQ(backend->query_calls, 0);
  EXPECT_EQ(backend->calls, (std::vector<std::string>{"begin:2", "show:2"}));
  EXPECT_EQ(backend->active_handle, 84);
  EXPECT_TRUE(bridge->Shutdown().ok());
}

TEST(RobloxTextInputJniBridgeTest,
     NullPropertySnapshotKeepsCurrentSessionWithoutFailingClosed) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(42, "current")));
  ASSERT_TRUE(backend->Pump());
  backend->calls.clear();

  ASSERT_TRUE(vm.DispatchRobloxTextInputPropertiesChanged());
  ASSERT_TRUE(backend->Pump());

  EXPECT_EQ(backend->calls, (std::vector<std::string>{"query:1"}));
  EXPECT_TRUE(backend->active);
  EXPECT_TRUE(backend->owner_enabled);
  EXPECT_TRUE(bridge->Shutdown().ok());
}

TEST(RobloxTextInputJniBridgeTest,
     MissingOverlayGeometryDoesNotRejectNativeTextSession) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());
  jnivm::RobloxTextInputShowRequest request = ShowRequest(43, "editable");
  request.info.width = 0.0F;
  request.info.height = 0.0F;

  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(request));
  EXPECT_TRUE(backend->Pump());
  EXPECT_TRUE(backend->active);
  EXPECT_TRUE(backend->owner_enabled);
  EXPECT_EQ(backend->last_area_width, 0);
  EXPECT_EQ(backend->last_area_height, 0);
  EXPECT_TRUE(bridge->Shutdown().ok());
}

TEST(RobloxTextInputJniBridgeTest,
     ShutdownClosesAppliedSessionBeforeReleasingBackend) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(51, "private")));
  ASSERT_TRUE(backend->Pump());
  ASSERT_TRUE(backend->active);

  EXPECT_TRUE(bridge->Shutdown().ok());
  EXPECT_FALSE(backend->active);
  EXPECT_FALSE(backend->owner_enabled);
  EXPECT_NE(
      std::find(backend->calls.begin(), backend->calls.end(), "end:1:notify"),
      backend->calls.end());
  EXPECT_NE(std::find(backend->calls.begin(), backend->calls.end(), "hide:1"),
            backend->calls.end());
  EXPECT_EQ(backend->clear_calls, 1);
}

TEST(RobloxTextInputJniBridgeTest,
     CommandCountOverflowStopsAcceptanceAndClosesActiveFocus) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(7, "password")));
  ASSERT_TRUE(backend->Pump());
  ASSERT_TRUE(backend->active);

  for (int index = 0; index < 65; ++index) {
    ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(
        100 + index, "queued-sensitive-value-" + std::to_string(index))));
  }
  EXPECT_FALSE(backend->Pump());
  EXPECT_FALSE(backend->active);
  EXPECT_FALSE(backend->owner_enabled);
  const std::size_t calls_after_cleanup = backend->calls.size();
  EXPECT_TRUE(backend->Pump());
  EXPECT_TRUE(backend->Pump());
  EXPECT_EQ(backend->calls.size(), calls_after_cleanup);
  EXPECT_NE(
      std::find(backend->calls.begin(), backend->calls.end(), "end:1:notify"),
      backend->calls.end());
  EXPECT_NE(std::find(backend->calls.begin(), backend->calls.end(), "hide:1"),
            backend->calls.end());
  const Status shutdown = bridge->Shutdown();
  EXPECT_FALSE(shutdown.ok());
  EXPECT_EQ(shutdown.message(), "Roblox text-input command queue overflow");
  EXPECT_EQ(shutdown.message().find("queued-sensitive-value"),
            std::string::npos);
}

TEST(RobloxTextInputJniBridgeTest,
     AggregateByteOverflowDoesNotReachRuntimeAndFailsClosed) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());
  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(9, "initial")));
  ASSERT_TRUE(backend->Pump());
  const std::size_t calls_before = backend->calls.size();

  std::string oversized(4U * 1024U * 1024U + 1U, 's');
  ASSERT_TRUE(vm.DispatchRobloxTextInputReplaceText(oversized));
  std::fill(oversized.begin(), oversized.end(), '\0');
  oversized.clear();
  EXPECT_FALSE(backend->Pump());
  EXPECT_FALSE(backend->owner_enabled);
  EXPECT_EQ(std::count(backend->calls.begin() + calls_before,
                       backend->calls.end(), "replace:1"),
            0);
  EXPECT_FALSE(bridge->Shutdown().ok());
}

TEST(RobloxTextInputJniBridgeTest,
     RejectedShowRollsBackRuntimeAndDisablesHostInput) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  backend->allow_show = false;
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());

  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(88, "credential")));
  EXPECT_FALSE(backend->Pump());
  EXPECT_FALSE(backend->active);
  EXPECT_FALSE(backend->owner_enabled);
  const std::size_t calls_after_cleanup = backend->calls.size();
  EXPECT_TRUE(backend->Pump());
  EXPECT_EQ(backend->calls.size(), calls_after_cleanup);
  EXPECT_EQ(backend->calls,
            (std::vector<std::string>{"begin:1", "show:1", "end:1:notify",
                                      "hide:1"}));
  const Status shutdown = bridge->Shutdown();
  EXPECT_FALSE(shutdown.ok());
  EXPECT_EQ(shutdown.message(), "SDL text-input show request was rejected");
  EXPECT_EQ(shutdown.message().find("credential"), std::string::npos);
}

TEST(RobloxTextInputJniBridgeTest,
     BackendFailureDetailCannotLeakSensitiveTextIntoStatus) {
  jnivm::VM vm;
  auto backend = std::make_shared<FakeBackend>();
  backend->begin_status =
      Status::Error(StatusCode::kPlatformError,
                    "backend accidentally included credential hunter2");
  std::unique_ptr<RobloxTextInputJniBridge> bridge;
  ASSERT_TRUE(
      RobloxTextInputJniBridge::CreateForTesting(&vm, backend, &bridge).ok());

  ASSERT_TRUE(vm.DispatchRobloxTextInputShow(ShowRequest(12, "hunter2")));
  EXPECT_FALSE(backend->Pump());
  const Status shutdown = bridge->Shutdown();
  EXPECT_FALSE(shutdown.ok());
  EXPECT_EQ(shutdown.message(), "Roblox TextBox focus session failed");
  EXPECT_EQ(shutdown.message().find("hunter2"), std::string::npos);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
