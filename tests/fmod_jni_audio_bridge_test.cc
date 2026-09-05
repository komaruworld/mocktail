#include "mocktail/audio/fmod_jni_audio_bridge.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "jnivm/jnivm.h"
#include "mocktail/audio/sdl_audio_capture.h"
#include "mocktail/audio/sdl_audio_sink.h"
#include "mocktail/audio/webrtc_jni_audio_bridge.h"

namespace mocktail::audio {
namespace {

class ScopedEnvironment final {
public:
  ScopedEnvironment(const char *name, const char *value) : name_(name) {
    const char *current = std::getenv(name);
    if (current != nullptr) {
      had_value_ = true;
      previous_ = current;
    }
    setenv(name, value, 1);
  }

  ~ScopedEnvironment() {
    if (had_value_) {
      setenv(name_.c_str(), previous_.c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ScopedEnvironment(const ScopedEnvironment &) = delete;
  ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;

private:
  std::string name_;
  std::string previous_;
  bool had_value_ = false;
};

class ScopedSdlAudioShutdown final {
public:
  ~ScopedSdlAudioShutdown() { (void)ShutdownSdlAudioSubsystem(); }
};

class ScopedSdlHint final {
 public:
  ScopedSdlHint(const char* name, const char* value) : name_(name) {
    const char* previous = SDL_GetHint(name);
    had_value_ = previous != nullptr;
    if (had_value_) previous_ = previous;
    EXPECT_TRUE(SDL_SetHintWithPriority(name, value, SDL_HINT_OVERRIDE));
  }
  ~ScopedSdlHint() {
    if (had_value_) {
      SDL_SetHintWithPriority(name_.c_str(), previous_.c_str(),
                              SDL_HINT_OVERRIDE);
    } else {
      SDL_ResetHint(name_.c_str());
    }
  }

 private:
  std::string name_;
  std::string previous_;
  bool had_value_ = false;
};

class ScopedWebRtcBridgeShutdown final {
 public:
  explicit ScopedWebRtcBridgeShutdown(jnivm::VM* vm) : vm_(vm) {}
  ~ScopedWebRtcBridgeShutdown() {
    if (vm_ != nullptr) (void)ShutdownWebRtcJniAudioBridge(vm_);
  }
  void Release() { vm_ = nullptr; }

 private:
  jnivm::VM* vm_;
};

struct WebRtcManagerProbe {
  int callbacks = 0;
  jnivm::WebRtcAudioManagerParameters parameters;
};

void JNICALL CacheWebRtcParameters(JNIEnv*, jobject, jint rate,
                                   jint output_channels, jint input_channels,
                                   jboolean aec, jboolean agc, jboolean ns,
                                   jboolean low_output, jboolean low_input,
                                   jboolean pro, jboolean aaudio,
                                   jint output_frames, jint input_frames,
                                   jlong native_manager) {
  auto* probe = reinterpret_cast<WebRtcManagerProbe*>(native_manager);
  ++probe->callbacks;
  probe->parameters = {
      rate,     output_channels, input_channels,  aec != 0,
      agc != 0, ns != 0,         low_output != 0, low_input != 0,
      pro != 0, aaudio != 0,     output_frames,   input_frames};
}

jobject CreateWebRtcManager(JNIEnv* env, WebRtcManagerProbe* probe) {
  jclass cls = env->FindClass("org/webrtc/voiceengine/WebRtcAudioManager");
  const JNINativeMethod cache = {
      const_cast<char*>("nativeCacheAudioParameters"),
      const_cast<char*>("(IIIZZZZZZZIIJ)V"),
      reinterpret_cast<void*>(&CacheWebRtcParameters)};
  EXPECT_EQ(env->RegisterNatives(cls, &cache, 1), JNI_OK);
  return env->NewObject(cls, env->GetMethodID(cls, "<init>", "(J)V"),
                        reinterpret_cast<jlong>(probe));
}

struct WebRtcCaptureProbe {
  std::mutex mutex;
  std::condition_variable cv;
  void *buffer = nullptr;
  jlong capacity = -1;
  int callbacks = 0;
  int last_size = 0;
  int non_silent_callbacks = 0;
  int silent_callbacks = 0;
};

struct WebRtcPlayoutProbe {
  std::mutex mutex;
  std::condition_variable cv;
  void *buffer = nullptr;
  jlong capacity = -1;
  int callbacks = 0;
  int last_size = 0;
  bool invalid_buffer = false;
};

void JNICALL CacheWebRtcBuffer(JNIEnv *env, jobject, jobject buffer,
                               jlong native_audio_record) {
  auto *probe = reinterpret_cast<WebRtcCaptureProbe *>(native_audio_record);
  std::lock_guard<std::mutex> lock(probe->mutex);
  probe->buffer = env->GetDirectBufferAddress(buffer);
  probe->capacity = env->GetDirectBufferCapacity(buffer);
}

void JNICALL WebRtcDataIsRecorded(JNIEnv *, jobject, jint size,
                                  jlong native_audio_record) {
  auto *probe = reinterpret_cast<WebRtcCaptureProbe *>(native_audio_record);
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    ++probe->callbacks;
    probe->last_size = size;
    if (probe->buffer != nullptr && size > 0 && size <= probe->capacity) {
      const auto* bytes = static_cast<const unsigned char*>(probe->buffer);
      const bool silent =
          std::all_of(bytes, bytes + size,
                      [](unsigned char sample) { return sample == 0; });
      if (silent)
        ++probe->silent_callbacks;
      else
        ++probe->non_silent_callbacks;
    }
  }
  probe->cv.notify_all();
}

void JNICALL CacheWebRtcPlayoutBuffer(JNIEnv *env, jobject, jobject buffer,
                                      jlong native_audio_track) {
  auto *probe = reinterpret_cast<WebRtcPlayoutProbe *>(native_audio_track);
  std::lock_guard<std::mutex> lock(probe->mutex);
  probe->buffer = env->GetDirectBufferAddress(buffer);
  probe->capacity = env->GetDirectBufferCapacity(buffer);
}

void JNICALL WebRtcGetPlayoutData(JNIEnv *, jobject, jint size,
                                  jlong native_audio_track) {
  auto *probe = reinterpret_cast<WebRtcPlayoutProbe *>(native_audio_track);
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    if (probe->buffer == nullptr || size <= 0 || size > probe->capacity) {
      probe->invalid_buffer = true;
    } else {
      std::memset(probe->buffer, 0x35, static_cast<std::size_t>(size));
    }
    ++probe->callbacks;
    probe->last_size = size;
  }
  probe->cv.notify_all();
}

TEST(FmodJniAudioBridgeTest, RunsExactJavaContractThroughSdlSink) {
  unsetenv("MOCKTAIL_AUDIO_OUTPUT_DEVICE");
  jnivm::VM vm;
  const Status install_status = InstallFmodJniAudioBridge(&vm);
  ASSERT_TRUE(install_status.ok()) << install_status.message();

  JNIEnv* env = vm.GetJNIEnv();
  jclass device_class = env->FindClass("org/fmod/AudioDevice");
  ASSERT_NE(device_class, nullptr);
  const jmethodID constructor =
      env->GetMethodID(device_class, "<init>", "()V");
  const jmethodID init =
      env->GetMethodID(device_class, "init", "(IIII)Z");
  const jmethodID write =
      env->GetMethodID(device_class, "write", "([BI)V");
  const jmethodID close = env->GetMethodID(device_class, "close", "()V");
  jobject device = env->NewObject(device_class, constructor);
  ASSERT_NE(device, nullptr);

  ASSERT_EQ(env->CallBooleanMethod(device, init, 2, 48000, 256, 4),
            JNI_TRUE);
  std::vector<jbyte> pcm(256U * 2U * sizeof(std::int16_t), 0);
  jbyteArray array = env->NewByteArray(static_cast<jsize>(pcm.size()));
  ASSERT_NE(array, nullptr);
  env->SetByteArrayRegion(array, 0, static_cast<jsize>(pcm.size()),
                          pcm.data());
  for (int block = 0; block < 4; ++block) {
    env->CallVoidMethod(device, write, array, static_cast<jint>(pcm.size()));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  env->CallVoidMethod(device, close);
  const Status shutdown_status = ShutdownFmodJniAudioBridge(&vm);
  EXPECT_TRUE(shutdown_status.ok()) << shutdown_status.message();
}

TEST(FmodJniAudioBridgeTest, RejectsUnavailableConfiguredOutput) {
  ASSERT_EQ(setenv("MOCKTAIL_AUDIO_OUTPUT_DEVICE",
                   "Mocktail device that cannot exist", 1),
            0);
  jnivm::VM vm;
  const Status status = InstallFmodJniAudioBridge(&vm);
  EXPECT_EQ(status.code(), StatusCode::kUnavailable);
  EXPECT_NE(status.message().find("unavailable"), std::string::npos);
  if (status.ok()) {
    EXPECT_TRUE(ShutdownFmodJniAudioBridge(&vm).ok());
  }
  unsetenv("MOCKTAIL_AUDIO_OUTPUT_DEVICE");
}

TEST(WebRtcJniAudioBridgeTest, CapturesTenMillisecondFramesThroughExactJni) {
  unsetenv("MOCKTAIL_AUDIO_INPUT_DEVICE");
  ScopedSdlAudioShutdown shutdown;
  jnivm::VM vm;
  const Status install_status = InstallWebRtcJniAudioBridge(&vm);
  ASSERT_TRUE(install_status.ok()) << install_status.message();

  JNIEnv *env = vm.GetJNIEnv();
  jclass recorder_class =
      env->FindClass("org/webrtc/voiceengine/WebRtcAudioRecord");
  ASSERT_NE(recorder_class, nullptr);
  const JNINativeMethod native_methods[] = {
      {const_cast<char *>("nativeCacheDirectBufferAddress"),
       const_cast<char *>("(Ljava/nio/ByteBuffer;J)V"),
       reinterpret_cast<void *>(&CacheWebRtcBuffer)},
      {const_cast<char *>("nativeDataIsRecorded"), const_cast<char *>("(IJ)V"),
       reinterpret_cast<void *>(&WebRtcDataIsRecorded)},
  };
  ASSERT_EQ(env->RegisterNatives(recorder_class, native_methods, 2), JNI_OK);

  const jmethodID constructor =
      env->GetMethodID(recorder_class, "<init>", "(J)V");
  const jmethodID init =
      env->GetMethodID(recorder_class, "initRecording", "(II)I");
  const jmethodID start =
      env->GetMethodID(recorder_class, "startRecording", "()Z");
  const jmethodID stop =
      env->GetMethodID(recorder_class, "stopRecording", "()Z");
  WebRtcCaptureProbe probe;
  jobject recorder = env->NewObject(recorder_class, constructor,
                                    reinterpret_cast<jlong>(&probe));
  ASSERT_NE(recorder, nullptr);

  EXPECT_EQ(env->CallIntMethod(recorder, init, 48000, 1), 480);
  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    EXPECT_NE(probe.buffer, nullptr);
    EXPECT_EQ(probe.capacity, 480 * static_cast<jlong>(sizeof(std::int16_t)));
  }
  ASSERT_EQ(env->CallBooleanMethod(recorder, start), JNI_TRUE);
  {
    std::unique_lock<std::mutex> lock(probe.mutex);
    ASSERT_TRUE(probe.cv.wait_for(lock, std::chrono::seconds(2),
                                  [&probe] { return probe.callbacks > 0; }));
    EXPECT_EQ(probe.last_size, probe.capacity);
  }
  EXPECT_EQ(env->CallBooleanMethod(recorder, stop), JNI_TRUE);

  EXPECT_TRUE(ShutdownWebRtcJniAudioBridge(&vm).ok());
  EXPECT_TRUE(ShutdownSdlAudioSubsystem().ok());
}

TEST(WebRtcJniAudioBridgeTest, DisabledMicrophoneRejectsRecordingInit) {
  ScopedEnvironment input("MOCKTAIL_AUDIO_INPUT_DEVICE", "disabled");
  ScopedSdlAudioShutdown shutdown;
  jnivm::VM vm;
  const Status install_status = InstallWebRtcJniAudioBridge(&vm);
  ASSERT_TRUE(install_status.ok()) << install_status.message();

  JNIEnv *env = vm.GetJNIEnv();
  WebRtcManagerProbe manager_probe;
  jobject manager = CreateWebRtcManager(env, &manager_probe);
  jclass manager_class = env->GetObjectClass(manager);
  EXPECT_EQ(manager_probe.callbacks, 1);
  EXPECT_EQ(env->CallBooleanMethod(
                manager, env->GetMethodID(manager_class, "init", "()Z")),
            JNI_TRUE);
  jclass recorder_class =
      env->FindClass("org/webrtc/voiceengine/WebRtcAudioRecord");
  ASSERT_NE(recorder_class, nullptr);
  const jmethodID constructor =
      env->GetMethodID(recorder_class, "<init>", "(J)V");
  const jmethodID init =
      env->GetMethodID(recorder_class, "initRecording", "(II)I");
  ASSERT_NE(constructor, nullptr);
  ASSERT_NE(init, nullptr);

  jobject recorder = env->NewObject(recorder_class, constructor, 1LL);
  ASSERT_NE(recorder, nullptr);
  EXPECT_EQ(env->CallIntMethod(recorder, init, 48000, 1), -1);
  EXPECT_TRUE(ShutdownWebRtcJniAudioBridge(&vm).ok());
}

TEST(WebRtcJniAudioBridgeTest, PlaysTenMillisecondFramesThroughExactJni) {
  ScopedEnvironment input("MOCKTAIL_AUDIO_INPUT_DEVICE", "disabled");
  ScopedSdlAudioShutdown shutdown;
  jnivm::VM vm;
  const Status install_status = InstallWebRtcJniAudioBridge(&vm);
  ASSERT_TRUE(install_status.ok()) << install_status.message();

  JNIEnv *env = vm.GetJNIEnv();
  jclass track_class =
      env->FindClass("org/webrtc/voiceengine/WebRtcAudioTrack");
  ASSERT_NE(track_class, nullptr);
  const JNINativeMethod native_methods[] = {
      {const_cast<char *>("nativeCacheDirectBufferAddress"),
       const_cast<char *>("(Ljava/nio/ByteBuffer;J)V"),
       reinterpret_cast<void *>(&CacheWebRtcPlayoutBuffer)},
      {const_cast<char *>("nativeGetPlayoutData"), const_cast<char *>("(IJ)V"),
       reinterpret_cast<void *>(&WebRtcGetPlayoutData)},
  };
  ASSERT_EQ(env->RegisterNatives(track_class, native_methods, 2), JNI_OK);

  const jmethodID constructor = env->GetMethodID(track_class, "<init>", "(J)V");
  const jmethodID init = env->GetMethodID(track_class, "initPlayout", "(IID)I");
  const jmethodID buffer_size =
      env->GetMethodID(track_class, "getBufferSizeInFrames", "()I");
  const jmethodID max_volume =
      env->GetMethodID(track_class, "getStreamMaxVolume", "()I");
  const jmethodID volume =
      env->GetMethodID(track_class, "getStreamVolume", "()I");
  const jmethodID set_volume =
      env->GetMethodID(track_class, "setStreamVolume", "(I)Z");
  const jmethodID start = env->GetMethodID(track_class, "startPlayout", "()Z");
  const jmethodID stop = env->GetMethodID(track_class, "stopPlayout", "()Z");
  WebRtcPlayoutProbe probe;
  jobject track =
      env->NewObject(track_class, constructor, reinterpret_cast<jlong>(&probe));
  ASSERT_NE(track, nullptr);

  const jint android_buffer_bytes =
      env->CallIntMethod(track, init, 48000, 2, 1.0);
  EXPECT_GE(android_buffer_bytes,
            480 * 2 * static_cast<jint>(sizeof(std::int16_t)));
  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    EXPECT_NE(probe.buffer, nullptr);
    EXPECT_EQ(probe.capacity,
              480 * 2 * static_cast<jlong>(sizeof(std::int16_t)));
  }
  EXPECT_GE(env->CallIntMethod(track, buffer_size), 480);
  EXPECT_EQ(env->CallIntMethod(track, max_volume), 100);
  EXPECT_EQ(env->CallIntMethod(track, volume), 100);
  EXPECT_EQ(env->CallBooleanMethod(track, set_volume, 50), JNI_TRUE);

