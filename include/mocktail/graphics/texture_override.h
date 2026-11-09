#ifndef MOCKTAIL_GRAPHICS_TEXTURE_OVERRIDE_H_
#define MOCKTAIL_GRAPHICS_TEXTURE_OVERRIDE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mocktail::graphics {

struct RgbaImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> pixels;
};

std::uint64_t HashBytes(const std::uint8_t* data, std::size_t size);
std::string HashName(std::uint64_t hash);

bool ReadPngRgba(const std::string& path, RgbaImage* image);
bool WritePngRgba(const std::string& path, const RgbaImage& image);

void ResampleRgba(const RgbaImage& source, std::uint32_t width,
                  std::uint32_t height, std::uint8_t* destination);
void ResampleRgba(const std::uint8_t* source, std::uint32_t source_width,
                  std::uint32_t source_height, std::uint32_t width,
                  std::uint32_t height, std::uint8_t* destination);

class TextureOverrides {
 public:
  TextureOverrides(std::string override_dir, std::string dump_dir);
  static TextureOverrides FromEnvironment();

  bool enabled() const { return !override_dir_.empty() || !dump_dir_.empty(); }
  bool dumping() const { return !dump_dir_.empty(); }

  std::shared_ptr<const RgbaImage> Lookup(std::uint64_t hash);
  void Dump(std::uint64_t hash, std::uint32_t width, std::uint32_t height,
            const std::uint8_t* rgba);

 private:
  std::string override_dir_;
  std::string dump_dir_;
  std::mutex mutex_;
  std::unordered_map<std::uint64_t, std::shared_ptr<const RgbaImage>> cache_;
  std::unordered_set<std::uint64_t> dumped_;
};

}  // namespace mocktail::graphics

#endif  // MOCKTAIL_GRAPHICS_TEXTURE_OVERRIDE_H_
