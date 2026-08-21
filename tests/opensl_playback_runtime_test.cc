#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "mocktail/audio/opensl_playback_runtime.h"
#include "mocktail/audio/sdl_audio_sink.h"

namespace mocktail::audio {
namespace {

namespace abi = opensl_abi;
using namespace std::chrono_literals;

struct QueueCallbackProbe {
  std::mutex mutex;
  std::condition_variable cv;
  int calls = 0;
};

void QueueCallback(abi::AndroidSimpleBufferQueue, void* context) {
  auto* probe = static_cast<QueueCallbackProbe*>(context);
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    ++probe->calls;
  }
  probe->cv.notify_all();
}

class RuntimeGraph final {
 public:
  ~RuntimeGraph() {
    DestroyPlayer();
    if (output_mix_ != nullptr) {
      (*output_mix_)->Destroy(output_mix_);
    }
    if (engine_object_ != nullptr) {
      (*engine_object_)->Destroy(engine_object_);
    }
  }

  abi::Result Create() {
    abi::Result result =
        slCreateEngine(&engine_object_, 0, nullptr, 0, nullptr, nullptr);
    if (result != abi::kResultSuccess) {
      return result;
    }
    result = (*engine_object_)->Realize(engine_object_, abi::kBooleanFalse);
    if (result != abi::kResultSuccess) {
      return result;
    }
    result = (*engine_object_)
                 ->GetInterface(engine_object_, SL_IID_ENGINE,
                                &engine_interface_);
    if (result != abi::kResultSuccess) {
      return result;
    }
    result = (*engine_interface_)
                 ->CreateOutputMix(engine_interface_, &output_mix_, 0, nullptr,
                                   nullptr);
    if (result != abi::kResultSuccess) {
      return result;
    }
    return (*output_mix_)->Realize(output_mix_, abi::kBooleanFalse);
  }

  abi::Result CreatePlayer(const abi::DataFormatPcm& format) {
    const abi::DataLocatorAndroidSimpleBufferQueue queue_locator{
        abi::kDataLocatorAndroidSimpleBufferQueue, 4};
    const abi::DataSource source{const_cast<void*>(
                                     static_cast<const void*>(&queue_locator)),
                                 const_cast<void*>(
                                     static_cast<const void*>(&format))};
    const abi::DataLocatorOutputMix output_locator{
        abi::kDataLocatorOutputMix, output_mix_};
    const abi::DataSink sink{const_cast<void*>(
                                 static_cast<const void*>(&output_locator)),
                             nullptr};
    const abi::InterfaceId interfaces[] = {
        SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME,
        SL_IID_ANDROIDCONFIGURATION};
    const abi::Boolean required[] = {abi::kBooleanTrue, abi::kBooleanTrue,
                                     abi::kBooleanTrue};
    return (*engine_interface_)
        ->CreateAudioPlayer(engine_interface_, &player_,
                            const_cast<abi::DataSource*>(&source),
                            const_cast<abi::DataSink*>(&sink), 3, interfaces,
                            required);
  }

  void DestroyPlayer() {
    if (player_ != nullptr) {
      (*player_)->Destroy(player_);
      player_ = nullptr;
    }
  }

  abi::Object player() const { return player_; }
  abi::Engine engine_interface() const { return engine_interface_; }

