#ifndef MOCKTAIL_LIBC_SHIM_VULKAN_ETC1_SKY_TRANSCODER_H_
#define MOCKTAIL_LIBC_SHIM_VULKAN_ETC1_SKY_TRANSCODER_H_

#include <cstdint>
#include <vector>

namespace libc_shim {

// Converts one opaque ETC1 RGB block to an opaque BC1 RGB block.
void ConvertEtc1RgbBlockToBc1(const uint8_t *etc1, uint8_t *bc1);

bool TranscodeEtc1SkyTextureForVulkan(const char *path,
                                      std::vector<unsigned char> *data);

} // namespace libc_shim

#endif // MOCKTAIL_LIBC_SHIM_VULKAN_ETC1_SKY_TRANSCODER_H_
