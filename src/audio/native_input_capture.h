#ifndef MOCKTAIL_AUDIO_NATIVE_INPUT_CAPTURE_H_
#define MOCKTAIL_AUDIO_NATIVE_INPUT_CAPTURE_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "mocktail/audio/sdl_audio_capture.h"

// Sink calls and shared ownership use the validated guest vtables.
namespace mocktail::audio {
class NativeInputCapture;

struct NativePcmFormat {
  std::uint32_t rate = 48000;
  std::uint8_t channels = 1;
  std::uint8_t bytes_per_sample = 2;
  std::uint8_t flags = 0;
  std::uint8_t padding = 0;
};
struct NativeOptionalFormat {
  NativePcmFormat format{};
  std::uint64_t present = 0;
};
// std::__ndk1::shared_ptr<T> passed by value, i.e. by invisible reference.
struct NativeSharedSink { void* object; void* owner; };
static_assert(sizeof(NativeOptionalFormat) == 16);
static_assert(sizeof(NativePcmFormat) == 8);
inline std::atomic<NativeInputCapture*> g_native_capture{nullptr};

class NativeInputCapture {
 public:
  using DeliverPcm = std::size_t (*)(void*, const void*, std::size_t,
                                    std::uint64_t, const NativePcmFormat*);
  using OriginalLatency = void (*)(void*, std::uint32_t*, std::uint32_t*);
  using ImageRange = bool (*)(std::uintptr_t image_base, std::uintptr_t rva,
                              std::size_t size);
  struct SinkAbi {
    std::uintptr_t image_base;
    ImageRange is_image_relro;
    ImageRange is_image_code;
    OriginalLatency latency;
  };
  NativeInputCapture(SinkAbi abi, bool enabled)
      : abi_(abi), enabled_(enabled) {}
  ~NativeInputCapture() { Shutdown(); }

  static NativeOptionalFormat Format(void*) {
    auto* capture = g_native_capture.load(std::memory_order_acquire);
    return {{}, capture && capture->enabled_ &&
                        !capture->closed_.load(std::memory_order_acquire)
                    ? 1ULL
                    : 0ULL};
  }
  static void Latency(void* self, std::uint32_t* playback,
                      std::uint32_t* recording) {
    // Preserve the native playback latency and report the host capture queue.
    auto* capture = g_native_capture.load(std::memory_order_acquire);
    if (!capture) return;
    capture->abi_.latency(self, playback, recording);
    if (recording) {
      std::lock_guard lock(capture->mutex_);
      *recording = 10;
      auto it = capture->sessions_.find(self);
      if (it != capture->sessions_.end()) {
        std::lock_guard queue_lock(it->second->queue_mutex);
        *recording += static_cast<std::uint32_t>(it->second->count * 10);
      }
    }
  }
  static bool Start(void* self, NativeSharedSink* sink) {
    auto* capture = g_native_capture.load(std::memory_order_acquire);
    return capture && capture->StartSink(self, sink);
  }
  static void Stop(void* self, NativeSharedSink* sink) {
    auto* capture = g_native_capture.load(std::memory_order_acquire);
    if (capture) capture->StopSink(self, sink);
  }
  static bool Recording(void* self) {
    auto* capture = g_native_capture.load(std::memory_order_acquire);
    if (!capture) return false;
    std::lock_guard lock(capture->mutex_);
    return capture->sessions_.find(self) != capture->sessions_.end();
  }
  static void Poll(void* self) {
    auto* capture = g_native_capture.load(std::memory_order_acquire);
    if (capture) capture->Deliver(self);
  }
  void Shutdown() {
    std::lock_guard lock(mutex_);
    closed_.store(true, std::memory_order_release);
    for (auto& [self, session] : sessions_) Close(*session);
    sessions_.clear();
  }
  // Roblox threads may already be inside an entry point with the old pointer,
  // and its workers are never joined. A published capture is therefore only
  // closed, never freed, so late calls find an empty, closed object.
  static void Retire(std::unique_ptr<NativeInputCapture> capture) {
    g_native_capture.store(nullptr, std::memory_order_release);
    if (capture) capture.release()->Shutdown();
  }

