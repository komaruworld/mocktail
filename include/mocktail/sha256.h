#ifndef MOCKTAIL_MOCKTAIL_SHA256_H_
#define MOCKTAIL_MOCKTAIL_SHA256_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mocktail {
namespace foundation {

class Sha256 final {
 public:
  Sha256() = default;
  ~Sha256();

  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  void Update(const unsigned char* bytes, std::size_t size);
  void Update(std::string_view bytes);
  std::string FinalHex();

 private:
  void Transform(const unsigned char* block);

  std::array<std::uint32_t, 8> state_ = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  std::array<unsigned char, 64> block_{};
  std::size_t block_bytes_ = 0;
  std::uint64_t total_bytes_ = 0;
  bool finalized_ = false;
};

// Returns the lowercase SHA-256 digest of the exact byte sequence, including
// embedded NUL bytes.
std::string ComputeSha256Hex(std::string_view bytes);

}  // namespace foundation
}  // namespace mocktail

#endif  // MOCKTAIL_MOCKTAIL_SHA256_H_
