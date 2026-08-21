#include "runtime/roblox_capability_resolver.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

static_assert(std::is_same<decltype(RobloxStartupSymbols::jni_on_load),
                           JniOnLoadFn>::value,
              "JNI_OnLoad must retain its JNI function type");
static_assert(
    std::is_same<
        decltype(RobloxStartupSymbols::app_bridge_v2_start_game_with_param),
        NativeAppBridgeV2StartGameWithParamFn>::value,
    "StartGame must retain its jint return type");
static_assert(
    std::is_same<
        decltype(RobloxInputSymbols::gamepad_connect_event_with_gamepad_type),
        NativeGamepadConnectEventWithGamepadTypeFn>::value,
    "modern gamepad connect must retain its two-int JNI signature");
static_assert(std::is_same<decltype(RobloxInputSymbols::pass_key_event),
                           NativePassKeyEventFn>::value,
              "hardware keyboard must retain its four-argument JNI signature");
static_assert(
    std::is_same<decltype(RobloxInputSymbols::pass_text),
                 NativePassTextFn>::value,
    "TextBox updates must retain the handle/string/finished/cursor signature");
static_assert(
    std::is_same<
        decltype(RobloxInputSymbols::sync_textbox_text_and_cursor_position2),
        NativeSyncTextboxTextAndCursorPosition2Fn>::value,
    "TextBox synchronization must retain the string/cursor signature");
static_assert(std::is_same<decltype(RobloxInputSymbols::get_text_box_info),
                           NativeGetTextBoxInfoFn>::value,
              "TextBox property refresh must retain its jobject JNI signature");

std::vector<std::string> RequiredHeadlessStartupSymbolNames() {
  return {
      "JNI_OnLoad",
      "Java_com_roblox_engine_jni_NativeGLInterface_nativeGameGlobalInit",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeUpdateAdapterInit",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeV2InitWithParams",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeStartLuaAppDM",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeV2StartAppWithParams",
  };
}

std::string SurfaceAppSymbolName() {
  return "Java_com_roblox_engine_jni_NativeGLInterface_"
         "nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams";
}

std::vector<std::string> OptionalStartupSymbolNames() {
  return {
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeCallMessagesFromMainThread",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeV2StartGameWithParam",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams",
  };
}

