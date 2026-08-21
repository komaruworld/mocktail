#include <gtest/gtest.h>

#include <cstdlib>

struct ANativeWindow;

extern "C" ANativeWindow* ANativeWindow_fromSurface(void* env, void* surface);

namespace {

TEST(AndroidNativeWindowTest, DirectVulkanPreservesJavaSurfaceIdentity) {
  ASSERT_EQ(setenv("MOCKTAIL_GRAPHICS_BACKEND", "direct-vulkan", 1), 0);
  void* first_surface = reinterpret_cast<void*>(0x1000);
  void* second_surface = reinterpret_cast<void*>(0x2000);

  EXPECT_EQ(reinterpret_cast<void*>(
                ANativeWindow_fromSurface(nullptr, first_surface)),
            first_surface);
  EXPECT_EQ(reinterpret_cast<void*>(
                ANativeWindow_fromSurface(nullptr, second_surface)),
            second_surface);

  unsetenv("MOCKTAIL_GRAPHICS_BACKEND");
}

TEST(AndroidNativeWindowTest, NonVulkanFallbackKeepsStableWindowIdentity) {
  unsetenv("MOCKTAIL_GRAPHICS_BACKEND");
  ANativeWindow* first =
      ANativeWindow_fromSurface(nullptr, reinterpret_cast<void*>(0x1000));
  ANativeWindow* second =
      ANativeWindow_fromSurface(nullptr, reinterpret_cast<void*>(0x2000));

  EXPECT_NE(first, nullptr);
  EXPECT_EQ(first, second);
}

}  // namespace
