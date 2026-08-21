#include "compat/bionic_large_file_runtime.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>

#include <gtest/gtest.h>

namespace {

class ScopedFd {
public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;

  int get() const { return fd_; }

private:
  int fd_;
};

TEST(BionicLargeFileRuntimeTest, PreservesSparseOffsetsBeyond32Bits) {
  char path[] = "/tmp/mocktail-large-file-XXXXXX";
  ScopedFd file(mkstemp(path));
  ASSERT_GE(file.get(), 0);
  ASSERT_EQ(unlink(path), 0);

  constexpr mocktail::compat::BionicOff64 kSparseOffset =
      (mocktail::compat::BionicOff64{5} << 30) + 123;
  constexpr std::array<char, 7> kPayload{'m', 'o', 'c', 'k', 't', 'a', 'i'};

  ASSERT_EQ(lseek(file.get(), 17, SEEK_SET), 17);
  ASSERT_EQ(mocktail_bionic_pwrite64(file.get(), kPayload.data(),
                                     kPayload.size(), kSparseOffset),
            static_cast<ssize_t>(kPayload.size()));
  EXPECT_EQ(lseek(file.get(), 0, SEEK_CUR), 17);

  struct stat metadata{};
  ASSERT_EQ(fstat(file.get(), &metadata), 0);
  EXPECT_EQ(metadata.st_size,
            static_cast<off_t>(kSparseOffset + kPayload.size()));

  std::array<char, kPayload.size()> readback{};
  ASSERT_EQ(mocktail_bionic_pread64(file.get(), readback.data(),
                                    readback.size(), kSparseOffset),
            static_cast<ssize_t>(readback.size()));
  EXPECT_EQ(readback, kPayload);
  EXPECT_EQ(lseek(file.get(), 0, SEEK_CUR), 17);

  EXPECT_EQ(mocktail_bionic_lseek64(file.get(), kSparseOffset, SEEK_SET),
            kSparseOffset);
  EXPECT_EQ(lseek(file.get(), 0, SEEK_CUR), static_cast<off_t>(kSparseOffset));
}

} // namespace
