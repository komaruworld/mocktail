// Exact 3222 machine-code fixtures exercise production activation and teardown.
// The mapped image contains only recorded excerpts; guest code is never run.
#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "compat/build_profile.h"
#include "mocktail/vr/roblox_vr_device_bridge.h"
#include "vr/roblox_vr_device_bridge_internal.h"

namespace mocktail::vr {
namespace {

using Json = nlohmann::json;

std::uintptr_t Rva(const Json &value) {
  return std::strtoull(value.get_ref<const std::string &>().c_str(), nullptr,
                       16);
}

std::vector<unsigned char> CodeBytes(const Json &value) {
  const auto &encoded = value.get_ref<const std::string &>();
  std::vector<unsigned char> bytes;
  for (std::size_t i = 0; i < encoded.size(); i += 2) {
    bytes.push_back(static_cast<unsigned char>(
        std::strtoul(encoded.substr(i, 2).c_str(), nullptr, 16)));
  }
  return bytes;
}

class Recorded3222Image final {
public:
  explicit Recorded3222Image(const Json &fixture)
      : size_(fixture.at("image_size").get<std::size_t>()) {
    mapping_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping_ == MAP_FAILED)
      return;
    auto *header = reinterpret_cast<Elf64_Ehdr *>(mapping_);
    std::memcpy(header->e_ident, ELFMAG, SELFMAG);
    header->e_ident[EI_CLASS] = ELFCLASS64;
    header->e_ident[EI_DATA] = ELFDATA2LSB;
    header->e_ident[EI_VERSION] = EV_CURRENT;
    header->e_type = ET_DYN;
    header->e_machine = EM_X86_64;
    header->e_phoff = sizeof(Elf64_Ehdr);
    header->e_phentsize = sizeof(Elf64_Phdr);
    header->e_phnum = fixture.at("program_headers").size();
    auto *headers = reinterpret_cast<Elf64_Phdr *>(At(header->e_phoff));
    std::size_t index = 0;
    for (const auto &source : fixture.at("program_headers")) {
      auto &target = headers[index++];
      target.p_type = source.at("type").get<Elf64_Word>();
      target.p_flags = source.at("flags").get<Elf64_Word>();
      target.p_vaddr = source.at("vaddr").get<Elf64_Addr>();
      target.p_memsz = source.at("memsz").get<Elf64_Xword>();
    }
    for (const auto &entry : fixture.at("functions").items()) {
      const auto &function = entry.value();
      const auto code = CodeBytes(function.at("bytes"));
      std::memcpy(At(Rva(function.at("rva"))), code.data(), code.size());
    }
    const auto table =
        Rva(fixture.at("vr_debug_device_bridge").at("vtable_rva"));
    auto *vtable = reinterpret_cast<std::uintptr_t *>(At(table));
    index = 0;
    for (const auto &target : fixture.at("vtable_targets")) {
      vtable[index++] = base() + Rva(target);
    }
    for (const auto &entry : fixture.at("functions").items()) {
      const auto &function = entry.value();
      if (!Protect(Rva(function.at("rva")),
                   function.at("bytes").get_ref<const std::string &>().size() /
                       2,
                   PROT_READ | PROT_EXEC))
        return;
    }
    ready_ = Protect(table, index * sizeof(std::uintptr_t), PROT_READ);
  }

  ~Recorded3222Image() {
    if (mapping_ != MAP_FAILED)
      munmap(mapping_, size_);
  }
  Recorded3222Image(const Recorded3222Image &) = delete;
  Recorded3222Image &operator=(const Recorded3222Image &) = delete;

