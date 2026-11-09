#include "mocktail/audio/roblox_output_device_bridge.h"

#include <gtest/gtest.h>
#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

#include "audio/native_input_capture.h"
#include "audio/roblox_output_device_bridge_internal.h"
#include "compat/fmod_output_device_contract.h"
#include "mocktail/audio/sdl_audio_sink.h"

namespace mocktail::audio {
namespace {

compat::FmodOutputDeviceBridgeProfile TestProfile() {
  return compat::FmodOutputDeviceBridgeProfile{0x1000, 0x2000, 0x3000,
                                               0x4000, 0x5000, 0x6000};
}

TEST(RobloxOutputDeviceBridgeTest, ValidatesExactGuestStringAbiContract) {
  constexpr std::array<std::uint8_t, 24> kExpected = {
      0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48,
      0x89, 0xd3, 0x49, 0x89, 0xf6, 0x49, 0x89, 0xff, 0x48, 0x83, 0xfa, 0x16,
  };
  EXPECT_TRUE(internal::HasExpectedFmodStringConstructorContract(
      kExpected.data(), kExpected.size()));

  auto changed = kExpected;
  changed.back() = 0x17;
  EXPECT_FALSE(internal::HasExpectedFmodStringConstructorContract(
      changed.data(), changed.size()));
  EXPECT_FALSE(internal::HasExpectedFmodStringConstructorContract(
      kExpected.data(), kExpected.size() - 1));
}

TEST(RobloxOutputDeviceBridgeTest, ValidatesEveryInterposedVtableSlot) {
  constexpr std::uintptr_t kImageBase = 0x10000000;
  const compat::FmodOutputDeviceBridgeProfile profile = TestProfile();
  std::array<std::uintptr_t, 18> vtable{};
  vtable[5] = kImageBase + profile.count_method_rva;
  vtable[6] = kImageBase + profile.info_method_rva;
  vtable[7] = kImageBase + profile.current_method_rva;
  vtable[17] = kImageBase + profile.select_method_rva;
  EXPECT_TRUE(internal::HasExpectedFmodOutputDeviceVtable(vtable.data(),
                                                          kImageBase, profile));

  ++vtable[17];
  EXPECT_FALSE(internal::HasExpectedFmodOutputDeviceVtable(
      vtable.data(), kImageBase, profile));
}

TEST(RobloxOutputDeviceBridgeTest, Validates3092LayoutWithoutUsingListSlots) {
  constexpr std::uintptr_t kBase = 0x10000000;
  auto profile = TestProfile();
  profile.vtable_layout_version = 2;
  std::array<std::uintptr_t, 20> vtable{};
  vtable[5] = kBase + profile.count_method_rva;
  vtable[6] = kBase + profile.info_method_rva;
  vtable[7] = kBase + 0x7000;
  vtable[8] = kBase + profile.current_method_rva;
  vtable[17] = kBase + 0x8000;
  vtable[19] = kBase + profile.select_method_rva;
  EXPECT_TRUE(internal::HasExpectedFmodOutputDeviceVtable(vtable.data(), kBase,
                                                          profile));
  profile.vtable_layout_version = 1;
  EXPECT_FALSE(internal::HasExpectedFmodOutputDeviceVtable(vtable.data(), kBase,
                                                           profile));
  profile.vtable_layout_version = 3;
  EXPECT_FALSE(internal::HasExpectedFmodOutputDeviceVtable(vtable.data(), kBase,
                                                           profile));
}

TEST(RobloxOutputDeviceBridgeTest, PinsCaptureToBuildIdVtableAndLayout) {
  compat::BuildProfile profile;
  profile.allow_host_abi_bridges = true;
  profile.elf_build_id = "5f0704edd9064f566ee3d6df2bd2fabbcc709f03";
  EXPECT_EQ(internal::FindFmodInputCaptureProfile(profile), nullptr);
  profile.fmod_output_device_bridge = TestProfile();
  EXPECT_EQ(internal::FindFmodInputCaptureProfile(profile), nullptr);
  profile.fmod_output_device_bridge->vtable_rva = 0x6cd3ce0;
  EXPECT_EQ(internal::FindFmodInputCaptureProfile(profile), nullptr);
  profile.fmod_output_device_bridge->vtable_layout_version = 2;
  ASSERT_NE(internal::FindFmodInputCaptureProfile(profile), nullptr);
  profile.allow_host_abi_bridges = false;
  EXPECT_EQ(internal::FindFmodInputCaptureProfile(profile), nullptr);
  profile.allow_host_abi_bridges = true;
  profile.elf_build_id = "unknown";
  EXPECT_EQ(internal::FindFmodInputCaptureProfile(profile), nullptr);
}

TEST(RobloxOutputDeviceBridgeTest, ValidatesEveryCaptureSlotOnBothBuilds) {
  constexpr std::uintptr_t kBase = 0x10000000;
  for (int version : {2998, 3092}) {
    compat::BuildProfile profile;
    profile.allow_host_abi_bridges = true;
    profile.elf_build_id = version == 2998
                               ? "ade08266c67aee88ec9c1d00902150e1684dad3a"
                               : "5f0704edd9064f566ee3d6df2bd2fabbcc709f03";
    profile.fmod_output_device_bridge = TestProfile();
    profile.fmod_output_device_bridge->vtable_rva =
        version == 2998 ? 0x6c58040 : 0x6cd3ce0;
    profile.fmod_output_device_bridge->vtable_layout_version =
        version == 2998 ? 1 : 2;
    const auto* capture = internal::FindFmodInputCaptureProfile(profile);
    ASSERT_NE(capture, nullptr);
    const std::array<std::size_t, 6> expected_slots =
        version == 2998 ? std::array<std::size_t, 6>{4, 11, 12, 13, 14, 15}
                        : std::array<std::size_t, 6>{4, 13, 14, 15, 16, 17};
    EXPECT_EQ(capture->slots, expected_slots);
    std::array<std::uintptr_t, 20> vtable{};
    for (std::size_t i = 0; i < capture->slots.size(); ++i) {
      vtable[capture->slots[i]] = kBase + capture->method_rvas[i];
    }
    EXPECT_TRUE(internal::HasExpectedFmodInputCaptureVtable(vtable.data(),
                                                            kBase, *capture));
    for (const auto slot : capture->slots) {
      ++vtable[slot];
      EXPECT_FALSE(internal::HasExpectedFmodInputCaptureVtable(vtable.data(),
                                                               kBase, *capture))
          << slot;
      --vtable[slot];
    }
    EXPECT_FALSE(
        internal::HasExpectedFmodInputCaptureVtable(nullptr, kBase, *capture));
    EXPECT_FALSE(internal::HasExpectedFmodInputCaptureVtable(vtable.data(), 0,
                                                             *capture));
    EXPECT_FALSE(internal::HasExpectedFmodInputCaptureVtable(
        vtable.data(), UINTPTR_MAX, *capture));
  }
}

TEST(RobloxOutputDeviceBridgeTest, BuildsStableDistinctHostGuids) {
  const std::string first = internal::MakeOutputDeviceGuid(17, "USB Headset");
  EXPECT_EQ(first, internal::MakeOutputDeviceGuid(17, "USB Headset"));
  EXPECT_NE(first, internal::MakeOutputDeviceGuid(18, "USB Headset"));
  EXPECT_NE(first, internal::MakeOutputDeviceGuid(17, "HDMI Output"));
  EXPECT_EQ(internal::MakeOutputDeviceGuid(0, "ignored"), "mocktail:default");
}

TEST(RobloxOutputDeviceBridgeTest, UsesDeviceListSlotsWithoutReusingLegacySlots) {
  constexpr std::uintptr_t kImageBase = 0x10000000;
  auto profile = TestProfile();
  profile.vtable_layout_version = 2;
  std::array<std::uintptr_t, 20> vtable{};
  vtable[5] = kImageBase + profile.count_method_rva;
  vtable[6] = kImageBase + profile.info_method_rva;
  vtable[8] = kImageBase + profile.current_method_rva;
  vtable[19] = kImageBase + profile.select_method_rva;
  EXPECT_TRUE(internal::HasExpectedFmodOutputDeviceVtable(
      vtable.data(), kImageBase, profile));
  profile.vtable_layout_version = 1;
  EXPECT_FALSE(internal::HasExpectedFmodOutputDeviceVtable(
      vtable.data(), kImageBase, profile));
  profile.vtable_layout_version = 2;
  ++vtable[19];
  EXPECT_FALSE(internal::HasExpectedFmodOutputDeviceVtable(
      vtable.data(), kImageBase, profile));
}

TEST(RobloxOutputDeviceBridgeTest, RejectsAnUnknownLayoutBeforeInstalling) {
  compat::BuildProfile profile;
  profile.allow_host_abi_bridges = true;
  profile.fmod_output_device_bridge = TestProfile();
  profile.fmod_output_device_bridge->vtable_layout_version = 3;
  RobloxOutputDeviceBridge bridge;
  EXPECT_EQ(bridge.Install(profile).code(), StatusCode::kFailedPrecondition);
}

TEST(RobloxOutputDeviceBridgeTest, DeviceListSelectorKeepsArgumentsAndCountSlotExact) {
  std::string relocated(compat::kFmodDeviceListsSelectContract,
                        compat::kFmodDeviceListsSelectContractSize);
  for (const std::size_t start : {24, 38, 55, 78, 96, 116}) {
    relocated.replace(start, 4, 4, '\x17');
  }
  EXPECT_TRUE(compat::HasFmodDeviceListsSelectContract(relocated));
  // Device index register, branch into legacy path, count slot, system offset.
  for (const std::size_t index : {17, 44, 90, 112}) {
    auto changed = relocated;
    changed[index] ^= 1;
    EXPECT_FALSE(compat::HasFmodDeviceListsSelectContract(changed));
  }
  relocated.pop_back();
  EXPECT_FALSE(compat::HasFmodDeviceListsSelectContract(relocated));
}

TEST(RobloxOutputDeviceBridgeTest, ValidatesInputSlotsForBothLayouts) {
  constexpr std::uintptr_t base = 0x10000000;
  for (int layout : {1, 2}) {
    auto profile = TestProfile();
    profile.vtable_layout_version = layout;
    profile.input_method_rvas = {0x7000, 0x8000, 0x9000, 0xa000};
    std::array<std::uintptr_t, 20> vtable{};
    vtable[5] = base + profile.count_method_rva;
    vtable[6] = base + profile.info_method_rva;
    vtable[profile.current_vtable_index()] = base + profile.current_method_rva;
    vtable[profile.select_vtable_index()] = base + profile.select_method_rva;
    const auto indexes = profile.input_vtable_indexes();
    for (std::size_t i = 0; i < 4; ++i)
      vtable[indexes[i]] = base + profile.input_method_rvas[i];
    ASSERT_TRUE(internal::HasExpectedFmodOutputDeviceVtable(vtable.data(), base,
                                                            profile));
    for (const auto index : indexes) {
      ++vtable[index];
      EXPECT_FALSE(internal::HasExpectedFmodOutputDeviceVtable(vtable.data(),
                                                               base, profile));
      --vtable[index];
    }
  }
}

TEST(RobloxOutputDeviceBridgeTest, EnforcesSingleProcessOwner) {
  compat::BuildProfile profile;
  profile.elf_build_id = "d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21";
  profile.allow_host_abi_bridges = true;
  profile.fmod_output_device_bridge = TestProfile();

  RobloxOutputDeviceBridge first;
  RobloxOutputDeviceBridge second;
  ASSERT_TRUE(first.Install(profile).ok());
  EXPECT_EQ(second.Install(profile).code(), StatusCode::kFailedPrecondition);
  first.Shutdown();
  EXPECT_TRUE(second.Install(profile).ok());
  second.Shutdown();
}

class NativeInputCaptureTest : public testing::Test {
 protected:
  struct PcmSink {
    const std::uintptr_t* vtable = sink_table.data();
    std::size_t frames = 0;
    int peak = 0;
  };
  struct Owner {
    const std::uintptr_t* vtable = owner_table.data();
    long shared = 0;
    long weak = 0;
    int released = 0;
    int freed = 0;
    int* freed_counter = nullptr;
  };
  static std::size_t Deliver(void* object, const void* data, std::size_t frames,
                             std::uint64_t delay, const NativePcmFormat* format) {
    EXPECT_EQ(frames, 480U);
    EXPECT_EQ(format->rate, 48000U);
    EXPECT_EQ(format->channels, 1);
    EXPECT_EQ(format->bytes_per_sample, 2);
    EXPECT_GE(delay, 10000000U);
    EXPECT_LE(delay, 200000000U);
    auto& sink = *static_cast<PcmSink*>(object);
    sink.frames += frames;
    const auto* pcm = static_cast<const std::int16_t*>(data);
    for (std::size_t i = 0; i < frames; ++i)
      sink.peak = std::max(sink.peak, std::abs(static_cast<int>(pcm[i])));
    return frames;
  }
  static void Release(void* owner) { ++static_cast<Owner*>(owner)->released; }
  static void Free(void* opaque) {
    auto* owner = static_cast<Owner*>(opaque);
    ++owner->freed;
    if (owner->freed_counter) {
      ++*owner->freed_counter;
      delete owner;
    }
  }
  static void Latency(void*, std::uint32_t* playback, std::uint32_t* recording) {
    if (playback) *playback = 25;
    if (recording) *recording = 0;
  }
  bool PollUntil(PcmSink& sink, std::size_t frames) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sink.frames < frames && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      NativeInputCapture::Poll(this);
    }
    return sink.frames >= frames;
  }
  static bool Relro(std::uintptr_t base, std::uintptr_t rva, std::size_t size) {
    return (base + rva == reinterpret_cast<std::uintptr_t>(sink_table.data()) &&
            size <= sizeof(sink_table)) ||
           (base + rva == reinterpret_cast<std::uintptr_t>(owner_table.data()) &&
            size <= sizeof(owner_table));
  }
  static bool Code(std::uintptr_t base, std::uintptr_t rva, std::size_t) {
    return base + rva == reinterpret_cast<std::uintptr_t>(&Deliver) ||
           base + rva == reinterpret_cast<std::uintptr_t>(&Release) ||
           base + rva == reinterpret_cast<std::uintptr_t>(&Free);
  }
  void SetUp() override {
    ASSERT_TRUE(SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy",
                                       SDL_HINT_OVERRIDE));
    ASSERT_TRUE(InitializeSdlAudioSubsystem().ok());
    capture = std::make_shared<NativeInputCapture>(
        NativeInputCapture::SinkAbi{1, &Relro, &Code, &Latency}, true);
    std::atomic_store(&g_native_capture, capture);
  }
  void TearDown() override {
    std::atomic_store(&g_native_capture, std::shared_ptr<NativeInputCapture>{});
    capture.reset();
    EXPECT_TRUE(ShutdownSdlAudioSubsystem().ok());
    SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
    SDL_ResetHint(SDL_HINT_AUDIO_DISK_INPUT_FILE);
    SDL_ResetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE);
    if (!pcm_path.empty()) std::remove(pcm_path.c_str());
  }
  inline static const std::array<std::uintptr_t, 3> sink_table{
      0, 0, reinterpret_cast<std::uintptr_t>(&Deliver)};
  inline static const std::array<std::uintptr_t, 5> owner_table{
      0, 0, reinterpret_cast<std::uintptr_t>(&Release), 0,
      reinterpret_cast<std::uintptr_t>(&Free)};
  PcmSink first, second;
  Owner first_owner, second_owner;
  std::shared_ptr<NativeInputCapture> capture;
  std::string pcm_path;
};

