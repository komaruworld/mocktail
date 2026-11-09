#include "mocktail/audio/sdl_audio_capture.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace mocktail::audio {
namespace {

class SdlAudioCapture;

struct SdlRecordingSubsystemState {
  std::mutex mutex;
  std::uint32_t configured_device_id = 0;
  std::string configured_device_name = "default";
  std::vector<SdlAudioCapture *> live_captures;
};

SdlRecordingSubsystemState& RecordingSubsystemState() {
  static SdlRecordingSubsystemState state;
  return state;
}

Status SdlError(const char* operation) {
  std::string message(operation);
  message.append(": ");
  const char* detail = SDL_GetError();
  message.append(detail != nullptr && detail[0] != '\0' ? detail
                                                        : "unknown SDL error");
  return Status::Error(StatusCode::kPlatformError, std::move(message));
}

Status InvalidArgument(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status FailedPrecondition(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

bool AudioSubsystemInitialized() {
  return (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) != 0;
}

SDL_AudioFormat ToSdlFormat(PcmSampleFormat format) {
  switch (format) {
    case PcmSampleFormat::kUnsigned8:
      return SDL_AUDIO_U8;
    case PcmSampleFormat::kSigned16LittleEndian:
      return SDL_AUDIO_S16LE;
    case PcmSampleFormat::kSigned32LittleEndian:
      return SDL_AUDIO_S32LE;
    case PcmSampleFormat::kFloat32LittleEndian:
      return SDL_AUDIO_F32LE;
  }
  return SDL_AUDIO_UNKNOWN;
}

class SdlAudioCapture final : public AudioCapture {
 public:
  SdlAudioCapture(PcmSpec spec, std::size_t frames_per_buffer,
                  std::vector<std::uint8_t> buffer, SDL_AudioStream* stream,
                  AudioCaptureDataCallback callback, void* callback_context)
      : output_spec_(spec),
        frames_per_buffer_(frames_per_buffer),
        buffer_(std::move(buffer)),
        stream_(stream),
        callback_(callback),
        callback_context_(callback_context) {}

  ~SdlAudioCapture() override { Shutdown(); }

  const PcmSpec& output_spec() const override { return output_spec_; }
  void* buffer_data() override { return buffer_.data(); }
  std::size_t buffer_size_bytes() const override { return buffer_.size(); }
  std::size_t frames_per_buffer() const override {
    return frames_per_buffer_;
  }

  void AdoptStream(SDL_AudioStream* stream) { stream_ = stream; }

  void MarkRegistered() { registered_ = true; }

  Status Start() override {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (stream_ == nullptr || stopping_.load(std::memory_order_acquire)) {
      return FailedPrecondition("SDL audio capture is shut down");
    }
    if (running_.load(std::memory_order_acquire)) {
      return Status::Ok();
    }
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
      return SdlError("SDL_ResumeAudioStreamDevice");
    }
    running_.store(true, std::memory_order_release);
    return Status::Ok();
  }

  Status Stop() override {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (stream_ == nullptr || stopping_.load(std::memory_order_acquire)) {
      return Status::Ok();
    }
    running_.store(false, std::memory_order_release);
    if (!SDL_PauseAudioStreamDevice(stream_)) {
      return SdlError("SDL_PauseAudioStreamDevice");
    }
    if (!SDL_ClearAudioStream(stream_)) {
      return SdlError("SDL_ClearAudioStream");
    }
    return Status::Ok();
  }

  void Shutdown() override {
    auto &state = RecordingSubsystemState();
    std::lock_guard<std::mutex> registry_lock(state.mutex);
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (stream_ == nullptr) {
      return;
    }
    stopping_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    (void)SDL_PauseAudioStreamDevice(stream_);
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    if (registered_) {
      registered_ = false;
      auto &captures = state.live_captures;
      captures.erase(std::remove(captures.begin(), captures.end(), this),
                     captures.end());
    }
  }

  std::mutex &operation_mutex() { return operation_mutex_; }

  // The registry and operation mutexes are held throughout a migration.
  Status PrepareSwitch(std::uint32_t id) {
    SDL_AudioSpec spec{};
    spec.format = ToSdlFormat(output_spec_.format);
    spec.channels = output_spec_.channels;
    spec.freq = output_spec_.sample_rate_hz;
    replacement_ = SDL_OpenAudioDeviceStream(
        id == 0 ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : id, &spec,
        &OnAudioAvailable, this);
    return replacement_ ? Status::Ok()
                        : SdlError("open replacement microphone");
  }

  Status PauseForSwitch() {
    if (!SDL_PauseAudioStreamDevice(stream_))
      return SdlError("pause old microphone");
    // Wait for an in-flight callback before the replacement can use buffer_.
    if (!SDL_LockAudioStream(stream_))
      return SdlError("lock old microphone");
    SDL_UnlockAudioStream(stream_);
    return Status::Ok();
  }

  Status StartReplacement() {
    if (running_.load(std::memory_order_acquire) &&
        !SDL_ResumeAudioStreamDevice(replacement_))
      return SdlError("resume replacement microphone");
    return Status::Ok();
  }

  void CancelReplacement() {
    if (replacement_) {
      SDL_DestroyAudioStream(replacement_);
      replacement_ = nullptr;
    }
  }

  Status RestoreAfterSwitch() {
    if (running_.load(std::memory_order_acquire) &&
        !SDL_ResumeAudioStreamDevice(stream_))
      return SdlError("restore original microphone");
    return Status::Ok();
  }

  void CommitSwitch() {
    SDL_DestroyAudioStream(stream_);
    stream_ = std::exchange(replacement_, nullptr);
  }

  static void SDLCALL OnAudioAvailable(void* userdata,
                                       SDL_AudioStream* stream,
                                       int /*additional_amount*/,
                                       int /*total_amount*/) {
    auto* capture = static_cast<SdlAudioCapture*>(userdata);
    if (capture == nullptr || stream == nullptr ||
        capture->stopping_.load(std::memory_order_acquire) ||
        !capture->running_.load(std::memory_order_acquire)) {
      return;
    }
    const int frame_bytes = static_cast<int>(capture->buffer_.size());
    while (!capture->stopping_.load(std::memory_order_acquire) &&
           capture->running_.load(std::memory_order_acquire)) {
      const int available = SDL_GetAudioStreamAvailable(stream);
      if (available < frame_bytes) {
        return;
      }
      const int read = SDL_GetAudioStreamData(stream, capture->buffer_.data(),
                                              frame_bytes);
      if (read != frame_bytes) {
        return;
      }
      capture->callback_(capture->callback_context_,
                         static_cast<std::size_t>(read));
    }
  }

 private:
  PcmSpec output_spec_;
  std::size_t frames_per_buffer_ = 0;
  std::vector<std::uint8_t> buffer_;
  SDL_AudioStream* stream_ = nullptr;
  SDL_AudioStream *replacement_ = nullptr;
  AudioCaptureDataCallback callback_ = nullptr;
  void* callback_context_ = nullptr;
  std::mutex operation_mutex_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  bool registered_ = false;
};

}  // namespace

Status ListSdlRecordingDevices(std::vector<SdlRecordingDevice>* devices) {
  if (devices == nullptr) {
    return InvalidArgument("SDL recording device output pointer is null");
  }
  devices->clear();
  if (!AudioSubsystemInitialized()) {
    return FailedPrecondition(
        "SDL audio subsystem must be initialized before device enumeration");
  }

  int count = 0;
  SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
  if (ids == nullptr) {
    return SdlError("SDL_GetAudioRecordingDevices");
  }
  devices->reserve(count > 0 ? static_cast<std::size_t>(count) : 0U);
  for (int index = 0; index < count; ++index) {
    const char* name = SDL_GetAudioDeviceName(ids[index]);
    if (name == nullptr || name[0] == '\0') {
      SDL_free(ids);
      devices->clear();
      return SdlError("SDL_GetAudioDeviceName(recording)");
    }
    devices->push_back(
        {static_cast<std::uint32_t>(ids[index]), std::string(name)});
  }
  SDL_free(ids);
  return Status::Ok();
}

Status ResolveSdlRecordingDevice(
    std::string_view requested,
    const std::vector<SdlRecordingDevice>& available_devices,
    std::uint32_t* recording_device_id, std::string* resolved_name) {
  if (recording_device_id == nullptr || resolved_name == nullptr) {
    return InvalidArgument("SDL recording selection output pointer is null");
  }
  *recording_device_id = 0;
  resolved_name->clear();
  if (requested.empty() || requested.size() > 512U) {
    return InvalidArgument(
        "audio input device must be `default`, `id:<number>`, or a bounded "
        "device name");
  }
  for (const unsigned char character : requested) {
    if (character < 0x20U || character == 0x7fU) {
      return InvalidArgument(
          "audio input device must not contain control bytes");
    }
  }
  if (requested == "default") {
    *resolved_name = "default";
    return Status::Ok();
  }

  if (requested.rfind("id:", 0) == 0) {
    std::uint32_t parsed = 0;
    const std::string_view value = requested.substr(3);
    const auto parse =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || parse.ec != std::errc{} ||
        parse.ptr != value.data() + value.size() || parsed == 0) {
      return InvalidArgument("audio input device ID is invalid");
    }
    const auto found =
        std::find_if(available_devices.begin(), available_devices.end(),
                     [parsed](const SdlRecordingDevice& device) {
                       return device.id == parsed;
                     });
    if (found == available_devices.end()) {
      return Status::Error(StatusCode::kUnavailable,
                           "configured audio input device ID is unavailable");
    }
    *recording_device_id = found->id;
    *resolved_name = found->name;
    return Status::Ok();
  }

