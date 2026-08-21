#include "window/input_pump_pacer.h"

#include <gtest/gtest.h>

namespace mocktail {
namespace window {
namespace {

TEST(InputPumpPacerTest, MaintainsCadenceWithoutAddingWorkTime) {
  constexpr uint64_t kInterval = 4000000;
  InputPumpPacer pacer(kInterval);

  EXPECT_EQ(pacer.DelayBeforeNextPump(1000000), kInterval);
  EXPECT_EQ(pacer.DelayBeforeNextPump(6000000), kInterval - 1000000);
  EXPECT_EQ(pacer.DelayBeforeNextPump(9000000), kInterval);
}

TEST(InputPumpPacerTest, ExactDeadlineDoesNotInsertAnotherFrameDelay) {
  constexpr uint64_t kInterval = 4000000;
  InputPumpPacer pacer(kInterval);

  EXPECT_EQ(pacer.DelayBeforeNextPump(1000000), kInterval);
  EXPECT_EQ(pacer.DelayBeforeNextPump(9000000), 0U);
  EXPECT_EQ(pacer.DelayBeforeNextPump(9000000), kInterval);
}

TEST(InputPumpPacerTest, MissedDeadlineRebasesInsteadOfBursting) {
  constexpr uint64_t kInterval = 4000000;
  InputPumpPacer pacer(kInterval);

  EXPECT_EQ(pacer.DelayBeforeNextPump(1000000), kInterval);
  EXPECT_EQ(pacer.DelayBeforeNextPump(12000000), kInterval);
  EXPECT_EQ(pacer.DelayBeforeNextPump(17000000), kInterval - 1000000);
}

TEST(InputPumpPacerTest, ResetStartsAFreshCadence) {
  constexpr uint64_t kInterval = 4000000;
  InputPumpPacer pacer(kInterval);

  EXPECT_EQ(pacer.DelayBeforeNextPump(1000000), kInterval);
  pacer.Reset();
  EXPECT_EQ(pacer.DelayBeforeNextPump(100000000), kInterval);
}

}  // namespace
}  // namespace window
}  // namespace mocktail
