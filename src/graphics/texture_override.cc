#include "mocktail/graphics/texture_override.h"

#include <png.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace mocktail::graphics {

std::uint64_t HashBytes(const std::uint8_t* data, std::size_t size) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

std::string HashName(std::uint64_t hash) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx",
                static_cast<unsigned long long>(hash));
  return text;
}

bool ReadPngRgba(const std::string& path, RgbaImage* image) {
  if (image == nullptr) {
    return false;
  }
  png_image png{};
  png.version = PNG_IMAGE_VERSION;
  if (png_image_begin_read_from_file(&png, path.c_str()) == 0) {
    return false;
  }
  png.format = PNG_FORMAT_RGBA;
  RgbaImage result;
  result.width = png.width;
  result.height = png.height;
  result.pixels.resize(PNG_IMAGE_SIZE(png));
  if (png_image_finish_read(&png, nullptr, result.pixels.data(), 0,
                            nullptr) == 0) {
    png_image_free(&png);
    return false;
  }
  *image = std::move(result);
  return true;
}

bool WritePngRgba(const std::string& path, const RgbaImage& image) {
  if (image.width == 0 || image.height == 0 ||
      image.pixels.size() !=
          static_cast<std::size_t>(image.width) * image.height * 4) {
    return false;
  }
  png_image png{};
  png.version = PNG_IMAGE_VERSION;
  png.width = image.width;
  png.height = image.height;
  png.format = PNG_FORMAT_RGBA;
  const int written = png_image_write_to_file(&png, path.c_str(), 0,
                                              image.pixels.data(), 0, nullptr);
  png_image_free(&png);
  return written != 0;
}

void ResampleRgba(const RgbaImage& source, std::uint32_t width,
                  std::uint32_t height, std::uint8_t* destination) {
  if (source.pixels.size() <
      static_cast<std::size_t>(source.width) * source.height * 4) {
    return;
  }
  ResampleRgba(source.pixels.data(), source.width, source.height, width,
               height, destination);
}

void ResampleRgba(const std::uint8_t* source, std::uint32_t source_width,
                  std::uint32_t source_height, std::uint32_t width,
                  std::uint32_t height, std::uint8_t* destination) {
  if (source == nullptr || destination == nullptr || width == 0 ||
      height == 0 || source_width == 0 || source_height == 0) {
    return;
  }
  if (width == source_width && height == source_height) {
    if (source != destination) {
      std::memcpy(destination, source,
                  static_cast<std::size_t>(width) * height * 4);
    }
    return;
  }
  if (width % source_width == 0 && height % source_height == 0) {
    const std::uint32_t scale_x = width / source_width;
    const std::uint32_t scale_y = height / source_height;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    std::vector<std::uint8_t> row(row_bytes);
    for (std::uint32_t y = 0; y < source_height; ++y) {
      const std::uint8_t* source_row =
          source + static_cast<std::size_t>(y) * source_width * 4;
      for (std::uint32_t x = 0; x < source_width; ++x) {
        for (std::uint32_t repeat = 0; repeat < scale_x; ++repeat) {
          std::memcpy(row.data() +
                          (static_cast<std::size_t>(x) * scale_x + repeat) * 4,
                      source_row + static_cast<std::size_t>(x) * 4, 4);
        }
      }
      std::uint8_t* target_row =
          destination + static_cast<std::size_t>(y) * scale_y * row_bytes;
      for (std::uint32_t repeat = 0; repeat < scale_y; ++repeat) {
        std::memcpy(target_row + repeat * row_bytes, row.data(), row_bytes);
      }
    }
    return;
  }
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint64_t y0 = static_cast<std::uint64_t>(y) * source_height / height;
    std::uint64_t y1 =
        (static_cast<std::uint64_t>(y) + 1) * source_height / height;
    if (y1 <= y0) {
      y1 = y0 + 1;
    }
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::uint64_t x0 = static_cast<std::uint64_t>(x) * source_width / width;
      std::uint64_t x1 =
          (static_cast<std::uint64_t>(x) + 1) * source_width / width;
      if (x1 <= x0) {
        x1 = x0 + 1;
      }
      std::uint64_t sum[4] = {0, 0, 0, 0};
      for (std::uint64_t sy = y0; sy < y1; ++sy) {
        for (std::uint64_t sx = x0; sx < x1; ++sx) {
          const std::uint8_t* pixel = source + (sy * source_width + sx) * 4;
          for (int channel = 0; channel < 4; ++channel) {
            sum[channel] += pixel[channel];
          }
        }
      }
      const std::uint64_t count = (y1 - y0) * (x1 - x0);
      std::uint8_t* out = destination + (static_cast<std::size_t>(y) * width + x) * 4;
      for (int channel = 0; channel < 4; ++channel) {
        out[channel] =
            static_cast<std::uint8_t>((sum[channel] + count / 2) / count);
      }
    }
  }
}

namespace {

std::string EnvironmentString(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? value : "";
}

}  // namespace

TextureOverrides::TextureOverrides(std::string override_dir,
                                   std::string dump_dir)
    : override_dir_(std::move(override_dir)), dump_dir_(std::move(dump_dir)) {}

TextureOverrides TextureOverrides::FromEnvironment() {
  return TextureOverrides(EnvironmentString("MOCKTAIL_TEXTURE_OVERRIDE_DIR"),
                          EnvironmentString("MOCKTAIL_TEXTURE_DUMP_DIR"));
}

std::shared_ptr<const RgbaImage> TextureOverrides::Lookup(std::uint64_t hash) {
  if (override_dir_.empty()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto cached = cache_.find(hash);
  if (cached != cache_.end()) {
    return cached->second;
  }
  std::shared_ptr<const RgbaImage> loaded;
  RgbaImage image;
  const std::string path = override_dir_ + "/" + HashName(hash) + ".png";
  if (ReadPngRgba(path, &image) && image.width > 0 && image.height > 0) {
    loaded = std::make_shared<const RgbaImage>(std::move(image));
    std::fprintf(stderr, "  [vulkan] texture override loaded: %s (%ux%u)\n",
                 path.c_str(), loaded->width, loaded->height);
  }
  cache_[hash] = loaded;
  return loaded;
}

void TextureOverrides::Dump(std::uint64_t hash, std::uint32_t width,
                            std::uint32_t height, const std::uint8_t* rgba) {
  if (dump_dir_.empty() || rgba == nullptr || width == 0 || height == 0) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dumped_.insert(hash).second) {
      return;
    }
  }
  std::error_code error;
  std::filesystem::create_directories(dump_dir_, error);
  RgbaImage image;
  image.width = width;
  image.height = height;
  image.pixels.assign(rgba, rgba + static_cast<std::size_t>(width) * height * 4);
  const std::string path = dump_dir_ + "/" + HashName(hash) + "_" +
                           std::to_string(width) + "x" +
                           std::to_string(height) + ".png";
  if (!WritePngRgba(path, image)) {
    std::fprintf(stderr, "  [vulkan] texture dump failed: %s\n", path.c_str());
  }
}

}  // namespace mocktail::graphics
