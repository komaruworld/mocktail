#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mocktail/audio/audio_sink.h"
#include "mocktail/audio/opensl_simple_buffer_queue.h"
#include "mocktail/audio/sdl_audio_sink.h"

extern "C" std::uint32_t slCreateEngine(void** engine,
                                        std::uint32_t num_options,
                                        const void* options,
                                        std::uint32_t num_interfaces,
                                        const void* interface_ids,
                                        const void* interface_required);

namespace mocktail::audio {
namespace {

using namespace std::chrono_literals;

class ControlledAudioSink final : public AudioSink {
 public:
  const PcmSpec& source_spec() const override { return spec_; }

  Status Enqueue(const PcmBuffer& buffer) override {
    if (buffer.data == nullptr || buffer.size_bytes == 0) {
      return Status::Error(StatusCode::kInvalidArgument, "empty buffer");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      return Status::Error(StatusCode::kFailedPrecondition, "stopped");
    }
    buffers_.push_back(buffer);
    return Status::Ok();
  }

  Status Pause() override {
    std::lock_guard<std::mutex> lock(mutex_);
    playing_ = false;
    return stopped_ ? Status::Error(StatusCode::kFailedPrecondition, "stopped")
                    : Status::Ok();
  }

  Status Resume() override {
    std::lock_guard<std::mutex> lock(mutex_);
    playing_ = true;
    return stopped_ ? Status::Error(StatusCode::kFailedPrecondition, "stopped")
                    : Status::Ok();
  }

  Status SetGain(float linear_gain) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      return Status::Error(StatusCode::kFailedPrecondition, "stopped");
    }
    gain_ = linear_gain;
    return Status::Ok();
  }

  Status Flush() override { return Status::Ok(); }

  Status Clear() override {
    ReleaseAll();
    return Status::Ok();
  }

  Status GetQueuedBytes(std::size_t* size_bytes) const override {
    if (size_bytes == nullptr) {
      return Status::Error(StatusCode::kInvalidArgument, "null output");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    *size_bytes = 0;
    for (const PcmBuffer& buffer : buffers_) {
      *size_bytes += buffer.size_bytes;
    }
    return Status::Ok();
  }

  void Shutdown() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    ReleaseAll();
  }

  void ReleaseOne() {
    PcmBuffer buffer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (buffers_.empty()) {
        return;
      }
      buffer = buffers_.front();
      buffers_.erase(buffers_.begin());
    }
    if (buffer.release_callback != nullptr) {
      buffer.release_callback(buffer.release_context, buffer.data,
                              buffer.size_bytes);
    }
  }

  std::size_t buffer_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffers_.size();
  }

  bool playing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_;
  }

  float gain() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return gain_;
  }

 private:
  void ReleaseAll() {
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (buffers_.empty()) {
          return;
        }
      }
      ReleaseOne();
    }
  }

  const PcmSpec spec_;
  mutable std::mutex mutex_;
  std::vector<PcmBuffer> buffers_;
  bool playing_ = false;
  float gain_ = 1.0F;
  bool stopped_ = false;
};

struct CallbackProbe {
  std::mutex mutex;
  std::condition_variable cv;
  int calls = 0;
};

void OpenSlQueueCallback(opensl_abi::AndroidSimpleBufferQueue,
                         void* context) {
  auto* probe = static_cast<CallbackProbe*>(context);
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    ++probe->calls;
  }
  probe->cv.notify_all();
}

struct ReleaseProbe {
  std::mutex mutex;
  std::condition_variable cv;
  const void* data = nullptr;
  std::size_t size = 0;
  int calls = 0;
};

struct BlockingCallbackProbe {
  std::mutex mutex;
  std::condition_variable cv;
  bool started = false;
  bool release = false;
};

void BlockingOpenSlQueueCallback(opensl_abi::AndroidSimpleBufferQueue,
                                 void* context) {
  auto* probe = static_cast<BlockingCallbackProbe*>(context);
  std::unique_lock<std::mutex> lock(probe->mutex);
  probe->started = true;
  probe->cv.notify_all();
  probe->cv.wait(lock, [&probe] { return probe->release; });
}