  const SdlRecordingDevice* match = nullptr;
  for (const SdlRecordingDevice& device : available_devices) {
    if (device.name != requested) {
      continue;
    }
    if (match != nullptr) {
      return InvalidArgument(
          "audio input device name is ambiguous; use its printed `id:` "
          "selector");
    }
    match = &device;
  }
  if (match == nullptr) {
    return Status::Error(StatusCode::kUnavailable,
                         "configured audio input device is unavailable: " +
                             std::string(requested));
  }
  *recording_device_id = match->id;
  *resolved_name = match->name;
  return Status::Ok();
}

Status ConfigureSdlRecordingDevice(
    std::string_view requested,
    std::vector<SdlRecordingDevice>* available_devices,
    std::string* resolved_name) {
  if (available_devices == nullptr || resolved_name == nullptr) {
    return InvalidArgument("SDL recording configuration output is null");
  }
  Status status = ListSdlRecordingDevices(available_devices);
  if (!status.ok() && requested != "default") {
    return status;
  }
  if (!status.ok()) {
    available_devices->clear();
  }
  std::uint32_t recording_device_id = 0;
  status = ResolveSdlRecordingDevice(requested, *available_devices,
                                     &recording_device_id, resolved_name);
  if (!status.ok()) {
    return status;
  }

  SdlRecordingSubsystemState& state = RecordingSubsystemState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.live_captures.empty()) {
    return FailedPrecondition(
        "SDL recording device must be configured before opening a capture");
  }
  state.configured_device_id = recording_device_id;
  state.configured_device_name = *resolved_name;
  return Status::Ok();
}

