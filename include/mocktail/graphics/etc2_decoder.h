#ifndef MOCKTAIL_GRAPHICS_ETC2_DECODER_H_
#define MOCKTAIL_GRAPHICS_ETC2_DECODER_H_

#include <cstddef>
#include <cstdint>

namespace mocktail::graphics {

enum class EtcFormat {
  kEtc2Rgb8,
  kEtc2Rgb8A1,
  kEtc2Rgba8,
  kEacR11,
  kEacR11Signed,
  kEacRg11,
  kEacRg11Signed,
};

std::size_t EtcBlockBytes(EtcFormat format);

std::size_t EtcDecodedTexelBytes(EtcFormat format);

bool DecodeEtcImage(EtcFormat format, const std::uint8_t* source,
                    std::size_t source_bytes, std::uint32_t width,
                    std::uint32_t height, std::uint8_t* destination,
                    std::size_t destination_bytes);

bool DecodeEtcImageBlockRows(EtcFormat format, const std::uint8_t* source,
                             std::size_t source_bytes, std::uint32_t width,
                             std::uint32_t height,
                             std::uint32_t first_block_row,
                             std::uint32_t block_row_count,
                             std::uint8_t* destination,
                             std::size_t destination_bytes);

struct EtcDecodeJob {
  EtcFormat format = EtcFormat::kEtc2Rgb8;
  const std::uint8_t* source = nullptr;
  std::size_t source_bytes = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint8_t* destination = nullptr;
  std::size_t destination_bytes = 0;
  bool ok = false;
};

// Synchronous; job destination buffers must not overlap.
void DecodeEtcJobs(EtcDecodeJob* jobs, std::size_t count,
                   unsigned worker_count);

}  // namespace mocktail::graphics

#endif  // MOCKTAIL_GRAPHICS_ETC2_DECODER_H_
