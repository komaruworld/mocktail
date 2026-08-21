#include "linker/linker.h"

#include "compat/bionic_atfork_runtime.h"
#include "compat/bionic_dns_runtime.h"
#include "compat/bionic_host_libc_runtime.h"
#include "compat/bionic_large_file_runtime.h"
#include "compat/bionic_pthread_key_runtime.h"
#include "compat/bionic_rwlock_runtime.h"
#include "compat/bionic_semaphore_runtime.h"
#include "compat/bionic_stdio_runtime.h"
#include "compat/bionic_sysconf.h"
#include "libc_shim/libc_shim.h"

#include <gtest/gtest.h>

namespace {

TEST(LinkerRegistryTest, KeepsSyntheticExportsOwnedBySoname) {
  int android_export = 1;
  int log_export = 2;
  int replacement_export = 3;

  linker::RegisterSyntheticSymbol("libmocktail_test_android.so", "sharedName",
                                  &android_export);
  linker::RegisterSyntheticSymbol("libmocktail_test_log.so", "sharedName",
                                  &log_export);

  linker::SymbolMap android_symbols =
      linker::GetSyntheticLibrarySymbols("libmocktail_test_android.so");
  linker::SymbolMap log_symbols =
      linker::GetSyntheticLibrarySymbols("libmocktail_test_log.so");

  ASSERT_EQ(android_symbols.size(), 1U);
  ASSERT_EQ(log_symbols.size(), 1U);
  EXPECT_EQ(android_symbols.at("sharedName"), &android_export);
  EXPECT_EQ(log_symbols.at("sharedName"), &log_export);

  linker::RegisterSyntheticSymbol("libmocktail_test_android.so", "sharedName",
                                  &replacement_export);
  EXPECT_EQ(android_symbols.at("sharedName"), &android_export);
  EXPECT_EQ(linker::GetSyntheticLibrarySymbols("libmocktail_test_android.so")
                .at("sharedName"),
            &replacement_export);
  EXPECT_EQ(linker::GetSyntheticLibrarySymbols("libmocktail_test_log.so")
                .at("sharedName"),
            &log_export);
}

TEST(LinkerRegistryTest, ExposesBionicProgramHeaderIterator) {
  EXPECT_NE(linker::BionicDlIteratePhdrAddress(), nullptr);
}

TEST(LinkerRegistryTest, KeepsBionicPthreadKeyAbiOwnedByLibc) {
  linker::RegisterBionicPthreadKeyRuntimeForLibc();

  const linker::SymbolMap symbols =
      linker::GetSyntheticLibrarySymbols("libc.so");
  EXPECT_EQ(symbols.at("pthread_key_create"),
            reinterpret_cast<void *>(mocktail_bionic_pthread_key_create));
  EXPECT_EQ(symbols.at("pthread_key_delete"),
            reinterpret_cast<void *>(mocktail_bionic_pthread_key_delete));
  EXPECT_EQ(symbols.at("pthread_getspecific"),
            reinterpret_cast<void *>(mocktail_bionic_pthread_getspecific));
  EXPECT_EQ(symbols.at("pthread_setspecific"),
            reinterpret_cast<void *>(mocktail_bionic_pthread_setspecific));
  EXPECT_EQ(symbols.at("sem_init"),
            reinterpret_cast<void *>(mocktail_bionic_sem_init));
  EXPECT_EQ(symbols.at("sem_destroy"),
            reinterpret_cast<void *>(mocktail_bionic_sem_destroy));
  EXPECT_EQ(symbols.at("sem_wait"),
            reinterpret_cast<void *>(mocktail_bionic_sem_wait));
  EXPECT_EQ(symbols.at("sem_post"),
            reinterpret_cast<void *>(mocktail_bionic_sem_post));
  EXPECT_EQ(symbols.at("pthread_rwlock_init"),
            reinterpret_cast<void *>(mocktail_bionic_pthread_rwlock_init));
  EXPECT_EQ(symbols.at("pthread_rwlock_destroy"),
            reinterpret_cast<void *>(mocktail_bionic_pthread_rwlock_destroy));
  EXPECT_EQ(symbols.at("pthread_rwlock_rdlock"),
            reinterpret_cast<void *>(mocktail_bionic_pthread_rwlock_rdlock));
  EXPECT_EQ(symbols.at("pthread_rwlock_wrlock"),
            reinterpret_cast<void *>(mocktail_bionic_pthread_rwlock_wrlock));
  EXPECT_EQ(symbols.at("pthread_rwlock_unlock"),
            reinterpret_cast<void *>(mocktail_bionic_pthread_rwlock_unlock));
}

TEST(LinkerRegistryTest, KeepsBionicAtForkAbiOwnedByLibc) {
  linker::RegisterBionicAtForkRuntimeForLibc();

  const linker::SymbolMap symbols =
      linker::GetSyntheticLibrarySymbols("libc.so");
  EXPECT_EQ(symbols.at("__register_atfork"),
            reinterpret_cast<void *>(mocktail_bionic_register_atfork));
}

TEST(LinkerRegistryTest, KeepsHostIndependentBionicAbiOwnedByLibc) {
  linker::RegisterBionicHostLibcRuntimeForLibc();

  const linker::SymbolMap symbols =
      linker::GetSyntheticLibrarySymbols("libc.so");
  EXPECT_EQ(symbols.at("getaddrinfo"),
            reinterpret_cast<void *>(mocktail_bionic_getaddrinfo));
  EXPECT_EQ(symbols.at("freeaddrinfo"),
            reinterpret_cast<void *>(mocktail_bionic_freeaddrinfo));
  EXPECT_EQ(symbols.at("gethostbyname"),
            reinterpret_cast<void *>(mocktail_bionic_gethostbyname));
  EXPECT_EQ(symbols.at("__cmsg_nxthdr"),
            reinterpret_cast<void *>(mocktail_bionic_cmsg_nxthdr));
  EXPECT_EQ(symbols.at("__cxa_thread_atexit_impl"),
            reinterpret_cast<void *>(mocktail_bionic_cxa_thread_atexit_impl));
  EXPECT_EQ(symbols.at("__readlink_chk"),
            reinterpret_cast<void *>(mocktail___readlink_chk));
  EXPECT_EQ(symbols.at("arc4random_buf"),
            reinterpret_cast<void *>(mocktail_bionic_arc4random_buf));
  EXPECT_EQ(symbols.at("mallinfo"),
            reinterpret_cast<void *>(mocktail_bionic_mallinfo));
  EXPECT_EQ(symbols.at("sysinfo"),
            reinterpret_cast<void *>(mocktail_bionic_sysinfo));
  EXPECT_EQ(symbols.at("uname"),
            reinterpret_cast<void *>(mocktail_bionic_uname));
  EXPECT_EQ(symbols.at("getuid"),
            reinterpret_cast<void *>(mocktail_bionic_getuid));
  EXPECT_EQ(symbols.at("geteuid"),
            reinterpret_cast<void *>(mocktail_bionic_geteuid));
  EXPECT_EQ(symbols.at("strerror_r"),
            reinterpret_cast<void *>(mocktail_bionic_strerror_r));
  EXPECT_EQ(symbols.at("strtoll_l"),
            reinterpret_cast<void *>(mocktail_bionic_strtoll_l));
  EXPECT_EQ(symbols.at("strtoull_l"),
            reinterpret_cast<void *>(mocktail_bionic_strtoull_l));
  EXPECT_EQ(linker::GetBionicSymbols().at("strerror_r"),
            reinterpret_cast<void *>(mocktail_bionic_strerror_r));
  EXPECT_EQ(linker::GetBionicSymbols().at("getaddrinfo"),
            reinterpret_cast<void *>(mocktail_bionic_getaddrinfo));
  EXPECT_EQ(linker::GetBionicSymbols().at("freeaddrinfo"),
            reinterpret_cast<void *>(mocktail_bionic_freeaddrinfo));
  EXPECT_EQ(linker::GetBionicSymbols().at("gethostbyname"),
            reinterpret_cast<void *>(mocktail_bionic_gethostbyname));
  EXPECT_EQ(linker::GetBionicSymbols().at("getuid"),
            reinterpret_cast<void *>(mocktail_bionic_getuid));
  EXPECT_EQ(linker::GetBionicSymbols().at("geteuid"),
            reinterpret_cast<void *>(mocktail_bionic_geteuid));
  EXPECT_EQ(linker::GetBionicSymbols().at("sysinfo"),
            reinterpret_cast<void *>(mocktail_bionic_sysinfo));
  EXPECT_EQ(linker::GetBionicSymbols().at("uname"),
            reinterpret_cast<void *>(mocktail_bionic_uname));
}

TEST(LinkerRegistryTest, KeepsBionicLargeFileAbiOwnedByLibc) {
  linker::RegisterBionicLargeFileRuntimeForLibc();

  const linker::SymbolMap symbols =
      linker::GetSyntheticLibrarySymbols("libc.so");
  EXPECT_EQ(symbols.at("pread64"),
            reinterpret_cast<void *>(mocktail_bionic_pread64));
  EXPECT_EQ(symbols.at("pwrite64"),
            reinterpret_cast<void *>(mocktail_bionic_pwrite64));
  EXPECT_EQ(symbols.at("lseek64"),
            reinterpret_cast<void *>(mocktail_bionic_lseek64));
}

TEST(LinkerRegistryTest, KeepsBionicSysconfAbiOwnedByLibc) {
  linker::RegisterBionicSysconfRuntimeForLibc();

  const linker::SymbolMap symbols =
      linker::GetSyntheticLibrarySymbols("libc.so");
  EXPECT_EQ(symbols.at("sysconf"),
            reinterpret_cast<void *>(mocktail_bionic_sysconf));
}

TEST(LinkerRegistryTest, KeepsBionicStdioAbiOwnedByLibc) {
  linker::RegisterBionicStdioRuntimeForLibc();

  const linker::SymbolMap symbols =
      linker::GetSyntheticLibrarySymbols("libc.so");
  EXPECT_EQ(symbols.at("__sF"),
            mocktail::compat::BionicFileArraySymbolAddress());
  EXPECT_EQ(symbols.at("stdin"), mocktail::compat::BionicStdinSymbolAddress());
  EXPECT_EQ(symbols.at("stdout"),
            mocktail::compat::BionicStdoutSymbolAddress());
  EXPECT_EQ(symbols.at("stderr"),
            mocktail::compat::BionicStderrSymbolAddress());
  EXPECT_EQ(symbols.at("fflush"), reinterpret_cast<void *>(mocktail_fflush));
  EXPECT_EQ(symbols.at("fscanf"), reinterpret_cast<void *>(mocktail_fscanf));
  EXPECT_EQ(symbols.at("__fwrite_chk"),
            reinterpret_cast<void *>(mocktail___fwrite_chk));
}

TEST(LinkerRegistryTest, KeepsBionicDynamicLoaderOwnedByLibdl) {
  linker::RegisterBionicDynamicLoaderForLibdl();
  const auto exports = linker::GetSyntheticLibrarySymbols("libdl.so");

  EXPECT_NE(exports.find("dlopen"), exports.end());
  EXPECT_NE(exports.find("dlsym"), exports.end());
  EXPECT_NE(exports.find("dlclose"), exports.end());
  EXPECT_NE(exports.find("dlerror"), exports.end());
}

TEST(LinkerRegistryTest, WithholdsEveryCrashpadJniEntrypoint) {
  constexpr std::string_view kPrefix =
      "Java_com_roblox_engine_jni_NativeSettingsInterface_";
  EXPECT_TRUE(linker::IsBlockedRobloxCrashReportNativeSymbol(
      std::string(kPrefix) + "nativeInitCrashpad"));
  EXPECT_TRUE(linker::IsBlockedRobloxCrashReportNativeSymbol(
      std::string(kPrefix) + "nativeInitAppCrashpadReporter"));
  EXPECT_TRUE(linker::IsBlockedRobloxCrashReportNativeSymbol(
      std::string(kPrefix) + "nativeRunCrashpadHandler"));
  EXPECT_TRUE(linker::IsBlockedRobloxCrashReportNativeSymbol(
      std::string(kPrefix) +
      "nativeInitCrashpad__Lcom_roblox_engine_jni_model_Params_2"));

  EXPECT_FALSE(linker::IsBlockedRobloxCrashReportNativeSymbol(
      std::string(kPrefix) + "nativeInitClientSettings"));
  EXPECT_FALSE(linker::IsBlockedRobloxCrashReportNativeSymbol(
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeV2InitWithParams"));
}

} // namespace
