#include "mocktail/audio/fmod_java_audio_runtime.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "mocktail/audio/sdl_audio_sink.h"

namespace mocktail::audio {
namespace {

struct RuntimeCounters {
  std::atomic<std::uint64_t> init_attempts{0};
  std::atomic<std::uint64_t> initialized_devices{0};
  std::atomic<std::uint64_t> rejected_init_calls{0};
  std::atomic<std::uint64_t> close_calls{0};
  std::atomic<std::uint64_t> rejected_close_calls{0};
  std::atomic<std::uint64_t> write_attempts{0};
  std::atomic<std::uint64_t> submitted_buffers{0};
  std::atomic<std::uint64_t> submitted_bytes{0};
  std::atomic<std::uint64_t> consumed_buffers{0};
  std::atomic<std::uint64_t> consumed_bytes{0};
  std::atomic<std::uint64_t> discarded_buffers{0};
  std::atomic<std::uint64_t> discarded_bytes{0};
  std::atomic<std::uint64_t> rejected_write_calls{0};
  std::atomic<std::uint64_t> active_devices{0};
  std::atomic<std::uint64_t> pending_buffers{0};
  std::atomic<std::uint64_t> pending_bytes{0};
};

enum class BufferSlotState {
  kFree,
  kReserved,
  kPending,
};

struct DeviceState;

struct BufferSlot {
  DeviceState* device = nullptr;
  std::uint8_t* bytes = nullptr;
  std::size_t size_bytes = 0;
  BufferSlotState state = BufferSlotState::kFree;
};

struct DeviceState {
  DeviceState(std::unique_ptr<AudioSink> owned_sink, std::size_t frame_bytes,
              std::size_t buffer_size_bytes, std::size_t buffer_count,
              std::shared_ptr<RuntimeCounters> runtime_counters)
      : sink(std::move(owned_sink)),
        bytes_per_frame(frame_bytes),
        block_bytes(buffer_size_bytes),
        slot_count(buffer_count),
        slots(new (std::nothrow) BufferSlot[buffer_count]),
        storage(new (std::nothrow)
                    std::uint8_t[buffer_size_bytes * buffer_count]),
        counters(std::move(runtime_counters)) {}

  bool InitializeSlots() {
    if (slots == nullptr || storage == nullptr) {
      return false;
    }
    for (std::size_t index = 0; index < slot_count; ++index) {
      slots[index].device = this;
      slots[index].bytes = storage.get() + index * block_bytes;
    }
    return true;
  }

  BufferSlot* FindFreeSlot() {
    for (std::size_t index = 0; index < slot_count; ++index) {
      if (slots[index].state == BufferSlotState::kFree) {
        return &slots[index];
      }
    }
    return nullptr;
  }

  std::mutex operation_mutex;
  std::mutex mutex;
  std::condition_variable capacity_cv;
  std::unique_ptr<AudioSink> sink;
  const std::size_t bytes_per_frame;
  const std::size_t block_bytes;
  const std::size_t slot_count;
  const std::unique_ptr<BufferSlot[]> slots;
  const std::unique_ptr<std::uint8_t[]> storage;
  const std::shared_ptr<RuntimeCounters> counters;
  std::size_t pending_buffers = 0;
  std::size_t pending_bytes = 0;
  bool closing = false;
};

class ScopedApiCall final {
 public:
  ScopedApiCall(std::mutex* mutex, std::condition_variable* cv,
                std::size_t* active_calls)
      : mutex_(mutex), cv_(cv), active_calls_(active_calls) {}

  ~ScopedApiCall() {
    {
      std::lock_guard<std::mutex> lock(*mutex_);
      --*active_calls_;
    }
    cv_->notify_all();
  }

  ScopedApiCall(const ScopedApiCall&) = delete;
  ScopedApiCall& operator=(const ScopedApiCall&) = delete;

