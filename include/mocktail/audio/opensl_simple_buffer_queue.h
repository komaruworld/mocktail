#ifndef MOCKTAIL_AUDIO_OPENSL_SIMPLE_BUFFER_QUEUE_H_
#define MOCKTAIL_AUDIO_OPENSL_SIMPLE_BUFFER_QUEUE_H_

#include <cstdint>
#include <memory>

#include "mocktail/audio/audio_sink.h"
#include "mocktail/audio/opensl_abi.h"

namespace mocktail::audio {

enum class OpenSlBufferQueueEvent {
  kSubmitted,
  kConsumed,
  kDiscarded,
};

using OpenSlBufferQueueEventCallback = void (*)(
    void* context, OpenSlBufferQueueEvent event, std::size_t size_bytes);

struct OpenSlSimpleBufferQueueOptions {
  std::uint32_t max_buffers = 8;
  OpenSlBufferQueueEventCallback event_callback = nullptr;
  void* event_context = nullptr;
};

struct OpenSlSimpleBufferQueueStats {
  std::uint64_t submitted_buffers = 0;
  std::uint64_t consumed_buffers = 0;
  std::uint64_t discarded_buffers = 0;
  std::uint64_t consumed_bytes = 0;
  std::size_t pending_buffers = 0;
};

// Minimal AndroidSimpleBufferQueue-to-AudioSink adapter. It does
// not fabricate an OpenSL engine, mixer, recorder, or resampler. The adapter
// owns the sink, retains each borrowed OpenSL buffer until the sink releases
// it, and dispatches OpenSL callbacks outside all queue/device locks.
class OpenSlSimpleBufferQueueAdapter final {
 public:
  static Status Create(
      std::unique_ptr<AudioSink> sink,
      const OpenSlSimpleBufferQueueOptions& options,
      std::unique_ptr<OpenSlSimpleBufferQueueAdapter>* adapter);

  ~OpenSlSimpleBufferQueueAdapter();

  OpenSlSimpleBufferQueueAdapter(const OpenSlSimpleBufferQueueAdapter&) =
      delete;
  OpenSlSimpleBufferQueueAdapter& operator=(
      const OpenSlSimpleBufferQueueAdapter&) = delete;

  opensl_abi::AndroidSimpleBufferQueue interface() const;
  Status SetPlaying(bool playing);
  Status SetGain(float linear_gain);
  OpenSlSimpleBufferQueueStats GetStats() const;
  Status last_error() const;
  void Shutdown();

 private:
  struct State;

  explicit OpenSlSimpleBufferQueueAdapter(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;
};

// Capture is outside the current compatibility boundary. Callers get an
// explicit unsupported result instead of a successful recorder-shaped no-op.
Status OpenSlRecorderSupportStatus();

}  // namespace mocktail::audio

#endif  // MOCKTAIL_AUDIO_OPENSL_SIMPLE_BUFFER_QUEUE_H_
