#include "mocktail/audio/fmod_java_audio_runtime.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace mocktail::audio {
namespace {

using namespace std::chrono_literals;

class ControlledSink final : public AudioSink {
 public:
  explicit ControlledSink(PcmSpec spec) : spec_(spec) {}

  const PcmSpec& source_spec() const override { return spec_; }

  Status Enqueue(const PcmBuffer& buffer) override {
    std::unique_lock<std::mutex> lock(mutex_);
    enqueue_entered_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this] { return !block_enqueue_; });
    if (stopped_) {
      return Status::Error(StatusCode::kFailedPrecondition, "stopped");
    }
    if (fail_next_enqueue_) {
      fail_next_enqueue_ = false;
      return Status::Error(StatusCode::kUnavailable, "injected enqueue error");
    }
    borrowed_pointers_.push_back(buffer.data);
    if (release_synchronously_) {
      lock.unlock();
      if (buffer.release_callback != nullptr) {
        buffer.release_callback(buffer.release_context, buffer.data,
                                buffer.size_bytes);
      }
      return Status::Ok();
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
    if (stopped_) {
      return Status::Error(StatusCode::kFailedPrecondition, "stopped");
    }
    playing_ = true;
    return Status::Ok();
  }

  Status SetGain(float) override { return Status::Ok(); }
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
      block_enqueue_ = false;
    }
    cv_.notify_all();
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

  void FailNextEnqueue() {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_next_enqueue_ = true;
  }

  void ReleaseSynchronously() {
    std::lock_guard<std::mutex> lock(mutex_);
    release_synchronously_ = true;
  }

  void BlockEnqueue() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_enqueue_ = true;
    enqueue_entered_ = false;
  }

  void WaitForBlockedEnqueue() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return enqueue_entered_; });
  }

  void UnblockEnqueue() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_enqueue_ = false;
    cv_.notify_all();
  }

  bool playing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_;
  }

  std::size_t buffer_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffers_.size();
  }

  std::vector<std::uint8_t> FrontBufferCopy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (buffers_.empty()) {
      return {};
    }
    const PcmBuffer& buffer = buffers_.front();
    const auto* first = static_cast<const std::uint8_t*>(buffer.data);
    return std::vector<std::uint8_t>(first, first + buffer.size_bytes);
  }

  std::vector<const void*> BorrowedPointers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return borrowed_pointers_;
  }

 private:
  const PcmSpec spec_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<PcmBuffer> buffers_;
  std::vector<const void*> borrowed_pointers_;
  bool playing_ = false;
  bool stopped_ = false;
  bool fail_next_enqueue_ = false;
  bool block_enqueue_ = false;
  bool enqueue_entered_ = false;
  bool release_synchronously_ = false;
};

struct SinkFactoryProbe {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<ControlledSink*> sinks;
  bool block_creation = false;
  bool creation_entered = false;
};

Status CreateControlledSink(void* context, const PcmSpec& source_spec,
                            std::unique_ptr<AudioSink>* sink) {
  if (context == nullptr || sink == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument, "invalid factory");
  }
  sink->reset();
  auto* implementation = new (std::nothrow) ControlledSink(source_spec);
  if (implementation == nullptr) {
    return Status::Error(StatusCode::kUnavailable, "allocation failed");
  }
  auto* probe = static_cast<SinkFactoryProbe*>(context);
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    probe->creation_entered = true;
    probe->cv.notify_all();
    probe->cv.wait(lock, [probe] { return !probe->block_creation; });
    probe->sinks.push_back(implementation);
  }
  sink->reset(implementation);
  return Status::Ok();
}

ControlledSink* LastSink(SinkFactoryProbe* probe) {
  std::lock_guard<std::mutex> lock(probe->mutex);
  return probe->sinks.empty() ? nullptr : probe->sinks.back();
}

FmodJavaAudioRuntimeOptions TestOptions(SinkFactoryProbe* probe) {
  FmodJavaAudioRuntimeOptions options;
  options.sink_factory = &CreateControlledSink;
  options.sink_factory_context = probe;
  return options;
}

FmodJavaAudioDeviceIdentity Identity(std::uintptr_t value) {
  return reinterpret_cast<FmodJavaAudioDeviceIdentity>(value);
}