 private:
  std::mutex* const mutex_;
  std::condition_variable* const cv_;
  std::size_t* const active_calls_;
};

Status DefaultSinkFactory(void* /*factory_context*/, const PcmSpec& source_spec,
                          std::unique_ptr<AudioSink>* sink) {
  SdlAudioSinkOptions options;
  options.source_spec = source_spec;
  options.start_paused = true;
  return CreateSdlAudioSink(options, sink);
}

bool IsFmodChannelCount(int channels) {
  // Mirrors AudioDevice.fetchChannelConfigFromCount() from the APK. Android
  // rejects every other value before constructing AudioTrack.
  return channels == 1 || channels == 2 || channels == 6 || channels == 8;
}

bool SameSpec(const PcmSpec& left, const PcmSpec& right) {
  return left.sample_rate_hz == right.sample_rate_hz &&
         left.channels == right.channels && left.format == right.format;
}

void ReleaseOwnedBuffer(void* context, const void* /*data*/,
                        std::size_t /*size_bytes*/) {
  auto* const slot = static_cast<BufferSlot*>(context);
  if (slot == nullptr || slot->device == nullptr) {
    return;
  }

  DeviceState* const device = slot->device;
  bool discarded = false;
  std::size_t released_bytes = 0;
  {
    std::lock_guard<std::mutex> lock(device->mutex);
    // AudioSink promises exactly one callback after a successful Enqueue().
    // Still ignore a callback in any other state so counters and pool
    // ownership cannot underflow during shutdown diagnostics.
    if (slot->state != BufferSlotState::kPending) {
      return;
    }
    discarded = device->closing;
    released_bytes = slot->size_bytes;
    slot->size_bytes = 0;
    slot->state = BufferSlotState::kFree;
    if (device->pending_buffers > 0) {
      --device->pending_buffers;
    }
    if (device->pending_bytes >= released_bytes) {
      device->pending_bytes -= released_bytes;
    } else {
      device->pending_bytes = 0;
    }
  }
  device->capacity_cv.notify_all();

  device->counters->pending_buffers.fetch_sub(1,
                                               std::memory_order_relaxed);
  device->counters->pending_bytes.fetch_sub(released_bytes,
                                             std::memory_order_relaxed);
  if (discarded) {
    device->counters->discarded_buffers.fetch_add(1,
                                                  std::memory_order_relaxed);
    device->counters->discarded_bytes.fetch_add(released_bytes,
                                                std::memory_order_relaxed);
  } else {
    device->counters->consumed_buffers.fetch_add(1,
                                                 std::memory_order_relaxed);
    device->counters->consumed_bytes.fetch_add(released_bytes,
                                               std::memory_order_relaxed);
  }
}

Status ValidateRuntimeOptions(const FmodJavaAudioRuntimeOptions& options) {
  if (options.max_devices == 0 || options.max_block_count == 0 ||
      options.max_buffer_bytes_per_device == 0) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "FMOD Java audio runtime limits must be non-zero");
  }
  return Status::Ok();
}

Status CloseDevice(const std::shared_ptr<DeviceState>& device) {
  if (device == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "FMOD Java audio device state is null");
  }

  {
    std::lock_guard<std::mutex> lock(device->mutex);
    device->closing = true;
  }
  // A Write() may be sleeping at the bounded-capacity gate while retaining
  // operation_mutex. Wake it before waiting for that mutex so shutdown cannot
  // deadlock behind a producer whose queue will never gain capacity.
  device->capacity_cv.notify_all();

  std::lock_guard<std::mutex> operation_lock(device->operation_mutex);
  if (device->sink != nullptr) {
    device->sink->Shutdown();
  }

  std::size_t pending_buffers = 0;
  std::size_t pending_bytes = 0;
  {
    std::lock_guard<std::mutex> lock(device->mutex);
    pending_buffers = device->pending_buffers;
    pending_bytes = device->pending_bytes;
  }
  device->sink.reset();
  if (pending_buffers != 0 || pending_bytes != 0) {
    return Status::Error(
        StatusCode::kPlatformError,
        "audio sink did not release every owned FMOD Java buffer");
  }
  return Status::Ok();
}

}  // namespace

struct FmodJavaAudioRuntime::State {
  explicit State(FmodJavaAudioRuntimeOptions configured_options)
      : options(std::move(configured_options)),
        counters(new (std::nothrow) RuntimeCounters()) {}

