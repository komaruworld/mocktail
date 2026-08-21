#include "runtime/roblox_text_display_state.h"

#include <utf8proc.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

constexpr int kMinimumNativeWidth = 32;
constexpr int kMinimumNativeHeight = 18;
constexpr char kPasswordBullet[] = "\xE2\x80\xA2";

void SecureClear(std::string* value) {
  if (value == nullptr) {
    return;
  }
  volatile char* bytes = value->empty() ? nullptr : value->data();
  for (std::size_t index = 0; index < value->size(); ++index) {
    bytes[index] = 0;
  }
  value->clear();
}

std::size_t Utf8ByteForUtf16Cursor(const char* text, std::size_t size,
                                   int32_t requested_utf16,
                                   std::size_t* codepoints_before_cursor) {
  const std::size_t target =
      requested_utf16 > 0 ? static_cast<std::size_t>(requested_utf16) : 0;
  std::size_t byte = 0;
  std::size_t utf16 = 0;
  std::size_t codepoints = 0;
  while (byte < size) {
    utf8proc_int32_t codepoint = 0;
    const utf8proc_ssize_t count = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(text + byte),
        static_cast<utf8proc_ssize_t>(size - byte), &codepoint);
    if (count <= 0) {
      break;
    }
    const std::size_t units = codepoint > 0xFFFF ? 2U : 1U;
    if (utf16 + units > target) {
      break;
    }
    byte += static_cast<std::size_t>(count);
    utf16 += units;
    ++codepoints;
  }
  if (codepoints_before_cursor != nullptr) {
    *codepoints_before_cursor = codepoints;
  }
  return byte;
}

bool ValidateAndCountUtf8(const char* text, std::size_t size,
                          std::size_t* codepoints) {
  std::size_t offset = 0;
  std::size_t count = 0;
  while (offset < size) {
    utf8proc_int32_t codepoint = 0;
    const utf8proc_ssize_t length = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(text + offset),
        static_cast<utf8proc_ssize_t>(size - offset), &codepoint);
    if (length <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(length);
    ++count;
  }
  if (codepoints != nullptr) {
    *codepoints = count;
  }
  return true;
}

bool NativeGeometryIntersectsViewport(
    const RobloxTextDisplayUpdate& update,
    const RobloxTextOverlayViewport& viewport) {
  if (!viewport.valid() || update.area_width < kMinimumNativeWidth ||
      update.area_height < kMinimumNativeHeight) {
    return false;
  }
  const int64_t right = static_cast<int64_t>(update.area_x) +
                        static_cast<int64_t>(update.area_width);
  const int64_t bottom = static_cast<int64_t>(update.area_y) +
                         static_cast<int64_t>(update.area_height);
  return right > 0 && bottom > 0 && update.area_x < viewport.width &&
         update.area_y < viewport.height;
}

}  // namespace

RobloxTextOverlayPresentation::~RobloxTextOverlayPresentation() {
  ClearSensitive();
}

RobloxTextOverlayPresentation::RobloxTextOverlayPresentation(
    RobloxTextOverlayPresentation&& other)
    : visible(other.visible),
      generation(other.generation),
      geometry(other.geometry),
      font_size(other.font_size),
      x_alignment(other.x_alignment),
      y_alignment(other.y_alignment),
      text_color(other.text_color),
      text_input_type(other.text_input_type),
      display_utf8(other.display_utf8),
      caret_utf8_byte(other.caret_utf8_byte),
      selection_begin_utf8_byte(other.selection_begin_utf8_byte),
      selection_end_utf8_byte(other.selection_end_utf8_byte) {
  other.ClearSensitive();
}

RobloxTextOverlayPresentation& RobloxTextOverlayPresentation::operator=(
    RobloxTextOverlayPresentation&& other) {
  if (this == &other) {
    return *this;
  }
  ClearSensitive();
  visible = other.visible;
  generation = other.generation;
  geometry = other.geometry;
  font_size = other.font_size;
  x_alignment = other.x_alignment;
  y_alignment = other.y_alignment;
  text_color = other.text_color;
  text_input_type = other.text_input_type;
  display_utf8 = other.display_utf8;
  caret_utf8_byte = other.caret_utf8_byte;
  selection_begin_utf8_byte = other.selection_begin_utf8_byte;
  selection_end_utf8_byte = other.selection_end_utf8_byte;
  other.ClearSensitive();
  return *this;
}

void RobloxTextOverlayPresentation::ClearSensitive() {
  SecureClear(&display_utf8);
  visible = false;
  generation = 0;
  geometry = {};
  font_size = 0.0F;
  x_alignment = 0;
  y_alignment = 1;
  text_color = 0;
  text_input_type = 0;
  caret_utf8_byte = 0;
  selection_begin_utf8_byte = 0;
  selection_end_utf8_byte = 0;
}

bool IsRobloxPasswordTextInputType(int32_t text_input_type) {
  return text_input_type == 5 || text_input_type == 9 || text_input_type == 10;
}