void BufferReleased(void* context, const void* data, std::size_t size) {
  auto* probe = static_cast<ReleaseProbe*>(context);
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    probe->data = data;
    probe->size = size;
    ++probe->calls;
  }
  probe->cv.notify_all();
}

TEST(AudioSpecTest, ValidatesTypedPcmAndFrameSize) {
  const PcmSpec stereo_s16{48000, 2, PcmSampleFormat::kSigned16LittleEndian};
  EXPECT_TRUE(ValidatePcmSpec(stereo_s16).ok());
  EXPECT_EQ(BytesPerFrame(stereo_s16), 4u);

  PcmSpec invalid = stereo_s16;
  invalid.channels = 0;
  EXPECT_EQ(ValidatePcmSpec(invalid).code(), StatusCode::kInvalidArgument);
  invalid = stereo_s16;
  invalid.sample_rate_hz = 1000;
  EXPECT_EQ(ValidatePcmSpec(invalid).code(), StatusCode::kInvalidArgument);
}

TEST(SdlPlaybackDeviceTest, ResolvesDefaultAndExactPhysicalName) {
  const std::vector<SdlPlaybackDevice> devices = {
      {17, "Built-in Audio Analog Stereo"}, {23, "USB Headset"}};
  std::uint32_t device_id = 99;
  std::string resolved;

  ASSERT_TRUE(
      ResolveSdlPlaybackDevice("default", devices, &device_id, &resolved).ok());
  EXPECT_EQ(device_id, 0U);
  EXPECT_EQ(resolved, "default");

  ASSERT_TRUE(
      ResolveSdlPlaybackDevice("USB Headset", devices, &device_id, &resolved)
          .ok());
  EXPECT_EQ(device_id, 23U);
  EXPECT_EQ(resolved, "USB Headset");
}

TEST(SdlPlaybackDeviceTest, RejectsMissingAmbiguousAndUnsafeNames) {
  std::uint32_t device_id = 0;
  std::string resolved;
  EXPECT_EQ(ResolveSdlPlaybackDevice("Missing", {{17, "Speakers"}}, &device_id,
                                     &resolved)
                .code(),
            StatusCode::kUnavailable);
  EXPECT_EQ(
      ResolveSdlPlaybackDevice("Speakers", {{17, "Speakers"}, {23, "Speakers"}},
                               &device_id, &resolved)
          .code(),
      StatusCode::kInvalidArgument);
  EXPECT_EQ(
      ResolveSdlPlaybackDevice("Speaker\nInjected", {}, &device_id, &resolved)
          .code(),
      StatusCode::kInvalidArgument);
}

TEST(SdlAudioSubsystemTest, EnforcesMainThreadOwnershipAndLiveSinkOrder) {
  ASSERT_TRUE(InitializeSdlAudioSubsystem().ok());
  std::vector<SdlPlaybackDevice> devices;
  std::string resolved;
  ASSERT_TRUE(ConfigureSdlPlaybackDevice("default", &devices, &resolved).ok());
  EXPECT_EQ(resolved, "default");
  SdlAudioSinkOptions options;
  std::unique_ptr<AudioSink> sink;
  ASSERT_TRUE(CreateSdlAudioSink(options, &sink).ok());
  EXPECT_EQ(ConfigureSdlPlaybackDevice("default", &devices, &resolved).code(),
            StatusCode::kFailedPrecondition);
  EXPECT_EQ(ShutdownSdlAudioSubsystem().code(),
            StatusCode::kFailedPrecondition);

  Status worker_shutdown;
  std::thread worker(
      [&worker_shutdown] { worker_shutdown = ShutdownSdlAudioSubsystem(); });
  worker.join();
  EXPECT_EQ(worker_shutdown.code(), StatusCode::kFailedPrecondition);

  sink->Shutdown();
  sink.reset();
  EXPECT_TRUE(ShutdownSdlAudioSubsystem().ok());
}