  std::mutex mutex;
  std::condition_variable shutdown_cv;
  FmodJavaAudioRuntimeOptions options;
  std::shared_ptr<RuntimeCounters> counters;
  std::unordered_map<FmodJavaAudioDeviceIdentity,
                     std::shared_ptr<DeviceState>>
      devices;
  std::unordered_set<FmodJavaAudioDeviceIdentity> initializing_devices;
  bool shutdown_started = false;
  bool shutdown_complete = false;
  std::size_t active_api_calls = 0;
};

FmodJavaAudioRuntime::FmodJavaAudioRuntime(
    const FmodJavaAudioRuntimeOptions& options)
    : state_(new (std::nothrow) State(options)) {
  if (state_ != nullptr && state_->options.sink_factory == nullptr) {
    state_->options.sink_factory = &DefaultSinkFactory;
  }
}

FmodJavaAudioRuntime::~FmodJavaAudioRuntime() { Shutdown(); }

Status FmodJavaAudioRuntime::Init(FmodJavaAudioDeviceIdentity device_identity,
                                  int channels, int sample_rate_hz,
                                  int block_size_frames, int block_count) {
  State* const state = state_.get();
  if (state == nullptr || state->counters == nullptr) {
    return Status::Error(StatusCode::kUnavailable,
                         "FMOD Java audio runtime allocation failed");
  }
  state->counters->init_attempts.fetch_add(1, std::memory_order_relaxed);
  const auto reject = [&state](Status status) {
    state->counters->rejected_init_calls.fetch_add(1,
                                                   std::memory_order_relaxed);
    return status;
  };

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->shutdown_started) {
      return reject(Status::Error(StatusCode::kFailedPrecondition,
                                  "FMOD Java audio runtime is shut down"));
    }
    ++state->active_api_calls;
  }
  ScopedApiCall api_call(&state->mutex, &state->shutdown_cv,
                         &state->active_api_calls);

  Status status = ValidateRuntimeOptions(state->options);
  if (!status.ok()) {
    return reject(status);
  }
  if (device_identity == nullptr) {
    return reject(Status::Error(StatusCode::kInvalidArgument,
                                "FMOD Java AudioDevice identity is null"));
  }
  if (!IsFmodChannelCount(channels)) {
    return reject(Status::Error(
        StatusCode::kInvalidArgument,
        "FMOD Java channel count must be 1, 2, 6, or 8"));
  }
  if (block_size_frames <= 0 || block_count <= 0) {
    return reject(Status::Error(
        StatusCode::kInvalidArgument,
        "FMOD Java block size and block count must be positive"));
  }
  if (static_cast<std::uint64_t>(block_count) >
      state->options.max_block_count) {
    return reject(Status::Error(StatusCode::kInvalidArgument,
                                "FMOD Java block count exceeds runtime limit"));
  }

  const PcmSpec source_spec{
      sample_rate_hz, static_cast<std::uint8_t>(channels),
      PcmSampleFormat::kSigned16LittleEndian};
  status = ValidatePcmSpec(source_spec);
  if (!status.ok()) {
    return reject(status);
  }
  const std::size_t bytes_per_frame = BytesPerFrame(source_spec);
  const std::size_t block_frames =
      static_cast<std::size_t>(block_size_frames);
  const std::size_t blocks = static_cast<std::size_t>(block_count);
  if (block_frames >
      std::numeric_limits<std::size_t>::max() / bytes_per_frame) {
    return reject(Status::Error(StatusCode::kInvalidArgument,
                                "FMOD Java block byte size overflows"));
  }
  const std::size_t block_bytes = block_frames * bytes_per_frame;
  if (block_bytes > std::numeric_limits<std::size_t>::max() / blocks) {
    return reject(Status::Error(StatusCode::kInvalidArgument,
                                "FMOD Java queue byte size overflows"));
  }
  const std::size_t queue_bytes = block_bytes * blocks;
  if (queue_bytes == 0 ||
      queue_bytes > state->options.max_buffer_bytes_per_device ||
      queue_bytes >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return reject(Status::Error(
        StatusCode::kInvalidArgument,
        "FMOD Java queue byte size exceeds runtime limit"));
  }

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->shutdown_started) {
      return reject(Status::Error(StatusCode::kFailedPrecondition,
                                  "FMOD Java audio runtime is shut down"));
    }
    if (state->devices.find(device_identity) != state->devices.end() ||
        state->initializing_devices.find(device_identity) !=
            state->initializing_devices.end()) {
      return reject(Status::Error(
          StatusCode::kFailedPrecondition,
          "FMOD Java AudioDevice is already initialized"));
    }
    if (state->devices.size() + state->initializing_devices.size() >=
        state->options.max_devices) {
      return reject(Status::Error(StatusCode::kUnavailable,
                                  "FMOD Java audio device limit reached"));
    }
    state->initializing_devices.insert(device_identity);
  }

  const auto cancel_initialization = [state, device_identity] {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->initializing_devices.erase(device_identity);
  };

  std::unique_ptr<AudioSink> sink;
  status = state->options.sink_factory(state->options.sink_factory_context,
                                       source_spec, &sink);
  if (!status.ok() || sink == nullptr) {
    if (sink != nullptr) {
      sink->Shutdown();
    }
    cancel_initialization();
    return reject(status.ok()
                      ? Status::Error(StatusCode::kPlatformError,
                                      "FMOD Java sink factory returned null")
                      : status);
  }
  if (!SameSpec(sink->source_spec(), source_spec)) {
    sink->Shutdown();
    cancel_initialization();
    return reject(Status::Error(
        StatusCode::kPlatformError,
        "FMOD Java sink factory returned a mismatched PCM format"));
  }
  status = sink->Resume();
  if (!status.ok()) {
    sink->Shutdown();
    cancel_initialization();
    return reject(status);
  }

  auto device = std::shared_ptr<DeviceState>(new (std::nothrow) DeviceState(
      std::move(sink), bytes_per_frame, block_bytes, blocks,
      state->counters));
  if (device == nullptr || !device->InitializeSlots()) {
    if (device != nullptr && device->sink != nullptr) {
      device->sink->Shutdown();
    }
    cancel_initialization();
    return reject(Status::Error(StatusCode::kUnavailable,
                                "unable to allocate FMOD Java buffer pool"));
  }

  Status publish_status = Status::Ok();
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->initializing_devices.erase(device_identity);
    if (state->shutdown_started) {
      publish_status =
          Status::Error(StatusCode::kFailedPrecondition,
                        "FMOD Java audio runtime is shutting down");
    } else if (state->devices.find(device_identity) != state->devices.end()) {
      publish_status = Status::Error(
          StatusCode::kFailedPrecondition,
          "FMOD Java AudioDevice was initialized concurrently");
    } else if (state->devices.size() >= state->options.max_devices) {
      publish_status = Status::Error(StatusCode::kUnavailable,
                                     "FMOD Java audio device limit reached");
    } else {
      state->devices.emplace(device_identity, device);
    }
  }
  if (!publish_status.ok()) {
    CloseDevice(device);
    return reject(publish_status);
  }

  state->counters->initialized_devices.fetch_add(1,
                                                  std::memory_order_relaxed);
  state->counters->active_devices.fetch_add(1, std::memory_order_relaxed);
  return Status::Ok();
}

