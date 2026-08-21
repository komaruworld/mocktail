#include "mocktail/audio/audio_sink.h"

#include <limits>

namespace mocktail::audio {

std::size_t BytesPerSample(PcmSampleFormat format) {
  switch (format) {
    case PcmSampleFormat::kUnsigned8:
      return 1;
    case PcmSampleFormat::kSigned16LittleEndian:
      return 2;
    case PcmSampleFormat::kSigned32LittleEndian:
    case PcmSampleFormat::kFloat32LittleEndian:
      return 4;
  }
  return 0;
}

std::size_t BytesPerFrame(const PcmSpec& spec) {
  const std::size_t bytes_per_sample = BytesPerSample(spec.format);
  if (bytes_per_sample == 0 || spec.channels == 0 ||
      bytes_per_sample >
          std::numeric_limits<std::size_t>::max() / spec.channels) {
    return 0;
  }
  return bytes_per_sample * spec.channels;
}

Status ValidatePcmSpec(const PcmSpec& spec) {
  if (spec.sample_rate_hz < 8000 || spec.sample_rate_hz > 384000) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "PCM sample rate must be between 8000 and 384000 Hz");
  }
  if (spec.channels == 0 || spec.channels > 8) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "PCM channel count must be between 1 and 8");
  }
  if (BytesPerFrame(spec) == 0) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "PCM sample format is unsupported");
  }
  return Status::Ok();
}

}  // namespace mocktail::audio
