#ifndef MOCKTAIL_AUDIO_SDL_AUDIO_CAPTURE_H_
#define MOCKTAIL_AUDIO_SDL_AUDIO_CAPTURE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mocktail/audio/audio_sink.h"

namespace mocktail::audio {

struct SdlRecordingDevice {
  std::uint32_t id = 0;
  std::string name;
};

// The virtual `default` target is not returned. Device IDs are process-local;
// names are the stable configuration choice, while `id:<number>` disambiguates
// duplicate names for the current host session.
Status ListSdlRecordingDevices(std::vector<SdlRecordingDevice>* devices);

Status ResolveSdlRecordingDevice(
    std::string_view requested,
    const std::vector<SdlRecordingDevice>& available_devices,
    std::uint32_t* recording_device_id, std::string* resolved_name);

// Must run after SDL audio initialization and before the first capture opens.
Status ConfigureSdlRecordingDevice(
    std::string_view requested,
    std::vector<SdlRecordingDevice>* available_devices,
    std::string* resolved_name);

Status GetConfiguredSdlRecordingDevice(std::uint32_t* recording_device_id,
                                       std::string* resolved_name);

// Migrates live captures without changing their format or callback buffers.
Status SwitchSdlRecordingDevice(std::uint32_t recording_device_id,
                                std::string *resolved_name);

using AudioCaptureDataCallback = void (*)(void* context,
                                          std::size_t size_bytes);

class AudioCapture {
 public:
  virtual ~AudioCapture() = default;

  virtual const PcmSpec& output_spec() const = 0;
  virtual void* buffer_data() = 0;
  virtual std::size_t buffer_size_bytes() const = 0;
  virtual std::size_t frames_per_buffer() const = 0;
  virtual Status Start() = 0;
  virtual Status Stop() = 0;
  virtual void Shutdown() = 0;
};

struct SdlAudioCaptureOptions {
  PcmSpec output_spec;

  // Zero selects the configured process target.
  std::uint32_t recording_device_id = 0;
  std::uint32_t frame_duration_ms = 10;
  AudioCaptureDataCallback data_callback = nullptr;
  void* data_context = nullptr;
};

// SDL converts the host input to output_spec and delivers fixed-duration
// frames through one stable buffer. The callback runs synchronously on SDL's
// recording thread after that buffer has been filled.
Status CreateSdlAudioCapture(const SdlAudioCaptureOptions& options,
                             std::unique_ptr<AudioCapture>* capture);

// Called by the SDL subsystem owner before SDL_QuitSubSystem.
Status PrepareSdlAudioCaptureSubsystemShutdown();

}  // namespace mocktail::audio

#endif  // MOCKTAIL_AUDIO_SDL_AUDIO_CAPTURE_H_