Status FmodJavaAudioRuntime::Write(
    FmodJavaAudioDeviceIdentity device_identity, const std::uint8_t* bytes,
    std::size_t size_bytes) {
  State* const state = state_.get();
  if (state == nullptr || state->counters == nullptr) {
    return Status::Error(StatusCode::kUnavailable,
                         "FMOD Java audio runtime allocation failed");
  }
  state->counters->write_attempts.fetch_add(1, std::memory_order_relaxed);
  const auto reject = [&state](Status status) {
    state->counters->rejected_write_calls.fetch_add(
        1, std::memory_order_relaxed);
    return status;
  };
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->shutdown_started) {
      return reject(Status::Error(StatusCode::kFailedPrecondition,
                                  "FMOD Java audio runtime is shut down"));
    }
    ++state->active_api_calls;
  }
  ScopedApiCall api_call(&state->mutex, &state->shutdown_cv,
                         &state->active_api_calls);
  if (device_identity == nullptr || bytes == nullptr || size_bytes == 0) {
    return reject(Status::Error(
        StatusCode::kInvalidArgument,
        "FMOD Java write requires a device and non-empty bytes"));
  }

  std::shared_ptr<DeviceState> device;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->shutdown_started) {
      return reject(Status::Error(StatusCode::kFailedPrecondition,
                                  "FMOD Java audio runtime is shut down"));
    }
    const auto found = state->devices.find(device_identity);
    if (found == state->devices.end()) {
      return reject(Status::Error(
          StatusCode::kFailedPrecondition,
          "FMOD Java AudioDevice is not initialized"));
    }
    device = found->second;
  }

  std::lock_guard<std::mutex> operation_lock(device->operation_mutex);
  BufferSlot* slot = nullptr;
  {
    std::unique_lock<std::mutex> lock(device->mutex);
    if (device->closing || device->sink == nullptr) {
      return reject(Status::Error(StatusCode::kFailedPrecondition,
                                  "FMOD Java AudioDevice is closing"));
    }
    if (size_bytes % device->bytes_per_frame != 0) {
      return reject(Status::Error(
          StatusCode::kInvalidArgument,
          "FMOD Java write must contain complete PCM frames"));
    }
    if (size_bytes > device->block_bytes) {
      return reject(Status::Error(
          StatusCode::kInvalidArgument,
          "FMOD Java write exceeds the configured mix block size"));
    }

    // Android AudioTrack.write(byte[], 0, length) uses WRITE_BLOCKING for the
    // APK's MODE_STREAM device. Preserve that backpressure instead of dropping
    // a valid FMOD mix block when SDL still owns earlier borrowed buffers.
    device->capacity_cv.wait(lock, [&device] {
      return device->closing || device->sink == nullptr ||
             device->pending_buffers < device->slot_count;
    });
    if (device->closing || device->sink == nullptr) {
      return reject(Status::Error(
          StatusCode::kFailedPrecondition,
          "FMOD Java AudioDevice closed while write awaited capacity"));
    }
    slot = device->FindFreeSlot();
    if (slot == nullptr) {
      return reject(Status::Error(
          StatusCode::kPlatformError,
          "FMOD Java buffer pool accounting is inconsistent"));
    }
    slot->state = BufferSlotState::kReserved;
    slot->size_bytes = size_bytes;
  }

  std::memcpy(slot->bytes, bytes, size_bytes);

  {
    std::lock_guard<std::mutex> lock(device->mutex);
    if (device->closing || device->sink == nullptr) {
      slot->size_bytes = 0;
      slot->state = BufferSlotState::kFree;
      device->capacity_cv.notify_all();
      return reject(Status::Error(
          StatusCode::kFailedPrecondition,
          "FMOD Java AudioDevice closed while write copied PCM"));
    }
    slot->state = BufferSlotState::kPending;
    ++device->pending_buffers;
    device->pending_bytes += size_bytes;
  }
  state->counters->pending_buffers.fetch_add(1, std::memory_order_relaxed);
  state->counters->pending_bytes.fetch_add(size_bytes,
                                           std::memory_order_relaxed);

  const PcmBuffer buffer{slot->bytes, size_bytes, &ReleaseOwnedBuffer, slot};
  const Status status = device->sink->Enqueue(buffer);
  if (!status.ok()) {
    bool rolled_back = false;
    {
      std::lock_guard<std::mutex> lock(device->mutex);
      if (slot->state == BufferSlotState::kPending) {
        slot->size_bytes = 0;
        slot->state = BufferSlotState::kFree;
        --device->pending_buffers;
        device->pending_bytes -= size_bytes;
        rolled_back = true;
      }
    }
    if (rolled_back) {
      state->counters->pending_buffers.fetch_sub(1,
                                                 std::memory_order_relaxed);
      state->counters->pending_bytes.fetch_sub(size_bytes,
                                               std::memory_order_relaxed);
    }
    device->capacity_cv.notify_all();
    return reject(status);
  }

  state->counters->submitted_buffers.fetch_add(1,
                                                std::memory_order_relaxed);
  state->counters->submitted_bytes.fetch_add(size_bytes,
                                              std::memory_order_relaxed);
  return Status::Ok();
}