TEST_F(NativeInputCaptureTest, DeliversPcmAndReleasesEachGuestSink) {
  NativeSharedSink a{&first, &first_owner}, b{&second, &second_owner};
  ASSERT_TRUE(NativeInputCapture::Start(this, &a));
  ASSERT_TRUE(NativeInputCapture::Start(this, &b));
  EXPECT_EQ(a.object, nullptr);
  EXPECT_EQ(a.owner, nullptr);
  EXPECT_EQ(b.object, nullptr);
  EXPECT_EQ(b.owner, nullptr);
  EXPECT_TRUE(PollUntil(first, 480));
  EXPECT_EQ(first.frames, second.frames);
  a = {&first, &first_owner};
  NativeInputCapture::Stop(this, &a);
  EXPECT_TRUE(NativeInputCapture::Recording(this));
  EXPECT_EQ(first_owner.released, 1);
  EXPECT_EQ(first_owner.freed, 1);
  b = {&second, &second_owner};
  NativeInputCapture::Stop(this, &b);
  EXPECT_FALSE(NativeInputCapture::Recording(this));
  EXPECT_EQ(second_owner.released, 1);
  EXPECT_EQ(second_owner.freed, 1);
}

TEST_F(NativeInputCaptureTest, PreservesOtherSharedAndWeakOwners) {
  first_owner.shared = 1;
  second_owner.weak = 1;
  NativeSharedSink a{&first, &first_owner}, b{&second, &second_owner};
  ASSERT_TRUE(NativeInputCapture::Start(this, &a));
  ASSERT_TRUE(NativeInputCapture::Start(this, &b));
  capture->Shutdown();
  EXPECT_EQ(first_owner.shared, 0);
  EXPECT_EQ(first_owner.released, 0);
  EXPECT_EQ(first_owner.freed, 0);
  EXPECT_EQ(second_owner.weak, 0);
  EXPECT_EQ(second_owner.released, 1);
  EXPECT_EQ(second_owner.freed, 0);
}

