#include "runtime/roblox_text_surface_overlay.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "runtime/roblox_text_font_resolver.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr std::size_t kMaximumRasterTextBytes = 4096;
constexpr std::uint64_t kMaximumOverlayPixels = 16ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumFallbackFonts = 6;

std::mutex g_active_overlay_mutex;
RobloxTextSurfaceOverlay* g_active_overlay = nullptr;
std::atomic<bool> g_overlay_may_present{false};

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

void SecureClear(std::vector<std::uint8_t>* value) {
  if (value == nullptr) {
    return;
  }
  volatile std::uint8_t* bytes = value->empty() ? nullptr : value->data();
  for (std::size_t index = 0; index < value->size(); ++index) {
    bytes[index] = 0;
  }
  value->clear();
}

struct SensitiveString {
  ~SensitiveString() { SecureClear(&value); }
  std::string value;
};

bool IsUtf8Continuation(unsigned char value) {
  return (value & 0xC0U) == 0x80U;
}

SensitiveString VisibleTextWindow(
    const RobloxTextOverlayPresentation& presentation, std::size_t* caret_byte,
    std::size_t* selection_begin_byte, std::size_t* selection_end_byte) {
  SensitiveString result;
  const std::string& text = presentation.display_utf8;
  const std::size_t caret = std::min(presentation.caret_utf8_byte, text.size());
  const std::size_t selection_begin =
      std::min(presentation.selection_begin_utf8_byte, text.size());
  const std::size_t selection_end =
      std::min(presentation.selection_end_utf8_byte, text.size());
  if (text.size() <= kMaximumRasterTextBytes) {
    result.value = text;
    *caret_byte = caret;
    *selection_begin_byte = std::min(selection_begin, selection_end);
    *selection_end_byte = std::max(selection_begin, selection_end);
    return result;
  }

  const std::size_t half = kMaximumRasterTextBytes / 2;
  std::size_t begin = caret > half ? caret - half : 0;
  std::size_t end = std::min(text.size(), begin + kMaximumRasterTextBytes);
  if (end - begin < kMaximumRasterTextBytes && begin != 0) {
    begin = end - kMaximumRasterTextBytes;
  }
  while (begin < text.size() &&
         IsUtf8Continuation(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && end < text.size() &&
         IsUtf8Continuation(static_cast<unsigned char>(text[end]))) {
    --end;
  }
  result.value.assign(text.data() + begin, end - begin);
  *caret_byte = std::min(caret - begin, result.value.size());
  const auto map_to_window = [begin, end](std::size_t byte) {
    if (byte <= begin) {
      return std::size_t{0};
    }
    if (byte >= end) {
      return end - begin;
    }
    return byte - begin;
  };
  *selection_begin_byte =
      map_to_window(std::min(selection_begin, selection_end));
  *selection_end_byte = map_to_window(std::max(selection_begin, selection_end));
  return result;
}

std::vector<std::string> ResolveFontFiles(
    const RobloxTextFontSelection& selection) {
  std::vector<std::string> files;
  const char* configured = std::getenv("MOCKTAIL_TEXT_FONT");
  if (configured != nullptr && configured[0] != '\0') {
    files.emplace_back(configured);
  } else if (!selection.primary_file.empty()) {
    files.push_back(selection.primary_file);
  }
  if (!FcInit()) {
    return files;
  }

  FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>("sans"));
  if (pattern == nullptr) {
    return files;
  }
  FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);
  FcResult result = FcResultNoMatch;
  FcFontSet* fonts = FcFontSort(nullptr, pattern, FcTrue, nullptr, &result);
  FcPatternDestroy(pattern);
  if (fonts == nullptr) {
    return files;
  }
  for (int index = 0;
       index < fonts->nfont && files.size() < kMaximumFallbackFonts; ++index) {
    FcChar8* file = nullptr;
    if (FcPatternGetString(fonts->fonts[index], FC_FILE, 0, &file) !=
            FcResultMatch ||
        file == nullptr || file[0] == '\0') {
      continue;
    }
    const std::string candidate(reinterpret_cast<const char*>(file));
    if (std::find(files.begin(), files.end(), candidate) == files.end()) {
      files.push_back(candidate);
    }
  }
  FcFontSetDestroy(fonts);
  return files;
}