TEST(FmodJavaAudioRuntimeTest, InitializesExactApkPcmContract) {
  SinkFactoryProbe probe;
  FmodJavaAudioRuntime runtime(TestOptions(&probe));

  ASSERT_TRUE(runtime.Init(Identity(1), 2, 48000, 512, 4).ok());
  ControlledSink* sink = LastSink(&probe);
  ASSERT_NE(sink, nullptr);
  EXPECT_TRUE(sink->playing());
  EXPECT_EQ(sink->source_spec().sample_rate_hz, 48000);
  EXPECT_EQ(sink->source_spec().channels, 2);
  EXPECT_EQ(sink->source_spec().format,
            PcmSampleFormat::kSigned16LittleEndian);

  EXPECT_EQ(runtime.Init(Identity(1), 2, 48000, 512, 4).code(),
            StatusCode::kFailedPrecondition);
  EXPECT_EQ(runtime.Init(Identity(2), 3, 48000, 512, 4).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(runtime.Init(Identity(2), 2, 48000, 0, 4).code(),
            StatusCode::kInvalidArgument);

  const FmodJavaAudioRuntimeStats stats = runtime.GetStats();
  EXPECT_EQ(stats.init_attempts, 4u);
  EXPECT_EQ(stats.initialized_devices, 1u);
  EXPECT_EQ(stats.rejected_init_calls, 3u);
  EXPECT_EQ(stats.active_devices, 1u);
  EXPECT_TRUE(runtime.Close(Identity(1)).ok());
}

TEST(FmodJavaAudioRuntimeTest, ReservesDeviceLimitBeforeOpeningSink) {
  SinkFactoryProbe probe;
  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    probe.block_creation = true;
  }
  FmodJavaAudioRuntimeOptions options = TestOptions(&probe);
  options.max_devices = 1;
  FmodJavaAudioRuntime runtime(options);

  Status first_status;
  std::thread first_initializer([&] {
    first_status = runtime.Init(Identity(1), 2, 48000, 512, 4);
  });
  bool creation_entered = false;
  {
    std::unique_lock<std::mutex> lock(probe.mutex);
    creation_entered = probe.cv.wait_for(
        lock, 1s, [&probe] { return probe.creation_entered; });
  }
  if (!creation_entered) {
    {
      std::lock_guard<std::mutex> lock(probe.mutex);
      probe.block_creation = false;
    }
    probe.cv.notify_all();
    first_initializer.join();
    FAIL() << "sink factory did not enter before the timeout";
  }
  EXPECT_EQ(runtime.Init(Identity(2), 2, 48000, 512, 4).code(),
            StatusCode::kUnavailable);
  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    probe.block_creation = false;
  }
  probe.cv.notify_all();
  first_initializer.join();
  EXPECT_TRUE(first_status.ok()) << first_status.message();
  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    EXPECT_EQ(probe.sinks.size(), 1u);
  }
  EXPECT_TRUE(runtime.Close(Identity(1)).ok());
}

TEST(FmodJavaAudioRuntimeTest,
     CopiesGuestBytesAndAppliesFrameAndBackpressureBounds) {
  SinkFactoryProbe probe;
  FmodJavaAudioRuntime runtime(TestOptions(&probe));
  ASSERT_TRUE(runtime.Init(Identity(1), 2, 48000, 2, 1).ok());
  ControlledSink* sink = LastSink(&probe);
  ASSERT_NE(sink, nullptr);

  std::vector<std::uint8_t> first{1, 2, 3, 4, 5, 6, 7, 8};
  const std::vector<std::uint8_t> original = first;
  ASSERT_TRUE(runtime.Write(Identity(1), first.data(), first.size()).ok());
  std::memset(first.data(), 0, first.size());
  EXPECT_EQ(sink->FrontBufferCopy(), original);

  const std::vector<std::uint8_t> second(8, 9);
  EXPECT_EQ(runtime.Write(Identity(1), second.data(), 3).code(),
            StatusCode::kInvalidArgument);
  const std::vector<std::uint8_t> oversized(16, 10);
  EXPECT_EQ(
      runtime.Write(Identity(1), oversized.data(), oversized.size()).code(),
      StatusCode::kInvalidArgument);

  std::promise<Status> blocked_result;
  std::future<Status> blocked_future = blocked_result.get_future();
  std::thread blocked_writer([&] {
    blocked_result.set_value(
        runtime.Write(Identity(1), second.data(), second.size()));
  });
  for (int attempt = 0;
       attempt < 100 && runtime.GetStats().write_attempts < 4; ++attempt) {
    std::this_thread::sleep_for(1ms);
  }
  const std::future_status before_release = blocked_future.wait_for(20ms);
  EXPECT_EQ(before_release, std::future_status::timeout);

  sink->ReleaseOne();
  EXPECT_EQ(blocked_future.wait_for(1s), std::future_status::ready);
  const Status unblocked_status = blocked_future.get();
  blocked_writer.join();
  EXPECT_TRUE(unblocked_status.ok()) << unblocked_status.message();

  FmodJavaAudioRuntimeStats stats = runtime.GetStats();
  EXPECT_EQ(stats.consumed_buffers, 1u);
  EXPECT_EQ(stats.consumed_bytes, 8u);
  EXPECT_EQ(stats.pending_buffers, 1u);

  ASSERT_TRUE(runtime.Close(Identity(1)).ok());
  stats = runtime.GetStats();
  EXPECT_EQ(stats.submitted_buffers, 2u);
  EXPECT_EQ(stats.submitted_bytes, 16u);
  EXPECT_EQ(stats.consumed_buffers, 1u);
  EXPECT_EQ(stats.discarded_buffers, 1u);
  EXPECT_EQ(stats.discarded_bytes, 8u);
  EXPECT_EQ(stats.rejected_write_calls, 2u);
  EXPECT_EQ(stats.pending_buffers, 0u);
  EXPECT_EQ(stats.pending_bytes, 0u);
  EXPECT_EQ(stats.active_devices, 0u);
}

