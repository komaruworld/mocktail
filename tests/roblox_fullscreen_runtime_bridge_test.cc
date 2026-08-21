#include "runtime/roblox_fullscreen_runtime_bridge_internal.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace mocktail {
namespace runtime {
namespace {

std::array<std::uint8_t, 96> ValidSetterContract() {
  std::array<std::uint8_t, 96> code = {};
  code[0] = 0x55;
  code[1] = 0x48;
  code[2] = 0x89;
  code[3] = 0xe5;
  code[11] = 0x89;
  code[12] = 0xf3;
  const std::array<std::uint8_t, 6> compare = {
      0x38, 0x98, 0x59, 0x01, 0x00, 0x00};
  const std::array<std::uint8_t, 6> store = {
      0x88, 0x98, 0x59, 0x01, 0x00, 0x00};
  for (std::size_t index = 0; index < compare.size(); ++index) {
    code[35 + index] = compare[index];
    code[43 + index] = store[index];
  }
  return code;
}

TEST(RobloxFullscreenRuntimeBridgeTest, AcceptsExpectedSetterSemantics) {
  const auto code = ValidSetterContract();

  EXPECT_TRUE(internal::HasExpectedFullscreenSetterContract(code.data(),
                                                            code.size()));
}

TEST(RobloxFullscreenRuntimeBridgeTest, RejectsDifferentSettingsField) {
  auto code = ValidSetterContract();
  code[37] = 0x65;

  EXPECT_FALSE(internal::HasExpectedFullscreenSetterContract(code.data(),
                                                             code.size()));
}

TEST(RobloxFullscreenRuntimeBridgeTest, RejectsTruncatedFunction) {
  const auto code = ValidSetterContract();

  EXPECT_FALSE(
      internal::HasExpectedFullscreenSetterContract(code.data(), 64));
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