  ASSERT_EQ(env->CallBooleanMethod(track, start), JNI_TRUE);
  {
    std::unique_lock<std::mutex> lock(probe.mutex);
    ASSERT_TRUE(probe.cv.wait_for(lock, std::chrono::seconds(2),
                                  [&probe] { return probe.callbacks >= 3; }));
    EXPECT_FALSE(probe.invalid_buffer);
    EXPECT_EQ(probe.last_size, probe.capacity);
  }
  EXPECT_EQ(env->CallBooleanMethod(track, stop), JNI_TRUE);

  EXPECT_TRUE(ShutdownWebRtcJniAudioBridge(&vm).ok());
  EXPECT_TRUE(ShutdownSdlAudioSubsystem().ok());
}

TEST(WebRtcJniAudioBridgeTest,
     StartsDuplexAudioFromManagerParametersAndReopens) {
  ScopedEnvironment input("MOCKTAIL_AUDIO_INPUT_DEVICE", "default");
  ScopedSdlAudioShutdown shutdown;
  jnivm::VM vm;
  ASSERT_TRUE(InstallWebRtcJniAudioBridge(&vm).ok());
  JNIEnv* env = vm.GetJNIEnv();
  // Repeat the complete object lifecycle, as when leaving and rejoining VC.
  for (int iteration = 0; iteration != 2; ++iteration) {
    WebRtcManagerProbe manager_probe;
    WebRtcCaptureProbe capture_probe;
    WebRtcPlayoutProbe playout_probe;
    ScopedWebRtcBridgeShutdown close_on_failure(&vm);
    jobject manager = CreateWebRtcManager(env, &manager_probe);
    ASSERT_NE(manager, nullptr);
    ASSERT_EQ(manager_probe.callbacks, 1);
    const auto& format = manager_probe.parameters;
    EXPECT_EQ(format.sample_rate_hz, 48000);
    EXPECT_EQ(format.input_buffer_size_frames, format.sample_rate_hz / 100);
    EXPECT_FALSE(format.hardware_aec || format.hardware_agc ||
                 format.hardware_ns || format.low_latency_output ||
                 format.low_latency_input || format.pro_audio || format.aaudio);
    jclass manager_class = env->GetObjectClass(manager);
    ASSERT_EQ(env->CallBooleanMethod(
                  manager, env->GetMethodID(manager_class, "init", "()Z")),
              JNI_TRUE);

    jclass record_class =
        env->FindClass("org/webrtc/voiceengine/WebRtcAudioRecord");
    jclass track_class =
        env->FindClass("org/webrtc/voiceengine/WebRtcAudioTrack");
    const JNINativeMethod record_methods[] = {
        {const_cast<char*>("nativeCacheDirectBufferAddress"),
         const_cast<char*>("(Ljava/nio/ByteBuffer;J)V"),
         reinterpret_cast<void*>(&CacheWebRtcBuffer)},
        {const_cast<char*>("nativeDataIsRecorded"), const_cast<char*>("(IJ)V"),
         reinterpret_cast<void*>(&WebRtcDataIsRecorded)}};
    const JNINativeMethod track_methods[] = {
        {const_cast<char*>("nativeCacheDirectBufferAddress"),
         const_cast<char*>("(Ljava/nio/ByteBuffer;J)V"),
         reinterpret_cast<void*>(&CacheWebRtcPlayoutBuffer)},
        {const_cast<char*>("nativeGetPlayoutData"), const_cast<char*>("(IJ)V"),
         reinterpret_cast<void*>(&WebRtcGetPlayoutData)}};
    ASSERT_EQ(env->RegisterNatives(record_class, record_methods, 2), JNI_OK);
    ASSERT_EQ(env->RegisterNatives(track_class, track_methods, 2), JNI_OK);
    jobject record = env->NewObject(
        record_class, env->GetMethodID(record_class, "<init>", "(J)V"),
        reinterpret_cast<jlong>(&capture_probe));
    jobject track = env->NewObject(
        track_class, env->GetMethodID(track_class, "<init>", "(J)V"),
        reinterpret_cast<jlong>(&playout_probe));
    ASSERT_EQ(
        env->CallIntMethod(
            record, env->GetMethodID(record_class, "initRecording", "(II)I"),
            format.sample_rate_hz, format.input_channels),
        format.input_buffer_size_frames);
    ASSERT_GT(env->CallIntMethod(
                  track, env->GetMethodID(track_class, "initPlayout", "(IID)I"),
                  format.sample_rate_hz, format.output_channels, 1.0),
              0);
    ASSERT_EQ(
        env->CallBooleanMethod(
            record, env->GetMethodID(record_class, "startRecording", "()Z")),
        JNI_TRUE);
    ASSERT_EQ(env->CallBooleanMethod(
                  track, env->GetMethodID(track_class, "startPlayout", "()Z")),
              JNI_TRUE);
    {
      std::unique_lock<std::mutex> lock(capture_probe.mutex);
      ASSERT_TRUE(capture_probe.cv.wait_for(lock, std::chrono::seconds(2), [&] {
        return capture_probe.callbacks >= 3;
      }));
      EXPECT_EQ(capture_probe.last_size,
                format.input_buffer_size_frames * format.input_channels * 2);
    }
    {
      std::unique_lock<std::mutex> lock(playout_probe.mutex);
      ASSERT_TRUE(playout_probe.cv.wait_for(lock, std::chrono::seconds(2), [&] {
        return playout_probe.callbacks >= 3;
      }));
      EXPECT_EQ(playout_probe.last_size,
                format.output_buffer_size_frames * format.output_channels * 2);
      EXPECT_FALSE(playout_probe.invalid_buffer);
    }
    ASSERT_EQ(
        env->CallBooleanMethod(
            record, env->GetMethodID(record_class, "stopRecording", "()Z")),
        JNI_TRUE);
    ASSERT_EQ(env->CallBooleanMethod(
                  track, env->GetMethodID(track_class, "stopPlayout", "()Z")),
              JNI_TRUE);
    env->CallVoidMethod(manager,
                        env->GetMethodID(manager_class, "dispose", "()V"));
    env->DeleteLocalRef(record);
    env->DeleteLocalRef(track);
    env->DeleteLocalRef(manager);
    close_on_failure.Release();
  }
  ASSERT_TRUE(ShutdownWebRtcJniAudioBridge(&vm).ok());
}

TEST(WebRtcJniAudioBridgeTest,
     ManagerMuteSilencesNonzeroPcmAndUnmuteRestoresIt) {
  // Read synthetic PCM through SDL's disk driver, never a physical microphone.
  // A nonzero source is essential: dummy-driver silence cannot test muting.
  bool have_disk_driver = false;
  for (int index = 0; index < SDL_GetNumAudioDrivers(); ++index) {
    if (std::strcmp(SDL_GetAudioDriver(index), "disk") == 0)
      have_disk_driver = true;
  }
  if (!have_disk_driver) GTEST_SKIP() << "SDL disk audio driver is not built";
  ASSERT_TRUE(ShutdownSdlAudioSubsystem().ok());
  struct TemporaryPcm {
    char path[40] = "/tmp/mocktail-webrtc-pcm-XXXXXX";
    ~TemporaryPcm() {
      if (created) std::remove(path);
    }
    bool created = false;
  } pcm;
  const int fd = mkstemp(pcm.path);
  ASSERT_GE(fd, 0);
  pcm.created = true;
  FILE* file = fdopen(fd, "wb");
  if (file == nullptr) close(fd);
  ASSERT_NE(file, nullptr);
  // 0x3f is nonzero in both the S16 stream format and SDL's F32 device format.
  const std::vector<unsigned char> samples(4 * 1024 * 1024, 0x3f);
  const std::size_t written =
      std::fwrite(samples.data(), 1, samples.size(), file);
  const int closed = std::fclose(file);
  ASSERT_EQ(written, samples.size());
  ASSERT_EQ(closed, 0);
  // SDL caches environment values, so set hints explicitly when switching
  // drivers within a test process that already initialized dummy audio.
  ScopedSdlHint driver(SDL_HINT_AUDIO_DRIVER, "disk");
  ScopedSdlHint disk_input(SDL_HINT_AUDIO_DISK_INPUT_FILE, pcm.path);
  ScopedSdlHint disk_output(SDL_HINT_AUDIO_DISK_OUTPUT_FILE, "/dev/null");
  ScopedEnvironment input("MOCKTAIL_AUDIO_INPUT_DEVICE", "default");
  ScopedSdlAudioShutdown shutdown;
  jnivm::VM vm;
  WebRtcManagerProbe manager_probe;
  WebRtcCaptureProbe capture_probe;
  ScopedWebRtcBridgeShutdown close_before_probes(&vm);
  ASSERT_TRUE(InstallWebRtcJniAudioBridge(&vm).ok());
  ASSERT_STREQ(SDL_GetCurrentAudioDriver(), "disk");
  JNIEnv* env = vm.GetJNIEnv();
  jobject manager = CreateWebRtcManager(env, &manager_probe);
  jclass manager_class = env->GetObjectClass(manager);
  ASSERT_EQ(env->CallBooleanMethod(
                manager, env->GetMethodID(manager_class, "init", "()Z")),
            JNI_TRUE);
  jclass record_class =
      env->FindClass("org/webrtc/voiceengine/WebRtcAudioRecord");
  const JNINativeMethod methods[] = {
      {const_cast<char*>("nativeCacheDirectBufferAddress"),
       const_cast<char*>("(Ljava/nio/ByteBuffer;J)V"),
       reinterpret_cast<void*>(&CacheWebRtcBuffer)},
      {const_cast<char*>("nativeDataIsRecorded"), const_cast<char*>("(IJ)V"),
       reinterpret_cast<void*>(&WebRtcDataIsRecorded)}};
  ASSERT_EQ(env->RegisterNatives(record_class, methods, 2), JNI_OK);
  jobject record = env->NewObject(
      record_class, env->GetMethodID(record_class, "<init>", "(J)V"),
      reinterpret_cast<jlong>(&capture_probe));
  const auto& format = manager_probe.parameters;
  ASSERT_EQ(
      env->CallIntMethod(
          record, env->GetMethodID(record_class, "initRecording", "(II)I"),
          format.sample_rate_hz, format.input_channels),
      480);
  ASSERT_EQ(
      env->CallBooleanMethod(
          record, env->GetMethodID(record_class, "startRecording", "()Z")),
      JNI_TRUE);
  {
    std::unique_lock<std::mutex> lock(capture_probe.mutex);
    ASSERT_TRUE(capture_probe.cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return capture_probe.non_silent_callbacks >= 3;
    }));
  }
  const jmethodID mute =
      env->GetMethodID(manager_class, "setMicrophoneMute", "(Z)V");
  env->CallVoidMethod(manager, mute, JNI_TRUE);
  {
    std::unique_lock<std::mutex> lock(capture_probe.mutex);
    int silent_before = capture_probe.silent_callbacks;
    ASSERT_TRUE(capture_probe.cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return capture_probe.silent_callbacks >= silent_before + 3;
    }));
    // Allow the one already-running callback at the mute boundary to finish,
    // then verify an additional three frames contain only silence.
    const int non_silent_before = capture_probe.non_silent_callbacks;
    silent_before = capture_probe.silent_callbacks;
    ASSERT_TRUE(capture_probe.cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return capture_probe.silent_callbacks >= silent_before + 3;
    }));
    EXPECT_EQ(capture_probe.non_silent_callbacks, non_silent_before);
    env->CallVoidMethod(manager, mute, JNI_FALSE);
    ASSERT_TRUE(capture_probe.cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return capture_probe.non_silent_callbacks >= non_silent_before + 3;
    }));
  }
  EXPECT_EQ(env->CallBooleanMethod(
                record, env->GetMethodID(record_class, "stopRecording", "()Z")),
            JNI_TRUE);
  env->CallVoidMethod(manager,
                      env->GetMethodID(manager_class, "dispose", "()V"));
}