TEST_F(NativeInputCaptureTest, RejectsUnverifiedSinkAndOwnerTables) {
  auto unverified = sink_table;
  first.vtable = unverified.data();
  NativeSharedSink sink{&first, &first_owner};
  EXPECT_FALSE(NativeInputCapture::Start(this, &sink));
  first.vtable = sink_table.data();
  first_owner.vtable = sink_table.data();
  EXPECT_FALSE(NativeInputCapture::Start(this, &sink));
  EXPECT_FALSE(NativeInputCapture::Recording(this));
  EXPECT_EQ(sink.object, &first);
  EXPECT_EQ(first_owner.released, 0);
}

TEST_F(NativeInputCaptureTest, DisabledCaptureDoesNotTakeOwnership) {
  capture = std::make_shared<NativeInputCapture>(
      NativeInputCapture::SinkAbi{1, &Relro, &Code, nullptr}, false);
  std::atomic_store(&g_native_capture, capture);
  NativeSharedSink sink{&first, &first_owner};
  EXPECT_EQ(NativeInputCapture::Format(this).present, 0U);
  EXPECT_FALSE(NativeInputCapture::Start(this, &sink));
  EXPECT_EQ(sink.object, &first);
  EXPECT_EQ(first_owner.released, 0);
}

TEST_F(NativeInputCaptureTest, ShutdownRejectsLateRecordingRequests) {
  capture->Shutdown();
  NativeSharedSink sink{&first, &first_owner};
  EXPECT_EQ(NativeInputCapture::Format(this).present, 0U);
  EXPECT_FALSE(NativeInputCapture::Start(this, &sink));
  EXPECT_FALSE(NativeInputCapture::Recording(this));
  EXPECT_EQ(sink.object, &first);
  EXPECT_EQ(sink.owner, &first_owner);
}

