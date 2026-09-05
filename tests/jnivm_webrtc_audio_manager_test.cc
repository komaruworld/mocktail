#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <vector>

#include "jnivm/jnivm.h"

namespace jnivm {
namespace {

constexpr char kManagerClass[] = "org/webrtc/voiceengine/WebRtcAudioManager";
constexpr char kCacheSignature[] = "(IIIZZZZZZZIIJ)V";

struct ManagerProbe {
  WebRtcAudioManagerParameters parameters;
  bool available = true;
  bool init_succeeds = true;
  int cache_calls = 0;
  int init_calls = 0;
  int dispose_calls = 0;
  jobject receiver = nullptr;
  jlong native_handle = 0;
  std::array<jint, 5> cached_format{};
  std::array<jboolean, 7> cached_features{};
  std::vector<bool> mute_calls;
};

void JNICALL CacheParameters(JNIEnv* env, jobject manager, jint rate,
                             jint output_channels, jint input_channels,
                             jboolean aec, jboolean agc, jboolean ns,
                             jboolean low_output, jboolean low_input,
                             jboolean pro, jboolean aaudio, jint output_frames,
                             jint input_frames, jlong native_handle) {
  auto* probe = reinterpret_cast<ManagerProbe*>(native_handle);
  ++probe->cache_calls;
  probe->receiver = manager;
  probe->native_handle = native_handle;
  probe->cached_format = {rate, output_channels, input_channels, output_frames,
                          input_frames};
  probe->cached_features = {aec, agc, ns, low_output, low_input, pro, aaudio};
  // Native callbacks can re-enter JNI. This also checks constructor ordering.
  jclass cls = env->GetObjectClass(manager);
  EXPECT_EQ(env->GetLongField(manager,
                              env->GetFieldID(cls, "nativeAudioManager", "J")),
            native_handle);
  env->DeleteLocalRef(cls);
}

WebRtcAudioManagerCallbacks ProbeCallbacks() {
  WebRtcAudioManagerCallbacks callbacks;
  callbacks.get_parameters = [](void* context,
                                WebRtcAudioManagerParameters* parameters) {
    auto* probe = static_cast<ManagerProbe*>(context);
    *parameters = probe->parameters;
    return probe->available;
  };
  callbacks.init = [](void* context, const void*) {
    auto* probe = static_cast<ManagerProbe*>(context);
    ++probe->init_calls;
    return probe->init_succeeds;
  };
  callbacks.dispose = [](void* context, const void*) {
    ++static_cast<ManagerProbe*>(context)->dispose_calls;
  };
  callbacks.set_microphone_mute = [](void* context, bool muted) {
    static_cast<ManagerProbe*>(context)->mute_calls.push_back(muted);
  };
  return callbacks;
}

enum class JniRoute { kDirect, kVariadic, kArray };

class JniVmWebRtcAudioManagerTest : public testing::TestWithParam<JniRoute> {
 protected:
  void SetUp() override {
    env_ = vm_.GetJNIEnv();
    cls_ = env_->FindClass(kManagerClass);
    probe_ = std::make_shared<ManagerProbe>();
    probe_->parameters.sample_rate_hz = 48000;
    probe_->parameters.output_channels = 2;
    probe_->parameters.input_channels = 1;
    probe_->parameters.output_buffer_size_frames = 960;
    probe_->parameters.input_buffer_size_frames = 480;
    vm_.SetWebRtcAudioManagerCallbacks(probe_, ProbeCallbacks());
    RegisterCache(kCacheSignature);
  }

  void RegisterCache(const char* signature) {
    JNINativeMethod method = {const_cast<char*>("nativeCacheAudioParameters"),
                              const_cast<char*>(signature),
                              reinterpret_cast<void*>(&CacheParameters)};
    ASSERT_EQ(env_->RegisterNatives(cls_, &method, 1), JNI_OK);
  }

  jobject Construct(jlong handle, const char* signature = "(J)V") {
    jmethodID ctor = env_->GetMethodID(cls_, "<init>", signature);
    if (GetParam() == JniRoute::kDirect) {
      return env_->functions->NewObject(env_, cls_, ctor, handle);
    }
    if (GetParam() == JniRoute::kVariadic) {
      return env_->NewObject(cls_, ctor, handle);
    }
    jvalue argument{};
    argument.j = handle;
    return env_->NewObjectA(cls_, ctor, &argument);
  }

  jobject Construct() {
    return Construct(reinterpret_cast<jlong>(probe_.get()));
  }

  jboolean Boolean(jobject manager, const char* name) {
    jmethodID method = env_->GetMethodID(cls_, name, "()Z");
    if (GetParam() == JniRoute::kDirect) {
      return env_->functions->CallBooleanMethod(env_, manager, method);
    }
    if (GetParam() == JniRoute::kVariadic) {
      return env_->CallBooleanMethod(manager, method);
    }
    return env_->CallBooleanMethodA(manager, method, nullptr);
  }