void SetPixel(std::vector<std::uint8_t>* pixels, int width, int x, int y,
              std::uint8_t red, std::uint8_t green, std::uint8_t blue,
              std::uint8_t alpha) {
  if (pixels == nullptr || x < 0 || y < 0 || x >= width) {
    return;
  }
  const std::size_t offset =
      (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
       static_cast<std::size_t>(x)) *
      4;
  if (offset + 3 >= pixels->size()) {
    return;
  }
  (*pixels)[offset] = red;
  (*pixels)[offset + 1] = green;
  (*pixels)[offset + 2] = blue;
  (*pixels)[offset + 3] = alpha;
}

void BlendSurface(const SDL_Surface* surface, int destination_x,
                  int destination_y, int clip_left, int clip_top,
                  int clip_right, int clip_bottom, int destination_width,
                  std::uint8_t opacity,
                  std::vector<std::uint8_t>* destination) {
  if (surface == nullptr || surface->pixels == nullptr ||
      destination == nullptr) {
    return;
  }
  const auto* source = static_cast<const std::uint8_t*>(surface->pixels);
  for (int source_y = 0; source_y < surface->h; ++source_y) {
    const int y = destination_y + source_y;
    if (y < clip_top || y >= clip_bottom) {
      continue;
    }
    for (int source_x = 0; source_x < surface->w; ++source_x) {
      const int x = destination_x + source_x;
      if (x < clip_left || x >= clip_right) {
        continue;
      }
      const std::size_t source_offset =
          static_cast<std::size_t>(source_y) *
              static_cast<std::size_t>(surface->pitch) +
          static_cast<std::size_t>(source_x) * 4;
      const std::size_t destination_offset =
          (static_cast<std::size_t>(y) *
               static_cast<std::size_t>(destination_width) +
           static_cast<std::size_t>(x)) *
          4;
      const std::uint32_t alpha =
          (static_cast<std::uint32_t>(source[source_offset + 3]) * opacity +
           127U) /
          255U;
      if (alpha == 0) {
        continue;
      }
      (*destination)[destination_offset] = source[source_offset];
      (*destination)[destination_offset + 1] = source[source_offset + 1];
      (*destination)[destination_offset + 2] = source[source_offset + 2];
      (*destination)[destination_offset + 3] = static_cast<std::uint8_t>(alpha);
    }
  }
}

Status RasterizationFailure(const char* operation) {
  std::string message = operation != nullptr ? operation : "text rasterization";
  const char* detail = SDL_GetError();
  if (detail != nullptr && detail[0] != '\0') {
    message.append(": ").append(detail);
  }
  return Status::Error(StatusCode::kPlatformError, std::move(message));
}

SDL_Color ResolveTextColor(std::int32_t packed_color) {
  const std::uint32_t packed = static_cast<std::uint32_t>(packed_color);
  const std::uint8_t alpha = static_cast<std::uint8_t>(packed >> 24U);
  if (alpha == 0) {
    return {212, 216, 225, 255};
  }
  return {static_cast<std::uint8_t>(packed >> 16U),
          static_cast<std::uint8_t>(packed >> 8U),
          static_cast<std::uint8_t>(packed), alpha};
}

struct TextBytePosition {
  int x = 0;
  int y = 0;
  int height = 0;
};

