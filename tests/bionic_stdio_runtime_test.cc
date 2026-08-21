#include "compat/bionic_stdio_runtime.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

TEST(BionicStdioRuntimeTest, PreservesRandomAccessFileContract) {
  FILE* stream = std::tmpfile();
  ASSERT_NE(nullptr, stream);
  constexpr char kPayload[] = "shader-pack";

  ASSERT_EQ(sizeof(kPayload) - 1,
            mocktail_fwrite(kPayload, 1, sizeof(kPayload) - 1, stream));
  ASSERT_EQ(0, mocktail_fflush(stream));
  EXPECT_EQ(static_cast<long>(sizeof(kPayload) - 1), mocktail_ftell(stream));

  ASSERT_EQ(0, mocktail_fseek(stream, 7, SEEK_SET));
  std::array<char, 5> suffix{};
  EXPECT_EQ(4U, mocktail_fread(suffix.data(), 1, 4, stream));
  EXPECT_STREQ("pack", suffix.data());
  EXPECT_EQ(static_cast<long>(sizeof(kPayload) - 1), mocktail_ftell(stream));
  EXPECT_EQ(0, mocktail_fclose(stream));
}

TEST(BionicStdioRuntimeTest, PreservesLargeFileSeekContract) {
  FILE* stream = std::tmpfile();
  ASSERT_NE(nullptr, stream);

  ASSERT_EQ(0, mocktail_fseeko(stream, 4096, SEEK_SET));
  EXPECT_EQ(static_cast<off_t>(4096), mocktail_ftello(stream));
  ASSERT_EQ('x', mocktail_fputc('x', stream));
  EXPECT_EQ(static_cast<off_t>(4097), mocktail_ftello(stream));
  EXPECT_EQ(0, mocktail_fclose(stream));
}

TEST(BionicStdioRuntimeTest, FlushAllStaysInsideGuestStreamBoundary) {
  EXPECT_EQ(0, mocktail_fflush(nullptr));
}

TEST(BionicStdioRuntimeTest, ExposesExactLp64BionicStandardStreamLayout) {
  EXPECT_EQ(456U, sizeof(__sF));
  EXPECT_EQ(152U, reinterpret_cast<uintptr_t>(&__sF[1]) -
                      reinterpret_cast<uintptr_t>(&__sF[0]));
  EXPECT_EQ(152U, reinterpret_cast<uintptr_t>(&__sF[2]) -
                      reinterpret_cast<uintptr_t>(&__sF[1]));
  EXPECT_EQ(0U, reinterpret_cast<uintptr_t>(&__sF[0]) % sizeof(void*));
}

TEST(BionicStdioRuntimeTest, TranslatesOnlyBionicStandardStreams) {
  FILE* guest_stdin = reinterpret_cast<FILE*>(&__sF[0]);
  FILE* guest_stdout = reinterpret_cast<FILE*>(&__sF[1]);
  FILE* guest_stderr = reinterpret_cast<FILE*>(&__sF[2]);
  FILE* ordinary_stream = std::tmpfile();
  ASSERT_NE(nullptr, ordinary_stream);

  EXPECT_EQ(stdin, mocktail::compat::TranslateBionicFile(guest_stdin));
  EXPECT_EQ(stdout, mocktail::compat::TranslateBionicFile(guest_stdout));
  EXPECT_EQ(stderr, mocktail::compat::TranslateBionicFile(guest_stderr));
  EXPECT_EQ(ordinary_stream,
            mocktail::compat::TranslateBionicFile(ordinary_stream));
  EXPECT_EQ(STDIN_FILENO, mocktail_fileno(guest_stdin));
  EXPECT_EQ(STDOUT_FILENO, mocktail_fileno(guest_stdout));
  EXPECT_EQ(STDERR_FILENO, mocktail_fileno(guest_stderr));
  EXPECT_EQ(0, mocktail_fflush(guest_stdout));

  EXPECT_EQ(0, mocktail_fclose(ordinary_stream));
}

TEST(BionicStdioRuntimeTest, FortifiedWritePreservesIoSideEffect) {
  FILE* stream = std::tmpfile();
  ASSERT_NE(nullptr, stream);
  constexpr char kPayload[] = "fortified";

  EXPECT_EQ(sizeof(kPayload) - 1,
            mocktail___fwrite_chk(kPayload, sizeof(kPayload) - 1, 1,
                                  sizeof(kPayload) - 1, stream));
  ASSERT_EQ(0, mocktail_fseek(stream, 0, SEEK_SET));
  std::array<char, sizeof(kPayload)> read_back{};
  EXPECT_EQ(sizeof(kPayload) - 1,
            mocktail___fread_chk(read_back.data(), read_back.size(), 1,
                                 sizeof(kPayload) - 1, stream));
  EXPECT_STREQ(kPayload, read_back.data());
  EXPECT_EQ(0, mocktail_fclose(stream));
}

}  // namespace