  void Void(jobject manager, const char* name, const char* signature = "()V",
            jboolean value = JNI_FALSE) {
    jmethodID method = env_->GetMethodID(cls_, name, signature);
    if (GetParam() == JniRoute::kDirect) {
      env_->functions->CallVoidMethod(env_, manager, method,
                                      static_cast<jint>(value));
    } else if (GetParam() == JniRoute::kVariadic) {
      env_->CallVoidMethod(manager, method, static_cast<jint>(value));
    } else {
      jvalue argument{};
      argument.z = value;
      env_->CallVoidMethodA(manager, method, &argument);
    }
  }

  VM vm_;
  JNIEnv* env_ = nullptr;
  jclass cls_ = nullptr;
  std::shared_ptr<ManagerProbe> probe_;
};

TEST_P(JniVmWebRtcAudioManagerTest,
       ConstructorCachesExactParametersBeforeInit) {
  // Alternating bits catch swapped/missing booleans in the long JNI signature.
  probe_->parameters.hardware_aec = true;
  probe_->parameters.hardware_ns = true;
  probe_->parameters.low_latency_input = true;
  probe_->parameters.aaudio = true;
  jobject manager = Construct();
  ASSERT_NE(manager, nullptr);
  EXPECT_EQ(probe_->cache_calls, 1);
  EXPECT_EQ(probe_->init_calls, 0);
  EXPECT_EQ(probe_->receiver, manager);
  EXPECT_EQ(probe_->native_handle, reinterpret_cast<jlong>(probe_.get()));
  EXPECT_EQ(probe_->cached_format,
            (std::array<jint, 5>{48000, 2, 1, 960, 480}));
  EXPECT_EQ(probe_->cached_features,
            (std::array<jboolean, 7>{1, 0, 1, 0, 1, 0, 1}));
  EXPECT_EQ(Boolean(manager, "init"), JNI_TRUE);
  EXPECT_EQ(Boolean(manager, "init"), JNI_TRUE);
  EXPECT_EQ(probe_->init_calls, 1);
  EXPECT_EQ(probe_->cache_calls, 1);
  EXPECT_EQ(Boolean(manager, "isCommunicationModeEnabled"), JNI_TRUE);
  EXPECT_EQ(Boolean(manager, "isLowLatencyOutputSupported"), JNI_FALSE);
  EXPECT_EQ(Boolean(manager, "isLowLatencyInputSupported"), JNI_TRUE);
  EXPECT_EQ(Boolean(manager, "isAAudioSupported"), JNI_TRUE);
  EXPECT_EQ(Boolean(manager, "isDeviceBlacklistedForOpenSLESUsage"), JNI_TRUE);
}

TEST_P(JniVmWebRtcAudioManagerTest,
       DisposeIsIdempotentAndAllowsReinitialization) {
  jobject manager = Construct();
  ASSERT_EQ(Boolean(manager, "init"), JNI_TRUE);
  Void(manager, "dispose");
  Void(manager, "dispose");
  EXPECT_EQ(probe_->dispose_calls, 1);
  EXPECT_EQ(Boolean(manager, "isCommunicationModeEnabled"), JNI_FALSE);
  EXPECT_EQ(Boolean(manager, "init"), JNI_TRUE);
  EXPECT_EQ(probe_->init_calls, 2);
  EXPECT_EQ(probe_->cache_calls, 1);
}

TEST_P(JniVmWebRtcAudioManagerTest, ForwardsMuteAndUnmute) {
  jobject manager = Construct();
  ASSERT_EQ(Boolean(manager, "init"), JNI_TRUE);
  Void(manager, "setMicrophoneMute", "(Z)V", JNI_TRUE);
  Void(manager, "setMicrophoneMute", "(Z)V", JNI_FALSE);
  EXPECT_EQ(probe_->mute_calls, (std::vector<bool>{true, false}));
}

TEST_P(JniVmWebRtcAudioManagerTest, FailedHostInitDoesNotReportSuccess) {
  probe_->init_succeeds = false;
  jobject manager = Construct();
  EXPECT_EQ(Boolean(manager, "init"), JNI_FALSE);
  Void(manager, "dispose");
  EXPECT_EQ(probe_->dispose_calls, 0);
  probe_->init_succeeds = true;
  EXPECT_EQ(Boolean(manager, "init"), JNI_TRUE);
}

TEST_P(JniVmWebRtcAudioManagerTest, MissingHostAndInvalidParametersFailClosed) {
  probe_->available = false;
  jobject unavailable = Construct();
  EXPECT_EQ(Boolean(unavailable, "init"), JNI_FALSE);
  probe_->available = true;
  probe_->parameters.input_channels = 0;
  jobject invalid = Construct();
  EXPECT_EQ(Boolean(invalid, "init"), JNI_FALSE);
  probe_->parameters.input_channels = 1;
  probe_->parameters.input_buffer_size_frames = 0;
  EXPECT_EQ(Boolean(Construct(), "init"), JNI_FALSE);
  vm_.ClearWebRtcAudioManagerCallbacks();
  EXPECT_EQ(Boolean(Construct(), "init"), JNI_FALSE);
  EXPECT_EQ(probe_->cache_calls, 0);
  EXPECT_EQ(probe_->init_calls, 0);
}

TEST_P(JniVmWebRtcAudioManagerTest, NullHandleAndWrongConstructorDoNotCache) {
  EXPECT_EQ(Boolean(Construct(0), "init"), JNI_FALSE);
  EXPECT_EQ(
      Boolean(Construct(reinterpret_cast<jlong>(probe_.get()), "()V"), "init"),
      JNI_FALSE);
  EXPECT_EQ(probe_->cache_calls, 0);
}

TEST_P(JniVmWebRtcAudioManagerTest, ClearingBindingStopsFurtherDispatch) {
  jobject manager = Construct();
  ASSERT_EQ(Boolean(manager, "init"), JNI_TRUE);
  vm_.ClearWebRtcAudioManagerCallbacks();
  EXPECT_EQ(Boolean(manager, "init"), JNI_FALSE);
  Void(manager, "setMicrophoneMute", "(Z)V", JNI_TRUE);
  Void(manager, "dispose");
  EXPECT_TRUE(probe_->mute_calls.empty());
  EXPECT_EQ(probe_->dispose_calls, 0);
  EXPECT_EQ(probe_->init_calls, 1);
}

TEST_P(JniVmWebRtcAudioManagerTest,
       UnrelatedClassAndWrongSignaturesAreNotHandled) {
  jclass other_class = env_->FindClass("com/example/AudioManager");
  jobject other = env_->NewObject(
      other_class, env_->GetMethodID(other_class, "<init>", "()V"));
  EXPECT_EQ(Boolean(other, "init"), JNI_FALSE);
  jobject manager = Construct();
  jmethodID wrong_init = env_->GetMethodID(cls_, "init", "(I)Z");
  EXPECT_EQ(env_->CallBooleanMethod(manager, wrong_init, 1), JNI_FALSE);
  Void(manager, "setMicrophoneMute", "(I)V", JNI_TRUE);
  EXPECT_TRUE(probe_->mute_calls.empty());
  EXPECT_EQ(probe_->init_calls, 0);
}

TEST(JniVmWebRtcAudioManagerRegistrationTest,
     RejectsWrongClassAndNativeSignature) {
  VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  auto probe = std::make_shared<ManagerProbe>();
  probe->parameters.sample_rate_hz = 48000;
  probe->parameters.input_channels = probe->parameters.output_channels = 1;
  probe->parameters.input_buffer_size_frames =
      probe->parameters.output_buffer_size_frames = 480;
  vm.SetWebRtcAudioManagerCallbacks(probe, ProbeCallbacks());
  jclass cls = env->FindClass(kManagerClass);
  jclass other = env->FindClass("com/example/AudioManager");
  JNINativeMethod native = {const_cast<char*>("nativeCacheAudioParameters"),
                            const_cast<char*>(kCacheSignature),
                            reinterpret_cast<void*>(&CacheParameters)};
  ASSERT_EQ(env->RegisterNatives(other, &native, 1), JNI_OK);
  native.signature = const_cast<char*>("(IIIJ)V");
  ASSERT_EQ(env->RegisterNatives(cls, &native, 1), JNI_OK);
  jobject manager = env->NewObject(cls, env->GetMethodID(cls, "<init>", "(J)V"),
                                   reinterpret_cast<jlong>(probe.get()));
  EXPECT_EQ(
      env->CallBooleanMethod(manager, env->GetMethodID(cls, "init", "()Z")),
      JNI_FALSE);
  EXPECT_EQ(probe->cache_calls, 0);
}

INSTANTIATE_TEST_SUITE_P(AllJniRoutes, JniVmWebRtcAudioManagerTest,
                         testing::Values(JniRoute::kDirect, JniRoute::kVariadic,
                                         JniRoute::kArray),
                         [](const testing::TestParamInfo<JniRoute>& info) {
                           switch (info.param) {
                             case JniRoute::kDirect:
                               return "Direct";
                             case JniRoute::kVariadic:
                               return "Variadic";
                             case JniRoute::kArray:
                               return "Array";
                           }
                           return "Unknown";
                         });

}  // namespace
}  // namespace jnivm
