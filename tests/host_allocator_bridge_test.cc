#include "compat/host_allocator_bridge.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

namespace mocktail::compat {
namespace {

TEST(HostAllocatorBridgeTest, TracksAlignedOwnedAllocations) {
  void* pointer = HostAlignedAllocate(48, 64);
  ASSERT_NE(pointer, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(pointer) % 64, 0U);
  EXPECT_EQ(HostUsableSize(pointer), 48U);
  HostFree(pointer);
  EXPECT_EQ(HostUsableSize(pointer), 0U);
}

TEST(HostAllocatorBridgeTest, ReallocatesOwnedDataWithoutInspectingUnknowns) {
  auto* pointer = static_cast<unsigned char*>(HostAllocate(8));
  ASSERT_NE(pointer, nullptr);
  std::memcpy(pointer, "mocktail", 8);
  auto* replacement =
      static_cast<unsigned char*>(HostReallocate(pointer, 32));
  ASSERT_NE(replacement, nullptr);
  EXPECT_EQ(std::memcmp(replacement, "mocktail", 8), 0);
  HostFree(replacement);

  int native_value = 7;
  EXPECT_EQ(HostUsableSize(&native_value), 0U);
  HostFree(&native_value);
  EXPECT_EQ(native_value, 7);
}

}  // namespace
}  // namespace mocktail::compat
