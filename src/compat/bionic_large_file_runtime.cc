#include "compat/bionic_large_file_runtime.h"

#include <unistd.h>

#include <type_traits>

namespace mocktail::compat {
namespace {

static_assert(sizeof(void*) == 8,
              "the Bionic large-file bridge requires an LP64 host");
static_assert(sizeof(off_t) == sizeof(BionicOff64),
              "x86-64 host off_t must preserve Android off64_t offsets");
static_assert(std::is_signed_v<off_t>,
              "x86-64 host off_t must be a signed 64-bit type");

off_t HostOffset(BionicOff64 offset) noexcept {
  return static_cast<off_t>(offset);
}

}  // namespace

ssize_t BionicPread64(int fd, void* buffer, size_t count,
                      BionicOff64 offset) noexcept {
  return pread(fd, buffer, count, HostOffset(offset));
}

ssize_t BionicPwrite64(int fd, const void* buffer, size_t count,
                       BionicOff64 offset) noexcept {
  return pwrite(fd, buffer, count, HostOffset(offset));
}

BionicOff64 BionicLseek64(int fd, BionicOff64 offset, int whence) noexcept {
  return static_cast<BionicOff64>(lseek(fd, HostOffset(offset), whence));
}

}  // namespace mocktail::compat

extern "C" ssize_t
mocktail_bionic_pread64(int fd, void* buffer, size_t count,
                        mocktail::compat::BionicOff64 offset) {
  return mocktail::compat::BionicPread64(fd, buffer, count, offset);
}

extern "C" ssize_t
mocktail_bionic_pwrite64(int fd, const void* buffer, size_t count,
                         mocktail::compat::BionicOff64 offset) {
  return mocktail::compat::BionicPwrite64(fd, buffer, count, offset);
}

extern "C" mocktail::compat::BionicOff64
mocktail_bionic_lseek64(int fd, mocktail::compat::BionicOff64 offset,
                        int whence) {
  return mocktail::compat::BionicLseek64(fd, offset, whence);
}