TEST(SdlAudioSubsystemTest, OpensExactEnumeratedPlaybackDevice) {
  ASSERT_TRUE(InitializeSdlAudioSubsystem().ok());
  std::vector<SdlPlaybackDevice> devices;
  ASSERT_TRUE(ListSdlPlaybackDevices(&devices).ok());
  if (devices.empty()) {
    EXPECT_TRUE(ShutdownSdlAudioSubsystem().ok());
    GTEST_SKIP() << "SDL audio driver exposed no physical playback device";
  }

  const std::string requested = devices.front().name;
  std::string resolved;
  ASSERT_TRUE(ConfigureSdlPlaybackDevice(requested, &devices, &resolved).ok());
  EXPECT_EQ(resolved, requested);
  SdlAudioSinkOptions options;
  std::unique_ptr<AudioSink> sink;
  const Status status = CreateSdlAudioSink(options, &sink);
  ASSERT_TRUE(status.ok()) << status.message();
  sink->Shutdown();
  sink.reset();
  EXPECT_TRUE(ShutdownSdlAudioSubsystem().ok());
}

TEST(SdlAudioSubsystemTest, MigratesLiveStreamWithoutDroppingQueuedPcm) {
  ASSERT_TRUE(InitializeSdlAudioSubsystem().ok());
  std::vector<SdlPlaybackDevice> devices;
  std::string resolved;
  ASSERT_TRUE(ConfigureSdlPlaybackDevice("default", &devices, &resolved).ok());

  SdlAudioSinkOptions options;
  options.start_paused = true;
  std::unique_ptr<AudioSink> sink;
  ASSERT_TRUE(CreateSdlAudioSink(options, &sink).ok());
  const std::vector<std::int16_t> samples(256 * 2, 91);
  ASSERT_TRUE(sink->Enqueue(PcmBuffer{samples.data(),
                                      samples.size() * sizeof(samples.front())})
                  .ok());

  if (!devices.empty()) {
    ASSERT_TRUE(SwitchSdlPlaybackDevice(devices.front().id, &resolved).ok());
    EXPECT_EQ(resolved, devices.front().name);
    std::uint32_t configured_id = 0;
    std::string configured_name;
    ASSERT_TRUE(
        GetConfiguredSdlPlaybackDevice(&configured_id, &configured_name).ok());
    EXPECT_EQ(configured_id, devices.front().id);
    EXPECT_EQ(configured_name, devices.front().name);
  }

  std::size_t queued = 0;
  ASSERT_TRUE(sink->GetQueuedBytes(&queued).ok());
  EXPECT_EQ(queued, samples.size() * sizeof(samples.front()));
  ASSERT_TRUE(SwitchSdlPlaybackDevice(0, &resolved).ok());
  EXPECT_EQ(resolved, "default");
  ASSERT_TRUE(sink->GetQueuedBytes(&queued).ok());
  EXPECT_EQ(queued, samples.size() * sizeof(samples.front()));

  sink->Shutdown();
  sink.reset();
  EXPECT_TRUE(ShutdownSdlAudioSubsystem().ok());
}

TEST(SdlAudioSinkTest, QueuesRealPcmIntoPausedSdlStream) {
  ASSERT_TRUE(InitializeSdlAudioSubsystem().ok());
  SdlAudioSinkOptions options;
  options.start_paused = true;
  std::unique_ptr<AudioSink> sink;
  Status status = CreateSdlAudioSink(options, &sink);
  ASSERT_TRUE(status.ok()) << status.message();

  const std::vector<std::int16_t> samples(512 * 2, 123);
  status = sink->Enqueue(
      PcmBuffer{samples.data(), samples.size() * sizeof(samples.front())});
  ASSERT_TRUE(status.ok()) << status.message();
  std::size_t queued = 0;
  status = sink->GetQueuedBytes(&queued);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(queued, samples.size() * sizeof(samples.front()));

  status = sink->Clear();
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_TRUE(sink->GetQueuedBytes(&queued).ok());
  EXPECT_EQ(queued, 0u);
  sink->Shutdown();
  EXPECT_EQ(sink->Resume().code(), StatusCode::kFailedPrecondition);
  sink.reset();
  EXPECT_TRUE(ShutdownSdlAudioSubsystem().ok());
}