Status FmodJavaAudioRuntime::Close(
    FmodJavaAudioDeviceIdentity device_identity) {
  State* const state = state_.get();
  if (state == nullptr || state->counters == nullptr) {
    return Status::Error(StatusCode::kUnavailable,
                         "FMOD Java audio runtime allocation failed");
  }
  if (device_identity == nullptr) {
    state->counters->rejected_close_calls.fetch_add(
        1, std::memory_order_relaxed);
    return Status::Error(StatusCode::kInvalidArgument,
                         "FMOD Java AudioDevice identity is null");
  }

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->shutdown_started) {
      state->counters->rejected_close_calls.fetch_add(
          1, std::memory_order_relaxed);
      return Status::Error(StatusCode::kFailedPrecondition,
                           "FMOD Java audio runtime is shut down");
    }
    ++state->active_api_calls;
  }
  ScopedApiCall api_call(&state->mutex, &state->shutdown_cv,
                         &state->active_api_calls);

  std::shared_ptr<DeviceState> device;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto found = state->devices.find(device_identity);
    if (found == state->devices.end()) {
      state->counters->rejected_close_calls.fetch_add(
          1, std::memory_order_relaxed);
      return Status::Error(StatusCode::kFailedPrecondition,
                           "FMOD Java AudioDevice is not initialized");
    }
    device = found->second;
    state->devices.erase(found);
  }
  state->counters->active_devices.fetch_sub(1, std::memory_order_relaxed);
  const Status status = CloseDevice(device);
  state->counters->close_calls.fetch_add(1, std::memory_order_relaxed);
  return status;
}

