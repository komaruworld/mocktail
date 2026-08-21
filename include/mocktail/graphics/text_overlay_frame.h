#ifndef MOCKTAIL_GRAPHICS_TEXT_OVERLAY_FRAME_H_
#define MOCKTAIL_GRAPHICS_TEXT_OVERLAY_FRAME_H_

#include <cstddef>
#include <cstdint>

// Process-local ABI with libvulkan.so. Pixels are RGBA8; consumers must copy a
// matching revision before the producer clears its storage.
struct MocktailTextOverlayFrameInfo {
  static constexpr std::uint32_t kAbiVersion = 2;

  std::uint32_t abi_version = kAbiVersion;
  std::uint32_t visible = 0;
  std::uint64_t revision = 0;
  // Coordinates are logical, not swapchain pixels.
  std::uint32_t coordinate_width = 0;
  std::uint32_t coordinate_height = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t row_bytes = 0;
  std::uint64_t rgba_bytes = 0;
};

extern "C" {

// false guarantees no overlay; true still requires a synchronized query.
bool mocktail_text_overlay_may_present();
bool mocktail_text_overlay_query(MocktailTextOverlayFrameInfo* frame);
bool mocktail_text_overlay_copy(std::uint64_t revision, void* rgba,
                                std::size_t rgba_capacity);

}  // extern "C"

#endif  // MOCKTAIL_GRAPHICS_TEXT_OVERLAY_FRAME_H_
