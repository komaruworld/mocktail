#include "audio/native_input_capture.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "mocktail/audio/sdl_audio_sink.h"

namespace mocktail::audio {
namespace {

struct TestSink {
  const std::uintptr_t* vtable;
  std::thread::id poll_thread = std::this_thread::get_id();
  int frames = 0;
  bool nonzero = false;
  bool invalid = false;
};

struct TestOwner {
  const std::uintptr_t* vtable;
  long shared_owners = 0;
  long weak_owners = 0;
  int destroyed = 0;
  int released = 0;
};

std::size_t ReceivePcm(void* self, const void* pcm, std::size_t samples,
                       std::uint64_t delay, const NativePcmFormat* format) {
  auto& sink = *static_cast<TestSink*>(self);
  sink.invalid |= samples != 480 || format->rate != 48000 ||
                  format->channels != 1 || format->bytes_per_sample != 2 ||
                  delay < 10000000 || delay > 200000000 ||
                  std::this_thread::get_id() != sink.poll_thread;
  const auto* data = static_cast<const std::int16_t*>(pcm);
  sink.nonzero |= std::any_of(data, data + samples,
                              [](auto sample) { return sample != 0; });
  ++sink.frames;
  return samples;
}

void DestroySink(void* self) { ++static_cast<TestOwner*>(self)->destroyed; }
void ReleaseOwner(void* self) { ++static_cast<TestOwner*>(self)->released; }
void OriginalLatency(void*, std::uint32_t* playback, std::uint32_t* recording) {
  if (playback) *playback = 42;
  if (recording) *recording = 99;
}

const std::array<std::uintptr_t, 3> kSinkVtable{
    0, 0, reinterpret_cast<std::uintptr_t>(&ReceivePcm)};
const std::array<std::uintptr_t, 5> kOwnerVtable{
    0, 0, reinterpret_cast<std::uintptr_t>(&DestroySink), 0,
    reinterpret_cast<std::uintptr_t>(&ReleaseOwner)};

bool TestRelro(std::uintptr_t base, std::uintptr_t rva, std::size_t size) {
  return (base + rva == reinterpret_cast<std::uintptr_t>(kSinkVtable.data()) &&
          size <= sizeof(kSinkVtable)) ||
         (base + rva == reinterpret_cast<std::uintptr_t>(kOwnerVtable.data()) &&
          size <= sizeof(kOwnerVtable));
}

bool TestCode(std::uintptr_t base, std::uintptr_t rva, std::size_t) {
  return base + rva == kSinkVtable[2] || base + rva == kOwnerVtable[2] ||
         base + rva == kOwnerVtable[4];
}

class NativeInputCaptureTest : public testing::Test {
 protected:
  void SetUp() override {
    const int fd = mkstemp(pcm_path_);
    ASSERT_GE(fd, 0);
    FILE* file = fdopen(fd, "wb");
    if (!file) close(fd);
    ASSERT_NE(file, nullptr);
    const std::vector<unsigned char> samples(1024 * 1024, 0x3f);
    const auto written = std::fwrite(samples.data(), 1, samples.size(), file);
    const int closed = std::fclose(file);
    ASSERT_EQ(written, samples.size());
    ASSERT_EQ(closed, 0);
    ASSERT_TRUE(SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "disk",
                                        SDL_HINT_OVERRIDE));
    ASSERT_TRUE(SDL_SetHintWithPriority(SDL_HINT_AUDIO_DISK_INPUT_FILE,
                                        pcm_path_, SDL_HINT_OVERRIDE));
    ASSERT_TRUE(SDL_SetHintWithPriority(SDL_HINT_AUDIO_DISK_OUTPUT_FILE,
                                        "/dev/null", SDL_HINT_OVERRIDE));
    ASSERT_TRUE(InitializeSdlAudioSubsystem().ok());
    ASSERT_STREQ(SDL_GetCurrentAudioDriver(), "disk");
  }

  void TearDown() override {
    g_native_capture.store(nullptr);
    if (capture_) capture_->Shutdown();
    EXPECT_TRUE(ShutdownSdlAudioSubsystem().ok());
    SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
    SDL_ResetHint(SDL_HINT_AUDIO_DISK_INPUT_FILE);
    SDL_ResetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE);
    std::remove(pcm_path_);
  }

