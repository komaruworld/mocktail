#include "compat/bionic_dns_runtime.h"

#include <gtest/gtest.h>
#include <netdb.h>
#include <sys/socket.h>

namespace mocktail {
namespace compat {
namespace {

TEST(BionicDnsRuntimeTest, BlocksOnlyCrashUploadInfrastructure) {
  EXPECT_TRUE(IsBlockedCrashReportUploadHost("upload.crashes.rbxinfra.com"));
  EXPECT_TRUE(IsBlockedCrashReportUploadHost("UPLOAD.CRASHES.RBXINFRA.COM."));
  EXPECT_TRUE(IsBlockedCrashReportUploadHost("uploads.backtrace.rbx.com"));
  EXPECT_TRUE(IsBlockedCrashReportUploadHost("tenant.sp.backtrace.io"));
  EXPECT_TRUE(IsBlockedCrashReportUploadHost("backtrace.io"));

  EXPECT_FALSE(IsBlockedCrashReportUploadHost("auth.roblox.com"));
  EXPECT_FALSE(IsBlockedCrashReportUploadHost("assetdelivery.roblox.com"));
  EXPECT_FALSE(IsBlockedCrashReportUploadHost("apis.roblox.com"));
  EXPECT_FALSE(IsBlockedCrashReportUploadHost("notcrashes.rbxinfra.com"));
  EXPECT_FALSE(IsBlockedCrashReportUploadHost(
      "upload.crashes.rbxinfra.com.example.org"));
}

TEST(BionicDnsRuntimeTest, RejectsCrashHostBeforeAddressResolution) {
  BionicAddressInfo* result =
      reinterpret_cast<BionicAddressInfo*>(uintptr_t{1});

  EXPECT_EQ(BionicGetAddressInfo("upload.crashes.rbxinfra.com", "443", nullptr,
                                 &result),
            kBionicAddressInfoNameNotFound);
  EXPECT_EQ(result, nullptr);

  h_errno = 0;
  EXPECT_EQ(BionicGetHostByName("uploads.backtrace.rbx.com"), nullptr);
  EXPECT_EQ(h_errno, HOST_NOT_FOUND);
}

TEST(BionicDnsRuntimeTest, PreservesOrdinaryLocalDnsAndBionicLayout) {
  BionicAddressInfo hints;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  BionicAddressInfo* result = nullptr;

  ASSERT_EQ(BionicGetAddressInfo("localhost", "80", &hints, &result), 0);
  ASSERT_NE(result, nullptr);
  EXPECT_NE(result->ai_addr, nullptr);
  EXPECT_GT(result->ai_addrlen, 0U);
  EXPECT_EQ(result->ai_socktype, SOCK_STREAM);
  BionicFreeAddressInfo(result);

  EXPECT_NE(BionicGetHostByName("localhost"), nullptr);
}

TEST(BionicDnsRuntimeTest, RejectsMissingResultStorage) {
  EXPECT_NE(BionicGetAddressInfo("localhost", "80", nullptr, nullptr), 0);
  BionicFreeAddressInfo(nullptr);
}

}  // namespace
}  // namespace compat
}  // namespace mocktail
