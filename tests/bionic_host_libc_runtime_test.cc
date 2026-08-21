#include "compat/bionic_host_libc_runtime.h"

#include <gtest/gtest.h>
#include <limits.h>
#include <locale.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>

#include "libc_shim/libc_shim.h"

namespace {

constexpr size_t AlignControlMessage(size_t size) {
  return (size + sizeof(long) - 1U) & ~(sizeof(long) - 1U);
}

constexpr size_t ControlMessageLength(size_t payload_size) {
  return AlignControlMessage(
             sizeof(mocktail::compat::BionicControlMessageHeader)) +
         payload_size;
}

constexpr size_t ControlMessageSpace(size_t payload_size) {
  return AlignControlMessage(
             sizeof(mocktail::compat::BionicControlMessageHeader)) +
         AlignControlMessage(payload_size);
}

struct DestructorProbe {
  std::atomic<int> calls{0};
};

void RecordThreadDestructor(void *argument) {
  auto *probe = static_cast<DestructorProbe *>(argument);
  probe->calls.fetch_add(1, std::memory_order_relaxed);
}

TEST(BionicHostLibcRuntimeTest, WalksValidControlMessages) {
  constexpr size_t kPayloadSize = sizeof(int);
  using Header = mocktail::compat::BionicControlMessageHeader;
  alignas(Header)
      std::array<unsigned char, 2 * ControlMessageSpace(kPayloadSize)>
          control{};
  mocktail::compat::BionicMessageHeader message{};
  message.msg_control = control.data();
  message.msg_controllen = control.size();

  auto *first = reinterpret_cast<Header *>(control.data());
  first->cmsg_len = ControlMessageLength(kPayloadSize);
  auto *second = reinterpret_cast<Header *>(control.data() +
                                            ControlMessageSpace(kPayloadSize));
  second->cmsg_len = ControlMessageLength(kPayloadSize);

  EXPECT_EQ(mocktail_bionic_cmsg_nxthdr(&message, first), second);
  EXPECT_EQ(mocktail_bionic_cmsg_nxthdr(&message, second), nullptr);
}

TEST(BionicHostLibcRuntimeTest, RejectsMalformedControlMessages) {
  using Header = mocktail::compat::BionicControlMessageHeader;
  alignas(Header) std::array<unsigned char, 2 * sizeof(Header)> control{};
  mocktail::compat::BionicMessageHeader message{};
  message.msg_control = control.data();
  message.msg_controllen = control.size();
  auto *header = reinterpret_cast<Header *>(control.data());

  header->cmsg_len = sizeof(Header) - 1;
  EXPECT_EQ(mocktail_bionic_cmsg_nxthdr(&message, header), nullptr);

  header->cmsg_len = std::numeric_limits<size_t>::max();
  EXPECT_EQ(mocktail_bionic_cmsg_nxthdr(&message, header), nullptr);

  header->cmsg_len = sizeof(Header);
  message.msg_controllen = sizeof(Header);
  EXPECT_EQ(mocktail_bionic_cmsg_nxthdr(&message, header), nullptr);
}

TEST(BionicHostLibcRuntimeTest, RunsThreadDestructorAtThreadExit) {
  DestructorProbe probe;
  int result = -1;
  std::thread worker([&probe, &result]() {
    result = mocktail_bionic_cxa_thread_atexit_impl(RecordThreadDestructor,
                                                    &probe, &probe);
    EXPECT_EQ(probe.calls.load(std::memory_order_relaxed), 0);
  });
  worker.join();

  EXPECT_EQ(result, 0);
  EXPECT_EQ(probe.calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(mocktail_bionic_cxa_thread_atexit_impl(nullptr, nullptr, nullptr),
            EINVAL);
}

TEST(BionicHostLibcRuntimeTest, FillsEveryRequestedRandomByte) {
  std::array<unsigned char, 64> first;
  std::array<unsigned char, 64> second;
  first.fill(0xa5);
  second.fill(0xa5);

  mocktail_bionic_arc4random_buf(first.data(), first.size());
  mocktail_bionic_arc4random_buf(second.data(), second.size());
  mocktail_bionic_arc4random_buf(nullptr, 0);

  EXPECT_FALSE(std::all_of(first.begin(), first.end(),
                           [](unsigned char value) { return value == 0xa5; }));
  EXPECT_NE(first, second);
}

TEST(BionicHostLibcRuntimeTest, ReturnsAbiSizedAllocatorSnapshot) {
  static_assert(sizeof(mocktail::compat::BionicMallinfoSnapshot) == 80);
  const auto snapshot = mocktail_bionic_mallinfo();
  if (!mocktail::compat::BionicMallinfoHasHostTelemetry()) {
    EXPECT_EQ(snapshot.arena, 0U);
    EXPECT_EQ(snapshot.ordblks, 0U);
    EXPECT_EQ(snapshot.smblks, 0U);
    EXPECT_EQ(snapshot.hblks, 0U);
    EXPECT_EQ(snapshot.hblkhd, 0U);
    EXPECT_EQ(snapshot.usmblks, 0U);
    EXPECT_EQ(snapshot.fsmblks, 0U);
    EXPECT_EQ(snapshot.uordblks, 0U);
    EXPECT_EQ(snapshot.fordblks, 0U);
    EXPECT_EQ(snapshot.keepcost, 0U);
  }
}

TEST(BionicHostLibcRuntimeTest, PreservesValidSysInfoCounters) {
  struct sysinfo info{};
  info.totalram = 8UL * 1024UL * 1024UL * 1024UL;
  info.freeram = 2UL * 1024UL * 1024UL * 1024UL;
  info.totalswap = 4UL * 1024UL * 1024UL * 1024UL;
  info.freeswap = 1UL * 1024UL * 1024UL * 1024UL;

  mocktail::compat::NormalizeBionicSysInfo(&info);

  EXPECT_EQ(info.totalram, 8UL * 1024UL * 1024UL * 1024UL);
  EXPECT_EQ(info.freeram, 2UL * 1024UL * 1024UL * 1024UL);
  EXPECT_EQ(info.totalswap, 4UL * 1024UL * 1024UL * 1024UL);
  EXPECT_EQ(info.freeswap, 1UL * 1024UL * 1024UL * 1024UL);
}

TEST(BionicHostLibcRuntimeTest, ClearsLinuxulatorUnderflowedSwapCounters) {
  struct sysinfo info{};
  info.totalram = 8UL * 1024UL * 1024UL * 1024UL;
  info.freeram = 2UL * 1024UL * 1024UL * 1024UL;
  info.totalswap = std::numeric_limits<unsigned long>::max() - 40959UL;
  info.freeswap = std::numeric_limits<unsigned long>::max() - 11550719UL;

  mocktail::compat::NormalizeBionicSysInfo(&info);

  EXPECT_EQ(info.totalram, 8UL * 1024UL * 1024UL * 1024UL);
  EXPECT_EQ(info.freeram, 2UL * 1024UL * 1024UL * 1024UL);
  EXPECT_EQ(info.totalswap, 0UL);
  EXPECT_EQ(info.freeswap, 0UL);
}

TEST(BionicHostLibcRuntimeTest, CallsHostSysInfoThroughBionicAdapter) {
  struct sysinfo info{};
  ASSERT_EQ(mocktail_bionic_sysinfo(&info), 0);
  EXPECT_GT(info.totalram, 0UL);
  EXPECT_LE(info.freeswap, info.totalswap);
}

TEST(BionicHostLibcRuntimeTest, NormalizesContradictoryLinuxulatorUname) {
  struct utsname name{};
  std::snprintf(name.sysname, sizeof(name.sysname), "Linux");
  std::snprintf(name.release, sizeof(name.release), "5.15.0");
  std::snprintf(name.version, sizeof(name.version),
                "FreeBSD 15.1-RELEASE-p2 GENERIC");

  EXPECT_TRUE(mocktail::compat::NormalizeLinuxulatorUnameVersion(
      &name, "#4 SMP PREEMPT_DYNAMIC"));
  EXPECT_STREQ(name.sysname, "Linux");
  EXPECT_STREQ(name.release, "5.15.0");
  EXPECT_STREQ(name.version, "#4 SMP PREEMPT_DYNAMIC");
}

TEST(BionicHostLibcRuntimeTest, PreservesNativeLinuxUname) {
  struct utsname name{};
  std::snprintf(name.sysname, sizeof(name.sysname), "Linux");
  std::snprintf(name.version, sizeof(name.version), "#1 SMP PREEMPT_DYNAMIC");

  EXPECT_FALSE(mocktail::compat::NormalizeLinuxulatorUnameVersion(
      &name, "#4 SMP PREEMPT_DYNAMIC"));
  EXPECT_STREQ(name.version, "#1 SMP PREEMPT_DYNAMIC");

  ASSERT_EQ(mocktail_bionic_uname(&name), 0);
  EXPECT_STREQ(name.sysname, "Linux");
}

TEST(BionicHostLibcRuntimeTest, HidesPrivilegedHostIdentityFromAndroidGuest) {
  EXPECT_EQ(mocktail::compat::NormalizeBionicApplicationUid(0), 1000U);
  EXPECT_EQ(mocktail::compat::NormalizeBionicApplicationUid(1000), 1000U);
  EXPECT_EQ(mocktail::compat::NormalizeBionicApplicationUid(12345), 12345U);
  EXPECT_EQ(mocktail_bionic_getuid(),
            mocktail::compat::NormalizeBionicApplicationUid(::getuid()));
  EXPECT_EQ(mocktail_bionic_geteuid(),
            mocktail::compat::NormalizeBionicApplicationUid(::geteuid()));
}

TEST(BionicHostLibcRuntimeTest, UsesBionicPosixStrErrorContract) {
  std::array<char, 128> buffer{};

  errno = EDOM;
  EXPECT_EQ(mocktail_bionic_strerror_r(EINVAL, buffer.data(), buffer.size()),
            0);
  EXPECT_NE(std::strstr(buffer.data(), "Invalid"), nullptr);
  EXPECT_EQ(errno, EDOM);

  buffer.fill('\0');
  EXPECT_EQ(mocktail_bionic_strerror_r(4567, buffer.data(), buffer.size()), 0);
  EXPECT_NE(std::strstr(buffer.data(), "4567"), nullptr);

  std::array<char, 2> truncated{};
  errno = 0;
  EXPECT_EQ(
      mocktail_bionic_strerror_r(4567, truncated.data(), truncated.size()), -1);
  EXPECT_EQ(truncated[1], '\0');
  EXPECT_EQ(errno, ERANGE);

  errno = 0;
  EXPECT_EQ(mocktail_bionic_strerror_r(EINVAL, nullptr, 0), -1);
  EXPECT_EQ(errno, ERANGE);
}

TEST(BionicHostLibcRuntimeTest, ParsesIntegersInScopedCLocale) {
  const locale_t original_locale = uselocale(nullptr);
  auto guest_locale =
      reinterpret_cast<mocktail::compat::BionicLocale>(uintptr_t{1});

  char *signed_end = nullptr;
  errno = EDOM;
  EXPECT_EQ(
      mocktail_bionic_strtoll_l("-0x7f-rest", &signed_end, 0, guest_locale),
      -127);
  EXPECT_STREQ(signed_end, "-rest");
  EXPECT_EQ(errno, EDOM);

  char *unsigned_end = nullptr;
  errno = 0;
  EXPECT_EQ(mocktail_bionic_strtoull_l("18446744073709551616!", &unsigned_end,
                                       10, guest_locale),
            ULLONG_MAX);
  EXPECT_EQ(errno, ERANGE);
  EXPECT_STREQ(unsigned_end, "!");
  EXPECT_EQ(uselocale(nullptr), original_locale);
}

TEST(BionicHostLibcRuntimeTest, FortifiedReadlinkAcceptsInBoundsRequest) {
  std::array<char, PATH_MAX> executable{};
  const ssize_t length =
      mocktail___readlink_chk("/proc/self/exe", executable.data(),
                              executable.size(), executable.size());
  ASSERT_GT(length, 0);
  EXPECT_LT(static_cast<size_t>(length), executable.size());
}

TEST(BionicHostLibcRuntimeTest, FortifiedReadlinkAbortsOnBufferOverflow) {
  EXPECT_DEATH(
      {
        char buffer[4]{};
        mocktail___readlink_chk("/proc/self/exe", buffer, 5, sizeof(buffer));
      },
      "FORTIFY: readlink");
}

TEST(BionicHostLibcRuntimeTest, FortifiedReadlinkAbortsAboveSsizeLimit) {
  EXPECT_DEATH(
      {
        char buffer{};
        mocktail___readlink_chk("/proc/self/exe", &buffer,
                                static_cast<size_t>(SSIZE_MAX) + size_t{1},
                                std::numeric_limits<size_t>::max());
      },
      "FORTIFY: readlink");
}

} // namespace