  bool ready() const { return ready_; }
  std::uintptr_t base() const {
    return reinterpret_cast<std::uintptr_t>(mapping_);
  }
  unsigned char *At(std::uintptr_t rva) const {
    return static_cast<unsigned char *>(mapping_) + rva;
  }
  bool Corrupt(std::uintptr_t rva) {
    if (!Protect(rva, 1, PROT_READ | PROT_WRITE))
      return false;
    *At(rva) ^= 1;
    return Protect(rva, 1, PROT_READ | PROT_EXEC);
  }

private:
  bool Protect(std::uintptr_t rva, std::size_t length, int protection) {
    const auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    const auto first = rva - rva % page;
    const auto end = (rva + length + page - 1) / page * page;
    return mprotect(At(first), end - first, protection) == 0;
  }
  void *mapping_ = MAP_FAILED;
  std::size_t size_ = 0;
  bool ready_ = false;
};

class Vr3222Profile : public testing::Test {
protected:
  void SetUp() override {
    const std::string root = MOCKTAIL_TEST_SOURCE_DIRECTORY;
    std::ifstream input(root + "/tests/vr/fixtures/roblox_3222_contracts.json");
    ASSERT_TRUE(input.good());
    fixture_ = Json::parse(input, nullptr, false);
    ASSERT_FALSE(fixture_.is_discarded());
    auto result =
        compat::FindBuildProfile(root + "/config/roblox_compatibility.json",
                                 fixture_.at("build_id").get<std::string>());
    ASSERT_TRUE(result) << result.error;
    ASSERT_TRUE(result.profile.has_value());
    profile_ = *result.profile;
    ASSERT_EQ(profile_.version_code, 3222);
    ASSERT_TRUE(profile_.vr_debug_device_bridge.has_value());
  }
  Json fixture_;
  compat::BuildProfile profile_;
};

TEST_F(Vr3222Profile, ActivatesRecordedMachineCodeAndRestoresEveryHook) {
  Recorded3222Image image(fixture_);
  ASSERT_TRUE(image.ready());
  const auto &device = *profile_.vr_debug_device_bridge;
  const auto pointer_rva = Rva(fixture_["functions"]["pointer"]["rva"]);
  const auto pointer_code =
      CodeBytes(fixture_["functions"]["pointer"]["bytes"]);
  const auto *table =
      reinterpret_cast<const std::uintptr_t *>(image.At(device.vtable_rva));
  const std::vector<std::uintptr_t> originals(table, table + 27);
  RobloxVrDeviceBridge bridge;
  ASSERT_TRUE(bridge.Install(profile_).ok());
  const auto activated = bridge.Activate(image.base());
  ASSERT_TRUE(activated.ok()) << activated.message();
  EXPECT_TRUE(bridge.active());
  EXPECT_NE(table[3], originals[3]);
  EXPECT_NE(table[5], originals[5]);
  EXPECT_NE(table[9], originals[9]);
  EXPECT_NE(std::memcmp(image.At(pointer_rva), pointer_code.data(), 15), 0);
  bridge.Shutdown();
  EXPECT_FALSE(bridge.active());
  EXPECT_EQ(std::memcmp(table, originals.data(),
                        originals.size() * sizeof(originals[0])),
            0);
  EXPECT_EQ(std::memcmp(image.At(pointer_rva), pointer_code.data(),
                        pointer_code.size()),
            0);
}

TEST_F(Vr3222Profile, CachedManifestUsesTheSameExactBuildFallback) {
  Recorded3222Image image(fixture_);
  ASSERT_TRUE(image.ready());
  profile_.vr_debug_device_bridge.reset();
  const auto *fallback = FindRobloxVrDeviceProfile(profile_.elf_build_id);
  ASSERT_NE(fallback, nullptr);
  profile_.vr_debug_device_bridge = *fallback;
  RobloxVrDeviceBridge bridge;
  ASSERT_TRUE(bridge.Install(profile_).ok());
  const auto activated = bridge.Activate(image.base());
  EXPECT_TRUE(activated.ok()) << activated.message();
}

TEST_F(Vr3222Profile, RejectsChangedPointerLayoutBeforeInstallingHooks) {
  const auto pointer_rva = Rva(fixture_["functions"]["pointer"]["rva"]);
  for (const std::uintptr_t offset :
       {0x27, 0x37, 0x3f, 0x47, 0x5a, 0x6f, 0x142}) {
    SCOPED_TRACE(offset);
    Recorded3222Image image(fixture_);
    ASSERT_TRUE(image.ready());
    ASSERT_TRUE(image.Corrupt(pointer_rva + offset));
    const auto *table = reinterpret_cast<const std::uintptr_t *>(
        image.At(profile_.vr_debug_device_bridge->vtable_rva));
    const std::vector<std::uintptr_t> originals(table, table + 27);
    RobloxVrDeviceBridge bridge;
    ASSERT_TRUE(bridge.Install(profile_).ok());
    const auto activated = bridge.Activate(image.base());
    EXPECT_FALSE(activated.ok());
    EXPECT_EQ(activated.message(), "VR pointer function contract mismatch");
    EXPECT_FALSE(bridge.active());
    EXPECT_EQ(std::memcmp(table, originals.data(),
                          originals.size() * sizeof(originals[0])),
              0);
  }
}

TEST_F(Vr3222Profile, RejectsAFramebufferSlotFromTheOlderBuild) {
  profile_.vr_debug_device_bridge->device_create_framebuffer_vtable_offset =
      0x188;
  RobloxVrDeviceBridge bridge;
  const auto installed = bridge.Install(profile_);
  EXPECT_FALSE(installed.ok());
  EXPECT_EQ(installed.message(),
            "VR metadata does not match the verified Build ID");
}

} // namespace
} // namespace mocktail::vr
