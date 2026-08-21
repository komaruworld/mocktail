#include "runtime/roblox_text_display_state.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "runtime/roblox_text_surface_overlay.h"

namespace mocktail {
namespace runtime {
namespace {

RobloxTextDisplayUpdate Show(uint64_t generation, const std::string& text,
                             int32_t cursor_utf16 = 0,
                             int32_t text_input_type = 1) {
  RobloxTextDisplayUpdate update;
  update.event = RobloxTextDisplayEvent::kShow;
  update.generation = generation;
  update.area_x = 120;
  update.area_y = 80;
  update.area_width = 360;
  update.area_height = 44;
  update.text_input_type = text_input_type;
  update.utf8 = text.data();
  update.utf8_size = text.size();
  update.cursor_utf16 = cursor_utf16;
  update.selection_begin_utf16 = cursor_utf16;
  update.selection_end_utf16 = cursor_utf16;
  return update;
}

struct AlphaBounds {
  int minimum_x = std::numeric_limits<int>::max();
  int maximum_x = -1;

  bool valid() const { return maximum_x >= minimum_x; }
};

AlphaBounds FindAlphaBounds(const MocktailTextOverlayFrameInfo& frame,
                            const std::vector<std::uint8_t>& rgba) {
  AlphaBounds bounds;
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    for (std::uint32_t x = 0; x < frame.width; ++x) {
      const std::size_t alpha =
          (static_cast<std::size_t>(y) * frame.width + x) * 4 + 3;
      if (alpha < rgba.size() && rgba[alpha] != 0) {
        bounds.minimum_x = std::min(bounds.minimum_x, static_cast<int>(x));
        bounds.maximum_x = std::max(bounds.maximum_x, static_cast<int>(x));
      }
    }
  }
  return bounds;
}

TEST(RobloxTextDisplayStateTest, PreservesValidNativeTextBoxGeometry) {
  const std::string text = "hello";
  const RobloxTextDisplayUpdate update = Show(1, text, 5);

  const RobloxTextOverlayGeometry geometry =
      ResolveRobloxTextOverlayGeometry(update, {1280, 720});

  EXPECT_EQ(geometry.x, 120);
  EXPECT_EQ(geometry.y, 80);
  EXPECT_EQ(geometry.width, 360);
  EXPECT_EQ(geometry.height, 44);
  EXPECT_FALSE(geometry.used_fallback);
}

TEST(RobloxTextDisplayStateTest, RejectsDetachedFallbackForMissingGeometry) {
  const std::string text = "hello";
  RobloxTextDisplayUpdate update = Show(1, text, 5);
  update.area_x = 0;
  update.area_y = 0;
  update.area_width = 0;
  update.area_height = 0;

  const RobloxTextOverlayGeometry geometry =
      ResolveRobloxTextOverlayGeometry(update, {1000, 700});

  EXPECT_FALSE(geometry.valid());
  EXPECT_FALSE(geometry.used_fallback);
}

TEST(RobloxTextDisplayStateTest,
     PreservesNativeExtentForCompositorSourceClipping) {
  const std::string text = "edge";
  RobloxTextDisplayUpdate update = Show(1, text, 4);
  update.area_x = 760;
  update.area_y = 570;
  update.area_width = 120;
  update.area_height = 44;

  const RobloxTextOverlayGeometry geometry =
      ResolveRobloxTextOverlayGeometry(update, {800, 600});

  EXPECT_EQ(geometry.x, 760);
  EXPECT_EQ(geometry.y, 570);
  EXPECT_EQ(geometry.width, 120);
  EXPECT_EQ(geometry.height, 44);
}

TEST(RobloxTextDisplayStateTest,
     MasksEveryPasswordTypeWithoutRetainingPlaintext) {
  for (const int32_t type : {5, 9, 10}) {
    const std::string plaintext = u8"a😀z";
    RobloxTextOverlayPresentation presentation;

    ASSERT_TRUE(BuildRobloxTextOverlayPresentation(Show(1, plaintext, 3, type),
                                                   {1280, 720}, &presentation)
                    .ok());
    EXPECT_EQ(presentation.display_utf8, u8"•••");
    EXPECT_EQ(presentation.caret_utf8_byte, std::string(u8"••").size());
    EXPECT_EQ(presentation.display_utf8.find("a"), std::string::npos);
    EXPECT_EQ(presentation.display_utf8.find("z"), std::string::npos);
  }
}

TEST(RobloxTextDisplayStateTest, MapsUtf16CursorToUnicodeByteBoundary) {
  const std::string text = u8"A😀Б";
  RobloxTextOverlayPresentation presentation;

  ASSERT_TRUE(BuildRobloxTextOverlayPresentation(Show(7, text, 3), {1280, 720},
                                                 &presentation)
                  .ok());

  EXPECT_EQ(presentation.display_utf8, text);
  EXPECT_EQ(presentation.caret_utf8_byte, std::string(u8"A😀").size());
}

TEST(RobloxTextDisplayStateTest, MapsUnicodeSelectionToVisibleByteRange) {
  const std::string text = u8"A😀Б";
  RobloxTextDisplayUpdate update = Show(7, text, 3);
  update.selection_begin_utf16 = 1;
  update.selection_end_utf16 = 3;
  RobloxTextOverlayPresentation presentation;

  ASSERT_TRUE(BuildRobloxTextOverlayPresentation(update, {1280, 720},
                                                 &presentation)
                  .ok());

  EXPECT_EQ(presentation.selection_begin_utf8_byte, std::string("A").size());
  EXPECT_EQ(presentation.selection_end_utf8_byte,
            std::string(u8"A😀").size());
}

TEST(RobloxTextDisplayStateTest, MapsPasswordSelectionWithoutPlaintext) {
  const std::string plaintext = u8"a😀z";
  RobloxTextDisplayUpdate update = Show(1, plaintext, 3, 5);
  update.selection_begin_utf16 = 1;
  update.selection_end_utf16 = 3;
  RobloxTextOverlayPresentation presentation;

  ASSERT_TRUE(BuildRobloxTextOverlayPresentation(update, {1280, 720},
                                                 &presentation)
                  .ok());

  EXPECT_EQ(presentation.display_utf8, u8"•••");
  EXPECT_EQ(presentation.selection_begin_utf8_byte,
            std::string(u8"•").size());
  EXPECT_EQ(presentation.selection_end_utf8_byte,
            std::string(u8"••").size());
}

TEST(RobloxTextDisplayStateTest, IgnoresStaleUpdatesAndHideEvents) {
  RobloxTextDisplayState state;
  const std::string current = "current";
  bool changed = false;
  ASSERT_TRUE(state.Apply(Show(4, current, 7), {1280, 720}, &changed).ok());
  ASSERT_TRUE(changed);

  const std::string stale = "stale";
  RobloxTextDisplayUpdate stale_update = Show(3, stale, 5);
  stale_update.event = RobloxTextDisplayEvent::kUpdate;
  ASSERT_TRUE(state.Apply(stale_update, {1280, 720}, &changed).ok());
  EXPECT_FALSE(changed);
  EXPECT_EQ(state.presentation().display_utf8, current);

  RobloxTextDisplayUpdate stale_hide;
  stale_hide.event = RobloxTextDisplayEvent::kHide;
  stale_hide.generation = 3;
  ASSERT_TRUE(state.Apply(stale_hide, {1280, 720}, &changed).ok());
  EXPECT_FALSE(changed);
  EXPECT_TRUE(state.presentation().visible);

  RobloxTextDisplayUpdate current_hide = stale_hide;
  current_hide.generation = 4;
  ASSERT_TRUE(state.Apply(current_hide, {1280, 720}, &changed).ok());
  EXPECT_TRUE(changed);
  EXPECT_FALSE(state.presentation().visible);
  EXPECT_TRUE(state.presentation().display_utf8.empty());
}

TEST(RobloxTextDisplayStateTest,
     PublishesLibraryRasterForTheExistingVulkanSurface) {
  RobloxTextSurfaceOverlay overlay;
  EXPECT_FALSE(mocktail_text_overlay_may_present());
  ASSERT_TRUE(overlay.Initialize({800, 600}).ok());
  EXPECT_FALSE(mocktail_text_overlay_may_present());

  const std::string text = u8"Hello, мир";
  RobloxTextDisplayUpdate show = Show(7, text, 10);
  show.area_height = 48;
  RobloxTextDisplaySink sink = overlay.sink();
  ASSERT_NE(sink.update, nullptr);
  sink.update(sink.context, show);
  EXPECT_TRUE(mocktail_text_overlay_may_present());

  RobloxTextDisplayUpdate stale_hide;
  stale_hide.event = RobloxTextDisplayEvent::kHide;
  stale_hide.generation = 6;
  sink.update(sink.context, stale_hide);
  EXPECT_TRUE(mocktail_text_overlay_may_present());

  MocktailTextOverlayFrameInfo frame;
  ASSERT_TRUE(overlay.QueryFrame(&frame));
  EXPECT_EQ(frame.abi_version, MocktailTextOverlayFrameInfo::kAbiVersion);
  EXPECT_EQ(frame.visible, 1U);
  EXPECT_EQ(frame.coordinate_width, 800U);
  EXPECT_EQ(frame.coordinate_height, 600U);
  EXPECT_EQ(frame.x, 120);
  EXPECT_EQ(frame.y, 80);
  EXPECT_EQ(frame.width, 360U);
  EXPECT_EQ(frame.height, 48U);
  ASSERT_EQ(frame.rgba_bytes,
            static_cast<std::uint64_t>(frame.width) * frame.height * 4U);

  std::vector<std::uint8_t> rgba(frame.rgba_bytes);
  ASSERT_TRUE(overlay.CopyFrame(frame.revision, rgba.data(), rgba.size()));
  EXPECT_EQ(rgba[3], 0U);
  bool has_visible_pixel = false;
  for (std::size_t index = 3; index < rgba.size(); index += 4) {
    has_visible_pixel = has_visible_pixel || rgba[index] != 0;
  }
  EXPECT_TRUE(has_visible_pixel);

  RobloxTextDisplayUpdate hide;
  hide.event = RobloxTextDisplayEvent::kHide;
  hide.generation = 7;
  sink.update(sink.context, hide);
  EXPECT_FALSE(mocktail_text_overlay_may_present());
  MocktailTextOverlayFrameInfo hidden;
  ASSERT_TRUE(overlay.QueryFrame(&hidden));
  EXPECT_EQ(hidden.visible, 0U);
  EXPECT_GT(hidden.revision, frame.revision);
  EXPECT_FALSE(overlay.CopyFrame(frame.revision, rgba.data(), rgba.size()));
  EXPECT_TRUE(overlay.Shutdown().ok());
  EXPECT_FALSE(mocktail_text_overlay_may_present());
}

TEST(RobloxTextDisplayStateTest,
     InvalidGeometryNeverPublishesDrawableOverlayHint) {
  RobloxTextSurfaceOverlay overlay;
  ASSERT_TRUE(overlay.Initialize({800, 600}).ok());
  RobloxTextDisplaySink sink = overlay.sink();

  const std::string text = "hidden geometry";
  RobloxTextDisplayUpdate show = Show(9, text, 4);
  show.area_width = 0;
  show.area_height = 0;
  sink.update(sink.context, show);

  EXPECT_FALSE(mocktail_text_overlay_may_present());
  MocktailTextOverlayFrameInfo frame;
  ASSERT_TRUE(overlay.QueryFrame(&frame));
  EXPECT_EQ(frame.visible, 0U);
  EXPECT_TRUE(overlay.Shutdown().ok());
}

TEST(RobloxTextDisplayStateTest,
     InactiveOverlayCannotClearAnotherOverlayPublication) {
  RobloxTextSurfaceOverlay active;
  ASSERT_TRUE(active.Initialize({800, 600}).ok());
  RobloxTextDisplaySink sink = active.sink();
  const std::string text = "active";
  sink.update(sink.context, Show(11, text, 6));
  ASSERT_TRUE(mocktail_text_overlay_may_present());

  RobloxTextSurfaceOverlay rejected;
  EXPECT_FALSE(rejected.Initialize({800, 600}).ok());
  EXPECT_TRUE(rejected.Shutdown().ok());
  EXPECT_TRUE(mocktail_text_overlay_may_present());

  EXPECT_TRUE(active.Shutdown().ok());
  EXPECT_FALSE(mocktail_text_overlay_may_present());
}

TEST(RobloxTextDisplayStateTest, MatchesApkHorizontalGravityEnum) {
  RobloxTextSurfaceOverlay overlay;
  ASSERT_TRUE(overlay.Initialize({800, 600}).ok());
  RobloxTextDisplaySink sink = overlay.sink();
  const std::string text = "M";

  const auto render = [&](uint64_t generation, int32_t alignment) {
    RobloxTextDisplayUpdate show = Show(generation, text, 0);
    show.area_width = 320;
    show.area_height = 64;
    show.font_size = 28.0F;
    show.x_alignment = alignment;
    show.text_color = static_cast<int32_t>(0xFFFFFFFFU);
    sink.update(sink.context, show);

    MocktailTextOverlayFrameInfo frame;
    EXPECT_TRUE(overlay.QueryFrame(&frame));
    std::vector<std::uint8_t> rgba(frame.rgba_bytes);
    EXPECT_TRUE(overlay.CopyFrame(frame.revision, rgba.data(), rgba.size()));
    return FindAlphaBounds(frame, rgba);
  };

  const AlphaBounds left = render(1, 0);
  const AlphaBounds right = render(2, 1);
  const AlphaBounds center = render(3, 2);
  ASSERT_TRUE(left.valid());
  ASSERT_TRUE(center.valid());
  ASSERT_TRUE(right.valid());
  EXPECT_LT(left.minimum_x, center.minimum_x);
  EXPECT_LT(center.minimum_x, right.minimum_x);
  EXPECT_GT(center.minimum_x - left.minimum_x, 40);
  EXPECT_GT(right.minimum_x - center.minimum_x, 40);
  EXPECT_TRUE(overlay.Shutdown().ok());
}

TEST(RobloxTextDisplayStateTest, KeepsBackgroundTransparentAndArgbCaretAlpha) {
  RobloxTextSurfaceOverlay overlay;
  ASSERT_TRUE(overlay.Initialize({800, 600}).ok());
  const std::string text = "M";
  RobloxTextDisplayUpdate show = Show(1, text, 1);
  show.area_height = 64;
  show.font_size = 28.0F;
  show.text_color = static_cast<int32_t>(0x80FF0000U);
  RobloxTextDisplaySink sink = overlay.sink();
  sink.update(sink.context, show);

  MocktailTextOverlayFrameInfo frame;
  ASSERT_TRUE(overlay.QueryFrame(&frame));
  std::vector<std::uint8_t> rgba(frame.rgba_bytes);
  ASSERT_TRUE(overlay.CopyFrame(frame.revision, rgba.data(), rgba.size()));
  ASSERT_GE(rgba.size(), 4U);
  EXPECT_EQ(rgba[3], 0U);
  bool found_argb_pixel = false;
  std::uint8_t maximum_alpha = 0;
  for (std::size_t offset = 0; offset + 3 < rgba.size(); offset += 4) {
    maximum_alpha = std::max(maximum_alpha, rgba[offset + 3]);
    found_argb_pixel = found_argb_pixel ||
                       (rgba[offset] == 255U && rgba[offset + 1] == 0U &&
                        rgba[offset + 2] == 0U && rgba[offset + 3] == 128U);
  }
  EXPECT_LE(maximum_alpha, 128U);
  EXPECT_TRUE(found_argb_pixel);
  EXPECT_TRUE(overlay.Shutdown().ok());
}

TEST(RobloxTextDisplayStateTest, RasterizesSelectionHighlightBehindText) {
  RobloxTextSurfaceOverlay overlay;
  ASSERT_TRUE(overlay.Initialize({800, 600}).ok());
  const std::string text = "selected text";
  RobloxTextDisplayUpdate show = Show(1, text, 13);
  show.area_height = 64;
  show.font_size = 28.0F;
  show.selection_begin_utf16 = 0;
  show.selection_end_utf16 = 8;
  RobloxTextDisplaySink sink = overlay.sink();
  sink.update(sink.context, show);

  MocktailTextOverlayFrameInfo frame;
  ASSERT_TRUE(overlay.QueryFrame(&frame));
  std::vector<std::uint8_t> rgba(frame.rgba_bytes);
  ASSERT_TRUE(overlay.CopyFrame(frame.revision, rgba.data(), rgba.size()));
  bool found_highlight = false;
  for (std::size_t offset = 0; offset + 3 < rgba.size(); offset += 4) {
    found_highlight =
        found_highlight ||
        (rgba[offset] == 65U && rgba[offset + 1] == 132U &&
         rgba[offset + 2] == 228U && rgba[offset + 3] == 112U);
  }
  EXPECT_TRUE(found_highlight);
  EXPECT_TRUE(overlay.Shutdown().ok());
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
