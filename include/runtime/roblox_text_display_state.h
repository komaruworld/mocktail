#ifndef MOCKTAIL_RUNTIME_ROBLOX_TEXT_DISPLAY_STATE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_TEXT_DISPLAY_STATE_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "mocktail/status.h"
#include "runtime/roblox_text_editor.h"

namespace mocktail {
namespace runtime {

struct RobloxTextOverlayViewport {
  int width = 0;
  int height = 0;

  bool valid() const { return width > 0 && height > 0; }
};

struct RobloxTextOverlayGeometry {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool used_fallback = false;

  bool valid() const { return width > 0 && height > 0; }

  bool operator==(const RobloxTextOverlayGeometry& other) const {
    return x == other.x && y == other.y && width == other.width &&
           height == other.height && used_fallback == other.used_fallback;
  }
  bool operator!=(const RobloxTextOverlayGeometry& other) const {
    return !(*this == other);
  }
};

// Main-thread presentation owned by the host overlay. `display_utf8` is
// sensitive for ordinary fields and contains only bullets for password types.
// The destructor and move assignment wipe retained bytes before releasing
// their storage.
struct RobloxTextOverlayPresentation {
  RobloxTextOverlayPresentation() = default;
  ~RobloxTextOverlayPresentation();

  RobloxTextOverlayPresentation(const RobloxTextOverlayPresentation&) = delete;
  RobloxTextOverlayPresentation& operator=(
      const RobloxTextOverlayPresentation&) = delete;
  RobloxTextOverlayPresentation(RobloxTextOverlayPresentation&& other);
  RobloxTextOverlayPresentation& operator=(
      RobloxTextOverlayPresentation&& other);

  void ClearSensitive();

  bool visible = false;
  uint64_t generation = 0;
  RobloxTextOverlayGeometry geometry;
  float font_size = 0.0F;
  int32_t x_alignment = 0;
  int32_t y_alignment = 1;
  int32_t text_color = 0;
  int32_t text_input_type = 0;
  std::string display_utf8;
  std::size_t caret_utf8_byte = 0;
  std::size_t selection_begin_utf8_byte = 0;
  std::size_t selection_end_utf8_byte = 0;
};

bool IsRobloxPasswordTextInputType(int32_t text_input_type);

RobloxTextOverlayGeometry ResolveRobloxTextOverlayGeometry(
    const RobloxTextDisplayUpdate& update,
    const RobloxTextOverlayViewport& viewport);

Status BuildRobloxTextOverlayPresentation(
    const RobloxTextDisplayUpdate& update,
    const RobloxTextOverlayViewport& viewport,
    RobloxTextOverlayPresentation* presentation);

// Applies show/update/hide events in generation order. Stale events are
// harmless no-ops, which prevents a delayed hide from closing a newer field.
class RobloxTextDisplayState final {
 public:
  RobloxTextDisplayState() = default;
  ~RobloxTextDisplayState() = default;

  RobloxTextDisplayState(const RobloxTextDisplayState&) = delete;
  RobloxTextDisplayState& operator=(const RobloxTextDisplayState&) = delete;

  Status Apply(const RobloxTextDisplayUpdate& update,
               const RobloxTextOverlayViewport& viewport, bool* changed);
  void Reset();

  const RobloxTextOverlayPresentation& presentation() const {
    return presentation_;
  }

 private:
  RobloxTextOverlayPresentation presentation_;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_TEXT_DISPLAY_STATE_H_