TEST(FmodJavaAudioRuntimeTest, FailedSinkSubmissionRollsBackOwnership) {
  SinkFactoryProbe probe;
  FmodJavaAudioRuntime runtime(TestOptions(&probe));
  ASSERT_TRUE(runtime.Init(Identity(1), 1, 44100, 4, 2).ok());
  ControlledSink* sink = LastSink(&probe);
  ASSERT_NE(sink, nullptr);
  sink->FailNextEnqueue();

  const std::vector<std::uint8_t> samples(8, 42);
  EXPECT_EQ(runtime.Write(Identity(1), samples.data(), samples.size()).code(),
            StatusCode::kUnavailable);
  FmodJavaAudioRuntimeStats stats = runtime.GetStats();
  EXPECT_EQ(stats.submitted_buffers, 0u);
  EXPECT_EQ(stats.pending_buffers, 0u);
  EXPECT_EQ(stats.pending_bytes, 0u);

  ASSERT_TRUE(runtime.Write(Identity(1), samples.data(), samples.size()).ok());
  ASSERT_TRUE(runtime.Close(Identity(1)).ok());
  stats = runtime.GetStats();
  EXPECT_EQ(stats.submitted_buffers, 1u);
  EXPECT_EQ(stats.discarded_buffers, 1u);
}

TEST(FmodJavaAudioRuntimeTest,
     ReusesAStableFixedPoolAcrossThousandsOfMixBlocks) {
  SinkFactoryProbe probe;
  FmodJavaAudioRuntime runtime(TestOptions(&probe));
  constexpr std::size_t kBlockCount = 3;
  ASSERT_TRUE(runtime.Init(Identity(1), 2, 48000, 4, kBlockCount).ok());
  ControlledSink* sink = LastSink(&probe);
  ASSERT_NE(sink, nullptr);

  const std::vector<std::uint8_t> samples(16, 21);
  for (std::size_t index = 0; index < kBlockCount; ++index) {
    ASSERT_TRUE(runtime.Write(Identity(1), samples.data(), samples.size()).ok());
  }
  sink->ReleaseAll();
  for (int cycle = 0; cycle < 5000; ++cycle) {
    ASSERT_TRUE(runtime.Write(Identity(1), samples.data(), samples.size()).ok());
    sink->ReleaseOne();
  }

  const std::vector<const void*> pointers = sink->BorrowedPointers();
  const std::set<const void*> unique_pointers(pointers.begin(), pointers.end());
  EXPECT_EQ(pointers.size(), 5000u + kBlockCount);
  EXPECT_EQ(unique_pointers.size(), kBlockCount);
  const FmodJavaAudioRuntimeStats stats = runtime.GetStats();
  EXPECT_EQ(stats.submitted_buffers, 5000u + kBlockCount);
  EXPECT_EQ(stats.consumed_buffers, 5000u + kBlockCount);
  EXPECT_EQ(stats.pending_buffers, 0u);
  EXPECT_EQ(stats.pending_bytes, 0u);
  EXPECT_TRUE(runtime.Close(Identity(1)).ok());
}

TEST(FmodJavaAudioRuntimeTest,
     HandlesSynchronousReleaseWithoutLeakingOrDoubleCountingSlots) {
  SinkFactoryProbe probe;
  FmodJavaAudioRuntime runtime(TestOptions(&probe));
  ASSERT_TRUE(runtime.Init(Identity(1), 2, 48000, 4, 2).ok());
  ControlledSink* sink = LastSink(&probe);
  ASSERT_NE(sink, nullptr);
  sink->ReleaseSynchronously();

  const std::vector<std::uint8_t> samples(16, 37);
  for (int cycle = 0; cycle < 5000; ++cycle) {
    ASSERT_TRUE(runtime.Write(Identity(1), samples.data(), samples.size()).ok());
  }

  const FmodJavaAudioRuntimeStats stats = runtime.GetStats();
  EXPECT_EQ(stats.submitted_buffers, 5000u);
  EXPECT_EQ(stats.consumed_buffers, 5000u);
  EXPECT_EQ(stats.consumed_bytes, 5000u * samples.size());
  EXPECT_EQ(stats.discarded_buffers, 0u);
  EXPECT_EQ(stats.pending_buffers, 0u);
  EXPECT_EQ(stats.pending_bytes, 0u);
  EXPECT_TRUE(runtime.Close(Identity(1)).ok());
}

