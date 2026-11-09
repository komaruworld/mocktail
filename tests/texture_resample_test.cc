#include "mocktail/graphics/texture_override.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace mocktail::graphics {
namespace {

TEST(TextureResampleTest, IntegerUpscalingPreservesEveryChannel) {
  for (const auto& [width, height] :
       std::array<std::pair<std::uint32_t, std::uint32_t>, 5>{
           {{1, 1}, {4, 4}, {37, 37}, {60, 33}, {64, 64}}}) {
    RgbaImage source{width, height,
                     std::vector<std::uint8_t>(width * height * 4)};
    for (std::size_t i = 0; i < source.pixels.size(); ++i) {
      source.pixels[i] = static_cast<std::uint8_t>(i * 37 + 19);
    }
    for (std::uint32_t scale_x : {1U, 2U, 3U, 4U, 8U}) {
      for (std::uint32_t scale_y : {1U, 2U, 3U, 4U, 8U}) {
        SCOPED_TRACE(::testing::Message()
                     << width << 'x' << height << " scale=" << scale_x << 'x'
                     << scale_y);
        const std::uint32_t target_width = width * scale_x;
        const std::uint32_t target_height = height * scale_y;
        std::vector<std::uint8_t> expected(target_width * target_height * 4);
        for (std::uint32_t y = 0; y < target_height; ++y) {
          for (std::uint32_t x = 0; x < target_width; ++x) {
            for (unsigned channel = 0; channel < 4; ++channel) {
              expected[(y * target_width + x) * 4 + channel] =
                  source.pixels[((y / scale_y) * width + x / scale_x) * 4 +
                                channel];
            }
          }
        }
        std::vector<std::uint8_t> output(expected.size() + 2, 0xa5);
        ResampleRgba(source, target_width, target_height, output.data() + 1);
        EXPECT_EQ(output.front(), 0xa5);
        EXPECT_EQ(output.back(), 0xa5);
        EXPECT_EQ(std::vector<std::uint8_t>(output.begin() + 1, output.end() - 1),
                  expected);
      }
    }
  }
}

TEST(TextureResampleTest, SameSizeSupportsInPlaceResampling) {
  RgbaImage image{2, 1, {0, 127, 255, 0, 255, 128, 0, 255}};
  const auto expected = image.pixels;
  ResampleRgba(image, image.width, image.height, image.pixels.data());
  EXPECT_EQ(image.pixels, expected);
}

TEST(TextureResampleTest, FractionalUpscalingKeepsOriginalSampling) {
  const RgbaImage source{2, 1, {10, 20, 30, 0, 40, 50, 60, 255}};
  std::vector<std::uint8_t> output(12);
  ResampleRgba(source, 3, 1, output.data());
  EXPECT_EQ(output, (std::vector<std::uint8_t>{10, 20, 30, 0, 10, 20, 30, 0,
                                            40, 50, 60, 255}));
}

TEST(TextureResampleTest, DownscalingPreservesAreaAveragesAndAlphaRounding) {
  const RgbaImage source{3, 2,
                        {0, 10, 20, 0, 10, 20, 30, 100, 20, 30, 40, 200,
                         30, 40, 50, 255, 40, 50, 60, 0, 50, 60, 70, 255}};
  std::vector<std::uint8_t> output(8);
  ResampleRgba(source, 2, 1, output.data());
  EXPECT_EQ(output, (std::vector<std::uint8_t>{15, 25, 35, 128,
                                            30, 40, 50, 139}));
}

TEST(TextureResampleTest, MixedScalingPreservesAreaAverages) {
  const RgbaImage source{2, 2, {0, 10, 20, 0, 10, 20, 30, 100,
                              30, 40, 50, 255, 40, 50, 60, 0}};
  std::vector<std::uint8_t> output(16);
  ResampleRgba(source, 4, 1, output.data());
  EXPECT_EQ(output, (std::vector<std::uint8_t>{15, 25, 35, 128, 15, 25, 35, 128,
                                            25, 35, 45, 50, 25, 35, 45, 50}));
}

TEST(TextureResampleTest, InvalidInputDoesNotWriteDestination) {
  const std::array<std::uint8_t, 4> source{10, 20, 30, 40};
  std::array<std::uint8_t, 16> output{};
  const auto expected = output;
  ResampleRgba(nullptr, 1, 1, 2, 2, output.data());
  ResampleRgba(source.data(), 0, 1, 2, 2, output.data());
  ResampleRgba(source.data(), 1, 0, 2, 2, output.data());
  ResampleRgba(source.data(), 1, 1, 0, 2, output.data());
  ResampleRgba(source.data(), 1, 1, 2, 0, output.data());
  ResampleRgba(source.data(), 1, 1, 2, 2, nullptr);
  EXPECT_EQ(output, expected);
}

}
}
