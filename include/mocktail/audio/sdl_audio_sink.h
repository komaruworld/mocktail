#ifndef MOCKTAIL_AUDIO_SDL_AUDIO_SINK_H_
#define MOCKTAIL_AUDIO_SDL_AUDIO_SINK_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mocktail/audio/audio_sink.h"

namespace mocktail::audio {

// SDL requires subsystem startup and shutdown on the main thread.
Status InitializeSdlAudioSubsystem();
Status ShutdownSdlAudioSubsystem();

struct SdlPlaybackDevice {
  std::uint32_t id = 0;
  std::string name;
};

// The virtual `default` target is not returned.
Status ListSdlPlaybackDevices(std::vector<SdlPlaybackDevice>* devices);

Status ResolveSdlPlaybackDevice(
    std::string_view requested,
    const std::vector<SdlPlaybackDevice>& available_devices,
    std::uint32_t* playback_device_id, std::string* resolved_name);

// Must run after SDL audio initialization and before the first sink opens.
Status ConfigureSdlPlaybackDevice(
    std::string_view requested,
    std::vector<SdlPlaybackDevice>* available_devices,
    std::string* resolved_name);

// Device ID zero means the system default.
Status GetConfiguredSdlPlaybackDevice(std::uint32_t* playback_device_id,
                                      std::string* resolved_name);

// Migrates live streams without dropping queued PCM. New sinks inherit it.
Status SwitchSdlPlaybackDevice(std::uint32_t playback_device_id,
                               std::string* resolved_name);

struct SdlAudioSinkOptions {
  PcmSpec source_spec;

  // Zero selects the configured process target.
  std::uint32_t playback_device_id = 0;
  bool start_paused = true;
};

// SDL owns conversion, resampling, and borrowed-buffer consumption.
Status CreateSdlAudioSink(const SdlAudioSinkOptions& options,
                          std::unique_ptr<AudioSink>* sink);

}  // namespace mocktail::audio

#endif  // MOCKTAIL_AUDIO_SDL_AUDIO_SINK_H_