Status GetConfiguredSdlRecordingDevice(std::uint32_t* recording_device_id,
                                       std::string* resolved_name) {
  if (recording_device_id == nullptr || resolved_name == nullptr) {
    return InvalidArgument(
        "configured SDL recording device output pointer is null");
  }
  if (!AudioSubsystemInitialized()) {
    return FailedPrecondition(
        "SDL audio subsystem must be initialized before reading input route");
  }
  SdlRecordingSubsystemState& state = RecordingSubsystemState();
  std::lock_guard<std::mutex> lock(state.mutex);
  *recording_device_id = state.configured_device_id;
  *resolved_name = state.configured_device_name;
  return Status::Ok();
}

Status CreateSdlAudioCapture(const SdlAudioCaptureOptions& options,
                             std::unique_ptr<AudioCapture>* capture) {
  if (capture == nullptr) {
    return InvalidArgument("audio capture output pointer is null");
  }
  capture->reset();
  Status status = ValidatePcmSpec(options.output_spec);
  if (!status.ok()) {
    return status;
  }
  if (options.data_callback == nullptr || options.frame_duration_ms == 0 ||
      options.frame_duration_ms > 1000 ||
      (static_cast<std::uint64_t>(options.output_spec.sample_rate_hz) *
       options.frame_duration_ms) %
              1000U !=
          0) {
    return InvalidArgument("SDL capture requires a valid fixed-frame callback");
  }

  const std::uint64_t frames =
      static_cast<std::uint64_t>(options.output_spec.sample_rate_hz) *
      options.frame_duration_ms / 1000U;
  const std::uint64_t buffer_bytes =
      frames * BytesPerFrame(options.output_spec);
  if (frames == 0 || buffer_bytes == 0 ||
      buffer_bytes > static_cast<std::uint64_t>(
                         std::numeric_limits<int>::max())) {
    return InvalidArgument("SDL capture frame size is invalid");
  }

  auto &state = RecordingSubsystemState();
  std::unique_lock<std::mutex> registry_lock(state.mutex);
  if (!AudioSubsystemInitialized())
    return FailedPrecondition(
        "SDL audio subsystem stopped while opening capture");
  const std::uint32_t recording_device_id = options.recording_device_id == 0
                                                ? state.configured_device_id
                                                : options.recording_device_id;
  const SDL_AudioDeviceID device =
      recording_device_id == 0
          ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING
          : static_cast<SDL_AudioDeviceID>(recording_device_id);
  SDL_AudioSpec spec{};
  spec.format = ToSdlFormat(options.output_spec.format);
  spec.channels = options.output_spec.channels;
  spec.freq = options.output_spec.sample_rate_hz;

  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(buffer_bytes));
  auto* implementation = new (std::nothrow) SdlAudioCapture(
      options.output_spec, static_cast<std::size_t>(frames), std::move(buffer),
      nullptr, options.data_callback, options.data_context);
  if (implementation == nullptr) {
    return Status::Error(StatusCode::kUnavailable,
                         "unable to allocate SDL audio capture");
  }
  SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(
      device, &spec, &SdlAudioCapture::OnAudioAvailable, implementation);
  if (stream == nullptr) {
    registry_lock.unlock();
    delete implementation;
    return SdlError("SDL_OpenAudioDeviceStream(recording)");
  }

  implementation->AdoptStream(stream);
  state.live_captures.push_back(implementation);
  implementation->MarkRegistered();
  capture->reset(implementation);
  return Status::Ok();
}