 private:
  struct Sink {
    NativeSharedSink shared;
    DeliverPcm deliver;
  };
  // libc++ __shared_weak_count: both counters are stored minus one. Virtual
  // slots 0/1 are the destructors, 2 is __on_zero_shared, 3 __get_deleter
  // and 4 __on_zero_shared_weak.
  struct GuestSharedCount {
    const std::uintptr_t* vtable;
    long shared_owners;
    long weak_owners;
  };
  using ControlBlockHook = void (*)(GuestSharedCount*);
  struct Session {
    std::unique_ptr<AudioCapture> input;
    std::vector<Sink> sinks;
    std::mutex queue_mutex;
    // At most 200 ms; a stalled consumer must not accumulate stale speech.
    std::array<std::array<std::int16_t, 480>, 20> frames{};
    std::size_t head = 0, count = 0;
    std::uint64_t delivered = 0, dropped = 0;
    bool audible = false;
  };
  static void OnData(void* context, std::size_t bytes) {
    auto& session = *static_cast<Session*>(context);
    if (bytes != sizeof(session.frames[0])) return;
    std::lock_guard lock(session.queue_mutex);
    if (session.count == session.frames.size()) {
      session.head = (session.head + 1) % session.frames.size();
      --session.count;
      ++session.dropped;
    }
    std::memcpy(session.frames[(session.head + session.count) %
                                  session.frames.size()].data(),
                session.input->buffer_data(), bytes);
    ++session.count;
  }
  bool IsGuestVtable(const void* object, std::size_t slot_count,
                     std::initializer_list<std::size_t> code_slots) const {
    const auto base = abi_.image_base;
    const auto table = *static_cast<const std::uintptr_t* const*>(object);
    const auto address = reinterpret_cast<std::uintptr_t>(table);
    if (address < base ||
        !abi_.is_image_relro(base, address - base,
                             slot_count * sizeof(std::uintptr_t)))
      return false;
    for (const std::size_t slot : code_slots)
      if (table[slot] < base || !abi_.is_image_code(base, table[slot] - base, 1))
        return false;
    return true;
  }
  bool StartSink(void* self, NativeSharedSink* sink) {
    std::lock_guard lock(mutex_);
    if (closed_.load(std::memory_order_relaxed) || !enabled_ || !self ||
        !sink || !sink->object || !sink->owner)
      return false;
    if (!IsGuestVtable(sink->object, 3, {2}) ||
        !IsGuestVtable(sink->owner, 5, {2, 4})) {
      std::fprintf(stderr, "  [audio-input] unsupported native PCM sink ABI\n");
      return false;
    }
    auto it = sessions_.find(self);
    if (it != sessions_.end()) {
      for (const auto& existing : it->second->sinks)
        if (existing.shared.object == sink->object) return true;
    } else {
      auto session = std::make_unique<Session>();
      SdlAudioCaptureOptions options;
      options.output_spec.channels = 1;
      options.data_callback = &OnData;
      options.data_context = session.get();
      auto status = CreateSdlAudioCapture(options, &session->input);
      if (status.ok()) status = session->input->Start();
      if (!status.ok()) {
        std::fprintf(stderr, "  [audio-input] capture start failed: %s\n",
                     status.message().c_str());
        return false;
      }
      it = sessions_.emplace(self, std::move(session)).first;
      std::fprintf(stderr,
          "  [audio-input] native capture started: 48000 Hz mono signed16\n");
    }
    const auto* table = *static_cast<const std::uintptr_t* const*>(sink->object);
    // The native parameter is a by-value shared_ptr. Retain its existing
    // ownership by moving it, leaving the caller's temporary empty.
    it->second->sinks.push_back({*sink, reinterpret_cast<DeliverPcm>(table[2])});
    *sink = {};
    return true;
  }
  static void Release(Sink& sink) {
    // Mirrors __shared_weak_count::__release_shared and __release_weak.
    auto* count = static_cast<GuestSharedCount*>(sink.shared.owner);
    sink = {};
    if (!count ||
        __atomic_fetch_sub(&count->shared_owners, 1, __ATOMIC_ACQ_REL) != 0)
      return;
    reinterpret_cast<ControlBlockHook>(count->vtable[2])(count);
    if (__atomic_load_n(&count->weak_owners, __ATOMIC_ACQUIRE) == 0 ||
        __atomic_fetch_sub(&count->weak_owners, 1, __ATOMIC_ACQ_REL) == 0)
      reinterpret_cast<ControlBlockHook>(count->vtable[4])(count);
  }
  void Close(Session& session) {
    session.input->Shutdown();
    for (auto& sink : session.sinks) Release(sink);
    session.sinks.clear();
    std::fprintf(stderr,
        "  [audio-input] native capture stopped: delivered=%llu dropped=%llu\n",
        static_cast<unsigned long long>(session.delivered),
        static_cast<unsigned long long>(session.dropped));
  }
  void StopSink(void* self, NativeSharedSink* sink) {
    std::lock_guard lock(mutex_);
    auto it = sessions_.find(self);
    if (it == sessions_.end() || !sink) return;
    auto& session = *it->second;
    auto existing = std::find_if(session.sinks.begin(), session.sinks.end(),
        [&](const auto& entry) { return entry.shared.object == sink->object; });
    if (existing == session.sinks.end()) return;
    Release(*existing);
    session.sinks.erase(existing);
    if (session.sinks.empty()) {
      Close(session);
      sessions_.erase(it);
    }
  }
  void Deliver(void* self) {
    // Invoked by Roblox's recording thread. Guest code and allocator TLS must
    // never run on SDL's capture callback thread.
    std::lock_guard lock(mutex_);
    auto it = sessions_.find(self);
    if (it == sessions_.end()) return;
    auto& session = *it->second;
    for (std::size_t n = 0; n < session.frames.size(); ++n) {
      std::array<std::int16_t, 480> pcm;
      std::uint64_t delay;
      {
        std::lock_guard queue_lock(session.queue_mutex);
        if (!session.count) break;
        pcm = session.frames[session.head];
        delay = session.count * 10000000ULL;
        session.head = (session.head + 1) % session.frames.size();
        --session.count;
      }
      const NativePcmFormat format;
      for (const auto& sink : session.sinks)
        sink.deliver(sink.shared.object, pcm.data(), pcm.size(), delay,
                     &format);
      if (++session.delivered == 1)
        std::fprintf(stderr, "  [audio-input] PCM delivered to native voice sink\n");
      if (!session.audible && std::any_of(pcm.begin(), pcm.end(),
                                        [](auto value) { return value != 0; })) {
        session.audible = true;
        std::fprintf(stderr, "  [audio-input] nonzero microphone PCM delivered\n");
      }
    }
  }
  SinkAbi abi_;
  bool enabled_;
  std::atomic<bool> closed_{false};
  std::mutex mutex_;
  std::map<void*, std::unique_ptr<Session>> sessions_;
};


}  // namespace mocktail::audio
#endif