std::vector<std::string> InputSymbolNames() {
  const std::string prefix = "Java_com_roblox_engine_jni_NativeInputInterface_";
  const std::vector<std::string> method_names = {
      "nativeGamepadAxisEvent",
      "nativeGamepadButtonEvent",
      "nativeGamepadConnectEventWithGamepadType",
      "nativeGamepadDisconnectEvent",
      "nativeGetMainWindowIsMouseLockedCenter",
      "nativePassAccelerometerChange",
      "nativePassGravityChange",
      "nativePassGyroscopeChange",
      "nativePassInput",
      "nativePassLongPressGesture",
      "nativePassMouseButton",
      "nativePassMouseMove",
      "nativePassMousePan",
      "nativePassMousePinch",
      "nativePassMouseWheel",
      "nativePassPanGestureMultitouch",
      "nativePassPanGestureWithVelocity",
      "nativePassPinchGesture",
      "nativePassRotateGesture",
      "nativePassSwipeGesture",
      "nativePassTapGesture",
      "nativeSetAccelerometerEnabled",
      "nativeSetGamepadSupportedKeyWithGamepadType",
      "nativeSetGamepadSupportedMotionWithGamepadType",
      "nativeSetGyroscopeEnabled",
      "nativeSetTVRemoteSupportedKey",
      "nativeTVRemoteButtonEvent",
      "nativeTVRemoteConnectEvent",
      "nativeTVRemoteDisconnectEvent",
      "nativeUpdateScreenOrientation",
  };

  std::vector<std::string> symbols;
  symbols.reserve(method_names.size());
  for (const std::string& method_name : method_names) {
    symbols.push_back(prefix + method_name);
  }
  symbols.push_back(
      "Java_com_roblox_engine_jni_NativeGLInterface_nativePassKeyEvent");
  symbols.push_back(
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "syncTextboxTextAndCursorPosition2");
  symbols.push_back(
      "Java_com_roblox_engine_jni_NativeGLInterface_nativeGetTextBoxInfo");
  symbols.push_back(
      "Java_com_roblox_engine_jni_NativeGLInterface_nativePassText");
  symbols.push_back(
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeReturnPressedFromOnScreenKeyboard");
  symbols.push_back(
      "Java_com_roblox_engine_jni_NativeGLInterface_nativeReleaseFocus");
  return symbols;
}

std::vector<std::string> RemovedInputSymbolNames() {
  const std::string prefix = "Java_com_roblox_engine_jni_NativeInputInterface_";
  return {
      prefix + "nativeGamepadConnectEvent",
      prefix + "nativeSetGamepadSupportedKey",
      prefix + "nativeSetGamepadSupportedMotion",
      prefix + "nativePTFButtonEvent",
      prefix + "nativePTFConnectEvent",
      prefix + "nativePTFDisconnectEvent",
      prefix + "nativeSetPTFSupportedKey",
  };
}

std::vector<std::string> AllExpectedLookupSymbolNames() {
  std::vector<std::string> names = RequiredHeadlessStartupSymbolNames();
  names.push_back(SurfaceAppSymbolName());
  const std::vector<std::string> optional = OptionalStartupSymbolNames();
  names.insert(names.end(), optional.begin(), optional.end());
  const std::vector<std::string> input = InputSymbolNames();
  names.insert(names.end(), input.begin(), input.end());
  return names;
}

class FakeSymbolTable {
 public:
  RobloxSymbolLookup Lookup() {
    return RobloxSymbolLookup(&FakeSymbolTable::LookupCallback, this);
  }

  void ResolveAll(bool resolve_all) { resolve_all_ = resolve_all; }
  void Hide(const std::string& symbol_name) { hidden_.insert(symbol_name); }
  void Expose(const std::string& symbol_name) {
    available_.insert(symbol_name);
  }
  void ExposeRequiredHeadlessStartup() {
    for (const std::string& symbol_name :
         RequiredHeadlessStartupSymbolNames()) {
      Expose(symbol_name);
    }
  }

  const std::vector<std::string>& requests() const { return requests_; }

 private:
  static void* LookupCallback(void* context, const char* symbol_name) {
    auto* table = static_cast<FakeSymbolTable*>(context);
    if (table == nullptr || symbol_name == nullptr) {
      return nullptr;
    }
    table->requests_.emplace_back(symbol_name);
    if (table->hidden_.find(symbol_name) != table->hidden_.end()) {
      return nullptr;
    }
    if (!table->resolve_all_ &&
        table->available_.find(symbol_name) == table->available_.end()) {
      return nullptr;
    }
    // The resolver only stores the address. Tests never invoke it.
    return table;
  }

  bool resolve_all_ = true;
  std::unordered_set<std::string> hidden_;
  std::unordered_set<std::string> available_;
  std::vector<std::string> requests_;
};

TEST(RobloxCapabilityResolverTest, InvalidLookupFailsClosed) {
  const RobloxCapabilityResolution resolution =
      ResolveRobloxCapabilities(RobloxSymbolLookup());

  EXPECT_EQ(resolution.status(),
            RobloxCapabilityResolutionStatus::kInvalidLookup);
  EXPECT_FALSE(resolution.ok());
  EXPECT_EQ(resolution.capabilities(), nullptr);
  EXPECT_TRUE(resolution.missing_required_symbols().empty());
}

TEST(RobloxCapabilityResolverTest, ResolvesEveryCurrentTypedCapabilityOnce) {
  FakeSymbolTable symbols;

  const RobloxCapabilityResolution resolution =
      ResolveRobloxCapabilities(symbols.Lookup(), {true});

  ASSERT_TRUE(resolution.ok());
  ASSERT_NE(resolution.capabilities(), nullptr);
  const RobloxCapabilities& capabilities = *resolution.capabilities();
  EXPECT_NE(capabilities.startup.jni_on_load, nullptr);
  EXPECT_NE(capabilities.startup.game_global_init, nullptr);
  EXPECT_NE(capabilities.startup.update_adapter_init, nullptr);
  EXPECT_NE(capabilities.startup.app_bridge_v2_init_with_params, nullptr);
  EXPECT_NE(capabilities.startup.app_bridge_start_lua_app_dm, nullptr);
  EXPECT_NE(capabilities.startup.app_bridge_v2_start_app_with_params, nullptr);
  EXPECT_NE(capabilities.startup
                .app_bridge_v2_update_surface_app_with_platform_params,
            nullptr);
  EXPECT_TRUE(capabilities.startup_flags.has_surface_app);
  EXPECT_TRUE(capabilities.startup_flags.has_message_pump);
  EXPECT_TRUE(capabilities.startup_flags.has_start_game);
  EXPECT_TRUE(capabilities.startup_flags.has_surface_game);
  EXPECT_EQ(capabilities.input_flags.gamepad_api,
            RobloxGamepadApi::kWithGamepadType);
  EXPECT_TRUE(capabilities.input_flags.has_gamepad_events);
  EXPECT_TRUE(capabilities.input_flags.has_mouse);
  EXPECT_TRUE(capabilities.input_flags.has_touch);
  EXPECT_TRUE(capabilities.input_flags.has_hardware_keyboard);
  EXPECT_TRUE(capabilities.input_flags.has_text_input);
  EXPECT_TRUE(capabilities.input_flags.has_gestures);
  EXPECT_TRUE(capabilities.input_flags.has_sensors);
  EXPECT_TRUE(capabilities.input_flags.has_tv_remote);
  EXPECT_TRUE(capabilities.input_flags.has_screen_orientation);
  EXPECT_FALSE(capabilities.input_flags.has_legacy_gamepad_api);
  EXPECT_FALSE(capabilities.input_flags.has_ptf_gamepad_api);

  const std::vector<std::string> expected = AllExpectedLookupSymbolNames();
  const std::set<std::string> expected_set(expected.begin(), expected.end());
  const std::set<std::string> actual_set(symbols.requests().begin(),
                                         symbols.requests().end());
  EXPECT_EQ(symbols.requests().size(), expected.size());
  EXPECT_EQ(actual_set, expected_set);
}

TEST(RobloxCapabilityResolverTest, EveryCoreStartupSymbolIsRequired) {
  for (const std::string& missing : RequiredHeadlessStartupSymbolNames()) {
    SCOPED_TRACE(missing);
    FakeSymbolTable symbols;
    symbols.Hide(missing);

    const RobloxCapabilityResolution resolution =
        ResolveRobloxCapabilities(symbols.Lookup());

    EXPECT_EQ(resolution.status(),
              RobloxCapabilityResolutionStatus::kMissingRequiredSymbols);
    EXPECT_FALSE(resolution.ok());
    EXPECT_EQ(resolution.capabilities(), nullptr);
    ASSERT_EQ(resolution.missing_required_symbols().size(), 1U);
    EXPECT_EQ(resolution.missing_required_symbols().front(), missing);
  }
}

TEST(RobloxCapabilityResolverTest, ReportsAllMissingCoreSymbolsInStableOrder) {
  FakeSymbolTable symbols;
  const std::vector<std::string> required =
      RequiredHeadlessStartupSymbolNames();
  symbols.Hide(required[1]);
  symbols.Hide(required[4]);

  const RobloxCapabilityResolution resolution =
      ResolveRobloxCapabilities(symbols.Lookup());

  ASSERT_EQ(resolution.missing_required_symbols().size(), 2U);
  EXPECT_EQ(resolution.missing_required_symbols()[0], required[1]);
  EXPECT_EQ(resolution.missing_required_symbols()[1], required[4]);
  EXPECT_EQ(resolution.capabilities(), nullptr);
}

TEST(RobloxCapabilityResolverTest, SurfaceAppIsRequiredOnlyForWindowedRuntime) {
  FakeSymbolTable headless_symbols;
  headless_symbols.Hide(SurfaceAppSymbolName());
  const RobloxCapabilityResolution headless =
      ResolveRobloxCapabilities(headless_symbols.Lookup(), {false});
  ASSERT_TRUE(headless.ok());
  ASSERT_NE(headless.capabilities(), nullptr);
  EXPECT_EQ(headless.capabilities()
                ->startup.app_bridge_v2_update_surface_app_with_platform_params,
            nullptr);
  EXPECT_FALSE(headless.capabilities()->startup_flags.has_surface_app);

  FakeSymbolTable windowed_symbols;
  windowed_symbols.Hide(SurfaceAppSymbolName());
  const RobloxCapabilityResolution windowed =
      ResolveRobloxCapabilities(windowed_symbols.Lookup(), {true});
  EXPECT_FALSE(windowed.ok());
  ASSERT_EQ(windowed.missing_required_symbols().size(), 1U);
  EXPECT_EQ(windowed.missing_required_symbols().front(),
            SurfaceAppSymbolName());
}

TEST(RobloxCapabilityResolverTest, OptionalStartupSymbolsExposeFeatureFlags) {
  FakeSymbolTable symbols;
  for (const std::string& optional : OptionalStartupSymbolNames()) {
    symbols.Hide(optional);
  }

  const RobloxCapabilityResolution resolution =
      ResolveRobloxCapabilities(symbols.Lookup());

  ASSERT_TRUE(resolution.ok());
  ASSERT_NE(resolution.capabilities(), nullptr);
  EXPECT_FALSE(resolution.capabilities()->startup_flags.has_message_pump);
  EXPECT_FALSE(resolution.capabilities()->startup_flags.has_start_game);
  EXPECT_FALSE(resolution.capabilities()->startup_flags.has_surface_game);
  EXPECT_EQ(resolution.capabilities()->startup.call_messages_from_main_thread,
            nullptr);
  EXPECT_EQ(
      resolution.capabilities()->startup.app_bridge_v2_start_game_with_param,
      nullptr);
  EXPECT_EQ(
      resolution.capabilities()
          ->startup.app_bridge_v2_update_surface_game_with_platform_params,
      nullptr);
  EXPECT_TRUE(resolution.missing_required_symbols().empty());
}

TEST(RobloxCapabilityResolverTest, EveryInputSymbolIsOptionalForStartup) {
  for (const std::string& missing : InputSymbolNames()) {
    SCOPED_TRACE(missing);
    FakeSymbolTable symbols;
    symbols.Hide(missing);

    const RobloxCapabilityResolution resolution =
        ResolveRobloxCapabilities(symbols.Lookup(), {true});

    EXPECT_TRUE(resolution.ok());
    EXPECT_TRUE(resolution.missing_required_symbols().empty());
  }
}

TEST(RobloxCapabilityResolverTest,
     InputCanBeEntirelyUnavailableWithoutBlockingStartup) {
  FakeSymbolTable symbols;
  symbols.ResolveAll(false);
  symbols.ExposeRequiredHeadlessStartup();

  const RobloxCapabilityResolution resolution =
      ResolveRobloxCapabilities(symbols.Lookup());

  ASSERT_TRUE(resolution.ok());
  ASSERT_NE(resolution.capabilities(), nullptr);
  const RobloxInputCapabilityFlags& flags =
      resolution.capabilities()->input_flags;
  EXPECT_EQ(flags.gamepad_api, RobloxGamepadApi::kUnsupported);
  EXPECT_FALSE(flags.has_gamepad_events);
  EXPECT_FALSE(flags.has_mouse);
  EXPECT_FALSE(flags.has_touch);
  EXPECT_FALSE(flags.has_hardware_keyboard);
  EXPECT_FALSE(flags.has_text_input);
  EXPECT_FALSE(flags.has_gestures);
  EXPECT_FALSE(flags.has_sensors);
  EXPECT_FALSE(flags.has_tv_remote);
  EXPECT_FALSE(flags.has_screen_orientation);
  EXPECT_FALSE(flags.has_legacy_gamepad_api);
  EXPECT_FALSE(flags.has_ptf_gamepad_api);
}

TEST(RobloxCapabilityResolverTest,
     PartialModernGamepadApiIsNormalizedToUnsupported) {
  const std::vector<std::string> modern_symbols = {
      "Java_com_roblox_engine_jni_NativeInputInterface_"
      "nativeGamepadConnectEventWithGamepadType",
      "Java_com_roblox_engine_jni_NativeInputInterface_"
      "nativeSetGamepadSupportedKeyWithGamepadType",
      "Java_com_roblox_engine_jni_NativeInputInterface_"
      "nativeSetGamepadSupportedMotionWithGamepadType",
  };

  for (const std::string& missing : modern_symbols) {
    SCOPED_TRACE(missing);
    FakeSymbolTable symbols;
    symbols.Hide(missing);

    const RobloxCapabilityResolution resolution =
        ResolveRobloxCapabilities(symbols.Lookup());

    ASSERT_TRUE(resolution.ok());
    ASSERT_NE(resolution.capabilities(), nullptr);
    const RobloxCapabilities& capabilities = *resolution.capabilities();
    EXPECT_EQ(capabilities.input_flags.gamepad_api,
              RobloxGamepadApi::kUnsupported);
    EXPECT_FALSE(capabilities.input_flags.has_gamepad_events);
    EXPECT_EQ(capabilities.input.gamepad_connect_event_with_gamepad_type,
              nullptr);
    EXPECT_EQ(capabilities.input.set_gamepad_supported_key_with_gamepad_type,
              nullptr);
    EXPECT_EQ(capabilities.input.set_gamepad_supported_motion_with_gamepad_type,
              nullptr);
  }
}

TEST(RobloxCapabilityResolverTest, NeverQueriesRemovedLegacyOrPtfFallbacks) {
  FakeSymbolTable symbols;
  symbols.ResolveAll(false);
  symbols.ExposeRequiredHeadlessStartup();
  for (const std::string& removed : RemovedInputSymbolNames()) {
    symbols.Expose(removed);
  }

  const RobloxCapabilityResolution resolution =
      ResolveRobloxCapabilities(symbols.Lookup());

  ASSERT_TRUE(resolution.ok());
  ASSERT_NE(resolution.capabilities(), nullptr);
  EXPECT_EQ(resolution.capabilities()->input_flags.gamepad_api,
            RobloxGamepadApi::kUnsupported);
  EXPECT_FALSE(resolution.capabilities()->input_flags.has_legacy_gamepad_api);
  EXPECT_FALSE(resolution.capabilities()->input_flags.has_ptf_gamepad_api);
  for (const std::string& removed : RemovedInputSymbolNames()) {
    SCOPED_TRACE(removed);
    EXPECT_EQ(std::find(symbols.requests().begin(), symbols.requests().end(),
                        removed),
              symbols.requests().end());
  }
}

TEST(RobloxCapabilityResolverTest, StatusAndGamepadNamesAreStable) {
  EXPECT_STREQ(RobloxCapabilityResolutionStatusName(
                   RobloxCapabilityResolutionStatus::kReady),
               "Ready");
  EXPECT_STREQ(RobloxCapabilityResolutionStatusName(
                   RobloxCapabilityResolutionStatus::kInvalidLookup),
               "InvalidLookup");
  EXPECT_STREQ(RobloxCapabilityResolutionStatusName(
                   RobloxCapabilityResolutionStatus::kMissingRequiredSymbols),
               "MissingRequiredSymbols");
  EXPECT_STREQ(RobloxGamepadApiName(RobloxGamepadApi::kUnsupported),
               "Unsupported");
  EXPECT_STREQ(RobloxGamepadApiName(RobloxGamepadApi::kWithGamepadType),
               "WithGamepadType");
  EXPECT_STREQ(RobloxCapabilityResolutionStatusName(
                   static_cast<RobloxCapabilityResolutionStatus>(255)),
               "Unknown");
  EXPECT_STREQ(RobloxGamepadApiName(static_cast<RobloxGamepadApi>(255)),
               "Unknown");
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