TextBytePosition TextPosition(TTF_Text* layout, std::size_t byte_offset,
                              int text_width) {
  if (layout == nullptr) {
    return {};
  }
  TTF_SubString substring{};
  const int offset = static_cast<int>(std::min<std::size_t>(
      byte_offset, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  if (!TTF_GetTextSubString(layout, offset, &substring)) {
    return {};
  }
  TextBytePosition position;
  position.x = substring.rect.x;
  position.y = substring.rect.y;
  position.height = substring.rect.h;
  if ((substring.flags & TTF_SUBSTRING_TEXT_END) != 0) {
    position.x = std::min(text_width, substring.rect.x + substring.rect.w);
  }
  return position;
}

}  // namespace

RobloxTextSurfaceOverlay::~RobloxTextSurfaceOverlay() { (void)Shutdown(); }

Status RobloxTextSurfaceOverlay::Initialize(
    RobloxTextOverlayViewport viewport) {
  if (!viewport.valid()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "text surface overlay viewport is invalid");
  }
  std::lock_guard<std::mutex> active_lock(g_active_overlay_mutex);
  std::lock_guard<std::mutex> lock(mutex_);
  if (initialized_ || g_active_overlay != nullptr) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "text surface overlay is already initialized");
  }
  viewport_ = viewport;
  failure_ = Status::Ok();
  initialized_ = true;
  g_active_overlay = this;
  g_overlay_may_present.store(false, std::memory_order_release);
  return Status::Ok();
}

Status RobloxTextSurfaceOverlay::Shutdown() {
  std::lock_guard<std::mutex> active_lock(g_active_overlay_mutex);
  std::lock_guard<std::mutex> lock(mutex_);
  const bool owned_active_overlay = g_active_overlay == this;
  if (owned_active_overlay) {
    g_active_overlay = nullptr;
  }
  state_.Reset();
  ClearFrameLocked();
  viewport_ = {};
  initialized_ = false;
  if (owned_active_overlay) {
    g_overlay_may_present.store(false, std::memory_order_release);
  }
  return failure_;
}

Status RobloxTextSurfaceOverlay::UpdateViewport(
    RobloxTextOverlayViewport viewport) {
  if (!viewport.valid()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "text surface overlay viewport is invalid");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "text surface overlay is not initialized");
  }
  viewport_ = viewport;
  return Status::Ok();
}

RobloxTextDisplaySink RobloxTextSurfaceOverlay::sink() {
  return {this, &RobloxTextSurfaceOverlay::UpdateCallback};
}

void RobloxTextSurfaceOverlay::UpdateCallback(
    void* context, const RobloxTextDisplayUpdate& update) {
  if (context != nullptr) {
    static_cast<RobloxTextSurfaceOverlay*>(context)->ApplyUpdate(update);
  }
}

void RobloxTextSurfaceOverlay::ApplyUpdate(
    const RobloxTextDisplayUpdate& update) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_) {
    return;
  }
  bool changed = false;
  Status status = state_.Apply(update, viewport_, &changed);
  if (!status.ok()) {
    RecordFailureLocked(std::move(status));
    return;
  }
  if (!changed) {
    return;
  }
  ++state_revision_;
  if (state_revision_ == 0) {
    ++state_revision_;
  }
  const RobloxTextOverlayPresentation& presentation = state_.presentation();
  const bool drawable = presentation.visible && presentation.geometry.valid();
  if (!drawable) {
    ClearFrameLocked();
  }
  // Publish only after the synchronized state/revision transition is fully
  // committed. A stale true merely takes the slow path; false must never hide
  // a drawable committed presentation.
  g_overlay_may_present.store(drawable, std::memory_order_release);
}

bool RobloxTextSurfaceOverlay::QueryFrame(MocktailTextOverlayFrameInfo* frame) {
  if (frame == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  *frame = {};
  frame->revision = state_revision_;
  if (!initialized_) {
    return false;
  }
  const RobloxTextOverlayPresentation& presentation = state_.presentation();
  if (!presentation.visible) {
    return true;
  }
  if (!presentation.geometry.valid()) {
    return true;
  }
  if (raster_revision_ != state_revision_) {
    Status status = RasterizeLocked();
    if (!status.ok()) {
      RecordFailureLocked(std::move(status));
      return false;
    }
  }
  if (rgba_.empty() || !presentation.geometry.valid()) {
    return false;
  }
  frame->visible = 1;
  frame->coordinate_width = static_cast<std::uint32_t>(viewport_.width);
  frame->coordinate_height = static_cast<std::uint32_t>(viewport_.height);
  frame->x = presentation.geometry.x;
  frame->y = presentation.geometry.y;
  frame->width = static_cast<std::uint32_t>(presentation.geometry.width);
  frame->height = static_cast<std::uint32_t>(presentation.geometry.height);
  frame->row_bytes = frame->width * 4;
  frame->rgba_bytes = rgba_.size();
  return true;
}

bool RobloxTextSurfaceOverlay::CopyFrame(std::uint64_t revision, void* rgba,
                                         std::size_t rgba_capacity) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_ || revision == 0 || revision != raster_revision_ ||
      rgba == nullptr || rgba_capacity < rgba_.size() || rgba_.empty()) {
    return false;
  }
  std::memcpy(rgba, rgba_.data(), rgba_.size());
  return true;
}

