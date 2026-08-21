#include "mocktail/sha256.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace mocktail {
namespace foundation {
namespace {

TEST(Sha256Test, ComputesStandardVector) {
  EXPECT_EQ(ComputeSha256Hex("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256Test, IncludesEmbeddedNullBytes) {
  const std::string bytes("a\0b", 3);
  EXPECT_EQ(ComputeSha256Hex(bytes),
            "59b271ae1bbcb1d31d41929817f4b16fb439eb4f31520b5ad1d5ce98920a7138");
  EXPECT_NE(ComputeSha256Hex(bytes), ComputeSha256Hex(std::string_view("a")));
}

}  // namespace
}  // namespace foundation
}  // namespace mocktail