TEST_F(NativeInputCaptureTest, DeliversNonzeroPcmFromSyntheticRecording) {
  bool have_disk = false;
  for (int i = 0; i < SDL_GetNumAudioDrivers(); ++i)
    have_disk |= std::strcmp(SDL_GetAudioDriver(i), "disk") == 0;
  if (!have_disk) GTEST_SKIP() << "SDL disk audio driver is unavailable";
  ASSERT_TRUE(ShutdownSdlAudioSubsystem().ok());
  char path[] = "/tmp/mocktail-native-pcm-XXXXXX";
  const int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  pcm_path = path;
  FILE* file = fdopen(fd, "wb");
  if (!file) close(fd);
  ASSERT_NE(file, nullptr);
  const std::vector<unsigned char> pcm(1024 * 1024, 0x3f);
  const auto written = std::fwrite(pcm.data(), 1, pcm.size(), file);
  const int closed = std::fclose(file);
  ASSERT_EQ(written, pcm.size());
  ASSERT_EQ(closed, 0);
  ASSERT_TRUE(SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "disk",
                                     SDL_HINT_OVERRIDE));
  ASSERT_TRUE(SDL_SetHint(SDL_HINT_AUDIO_DISK_INPUT_FILE, path));
  ASSERT_TRUE(SDL_SetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE, "/dev/null"));
  ASSERT_TRUE(InitializeSdlAudioSubsystem().ok());
  NativeSharedSink sink{&first, &first_owner};
  ASSERT_TRUE(NativeInputCapture::Start(this, &sink));
  ASSERT_TRUE(PollUntil(first, 480 * 3));
  EXPECT_GT(first.peak, 0);
}