TEST(FmodJavaAudioRuntimeTest, KeepsOpaqueDevicesIndependentAndShutsDownOnce) {
  SinkFactoryProbe probe;
  FmodJavaAudioRuntime runtime(TestOptions(&probe));
  ASSERT_TRUE(runtime.Init(Identity(1), 2, 48000, 4, 4).ok());
  ASSERT_TRUE(runtime.Init(Identity(2), 2, 48000, 4, 4).ok());

  const std::vector<std::uint8_t> samples(16, 7);
  ASSERT_TRUE(runtime.Write(Identity(1), samples.data(), samples.size()).ok());
  ASSERT_TRUE(runtime.Write(Identity(2), samples.data(), samples.size()).ok());
  ASSERT_TRUE(runtime.Close(Identity(1)).ok());
  EXPECT_EQ(runtime.GetStats().active_devices, 1u);

  runtime.Shutdown();
  runtime.Shutdown();
  const FmodJavaAudioRuntimeStats stats = runtime.GetStats();
  EXPECT_EQ(stats.active_devices, 0u);
  EXPECT_EQ(stats.pending_buffers, 0u);
  EXPECT_EQ(stats.discarded_buffers, 2u);
  EXPECT_EQ(stats.close_calls, 2u);
  EXPECT_EQ(runtime.Write(Identity(2), samples.data(), samples.size()).code(),
            StatusCode::kFailedPrecondition);
  EXPECT_EQ(runtime.Init(Identity(3), 2, 48000, 4, 4).code(),
            StatusCode::kFailedPrecondition);
}

TEST(FmodJavaAudioRuntimeTest, ConcurrentCloseWaitsForInFlightWrite) {
  SinkFactoryProbe probe;
  FmodJavaAudioRuntime runtime(TestOptions(&probe));
  ASSERT_TRUE(runtime.Init(Identity(1), 2, 48000, 4, 4).ok());
  ControlledSink* sink = LastSink(&probe);
  ASSERT_NE(sink, nullptr);
  sink->BlockEnqueue();

  const std::vector<std::uint8_t> samples(16, 11);
  Status write_status;
  Status close_status;
  std::thread writer([&] {
    write_status = runtime.Write(Identity(1), samples.data(), samples.size());
  });
  sink->WaitForBlockedEnqueue();
  std::thread closer([&] { close_status = runtime.Close(Identity(1)); });
  sink->UnblockEnqueue();
  writer.join();
  closer.join();

  EXPECT_TRUE(write_status.ok()) << write_status.message();
  EXPECT_TRUE(close_status.ok()) << close_status.message();
  const FmodJavaAudioRuntimeStats stats = runtime.GetStats();
  EXPECT_EQ(stats.submitted_buffers, 1u);
  EXPECT_EQ(stats.discarded_buffers, 1u);
  EXPECT_EQ(stats.pending_buffers, 0u);
}

TEST(FmodJavaAudioRuntimeTest, CloseWakesWriteBlockedByQueueCapacity) {
  SinkFactoryProbe probe;
  FmodJavaAudioRuntime runtime(TestOptions(&probe));
  ASSERT_TRUE(runtime.Init(Identity(1), 2, 48000, 4, 1).ok());
  ControlledSink* sink = LastSink(&probe);
  ASSERT_NE(sink, nullptr);

  const std::vector<std::uint8_t> samples(16, 12);
  ASSERT_TRUE(runtime.Write(Identity(1), samples.data(), samples.size()).ok());

  std::promise<Status> write_result;
  std::future<Status> write_future = write_result.get_future();
  std::thread writer([&] {
    write_result.set_value(
        runtime.Write(Identity(1), samples.data(), samples.size()));
  });
  for (int attempt = 0;
       attempt < 100 && runtime.GetStats().write_attempts < 2; ++attempt) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(write_future.wait_for(20ms), std::future_status::timeout);

  const Status close_status = runtime.Close(Identity(1));
  EXPECT_EQ(write_future.wait_for(1s), std::future_status::ready);
  const Status blocked_write_status = write_future.get();
  writer.join();

  EXPECT_TRUE(close_status.ok()) << close_status.message();
  EXPECT_EQ(blocked_write_status.code(), StatusCode::kFailedPrecondition);
  const FmodJavaAudioRuntimeStats stats = runtime.GetStats();
  EXPECT_EQ(stats.submitted_buffers, 1u);
  EXPECT_EQ(stats.discarded_buffers, 1u);
  EXPECT_EQ(stats.rejected_write_calls, 1u);
  EXPECT_EQ(stats.pending_buffers, 0u);
}

}  // namespace
}  // namespace mocktail::audio
