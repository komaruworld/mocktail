#include <dlfcn.h>

#include <string>

#include <gtest/gtest.h>

#include "mocktail/graphics/bionic_egl_bridge.h"

namespace mocktail::graphics {
namespace {

TEST(BionicEglBridgeTest, LoadsExactAdapterBesideExecutable) {
  BionicEglBridge bridge;
  ASSERT_TRUE(bridge.Load()) << bridge.error();
  ASSERT_EQ(bridge.exports().size(), 21u);

  const auto create_surface = bridge.exports().find("eglCreateWindowSurface");
  ASSERT_NE(create_surface, bridge.exports().end());

  Dl_info origin = {};
  ASSERT_NE(::dladdr(create_surface->second, &origin), 0);
  ASSERT_NE(origin.dli_fname, nullptr);
  EXPECT_EQ(std::string(origin.dli_fname), bridge.library_path());
}

} // namespace
} // namespace mocktail::graphics
