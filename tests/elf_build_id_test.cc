#include "compat/elf_build_id.h"

#include <gtest/gtest.h>

#include <string>

namespace mocktail::compat {
namespace {

TEST(ElfBuildIdTest, ReadsBuildIdFromCurrentExecutable) {
  const BuildIdResult result = ReadElfBuildId("/proc/self/exe");

  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(IsValidBuildId(result.build_id));
}

TEST(ElfBuildIdTest, RejectsNonElfInput) {
  const BuildIdResult result = ReadElfBuildId("/dev/null");

  EXPECT_FALSE(result);
  EXPECT_FALSE(result.error.empty());
}

TEST(ElfBuildIdTest, ValidatesHexEncoding) {
  EXPECT_TRUE(IsValidBuildId("50e1b0abd123350e"));
  EXPECT_TRUE(IsValidBuildId("D0CB1FA0DEB3D916"));
  EXPECT_FALSE(IsValidBuildId(""));
  EXPECT_FALSE(IsValidBuildId("not-a-build-id"));
  EXPECT_FALSE(IsValidBuildId("abc"));
}

}  // namespace
}  // namespace mocktail::compat