RobloxTextOverlayGeometry ResolveRobloxTextOverlayGeometry(
    const RobloxTextDisplayUpdate& update,
    const RobloxTextOverlayViewport& viewport) {
  if (!NativeGeometryIntersectsViewport(update, viewport)) {
    // A guessed bottom-screen editor is visibly detached from Roblox's
    // TextBox. Missing native geometry stays fail-closed until a later focus
    // snapshot supplies a real rectangle.
    return {};
  }

  // Keep the native rectangle intact. The compositor clips its destination
  // against the swapchain and advances the source rectangle by the matching
  // amount. Cropping here would shrink the layout box and incorrectly
  // re-align text that is only partially outside the viewport.
  return {update.area_x, update.area_y, update.area_width, update.area_height,
          false};
}

Status BuildRobloxTextOverlayPresentation(
    const RobloxTextDisplayUpdate& update,
    const RobloxTextOverlayViewport& viewport,
    RobloxTextOverlayPresentation* presentation) {
  if (presentation == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "text overlay presentation output is required");
  }
  if (update.generation == 0) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "text overlay generation must be nonzero");
  }
  if (!viewport.valid()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "text overlay viewport must be positive");
  }
  if (update.utf8_size != 0 && update.utf8 == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "text overlay UTF-8 buffer is unavailable");
  }
  const char* text = update.utf8 != nullptr ? update.utf8 : "";
  std::size_t codepoint_count = 0;
  if (!ValidateAndCountUtf8(text, update.utf8_size, &codepoint_count)) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "text overlay received invalid UTF-8");
  }

  RobloxTextOverlayPresentation candidate;
  candidate.visible = true;
  candidate.generation = update.generation;
  candidate.geometry = ResolveRobloxTextOverlayGeometry(update, viewport);
  candidate.font_size = update.font_size;
  candidate.x_alignment = update.x_alignment;
  candidate.y_alignment = update.y_alignment;
  candidate.text_color = update.text_color;
  candidate.text_input_type = update.text_input_type;

  std::size_t codepoints_before_cursor = 0;
  const std::size_t caret_source_byte = Utf8ByteForUtf16Cursor(
      text, update.utf8_size, update.cursor_utf16, &codepoints_before_cursor);
  std::size_t codepoints_before_selection_begin = 0;
  std::size_t codepoints_before_selection_end = 0;
  const std::size_t selection_begin_source_byte = Utf8ByteForUtf16Cursor(
      text, update.utf8_size, update.selection_begin_utf16,
      &codepoints_before_selection_begin);
  const std::size_t selection_end_source_byte = Utf8ByteForUtf16Cursor(
      text, update.utf8_size, update.selection_end_utf16,
      &codepoints_before_selection_end);
  if (IsRobloxPasswordTextInputType(update.text_input_type)) {
    if (codepoint_count > std::numeric_limits<std::size_t>::max() /
                              (sizeof(kPasswordBullet) - 1)) {
      return Status::Error(StatusCode::kUnavailable,
                           "text overlay password mask is too large");
    }
    candidate.display_utf8.reserve(codepoint_count *
                                   (sizeof(kPasswordBullet) - 1));
    for (std::size_t index = 0; index < codepoint_count; ++index) {
      candidate.display_utf8.append(kPasswordBullet,
                                    sizeof(kPasswordBullet) - 1);
    }
    candidate.caret_utf8_byte =
        codepoints_before_cursor * (sizeof(kPasswordBullet) - 1);
    candidate.selection_begin_utf8_byte =
        std::min(codepoints_before_selection_begin,
                 codepoints_before_selection_end) *
        (sizeof(kPasswordBullet) - 1);
    candidate.selection_end_utf8_byte =
        std::max(codepoints_before_selection_begin,
                 codepoints_before_selection_end) *
        (sizeof(kPasswordBullet) - 1);
  } else {
    candidate.display_utf8.assign(text, update.utf8_size);
    candidate.caret_utf8_byte = caret_source_byte;
    candidate.selection_begin_utf8_byte =
        std::min(selection_begin_source_byte, selection_end_source_byte);
    candidate.selection_end_utf8_byte =
        std::max(selection_begin_source_byte, selection_end_source_byte);
  }

  *presentation = std::move(candidate);
  return Status::Ok();
}

Status RobloxTextDisplayState::Apply(const RobloxTextDisplayUpdate& update,
                                     const RobloxTextOverlayViewport& viewport,
                                     bool* changed) {
  if (changed == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "text overlay changed output is required");
  }
  *changed = false;

  if (update.event == RobloxTextDisplayEvent::kHide) {
    if (!presentation_.visible ||
        update.generation != presentation_.generation) {
      return Status::Ok();
    }
    presentation_.ClearSensitive();
    *changed = true;
    return Status::Ok();
  }

  if (update.event == RobloxTextDisplayEvent::kUpdate &&
      (!presentation_.visible ||
       update.generation != presentation_.generation)) {
    return Status::Ok();
  }
  if (update.event == RobloxTextDisplayEvent::kShow && presentation_.visible &&
      update.generation < presentation_.generation) {
    return Status::Ok();
  }

  RobloxTextOverlayPresentation candidate;
  Status status =
      BuildRobloxTextOverlayPresentation(update, viewport, &candidate);
  if (!status.ok()) {
    return status;
  }
  presentation_ = std::move(candidate);
  *changed = true;
  return Status::Ok();
}

void RobloxTextDisplayState::Reset() { presentation_.ClearSensitive(); }

}  // namespace runtime
}  // namespace mocktail