  void Publish(bool enabled) {
    capture_ = std::make_unique<NativeInputCapture>(
        NativeInputCapture::SinkAbi{1, &TestRelro, &TestCode, &OriginalLatency},
        enabled);
    g_native_capture.store(capture_.get());
  }

  char pcm_path_[64] = "/tmp/mocktail-native-capture-XXXXXX";
  std::unique_ptr<NativeInputCapture> capture_;
  TestSink sink_{kSinkVtable.data()};
  TestOwner owner_{kOwnerVtable.data()};
  int device_ = 0;
};

TEST_F(NativeInputCaptureTest, DeliversPcmAndReopensAfterStop) {
  Publish(true);
  const auto format = NativeInputCapture::Format(&device_);
  EXPECT_EQ(format.present, 1u);
  EXPECT_EQ(format.format.rate, 48000u);
  EXPECT_EQ(format.format.channels, 1u);
  for (int pass = 0; pass < 2; ++pass) {
    owner_ = {kOwnerVtable.data()};
    sink_ = {kSinkVtable.data()};
    NativeSharedSink parameter{&sink_, &owner_};
    ASSERT_TRUE(NativeInputCapture::Start(&device_, &parameter));
    EXPECT_EQ(parameter.object, nullptr);
    EXPECT_EQ(parameter.owner, nullptr);
    EXPECT_TRUE(NativeInputCapture::Recording(&device_));
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sink_.frames < 3 && std::chrono::steady_clock::now() < deadline) {
      NativeInputCapture::Poll(&device_);
      SDL_Delay(10);
    }
    EXPECT_GE(sink_.frames, 3);
    EXPECT_TRUE(sink_.nonzero);
    EXPECT_FALSE(sink_.invalid);
    std::uint32_t playback = 0, recording = 0;
    NativeInputCapture::Latency(&device_, &playback, &recording);
    EXPECT_EQ(playback, 42u);
    EXPECT_GE(recording, 10u);
    NativeSharedSink stop{&sink_, &owner_};
    NativeInputCapture::Stop(&device_, &stop);
    EXPECT_FALSE(NativeInputCapture::Recording(&device_));
    EXPECT_EQ(owner_.destroyed, 1);
    EXPECT_EQ(owner_.released, 1);
    const int frames = sink_.frames;
    NativeInputCapture::Poll(&device_);
    NativeInputCapture::Stop(&device_, &stop);
    EXPECT_EQ(sink_.frames, frames);
    EXPECT_EQ(owner_.released, 1);
  }
}

TEST_F(NativeInputCaptureTest, RejectsDisabledAndClosedCapture) {
  Publish(false);
  NativeSharedSink parameter{&sink_, &owner_};
  EXPECT_EQ(NativeInputCapture::Format(&device_).present, 0u);
  EXPECT_FALSE(NativeInputCapture::Start(&device_, &parameter));
  EXPECT_EQ(parameter.object, &sink_);
  Publish(true);
  ASSERT_TRUE(NativeInputCapture::Start(&device_, &parameter));
  capture_->Shutdown();
  EXPECT_EQ(NativeInputCapture::Format(&device_).present, 0u);
  EXPECT_FALSE(NativeInputCapture::Recording(&device_));
  parameter = {&sink_, &owner_};
  EXPECT_FALSE(NativeInputCapture::Start(&device_, &parameter));
  EXPECT_EQ(owner_.released, 1);
}

TEST_F(NativeInputCaptureTest, RejectsUnverifiedSinkWithoutTakingOwnership) {
  Publish(true);
  const std::array<std::uintptr_t, 3> unknown_vtable{};
  TestSink unknown{unknown_vtable.data()};
  NativeSharedSink parameter{&unknown, &owner_};
  EXPECT_FALSE(NativeInputCapture::Start(&device_, &parameter));
  EXPECT_EQ(parameter.object, &unknown);
  EXPECT_EQ(parameter.owner, &owner_);
  EXPECT_FALSE(NativeInputCapture::Recording(&device_));
  EXPECT_EQ(owner_.released, 0);
}

}  // namespace
}  // namespace mocktail::audio