FmodJavaAudioRuntimeStats FmodJavaAudioRuntime::GetStats() const {
  const State* const state = state_.get();
  if (state == nullptr || state->counters == nullptr) {
    return {};
  }
  const std::shared_ptr<RuntimeCounters> counters = state->counters;
  return FmodJavaAudioRuntimeStats{
      counters->init_attempts.load(std::memory_order_relaxed),
      counters->initialized_devices.load(std::memory_order_relaxed),
      counters->rejected_init_calls.load(std::memory_order_relaxed),
      counters->close_calls.load(std::memory_order_relaxed),
      counters->rejected_close_calls.load(std::memory_order_relaxed),
      counters->write_attempts.load(std::memory_order_relaxed),
      counters->submitted_buffers.load(std::memory_order_relaxed),
      counters->submitted_bytes.load(std::memory_order_relaxed),
      counters->consumed_buffers.load(std::memory_order_relaxed),
      counters->consumed_bytes.load(std::memory_order_relaxed),
      counters->discarded_buffers.load(std::memory_order_relaxed),
      counters->discarded_bytes.load(std::memory_order_relaxed),
      counters->rejected_write_calls.load(std::memory_order_relaxed),
      static_cast<std::size_t>(
          counters->active_devices.load(std::memory_order_relaxed)),
      static_cast<std::size_t>(
          counters->pending_buffers.load(std::memory_order_relaxed)),
      static_cast<std::size_t>(
          counters->pending_bytes.load(std::memory_order_relaxed))};
}

void FmodJavaAudioRuntime::Shutdown() {
  State* const state = state_.get();
  if (state == nullptr || state->counters == nullptr) {
    return;
  }

  std::unordered_map<FmodJavaAudioDeviceIdentity,
                     std::shared_ptr<DeviceState>>
      devices;
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->shutdown_started) {
      state->shutdown_cv.wait(lock,
                              [&state] { return state->shutdown_complete; });
      return;
    }
    state->shutdown_started = true;
    devices.swap(state->devices);
  }
  state->counters->active_devices.fetch_sub(devices.size(),
                                             std::memory_order_relaxed);

  for (const auto& entry : devices) {
    CloseDevice(entry.second);
    state->counters->close_calls.fetch_add(1, std::memory_order_relaxed);
  }

  {
    std::unique_lock<std::mutex> lock(state->mutex);
    state->shutdown_cv.wait(lock,
                            [&state] { return state->active_api_calls == 0; });
  }

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->shutdown_complete = true;
  }
  state->shutdown_cv.notify_all();
}

}  // namespace mocktail::audio
