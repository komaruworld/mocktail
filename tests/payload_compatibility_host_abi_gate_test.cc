#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "compat/elf_build_id.h"
#include "compat/host_abi_profile.h"
#include "compat/payload_compatibility.h"

#ifndef MOCKTAIL_HOST_ABI_FIXTURE_PATH
#error "MOCKTAIL_HOST_ABI_FIXTURE_PATH must name the host ABI ELF fixture"
#endif

namespace mocktail::compat {
namespace {

TEST(PayloadCompatibilityHostAbiGateTest,
     RejectsRunCapableUnknownBuildWithoutExactHostAbiProfile) {
  for (const char* name :
       {"MOCKTAIL_HOST_ABI_PROFILE_FILE", "MOCKTAIL_HOST_ABI_CANARY",
        "MOCKTAIL_ALLOW_CANDIDATE_HOST_ABI",
        "MOCKTAIL_HOST_ABI_APPROVAL_RECEIPT"}) {
    ASSERT_EQ(unsetenv(name), 0);
  }

  const std::filesystem::path fixture = MOCKTAIL_HOST_ABI_FIXTURE_PATH;
  const BuildIdResult build_id = ReadElfBuildId(fixture.string());
  ASSERT_TRUE(build_id) << build_id.error;
  ASSERT_EQ(FindHostAbiProfile(build_id.build_id), nullptr);

  std::array<char, 64> temporary_template{};
  const std::string encoded_template =
      "/tmp/mocktail-payload-host-abi-gate-XXXXXX";
  std::copy(encoded_template.begin(), encoded_template.end(),
            temporary_template.begin());
  char* directory = mkdtemp(temporary_template.data());
  ASSERT_NE(directory, nullptr);
  const std::filesystem::path temporary_root = directory;
  const std::filesystem::path manifest = temporary_root / "compatibility.json";
  {
    const nlohmann::json document = {
        {"schema_version", 1},
        {"profiles",
         {{{"version_name", "fixture"},
           {"version_code", 9999},
           {"elf_build_id", build_id.build_id},
           {"status", "experimental"},
           {"default_allowed", true},
           {"allow_legacy_binary_patches", false},
           {"allow_host_abi_bridges", true},
           {"allow_host_constructor_replay", true},
           {"reason", "fixture must not run without exact host ABI"}}}},
    };
    std::ofstream output(manifest, std::ios::binary);
    ASSERT_TRUE(output);
    output << document.dump(2) << '\n';
  }

  const PayloadCompatibilityResult result =
      CheckPayloadCompatibility(fixture.string(), manifest.string(), false);

  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("No exact host ABI profile"), std::string::npos);
  std::error_code code;
  std::filesystem::remove_all(temporary_root, code);
}

}  // namespace
}  // namespace mocktail::compat
