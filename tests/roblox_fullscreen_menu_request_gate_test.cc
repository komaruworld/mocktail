#include "window/roblox_fullscreen_menu_request_gate.h"

#include <gtest/gtest.h>

namespace mocktail {
namespace window {
namespace {

constexpr char kFullscreenCycle[] =
    "Maximum event re-entrancy depth exceeded for BindableEvent.Event when "
    "calling anonymous function on line 486 in "
    "CoreGui.RobloxGui.Modules.Settings.Pages.GameSettings";

TEST(RobloxFullscreenMenuRequestGateTest, IgnoresUnrelatedErrors) {
  RobloxFullscreenMenuRequestGate gate;

  EXPECT_FALSE(gate.RequestFromAndroidLog("Roblox", "network error", 1));
  EXPECT_FALSE(gate.RequestFromAndroidLog("Other", kFullscreenCycle, 2));
  EXPECT_FALSE(gate.Take(1'000'000'000ULL));
}

TEST(RobloxFullscreenMenuRequestGateTest, CoalescesRecursiveBurstAfterQuiet) {
  RobloxFullscreenMenuRequestGate gate;

  EXPECT_TRUE(gate.RequestFromAndroidLog("Roblox", kFullscreenCycle,
                                         1'000'000'000ULL));
  EXPECT_TRUE(gate.RequestFromAndroidLog("Roblox", kFullscreenCycle,
                                         1'005'000'000ULL));
  EXPECT_FALSE(gate.Take(1'024'000'000ULL));
  EXPECT_TRUE(gate.Take(1'025'000'000ULL));
  EXPECT_FALSE(gate.Take(1'500'000'000ULL));
}

TEST(RobloxFullscreenMenuRequestGateTest, SuppressesTrailingCycleMessages) {
  RobloxFullscreenMenuRequestGate gate;

  ASSERT_TRUE(gate.RequestFromAndroidLog("Roblox", kFullscreenCycle,
                                         1'000'000'000ULL));
  ASSERT_TRUE(gate.Take(1'020'000'000ULL));
  EXPECT_TRUE(gate.RequestFromAndroidLog("Roblox", kFullscreenCycle,
                                         1'100'000'000ULL));
  EXPECT_FALSE(gate.Take(1'500'000'000ULL));

  EXPECT_TRUE(gate.RequestFromAndroidLog("Roblox", kFullscreenCycle,
                                         1'500'000'000ULL));
  EXPECT_TRUE(gate.Take(1'520'000'000ULL));
}

}  // namespace
}  // namespace window
}  // namespace mocktail
