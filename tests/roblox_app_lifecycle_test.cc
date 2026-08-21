#include "runtime/roblox_app_lifecycle.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

constexpr char kPauseAppSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2PauseApp";
constexpr char kBackgroundModeSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "setTaskSchedulerBackgroundMode";
constexpr char kDestroyAppSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2DestroyApp";

struct Probe {
  std::vector<std::string> calls;
  std::string reason;
  jboolean background_enabled = JNI_FALSE;
  bool fail_string_allocation = false;
};

Probe *g_probe = nullptr;

void Pause(JNIEnv *, jclass) { g_probe->calls.emplace_back("pause"); }

void Background(JNIEnv *env, jclass, jboolean enabled, jstring context) {
  g_probe->calls.emplace_back("background");
  g_probe->background_enabled = enabled;
  const char *text = env->GetStringUTFChars(context, nullptr);
  g_probe->reason = text == nullptr ? "" : text;
  if (text != nullptr) {
    env->ReleaseStringUTFChars(context, text);
  }
}

void Destroy(JNIEnv *, jclass) { g_probe->calls.emplace_back("destroy"); }

jstring NewStringUtf(JNIEnv *, const char *text) {
  if (g_probe->fail_string_allocation) {
    return nullptr;
  }
  return reinterpret_cast<jstring>(const_cast<char *>(text));
}

const char *GetStringUtfChars(JNIEnv *, jstring string, jboolean *) {
  return reinterpret_cast<const char *>(string);
}

void ReleaseStringUtfChars(JNIEnv *, jstring, const char *) {}
void DeleteLocalRef(JNIEnv *, jobject) {}

struct FakeJni {
  FakeJni() {
    functions.NewStringUTF = NewStringUtf;
    functions.GetStringUTFChars = GetStringUtfChars;
    functions.ReleaseStringUTFChars = ReleaseStringUtfChars;
    functions.DeleteLocalRef = DeleteLocalRef;
    env.functions = &functions;
  }

  JNINativeInterface_ functions{};
  JNIEnv_ env{};
};

void *Lookup(void *context, const char *name) {
  const auto *symbols =
      static_cast<const std::unordered_map<std::string, void *> *>(context);
  const auto found = symbols->find(name);
  return found == symbols->end() ? nullptr : found->second;
}

std::unordered_map<std::string, void *> CompleteSymbolMap() {
  return {
      {kPauseAppSymbol, reinterpret_cast<void *>(Pause)},
      {kBackgroundModeSymbol, reinterpret_cast<void *>(Background)},
      {kDestroyAppSymbol, reinterpret_cast<void *>(Destroy)},
  };
}

RobloxAppLifecycleSymbols CompleteSymbols() {
  return {Pause, Background, Destroy};
}

class RobloxAppLifecycleTest : public testing::Test {
protected:
  void SetUp() override { g_probe = &probe_; }
  void TearDown() override { g_probe = nullptr; }

  Probe probe_;
  FakeJni jni_;
  jclass clazz_ = reinterpret_cast<jclass>(0x1234);
};

TEST_F(RobloxAppLifecycleTest, ResolvesAllExportsByStableNames) {
  auto symbols = CompleteSymbolMap();

  const RobloxAppLifecycleResolution result =
      ResolveRobloxAppLifecycleSymbols(RobloxSymbolLookup(Lookup, &symbols));

  ASSERT_TRUE(result.ok());
  ASSERT_NE(result.symbols(), nullptr);
  EXPECT_TRUE(result.symbols()->complete());
  EXPECT_TRUE(result.missing_required_symbols().empty());
}

