#include "compat/bionic_signal_runtime.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace mocktail::compat {
namespace {

void TestHandler(int) {}

TEST(BionicSignalRuntimeTest, TranslatesLp64LayoutWithoutOverwritingCaller) {
  struct sigaction original {};
  ASSERT_EQ(sigaction(SIGUSR2, nullptr, &original), 0);

  BionicSigaction requested;
  requested.callback.handler = TestHandler;
  requested.flags = SA_RESTART;
  requested.mask = std::uint64_t{1} << (SIGTERM - 1);

  struct GuardedAction {
    BionicSigaction action;
    std::array<std::uint8_t, 64> canary;
  } observed{};
  observed.canary.fill(0xa5);

  ASSERT_EQ(BionicSigactionCall(SIGUSR2, &requested, nullptr), 0);
  ASSERT_EQ(BionicSigactionCall(SIGUSR2, nullptr, &observed.action), 0);
  EXPECT_EQ(observed.action.callback.handler, TestHandler);
  EXPECT_NE(observed.action.flags & SA_RESTART, 0);
  EXPECT_NE(observed.action.mask & (std::uint64_t{1} << (SIGTERM - 1)), 0U);
  for (std::uint8_t byte : observed.canary) {
    EXPECT_EQ(byte, 0xa5);
  }

  ASSERT_EQ(sigaction(SIGUSR2, &original, nullptr), 0);
}

}  // namespace
}  // namespace mocktail::compat
