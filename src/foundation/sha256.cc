#include "mocktail/sha256.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace mocktail {
namespace foundation {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

std::uint32_t RotateRight(std::uint32_t value, unsigned int count) {
  return (value >> count) | (value << (32U - count));
}

}  // namespace

Sha256::~Sha256() {
  volatile unsigned char* block = block_.data();
  for (std::size_t index = 0; index < block_.size(); ++index) {
    block[index] = 0;
  }
  volatile std::uint32_t* state = state_.data();
  for (std::size_t index = 0; index < state_.size(); ++index) {
    state[index] = 0;
  }
  block_bytes_ = 0;
  total_bytes_ = 0;
}

void Sha256::Update(const unsigned char* bytes, std::size_t size) {
  if (finalized_ || (bytes == nullptr && size != 0)) {
    return;
  }
  total_bytes_ += size;
  while (size > 0) {
    const std::size_t copied = std::min(size, block_.size() - block_bytes_);
    std::memcpy(block_.data() + block_bytes_, bytes, copied);
    block_bytes_ += copied;
    bytes += copied;
    size -= copied;
    if (block_bytes_ == block_.size()) {
      Transform(block_.data());
      block_bytes_ = 0;
    }
  }
}

void Sha256::Update(std::string_view bytes) {
  Update(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
}

std::string Sha256::FinalHex() {
  if (finalized_) {
    return {};
  }
  finalized_ = true;
  const std::uint64_t bit_count = total_bytes_ * 8U;
  block_[block_bytes_++] = 0x80U;
  if (block_bytes_ > 56) {
    std::fill(block_.begin() + block_bytes_, block_.end(), 0);
    Transform(block_.data());
    block_bytes_ = 0;
  }
  std::fill(block_.begin() + block_bytes_, block_.begin() + 56, 0);
  for (std::size_t index = 0; index < 8; ++index) {
    block_[63 - index] = static_cast<unsigned char>(bit_count >> (index * 8U));
  }
  Transform(block_.data());

  constexpr char kHex[] = "0123456789abcdef";
  std::string digest(64, '0');
  for (std::size_t index = 0; index < state_.size(); ++index) {
    const std::uint32_t word = state_[index];
    for (std::size_t byte_index = 0; byte_index < 4; ++byte_index) {
      const unsigned int shift =
          24U - static_cast<unsigned int>(byte_index * 8);
      const unsigned char byte = static_cast<unsigned char>(word >> shift);
      const std::size_t output = index * 8 + byte_index * 2;
      digest[output] = kHex[byte >> 4U];
      digest[output + 1] = kHex[byte & 0x0fU];
    }
  }
  return digest;
}

void Sha256::Transform(const unsigned char* block) {
  std::array<std::uint32_t, 64> words{};
  for (std::size_t index = 0; index < 16; ++index) {
    words[index] = static_cast<std::uint32_t>(block[index * 4]) << 24U |
                   static_cast<std::uint32_t>(block[index * 4 + 1]) << 16U |
                   static_cast<std::uint32_t>(block[index * 4 + 2]) << 8U |
                   static_cast<std::uint32_t>(block[index * 4 + 3]);
  }
  for (std::size_t index = 16; index < words.size(); ++index) {
    const std::uint32_t first = words[index - 15];
    const std::uint32_t second = words[index - 2];
    const std::uint32_t sigma0 =
        RotateRight(first, 7) ^ RotateRight(first, 18) ^ (first >> 3U);
    const std::uint32_t sigma1 =
        RotateRight(second, 17) ^ RotateRight(second, 19) ^ (second >> 10U);
    words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (std::size_t index = 0; index < words.size(); ++index) {
    const std::uint32_t sum1 =
        RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t first =
        h + sum1 + choose + kRoundConstants[index] + words[index];
    const std::uint32_t sum0 =
        RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t second = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + first;
    d = c;
    c = b;
    b = a;
    a = first + second;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;

  volatile std::uint32_t* sensitive_words = words.data();
  for (std::size_t index = 0; index < words.size(); ++index) {
    sensitive_words[index] = 0;
  }
}

std::string ComputeSha256Hex(std::string_view bytes) {
  Sha256 sha256;
  sha256.Update(bytes);
  return sha256.FinalHex();
}

}  // namespace foundation
}  // namespace mocktail