TEST_F(NativeInputCaptureTest, CanStopRestartAndMigrateAnActiveRecording) {
  for (int run = 0; run < 3; ++run) {
    first_owner = {};
    NativeSharedSink sink{&first, &first_owner};
    ASSERT_TRUE(NativeInputCapture::Start(this, &sink));
    ASSERT_TRUE(PollUntil(first, first.frames + 480));
    std::string name;
    ASSERT_TRUE(SwitchSdlRecordingDevice(0, &name).ok());
    ASSERT_TRUE(PollUntil(first, first.frames + 480));
    EXPECT_FALSE(SwitchSdlRecordingDevice(0xffffffffU, &name).ok());
    ASSERT_TRUE(PollUntil(first, first.frames + 480));
    sink = {&first, &first_owner};
    NativeInputCapture::Stop(this, &sink);
    const auto frames = first.frames;
    NativeInputCapture::Poll(this);
    NativeInputCapture::Stop(this, &sink);
    EXPECT_EQ(first.frames, frames);
    EXPECT_EQ(first_owner.released, 1);
    EXPECT_EQ(first_owner.freed, 1);
  }
}

TEST_F(NativeInputCaptureTest, BoundsQueuedAudioWhenTheConsumerStalls) {
  NativeSharedSink sink{&first, &first_owner};
  ASSERT_TRUE(NativeInputCapture::Start(this, &sink));
  std::uint32_t playback = 0, recording = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (recording < 210 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    NativeInputCapture::Latency(this, &playback, &recording);
  }
  EXPECT_EQ(playback, 25U);
  ASSERT_EQ(recording, 210U);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  NativeInputCapture::Latency(this, nullptr, &recording);
  EXPECT_EQ(recording, 210U);
  NativeInputCapture::Poll(this);
  EXPECT_GT(first.frames, 0U);
  EXPECT_LE(first.frames, 20U * 480);
}