TEST_F(RobloxAppLifecycleTest, MissingExportFailsClosedWithoutPartialTable) {
  auto symbols = CompleteSymbolMap();
  symbols.erase(kBackgroundModeSymbol);

  const RobloxAppLifecycleResolution result =
      ResolveRobloxAppLifecycleSymbols(RobloxSymbolLookup(Lookup, &symbols));

  EXPECT_EQ(result.status(),
            RobloxAppLifecycleResolutionStatus::kMissingRequiredSymbols);
  EXPECT_EQ(result.symbols(), nullptr);
  ASSERT_EQ(result.missing_required_symbols().size(), 1U);
  EXPECT_EQ(result.missing_required_symbols().front(), kBackgroundModeSymbol);
}

TEST_F(RobloxAppLifecycleTest, InvalidLookupFailsClosed) {
  const RobloxAppLifecycleResolution result =
      ResolveRobloxAppLifecycleSymbols(RobloxSymbolLookup());

  EXPECT_EQ(result.status(),
            RobloxAppLifecycleResolutionStatus::kInvalidLookup);
  EXPECT_EQ(result.symbols(), nullptr);
}

TEST_F(RobloxAppLifecycleTest, ShutdownUsesSupportedOrderAndContext) {
  RobloxAppLifecycle lifecycle(CompleteSymbols());

  const RobloxAppShutdownResult result = lifecycle.Shutdown(&jni_.env, clazz_);

  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.status, RobloxAppShutdownStatus::kStopped);
  const std::vector<std::string> expected = {"pause", "background", "destroy"};
  EXPECT_EQ(probe_.calls, expected);
  EXPECT_EQ(probe_.background_enabled, JNI_TRUE);
  EXPECT_EQ(probe_.reason, "ASMA.stop");
  EXPECT_TRUE(lifecycle.stopped());
}

TEST_F(RobloxAppLifecycleTest, RepeatedShutdownIsIdempotent) {
  RobloxAppLifecycle lifecycle(CompleteSymbols());
  ASSERT_TRUE(lifecycle.Shutdown(&jni_.env, clazz_).ok());

  const RobloxAppShutdownResult repeated =
      lifecycle.Shutdown(&jni_.env, clazz_);

  EXPECT_EQ(repeated.status, RobloxAppShutdownStatus::kAlreadyStopped);
  const std::vector<std::string> expected = {"pause", "background", "destroy"};
  EXPECT_EQ(probe_.calls, expected);
}

TEST_F(RobloxAppLifecycleTest, InvalidJniStateDoesNotStartShutdown) {
  RobloxAppLifecycle lifecycle(CompleteSymbols());

  EXPECT_EQ(lifecycle.Shutdown(nullptr, clazz_).status,
            RobloxAppShutdownStatus::kInvalidJniState);
  EXPECT_EQ(lifecycle.Shutdown(&jni_.env, nullptr).status,
            RobloxAppShutdownStatus::kInvalidJniState);
  EXPECT_TRUE(probe_.calls.empty());
  EXPECT_FALSE(lifecycle.stopped());
}

TEST_F(RobloxAppLifecycleTest, StringAllocationFailureDoesNotPartiallyStop) {
  RobloxAppLifecycle lifecycle(CompleteSymbols());
  probe_.fail_string_allocation = true;

  const RobloxAppShutdownResult result = lifecycle.Shutdown(&jni_.env, clazz_);

  EXPECT_EQ(result.status, RobloxAppShutdownStatus::kReasonAllocationFailed);
  EXPECT_TRUE(probe_.calls.empty());
  EXPECT_FALSE(lifecycle.stopped());
}

TEST_F(RobloxAppLifecycleTest, IncompleteSymbolsNeverInvokeNativeCode) {
  RobloxAppLifecycleSymbols symbols = CompleteSymbols();
  symbols.app_bridge_v2_destroy_app = nullptr;
  RobloxAppLifecycle lifecycle(symbols);

  const RobloxAppShutdownResult result = lifecycle.Shutdown(&jni_.env, clazz_);

  EXPECT_EQ(result.status, RobloxAppShutdownStatus::kMissingRequiredSymbols);
  EXPECT_TRUE(probe_.calls.empty());
}

} // namespace
} // namespace runtime
} // namespace mocktail