TEST(SdlAudioSinkTest, ReleasesBorrowedBufferWhenStreamIsCleared) {
  ASSERT_TRUE(InitializeSdlAudioSubsystem().ok());
  SdlAudioSinkOptions options;
  std::unique_ptr<AudioSink> sink;
  Status status = CreateSdlAudioSink(options, &sink);
  ASSERT_TRUE(status.ok()) << status.message();

  const std::vector<std::int16_t> samples(128 * 2, 321);
  ReleaseProbe probe;
  status = sink->Enqueue(PcmBuffer{samples.data(),
                                   samples.size() * sizeof(samples.front()),
                                   BufferReleased, &probe});
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_TRUE(sink->Clear().ok());

  std::unique_lock<std::mutex> lock(probe.mutex);
  ASSERT_TRUE(
      probe.cv.wait_for(lock, 2s, [&probe] { return probe.calls == 1; }));
  EXPECT_EQ(probe.data, samples.data());
  EXPECT_EQ(probe.size, samples.size() * sizeof(samples.front()));
  sink->Shutdown();
  sink.reset();
  EXPECT_TRUE(ShutdownSdlAudioSubsystem().ok());
}

TEST(OpenSlQueueTest, DeliversPcmToSinkAndDispatchesCompletion) {
  auto controlled = std::make_unique<ControlledAudioSink>();
  ControlledAudioSink* controlled_view = controlled.get();
  std::unique_ptr<OpenSlSimpleBufferQueueAdapter> adapter;
  Status status = OpenSlSimpleBufferQueueAdapter::Create(std::move(controlled),
                                                         {}, &adapter);
  ASSERT_TRUE(status.ok()) << status.message();

  auto queue = adapter->interface();
  ASSERT_NE(queue, nullptr);
  CallbackProbe probe;
  ASSERT_EQ((*queue)->RegisterCallback(queue, OpenSlQueueCallback, &probe),
            opensl_abi::kResultSuccess);

  const std::vector<std::int16_t> samples(64 * 2, 456);
  ASSERT_EQ((*queue)->Enqueue(queue, samples.data(),
                              static_cast<opensl_abi::Uint32>(
                                  samples.size() * sizeof(samples.front()))),
            opensl_abi::kResultSuccess);
  EXPECT_EQ(controlled_view->buffer_count(), 1u);

  opensl_abi::AndroidSimpleBufferQueueState queue_state{};
  ASSERT_EQ((*queue)->GetState(queue, &queue_state),
            opensl_abi::kResultSuccess);
  EXPECT_EQ(queue_state.count, 1u);
  controlled_view->ReleaseOne();

  {
    std::unique_lock<std::mutex> lock(probe.mutex);
    ASSERT_TRUE(
        probe.cv.wait_for(lock, 2s, [&probe] { return probe.calls == 1; }));
  }
  ASSERT_EQ((*queue)->GetState(queue, &queue_state),
            opensl_abi::kResultSuccess);
  EXPECT_EQ(queue_state.count, 0u);
  EXPECT_EQ(queue_state.index, 1u);
  const OpenSlSimpleBufferQueueStats stats = adapter->GetStats();
  EXPECT_EQ(stats.submitted_buffers, 1u);
  EXPECT_EQ(stats.consumed_buffers, 1u);
  EXPECT_EQ(stats.discarded_buffers, 0u);
  EXPECT_EQ(stats.pending_buffers, 0u);
}

TEST(OpenSlQueueTest, DelegatesLinearGainToTheHostSink) {
  auto controlled = std::make_unique<ControlledAudioSink>();
  ControlledAudioSink* controlled_view = controlled.get();
  std::unique_ptr<OpenSlSimpleBufferQueueAdapter> adapter;
  ASSERT_TRUE(OpenSlSimpleBufferQueueAdapter::Create(std::move(controlled), {},
                                                     &adapter)
                  .ok());

  ASSERT_TRUE(adapter->SetGain(0.25F).ok());
  EXPECT_FLOAT_EQ(controlled_view->gain(), 0.25F);
  adapter->Shutdown();
  EXPECT_EQ(adapter->SetGain(1.0F).code(), StatusCode::kFailedPrecondition);
}