TEST_F(NativeInputCaptureTest, FreesAHeapControlBlockExactlyOnce) {
  int freed = 0;
  auto* owner = new Owner;
  owner->freed_counter = &freed;
  NativeSharedSink sink{&first, owner};
  ASSERT_TRUE(NativeInputCapture::Start(this, &sink));
  sink = {&first, owner};
  NativeInputCapture::Stop(this, &sink);
  NativeInputCapture::Stop(this, &sink);
  capture->Shutdown();
  EXPECT_EQ(freed, 1);
}

TEST_F(NativeInputCaptureTest, ConcurrentPollingStopsBeforeShutdownReturns) {
  NativeSharedSink sink{&first, &first_owner};
  ASSERT_TRUE(NativeInputCapture::Start(this, &sink));
  std::atomic<bool> done{false};
  std::promise<void> started;
  std::thread poller([&] {
    NativeInputCapture::Poll(this);
    started.set_value();
    while (!done.load()) {
      NativeInputCapture::Poll(this);
      NativeInputCapture::Format(this);
      NativeInputCapture::Recording(this);
      std::this_thread::yield();
    }
  });
  started.get_future().wait();
  capture->Shutdown();
  const auto frames = first.frames;
  std::atomic_store(&g_native_capture, std::shared_ptr<NativeInputCapture>{});
  capture.reset();
  done.store(true);
  poller.join();
  EXPECT_EQ(first.frames, frames);
  EXPECT_EQ(first_owner.released, 1);
  EXPECT_EQ(first_owner.freed, 1);
}

TEST_F(NativeInputCaptureTest, ShutdownWaitsForAnInFlightNativeCall) {
  struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool released = false;
  } gate;
  auto latency = +[](void* opaque, std::uint32_t*, std::uint32_t*) {
    auto& g = *static_cast<Gate*>(opaque);
    std::unique_lock lock(g.mutex);
    g.entered = true;
    g.cv.notify_all();
    g.cv.wait(lock, [&] { return g.released; });
  };
  capture = std::make_shared<NativeInputCapture>(
      NativeInputCapture::SinkAbi{1, &Relro, &Code, latency}, true);
  std::weak_ptr<NativeInputCapture> lifetime = capture;
  std::atomic_store(&g_native_capture, capture);
  std::thread caller([&] { NativeInputCapture::Latency(&gate, nullptr, nullptr); });
  {
    std::unique_lock lock(gate.mutex);
    gate.cv.wait(lock, [&] { return gate.entered; });
  }
  std::atomic_store(&g_native_capture, std::shared_ptr<NativeInputCapture>{});
  auto shutdown = std::async(std::launch::async, [owned = std::move(capture)]() mutable {
    owned->Shutdown();
    owned.reset();
  });
  EXPECT_EQ(shutdown.wait_for(std::chrono::milliseconds(30)),
            std::future_status::timeout);
  EXPECT_FALSE(lifetime.expired());
  {
    std::lock_guard lock(gate.mutex);
    gate.released = true;
  }
  gate.cv.notify_all();
  caller.join();
  shutdown.get();
  EXPECT_TRUE(lifetime.expired());
}

}  // namespace
}  // namespace mocktail::audio
