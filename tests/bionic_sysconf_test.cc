#include "compat/bionic_sysconf.h"

#include <errno.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace mocktail::compat {
namespace {

TEST(BionicSysconfTest, TranslatesPageSizeAliases) {
  const long host_page_size = ::sysconf(_SC_PAGESIZE);

  EXPECT_EQ(BionicSysconf(static_cast<int>(BionicSysconfName::kPageSize)),
            host_page_size);
  EXPECT_EQ(BionicSysconf(static_cast<int>(BionicSysconfName::kPageSizeAlias)),
            host_page_size);
  EXPECT_GT(host_page_size, 0);
  EXPECT_EQ(host_page_size & (host_page_size - 1), 0);
}

TEST(BionicSysconfTest, TranslatesObservedCapacityNames) {
  EXPECT_EQ(BionicSysconf(static_cast<int>(BionicSysconfName::kClockTicks)),
            100);
  EXPECT_EQ(BionicSysconf(static_cast<int>(BionicSysconfName::kIovMax)),
            ::sysconf(_SC_IOV_MAX));
  EXPECT_EQ(
      BionicSysconf(static_cast<int>(BionicSysconfName::kProcessorsConfigured)),
      ::sysconf(_SC_NPROCESSORS_CONF));
  EXPECT_EQ(
      BionicSysconf(static_cast<int>(BionicSysconfName::kProcessorsOnline)),
      ::sysconf(_SC_NPROCESSORS_ONLN));
  EXPECT_EQ(BionicSysconf(static_cast<int>(BionicSysconfName::kPhysicalPages)),
            ::sysconf(_SC_PHYS_PAGES));
}

TEST(BionicSysconfTest, RejectsUnknownBionicName) {
  errno = 0;

  EXPECT_EQ(BionicSysconf(0x7fff), -1);
  EXPECT_EQ(errno, EINVAL);
}

} // namespace
} // namespace mocktail::compat