Status SwitchSdlRecordingDevice(std::uint32_t recording_device_id,
                                std::string *resolved_name) {
  if (resolved_name == nullptr)
    return InvalidArgument("microphone switch output is null");
  resolved_name->clear();
  std::vector<SdlRecordingDevice> devices;
  Status status = ListSdlRecordingDevices(&devices);
  if (!status.ok())
    return status;
  std::uint32_t resolved_id = 0;
  std::string name;
  status = ResolveSdlRecordingDevice(
      recording_device_id == 0 ? "default"
                               : "id:" + std::to_string(recording_device_id),
      devices, &resolved_id, &name);
  if (!status.ok())
    return status;

  auto &state = RecordingSubsystemState();
  std::lock_guard<std::mutex> registry_lock(state.mutex);
  if (!AudioSubsystemInitialized())
    return FailedPrecondition(
        "SDL audio subsystem stopped during microphone switch");
  std::vector<std::unique_lock<std::mutex>> locks;
  locks.reserve(state.live_captures.size());
  for (auto *capture : state.live_captures)
    locks.emplace_back(capture->operation_mutex());
  // Keep the old streams alive until every replacement is ready and running.
  for (auto *capture : state.live_captures) {
    status = capture->PrepareSwitch(resolved_id);
    if (!status.ok())
      break;
  }
  if (status.ok()) {
    for (auto *capture : state.live_captures) {
      status = capture->PauseForSwitch();
      if (!status.ok())
        break;
    }
  }
  if (status.ok()) {
    for (auto *capture : state.live_captures) {
      status = capture->StartReplacement();
      if (!status.ok())
        break;
    }
  }
  if (!status.ok()) {
    for (auto *capture : state.live_captures)
      capture->CancelReplacement();
    for (auto *capture : state.live_captures) {
      const Status restored = capture->RestoreAfterSwitch();
      if (!restored.ok())
        return restored;
    }
    return status;
  }
  for (auto *capture : state.live_captures)
    capture->CommitSwitch();
  state.configured_device_id = resolved_id;
  state.configured_device_name = name;
  *resolved_name = name;
  return Status::Ok();
}

Status PrepareSdlAudioCaptureSubsystemShutdown() {
  SdlRecordingSubsystemState& state = RecordingSubsystemState();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.live_captures.empty()) {
    return FailedPrecondition(
        "SDL audio shutdown requires every capture to close");
  }
  state.configured_device_id = 0;
  state.configured_device_name = "default";
  return Status::Ok();
}

}  // namespace mocktail::audio