 private:
  abi::Object engine_object_ = nullptr;
  abi::Engine engine_interface_ = nullptr;
  abi::Object output_mix_ = nullptr;
  abi::Object player_ = nullptr;
};

abi::DataFormatPcm StereoS16Format() {
  return abi::DataFormatPcm{
      abi::kDataFormatPcm,
      2,
      48000000,
      abi::kPcmSampleFormatFixed16,
      abi::kPcmSampleFormatFixed16,
      abi::kSpeakerFrontLeft | abi::kSpeakerFrontRight,
      abi::kByteOrderLittleEndian,
  };
}

MocktailOpenSlRuntimeStats RuntimeStats() {
  MocktailOpenSlRuntimeStats stats{};
  EXPECT_EQ(mocktailOpenSlGetRuntimeStats(&stats, sizeof(stats)),
            abi::kResultSuccess);
  return stats;
}

TEST(OpenSlPlaybackRuntimeTest, ConsumesPcmOnlyAfterPlayAndShutsDownCleanly) {
  ASSERT_TRUE(InitializeSdlAudioSubsystem().ok());
  const MocktailOpenSlRuntimeStats before = RuntimeStats();
  RuntimeGraph graph;
  ASSERT_EQ(graph.Create(), abi::kResultSuccess);
  ASSERT_EQ(graph.CreatePlayer(StereoS16Format()), abi::kResultSuccess);

  abi::Uint32 object_state = 0;
  ASSERT_EQ((*graph.player())->GetState(graph.player(), &object_state),
            abi::kResultSuccess);
  EXPECT_EQ(object_state, abi::kObjectStateUnrealized);

  abi::AndroidConfiguration configuration = nullptr;
  ASSERT_EQ((*graph.player())
                ->GetInterface(graph.player(), SL_IID_ANDROIDCONFIGURATION,
                               &configuration),
            abi::kResultSuccess);
  const abi::Int32 media_stream = abi::kAndroidStreamMedia;
  ASSERT_EQ((*configuration)
                ->SetConfiguration(
                    configuration,
                    reinterpret_cast<const abi::Char*>(
                        abi::kAndroidPlaybackStreamTypeKey),
                    &media_stream, sizeof(media_stream)),
            abi::kResultSuccess);

  ASSERT_EQ((*graph.player())->Realize(graph.player(), abi::kBooleanFalse),
            abi::kResultSuccess);

  abi::AndroidSimpleBufferQueue queue = nullptr;
  abi::Play play = nullptr;
  abi::Volume volume = nullptr;
  ASSERT_EQ((*graph.player())
                ->GetInterface(graph.player(),
                               SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue),
            abi::kResultSuccess);
  ASSERT_EQ((*graph.player())
                ->GetInterface(graph.player(), SL_IID_PLAY, &play),
            abi::kResultSuccess);
  ASSERT_EQ((*graph.player())
                ->GetInterface(graph.player(), SL_IID_VOLUME, &volume),
            abi::kResultSuccess);

  ASSERT_EQ((*volume)->SetVolumeLevel(volume, -600), abi::kResultSuccess);
  abi::Millibel volume_level = 0;
  ASSERT_EQ((*volume)->GetVolumeLevel(volume, &volume_level),
            abi::kResultSuccess);
  EXPECT_EQ(volume_level, -600);

  QueueCallbackProbe callback;
  ASSERT_EQ((*queue)->RegisterCallback(queue, QueueCallback, &callback),
            abi::kResultSuccess);
  const std::vector<std::int16_t> samples(4096 * 2, 700);
  const std::vector<std::int16_t> following_samples(4096 * 2, 701);
  ASSERT_EQ((*queue)->Enqueue(
                queue, samples.data(),
                static_cast<abi::Uint32>(samples.size() * sizeof(samples[0]))),
            abi::kResultSuccess);
  ASSERT_EQ((*queue)->Enqueue(
                queue, following_samples.data(),
                static_cast<abi::Uint32>(following_samples.size() *
                                         sizeof(following_samples[0]))),
            abi::kResultSuccess);

  std::this_thread::sleep_for(30ms);
  {
    std::lock_guard<std::mutex> lock(callback.mutex);
    EXPECT_EQ(callback.calls, 0);
  }

  ASSERT_EQ((*play)->SetPlayState(play, abi::kPlayStatePlaying),
            abi::kResultSuccess);
  {
    std::unique_lock<std::mutex> lock(callback.mutex);
    ASSERT_TRUE(callback.cv.wait_for(lock, 2s,
                                     [&callback] { return callback.calls >= 1; }));
  }

  const MocktailOpenSlRuntimeStats active = RuntimeStats();
  EXPECT_EQ(active.submitted_buffers, before.submitted_buffers + 2);
  EXPECT_GE(active.consumed_buffers, before.consumed_buffers + 1);
  EXPECT_EQ(active.clean_player_shutdowns, before.clean_player_shutdowns);

  graph.DestroyPlayer();
  const MocktailOpenSlRuntimeStats after = RuntimeStats();
  EXPECT_EQ(after.clean_player_shutdowns, before.clean_player_shutdowns + 1);
  EXPECT_TRUE(ShutdownSdlAudioSubsystem().ok());
}

TEST(OpenSlPlaybackRuntimeTest, RejectsUnsupportedPcmBeforeOpeningDevice) {
  RuntimeGraph graph;
  ASSERT_EQ(graph.Create(), abi::kResultSuccess);

  abi::DataFormatPcm format{
      abi::kDataFormatPcm,
      2,
      48000000,
      24,
      24,
      abi::kSpeakerFrontLeft | abi::kSpeakerFrontRight,
      abi::kByteOrderLittleEndian,
  };
  EXPECT_EQ(graph.CreatePlayer(format), abi::kResultContentUnsupported);
  EXPECT_EQ(graph.player(), nullptr);

  format = abi::DataFormatPcm{
      abi::kDataFormatPcm,
      2,
      48000000,
      abi::kPcmSampleFormatFixed16,
      abi::kPcmSampleFormatFixed16,
      abi::kSpeakerFrontLeft | abi::kSpeakerFrontRight,
      abi::kByteOrderBigEndian,
  };
  EXPECT_EQ(graph.CreatePlayer(format), abi::kResultContentUnsupported);
  EXPECT_EQ(graph.player(), nullptr);
}

TEST(OpenSlPlaybackRuntimeTest, ClearAndDestroyDiscardWithoutGuestCallbacks) {
  ASSERT_TRUE(InitializeSdlAudioSubsystem().ok());
  const MocktailOpenSlRuntimeStats before = RuntimeStats();
  RuntimeGraph graph;
  ASSERT_EQ(graph.Create(), abi::kResultSuccess);
  ASSERT_EQ(graph.CreatePlayer(StereoS16Format()), abi::kResultSuccess);
  ASSERT_EQ((*graph.player())->Realize(graph.player(), abi::kBooleanFalse),
            abi::kResultSuccess);

  abi::AndroidSimpleBufferQueue queue = nullptr;
  ASSERT_EQ((*graph.player())
                ->GetInterface(graph.player(),
                               SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue),
            abi::kResultSuccess);
  QueueCallbackProbe callback;
  ASSERT_EQ((*queue)->RegisterCallback(queue, QueueCallback, &callback),
            abi::kResultSuccess);

  const std::vector<std::int16_t> samples(2048 * 2, 300);
  const abi::Uint32 byte_count =
      static_cast<abi::Uint32>(samples.size() * sizeof(samples[0]));
  ASSERT_EQ((*queue)->Enqueue(queue, samples.data(), byte_count),
            abi::kResultSuccess);
  ASSERT_EQ((*queue)->Clear(queue), abi::kResultSuccess);

  abi::AndroidSimpleBufferQueueState state{};
  ASSERT_EQ((*queue)->GetState(queue, &state), abi::kResultSuccess);
  EXPECT_EQ(state.count, 0u);
  ASSERT_EQ((*queue)->Enqueue(queue, samples.data(), byte_count),
            abi::kResultSuccess);

  graph.DestroyPlayer();
  std::this_thread::sleep_for(30ms);
  {
    std::lock_guard<std::mutex> lock(callback.mutex);
    EXPECT_EQ(callback.calls, 0);
  }
  const MocktailOpenSlRuntimeStats after = RuntimeStats();
  EXPECT_EQ(after.submitted_buffers, before.submitted_buffers + 2);
  EXPECT_EQ(after.consumed_buffers, before.consumed_buffers);
  EXPECT_EQ(after.clean_player_shutdowns, before.clean_player_shutdowns + 1);
  EXPECT_TRUE(ShutdownSdlAudioSubsystem().ok());
}

TEST(OpenSlPlaybackRuntimeTest, RecorderFailsClosed) {
  RuntimeGraph graph;
  ASSERT_EQ(graph.Create(), abi::kResultSuccess);
  abi::Object recorder = reinterpret_cast<abi::Object>(0x1);
  EXPECT_EQ((*graph.engine_interface())
                ->CreateAudioRecorder(graph.engine_interface(), &recorder,
                                      nullptr, nullptr, 0, nullptr, nullptr),
            abi::kResultFeatureUnsupported);
  EXPECT_EQ(recorder, nullptr);
}

}  // namespace
}  // namespace mocktail::audio