Status RobloxTextSurfaceOverlay::RasterizeLocked() {
  const RobloxTextOverlayPresentation& presentation = state_.presentation();
  if (!presentation.visible || !presentation.geometry.valid()) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "visible text surface geometry is unavailable");
  }
  const std::uint64_t width = presentation.geometry.width;
  const std::uint64_t height = presentation.geometry.height;
  if (width * height > kMaximumOverlayPixels ||
      width * height > std::numeric_limits<std::size_t>::max() / 4) {
    return Status::Error(StatusCode::kUnsupported,
                         "text surface dimensions exceed the safety limit");
  }

  std::vector<std::uint8_t> candidate(
      static_cast<std::size_t>(width * height * 4), 0);
  const int frame_width = static_cast<int>(width);
  const int frame_height = static_cast<int>(height);
  const int padding = std::clamp(frame_width / 12, 8, 14);
  const int clip_left = std::min(padding, frame_width);
  const int clip_right = std::max(clip_left, frame_width - padding);
  const int clip_top = 3;
  const int clip_bottom = std::max(clip_top, frame_height - 3);
  const int available_width = std::max(1, clip_right - clip_left);
  const bool wrapped_layout =
      presentation.multiline || presentation.text_wrapped;

  if (!TTF_Init()) {
    SecureClear(&candidate);
    return RasterizationFailure("unable to initialize SDL3_ttf");
  }
  const RobloxTextFontSelection font_selection =
      ResolveRobloxTextFont(presentation.font);
  const float native_point_size =
      std::isfinite(presentation.font_size) ? presentation.font_size : 0.0F;
  const float point_size =
      native_point_size > 0.0F
          ? std::clamp(native_point_size * font_selection.size_scale, 1.0F,
                       512.0F)
          : std::clamp(static_cast<float>(height) * 0.38F, 12.0F, 28.0F);
  const std::vector<std::string> font_files = ResolveFontFiles(font_selection);
  std::vector<TTF_Font*> fonts;
  fonts.reserve(font_files.size());
  for (const std::string& file : font_files) {
    TTF_Font* font = TTF_OpenFont(file.c_str(), point_size);
    if (font != nullptr) {
      fonts.push_back(font);
    }
  }
  if (fonts.empty()) {
    TTF_Quit();
    SecureClear(&candidate);
    return RasterizationFailure("unable to open a Roblox or fallback font");
  }
  for (std::size_t index = 1; index < fonts.size(); ++index) {
    (void)TTF_AddFallbackFont(fonts.front(), fonts[index]);
  }
  if (wrapped_layout) {
    TTF_HorizontalAlignment alignment = TTF_HORIZONTAL_ALIGN_LEFT;
    if (presentation.x_alignment == 1) {
      alignment = TTF_HORIZONTAL_ALIGN_RIGHT;
    } else if (presentation.x_alignment == 2) {
      alignment = TTF_HORIZONTAL_ALIGN_CENTER;
    }
    TTF_SetFontWrapAlignment(fonts.front(), alignment);
  }

  std::size_t caret_byte = 0;
  std::size_t selection_begin_byte = 0;
  std::size_t selection_end_byte = 0;
  SensitiveString text = VisibleTextWindow(
      presentation, &caret_byte, &selection_begin_byte, &selection_end_byte);
  SDL_Surface* rendered = nullptr;
  SDL_Surface* converted = nullptr;
  const SDL_Color text_color = ResolveTextColor(presentation.text_color);
  TTF_Text* layout = TTF_CreateText(nullptr, fonts.front(), text.value.data(),
                                    text.value.size());
  if (layout != nullptr && wrapped_layout) {
    (void)TTF_SetTextWrapWidth(layout, available_width);
  }
  int text_width = 0;
  int text_height = std::max(1, TTF_GetFontHeight(fonts.front()));
  TextBytePosition caret_position;
  TextBytePosition selection_begin_position;
  TextBytePosition selection_end_position;
  if (layout != nullptr) {
    (void)TTF_GetTextSize(layout, &text_width, &text_height);
    caret_position = TextPosition(layout, caret_byte, text_width);
    selection_begin_position =
        TextPosition(layout, selection_begin_byte, text_width);
    selection_end_position =
        TextPosition(layout, selection_end_byte, text_width);
  }
  if (!text.value.empty()) {
    // SDL_ttf implementations have historically differed on whether fg.a is
    // incorporated into glyph coverage. Rasterize opaque color and apply the
    // Android ARGB opacity explicitly while copying the straight-alpha mask.
    const SDL_Color raster_color = {text_color.r, text_color.g, text_color.b,
                                    255};
    rendered = wrapped_layout
                   ? TTF_RenderText_Blended_Wrapped(
                         fonts.front(), text.value.data(), text.value.size(),
                         raster_color, available_width)
                   : TTF_RenderText_Blended(fonts.front(), text.value.data(),
                                            text.value.size(), raster_color);
    if (rendered != nullptr) {
      converted = SDL_ConvertSurface(rendered, SDL_PIXELFORMAT_RGBA32);
    }
  }

  Status status = Status::Ok();
  if (!text.value.empty() && (rendered == nullptr || converted == nullptr)) {
    status = RasterizationFailure("unable to rasterize Unicode text");
  } else {
    int text_offset = 0;
    if (!wrapped_layout && text_width <= available_width) {
      if (presentation.x_alignment == 1) {
        text_offset = available_width - text_width;
      } else if (presentation.x_alignment == 2) {
        text_offset = (available_width - text_width) / 2;
      }
    } else if (!wrapped_layout && caret_position.x > available_width - 3) {
      text_offset = available_width - 3 - caret_position.x;
    }
    int text_y = clip_top;
    if (presentation.y_alignment == 1) {
      text_y = std::max(clip_top, (frame_height - text_height) / 2);
    } else if (presentation.y_alignment == 2) {
      text_y = std::max(clip_top, clip_bottom - text_height);
    }
    const auto fill_highlight = [&](int left, int top, int right, int bottom) {
      const int bounded_left = std::clamp(left, clip_left, clip_right);
      const int bounded_right = std::clamp(right, bounded_left, clip_right);
      const int bounded_top = std::clamp(top, clip_top, clip_bottom);
      const int bounded_bottom = std::clamp(bottom, bounded_top, clip_bottom);
      for (int y = bounded_top; y < bounded_bottom; ++y) {
        for (int x = bounded_left; x < bounded_right; ++x) {
          SetPixel(&candidate, frame_width, x, y, 65, 132, 228, 112);
        }
      }
    };
    if (selection_begin_byte != selection_end_byte) {
      if (wrapped_layout && layout != nullptr) {
        const std::size_t range_bytes =
            selection_end_byte - selection_begin_byte;
        const int range_offset = static_cast<int>(std::min<std::size_t>(
            selection_begin_byte,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int range_length = static_cast<int>(std::min<std::size_t>(
            range_bytes,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        int count = 0;
        TTF_SubString** substrings = TTF_GetTextSubStringsForRange(
            layout, range_offset, range_length, &count);
        if (substrings != nullptr) {
          for (int index = 0; index < count; ++index) {
            const TTF_SubString* substring = substrings[index];
            if (substring == nullptr) {
              continue;
            }
            fill_highlight(
                clip_left + text_offset + substring->rect.x,
                text_y + substring->rect.y,
                clip_left + text_offset + substring->rect.x + substring->rect.w,
                text_y + substring->rect.y + substring->rect.h);
          }
          SDL_free(substrings);
        }
      } else {
        fill_highlight(
            clip_left + text_offset +
                std::min(selection_begin_position.x, selection_end_position.x),
            text_y,
            clip_left + text_offset +
                std::max(selection_begin_position.x, selection_end_position.x),
            text_y + text_height);
      }
    }
    if (converted != nullptr) {
      BlendSurface(converted, clip_left + text_offset, text_y, clip_left,
                   clip_top, clip_right, clip_bottom, frame_width, text_color.a,
                   &candidate);
    }
    const int caret_screen_x =
        std::clamp(clip_left + text_offset + caret_position.x, clip_left,
                   std::max(clip_left, clip_right - 2));
    const int caret_line_height =
        caret_position.height > 0
            ? caret_position.height
            : std::max(1, TTF_GetFontHeight(fonts.front()));
    const int caret_height =
        std::min(std::max(12, caret_line_height), clip_bottom - clip_top);
    const int caret_y =
        std::clamp(text_y + caret_position.y, clip_top,
                   std::max(clip_top, clip_bottom - caret_height));
    for (int y = caret_y; y < caret_y + caret_height; ++y) {
      SetPixel(&candidate, frame_width, caret_screen_x, y, text_color.r,
               text_color.g, text_color.b, text_color.a);
    }
  }

  if (converted != nullptr) {
    SDL_DestroySurface(converted);
  }
  if (rendered != nullptr) {
    SDL_DestroySurface(rendered);
  }
  if (layout != nullptr) {
    TTF_DestroyText(layout);
  }
  TTF_ClearFallbackFonts(fonts.front());
  for (auto iterator = fonts.rbegin(); iterator != fonts.rend(); ++iterator) {
    TTF_CloseFont(*iterator);
  }
  TTF_Quit();

  if (!status.ok()) {
    SecureClear(&candidate);
    return status;
  }
  SecureClear(&rgba_);
  rgba_ = std::move(candidate);
  raster_revision_ = state_revision_;
  if (!ready_logged_) {
    ready_logged_ = true;
    std::fprintf(stderr,
                 "  [input] same-surface TextBox raster ready "
                 "(SDL3_ttf/FreeType/HarfBuzz)\n");
  }
  return Status::Ok();
}

void RobloxTextSurfaceOverlay::ClearFrameLocked() {
  SecureClear(&rgba_);
  raster_revision_ = 0;
}

void RobloxTextSurfaceOverlay::RecordFailureLocked(Status status) {
  if (status.ok()) {
    return;
  }
  failure_ = std::move(status);
  if (!failure_logged_) {
    failure_logged_ = true;
    std::fprintf(stderr, "  [input] same-surface TextBox raster failed: %s\n",
                 failure_.message().c_str());
  }
}

}  // namespace runtime
}  // namespace mocktail

extern "C" bool mocktail_text_overlay_may_present() {
  return mocktail::runtime::g_overlay_may_present.load(
      std::memory_order_acquire);
}

extern "C" bool mocktail_text_overlay_query(
    MocktailTextOverlayFrameInfo* frame) {
  std::lock_guard<std::mutex> lock(mocktail::runtime::g_active_overlay_mutex);
  return mocktail::runtime::g_active_overlay != nullptr &&
         mocktail::runtime::g_active_overlay->QueryFrame(frame);
}

extern "C" bool mocktail_text_overlay_copy(std::uint64_t revision, void* rgba,
                                           std::size_t rgba_capacity) {
  std::lock_guard<std::mutex> lock(mocktail::runtime::g_active_overlay_mutex);
  return mocktail::runtime::g_active_overlay != nullptr &&
         mocktail::runtime::g_active_overlay->CopyFrame(revision, rgba,
                                                        rgba_capacity);
}
