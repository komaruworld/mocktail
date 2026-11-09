#ifndef MOCKTAIL_RUNTIME_DISPLAY_SIZE_H_
#define MOCKTAIL_RUNTIME_DISPLAY_SIZE_H_

#include <cstdint>
#include <limits>

namespace mocktail {
namespace runtime {

struct DisplaySize {
  std::int32_t width = 1920;
  std::int32_t height = 1080;
};

// Physical pixels; window size is captured at startup.
inline constexpr const char* kDisplaySizeEnvironment =
    "MOCKTAIL_DISPLAY_SIZE_INTERNAL";
inline constexpr const char* kWindowSizeEnvironment =
    "MOCKTAIL_WINDOW_SIZE_INTERNAL";

inline DisplaySize ParseDisplaySize(const char* value) {
  const DisplaySize fallback;
  if (value == nullptr) {
    return fallback;
  }
  std::int64_t parts[2] = {0, 0};
  const char* cursor = value;
  for (int index = 0; index < 2; ++index) {
    if (*cursor < '0' || *cursor > '9') {
      return fallback;
    }
    std::int64_t parsed = 0;
    while (*cursor >= '0' && *cursor <= '9') {
      parsed = parsed * 10 + (*cursor - '0');
      if (parsed > std::numeric_limits<std::int32_t>::max()) {
        return fallback;
      }
      ++cursor;
    }
    if (parsed == 0) {
      return fallback;
    }
    parts[index] = parsed;
    if (index == 0) {
      if (*cursor != 'x') {
        return fallback;
      }
      ++cursor;
    }
  }
  if (*cursor != '\0') {
    return fallback;
  }
  return {static_cast<std::int32_t>(parts[0]),
          static_cast<std::int32_t>(parts[1])};
}

inline std::int32_t PixelsToMillimetersAt160Dpi(std::int32_t pixels) {
  return static_cast<std::int32_t>(static_cast<std::int64_t>(pixels) * 254 /
                                   1600);
}

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_DISPLAY_SIZE_H_