TEST(SdlAudioCaptureTest, ResolvesDefaultIdAndUnambiguousDeviceNames) {
  const std::vector<SdlRecordingDevice> devices = {
      {11, "USB Microphone"},
      {12, "Built-in Microphone"},
      {13, "USB Microphone"},
  };
  std::uint32_t id = 999;
  std::string name;

  EXPECT_TRUE(ResolveSdlRecordingDevice("default", devices, &id, &name).ok());
  EXPECT_EQ(id, 0U);
  EXPECT_EQ(name, "default");

  EXPECT_TRUE(ResolveSdlRecordingDevice("id:12", devices, &id, &name).ok());
  EXPECT_EQ(id, 12U);
  EXPECT_EQ(name, "Built-in Microphone");

  const Status ambiguous =
      ResolveSdlRecordingDevice("USB Microphone", devices, &id, &name);
  EXPECT_EQ(ambiguous.code(), StatusCode::kInvalidArgument);
  const Status unavailable =
      ResolveSdlRecordingDevice("Missing Microphone", devices, &id, &name);
  EXPECT_EQ(unavailable.code(), StatusCode::kUnavailable);
  const Status invalid_id =
      ResolveSdlRecordingDevice("id:0", devices, &id, &name);
  EXPECT_EQ(invalid_id.code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace mocktail::audio