TEST(OpenSlQueueTest, SdlCompletionFollowsRealStreamConsumption) {
  ASSERT_TRUE(InitializeSdlAudioSubsystem().ok());
  SdlAudioSinkOptions sink_options;
  sink_options.start_paused = true;
  std::unique_ptr<AudioSink> sink;
  Status status = CreateSdlAudioSink(sink_options, &sink);
  ASSERT_TRUE(status.ok()) << status.message();

  std::unique_ptr<OpenSlSimpleBufferQueueAdapter> adapter;
  status =
      OpenSlSimpleBufferQueueAdapter::Create(std::move(sink), {}, &adapter);
  ASSERT_TRUE(status.ok()) << status.message();
  auto queue = adapter->interface();
  CallbackProbe probe;
  ASSERT_EQ((*queue)->RegisterCallback(queue, OpenSlQueueCallback, &probe),
            opensl_abi::kResultSuccess);

  const std::vector<std::int16_t> samples(4096 * 2, 222);
  const std::vector<std::int16_t> following_samples(4096 * 2, 333);
  ASSERT_EQ((*queue)->Enqueue(queue, samples.data(),
                              static_cast<opensl_abi::Uint32>(
                                  samples.size() * sizeof(samples.front()))),
            opensl_abi::kResultSuccess);
  ASSERT_EQ((*queue)->Enqueue(queue, following_samples.data(),
                              static_cast<opensl_abi::Uint32>(
                                  following_samples.size() *
                                  sizeof(following_samples.front()))),
            opensl_abi::kResultSuccess);

  std::this_thread::sleep_for(20ms);
  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    EXPECT_EQ(probe.calls, 0) << "paused SDL stream consumed a borrowed buffer";
  }
  opensl_abi::AndroidSimpleBufferQueueState queue_state{};
  ASSERT_EQ((*queue)->GetState(queue, &queue_state),
            opensl_abi::kResultSuccess);
  EXPECT_EQ(queue_state.count, 2u);

  status = adapter->SetPlaying(true);
  ASSERT_TRUE(status.ok()) << status.message();
  {
    std::unique_lock<std::mutex> lock(probe.mutex);
    ASSERT_TRUE(
        probe.cv.wait_for(lock, 2s, [&probe] { return probe.calls >= 1; }));
  }
  ASSERT_EQ((*queue)->GetState(queue, &queue_state),
            opensl_abi::kResultSuccess);
  EXPECT_LT(queue_state.count, 2u);
  EXPECT_GE(queue_state.index, 1u);
  adapter->Shutdown();
  adapter.reset();
  EXPECT_TRUE(ShutdownSdlAudioSubsystem().ok());
}

TEST(OpenSlQueueTest, ClearSuppressesCallbacksForDiscardedBuffers) {
  auto controlled = std::make_unique<ControlledAudioSink>();
  std::unique_ptr<OpenSlSimpleBufferQueueAdapter> adapter;
  ASSERT_TRUE(OpenSlSimpleBufferQueueAdapter::Create(std::move(controlled), {},
                                                     &adapter)
                  .ok());
  auto queue = adapter->interface();
  CallbackProbe probe;
  ASSERT_EQ((*queue)->RegisterCallback(queue, OpenSlQueueCallback, &probe),
            opensl_abi::kResultSuccess);

  const std::vector<std::int16_t> samples(32 * 2, 789);
  ASSERT_EQ((*queue)->Enqueue(queue, samples.data(),
                              static_cast<opensl_abi::Uint32>(
                                  samples.size() * sizeof(samples.front()))),
            opensl_abi::kResultSuccess);
  ASSERT_EQ((*queue)->Clear(queue), opensl_abi::kResultSuccess);
  std::this_thread::sleep_for(20ms);
  std::lock_guard<std::mutex> lock(probe.mutex);
  EXPECT_EQ(probe.calls, 0);
}

