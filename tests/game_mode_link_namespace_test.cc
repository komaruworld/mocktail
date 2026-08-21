#include "runtime/game_mode.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

// Reproduce the process-global Bionic exports used by the production binary.
// Without a separate link-map namespace, libgamemode and libdbus can resolve
// these unversioned symbols instead of their host-glibc implementations.
extern "C" void* __memcpy_chk(void* destination, const void* source,
                               size_t count, size_t destination_size) {
  return std::memcpy(destination, source,
                     std::min(count, destination_size));
}

extern "C" void* __memset_chk(void* destination, int value, size_t count,
                               size_t destination_size) {
  return std::memset(destination, value,
                     std::min(count, destination_size));
}

extern "C" int __vsprintf_chk(char* destination, int, size_t destination_size,
                               const char* format, va_list arguments) {
  return std::vsnprintf(destination, destination_size, format, arguments);
}

extern "C" int __vsnprintf_chk(char* destination, size_t count, int,
                                size_t destination_size, const char* format,
                                va_list arguments) {
  return std::vsnprintf(destination, std::min(count, destination_size), format,
                        arguments);
}

namespace mocktail::runtime {
namespace {

TEST(GameModeLinkNamespaceTest, KeepsHostClientAwayFromBionicExports) {
#if defined(__GLIBC__)
  GameModeSession session = GameModeSession::Start(GameModePolicy::kOn);
  ASSERT_EQ(session.state(), GameModeSessionState::kActive) << session.detail();
  EXPECT_TRUE(session.Stop().ok());
#else
  GTEST_SKIP() << "separate ELF link-map namespaces require glibc";
#endif
}

}  // namespace
}  // namespace mocktail::runtime