TEST(OpenSlQueueTest, RejectsOverflowAndShutsDownFailClosed) {
  auto controlled = std::make_unique<ControlledAudioSink>();
  OpenSlSimpleBufferQueueOptions options;
  options.max_buffers = 1;
  std::unique_ptr<OpenSlSimpleBufferQueueAdapter> adapter;
  ASSERT_TRUE(OpenSlSimpleBufferQueueAdapter::Create(std::move(controlled),
                                                     options, &adapter)
                  .ok());
  auto queue = adapter->interface();
  const std::vector<std::int16_t> samples(32 * 2, 111);
  const auto bytes =
      static_cast<opensl_abi::Uint32>(samples.size() * sizeof(samples.front()));
  ASSERT_EQ((*queue)->Enqueue(queue, samples.data(), bytes),
            opensl_abi::kResultSuccess);
  EXPECT_EQ((*queue)->Enqueue(queue, samples.data(), bytes),
            opensl_abi::kResultBufferInsufficient);
  adapter->Shutdown();
  EXPECT_EQ((*queue)->Enqueue(queue, samples.data(), bytes),
            opensl_abi::kResultPreconditionsViolated);
}

TEST(OpenSlQueueTest, ShutdownWaitsForInFlightCallback) {
  auto controlled = std::make_unique<ControlledAudioSink>();
  ControlledAudioSink* controlled_view = controlled.get();
  std::unique_ptr<OpenSlSimpleBufferQueueAdapter> adapter;
  ASSERT_TRUE(OpenSlSimpleBufferQueueAdapter::Create(std::move(controlled), {},
                                                     &adapter)
                  .ok());
  auto queue = adapter->interface();
  BlockingCallbackProbe callback;
  ASSERT_EQ(
      (*queue)->RegisterCallback(queue, BlockingOpenSlQueueCallback, &callback),
      opensl_abi::kResultSuccess);

  const std::vector<std::int16_t> samples(32 * 2, 444);
  ASSERT_EQ((*queue)->Enqueue(queue, samples.data(),
                              static_cast<opensl_abi::Uint32>(
                                  samples.size() * sizeof(samples.front()))),
            opensl_abi::kResultSuccess);
  controlled_view->ReleaseOne();
  {
    std::unique_lock<std::mutex> lock(callback.mutex);
    ASSERT_TRUE(callback.cv.wait_for(lock, 2s,
                                     [&callback] { return callback.started; }));
  }

  std::atomic<bool> shutdown_done = false;
  std::thread shutdown_thread([&adapter, &shutdown_done] {
    adapter->Shutdown();
    shutdown_done.store(true);
  });
  std::this_thread::sleep_for(20ms);
  EXPECT_FALSE(shutdown_done.load());

  {
    std::lock_guard<std::mutex> lock(callback.mutex);
    callback.release = true;
  }
  callback.cv.notify_all();
  shutdown_thread.join();
  EXPECT_TRUE(shutdown_done.load());
  EXPECT_EQ((*queue)->Enqueue(queue, samples.data(),
                              static_cast<opensl_abi::Uint32>(
                                  samples.size() * sizeof(samples.front()))),
            opensl_abi::kResultPreconditionsViolated);
}

TEST(OpenSlQueueTest, RecorderIsExplicitlyUnsupported) {
  const Status status = OpenSlRecorderSupportStatus();
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kUnsupported);
}

TEST(LegacyOpenSlStubTest, RequiresExactTestOptIn) {
  unsetenv("MOCKTAIL_ENABLE_TEST_AUDIO_STUBS");
  void* engine = reinterpret_cast<void*>(0x1);
  EXPECT_EQ(slCreateEngine(&engine, 0, nullptr, 0, nullptr, nullptr),
            opensl_abi::kResultFeatureUnsupported);
  EXPECT_EQ(engine, nullptr);

  setenv("MOCKTAIL_ENABLE_TEST_AUDIO_STUBS", "true", 1);
  engine = reinterpret_cast<void*>(0x1);
  EXPECT_EQ(slCreateEngine(&engine, 0, nullptr, 0, nullptr, nullptr),
            opensl_abi::kResultFeatureUnsupported);
  EXPECT_EQ(engine, nullptr);

  setenv("MOCKTAIL_ENABLE_TEST_AUDIO_STUBS", "1", 1);
  EXPECT_EQ(slCreateEngine(&engine, 0, nullptr, 0, nullptr, nullptr),
            opensl_abi::kResultSuccess);
  EXPECT_NE(engine, nullptr);
  unsetenv("MOCKTAIL_ENABLE_TEST_AUDIO_STUBS");
}

}  // namespace
}  // namespace mocktail::audio
