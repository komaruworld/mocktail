#include "compat/host_abi_profile.h"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <gelf.h>
#include <gtest/gtest.h>
#include <libelf.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>

#include "compat/elf_build_id.h"
#include "compat/host_abi_profile_loader.h"

#ifndef MOCKTAIL_HOST_ABI_FIXTURE_PATH
#error "MOCKTAIL_HOST_ABI_FIXTURE_PATH must name the host ABI ELF fixture"
#endif

namespace mocktail::compat {
namespace {

TEST(HostAbiProfileTest, Supported2628HasExactResearchedBoundaries) {
  const HostAbiProfile* profile =
      FindHostAbiProfile("1686400865ae0e408cd7bd67de7a439625c6fd13");

  ASSERT_NE(profile, nullptr);
  ASSERT_EQ(profile->bridge_entry_count, 6U);
  EXPECT_EQ(profile->bridge_entries[0].rva, 0x1c39ea2U);
  EXPECT_EQ(profile->bridge_entries[1].rva, 0x1fe4c84U);
  EXPECT_EQ(profile->bridge_entries[2].rva, 0x203daaeU);
  EXPECT_EQ(profile->bridge_entries[3].rva, 0x1c39c3aU);
  EXPECT_EQ(profile->bridge_entries[4].rva, 0x1c8dbf8U);
  EXPECT_EQ(profile->bridge_entries[5].rva, 0x1c3eb72U);
  EXPECT_EQ(profile->data_seeds.allocator_object_slot, 0x7030ae0U);
  EXPECT_EQ(profile->data_seeds.empty_string_slot, 0x703a7e8U);
  EXPECT_EQ(profile->data_seeds.jni_singleton_slot, 0x72a16d0U);
  EXPECT_EQ(profile->data_seeds.jni_singleton_bytes, 0x400U);
  EXPECT_EQ(profile->data_seeds.arena_initializer, 0x1c39f1dU);
  EXPECT_EQ(profile->data_seeds.allocator_thread_initializer, 0x2040817U);
  EXPECT_EQ(profile->data_seeds.arena_guard_slot, 0x71e92f0U);
  EXPECT_EQ(profile->data_seeds.arena_table_slot, 0x71e9380U);
  EXPECT_EQ(profile->native_allocator.allocate, 0x1c39c3aU);
  EXPECT_EQ(profile->native_allocator.deallocate, 0x1c3eb72U);
  EXPECT_EQ(profile->init_array_offset, 0x6c2c9b0U);
  EXPECT_EQ(profile->init_array_count, 3481U);
  EXPECT_TRUE(profile->HasValidConstructorRanges());
  EXPECT_TRUE(profile->HasValidNativeMimallocConstructorRanges());
  EXPECT_EQ(profile->native_pre_jni_bootstrap.registry_initializer, 0x1fe72e5U);
  EXPECT_EQ(profile->native_pre_jni_bootstrap.registry_slot, 0x71ed428U);
  EXPECT_EQ(profile->default_allocator_strategy,
            HostAllocatorStrategy::kNativeMimalloc);
}

TEST(HostAbiProfileTest, CurrentPayloadHasResearchedBringupBoundaries) {
  const HostAbiProfile* profile =
      FindHostAbiProfile("d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21");

  ASSERT_NE(profile, nullptr);
  ASSERT_EQ(profile->bridge_entry_count, 6U);
  EXPECT_EQ(profile->bridge_entries[4].rva, 0x1c45f5bU);
  EXPECT_EQ(profile->bridge_entries[4].kind, HostBridgeKind::kAlignedAllocate);
  EXPECT_EQ(profile->init_array_count, 3455U);

  EXPECT_TRUE(profile->HasValidConstructorRanges());
  EXPECT_EQ(profile->ConstructorRangeBegin(), 2U);
  EXPECT_EQ(profile->ConstructorRangeEndExclusive(), 3455U);
  EXPECT_TRUE(profile->AllowsConstructor(2U));
  EXPECT_FALSE(profile->AllowsConstructor(3U));
  EXPECT_FALSE(profile->AllowsConstructor(4U));
  EXPECT_TRUE(profile->AllowsConstructor(5U));
  EXPECT_TRUE(profile->AllowsConstructor(3442U));
  EXPECT_TRUE(profile->AllowsConstructor(3443U));
  EXPECT_TRUE(profile->AllowsConstructor(3454U));
  EXPECT_FALSE(profile->AllowsConstructor(3455U));

  EXPECT_TRUE(profile->HasValidNativeMimallocConstructorRanges());
  EXPECT_EQ(profile->NativeMimallocConstructorRangeBegin(), 2U);
  EXPECT_EQ(profile->NativeMimallocConstructorRangeEndExclusive(), 3455U);
  EXPECT_FALSE(profile->AllowsNativeMimallocConstructor(1U));
  for (size_t index = 2; index < 3455; ++index) {
    EXPECT_TRUE(profile->AllowsNativeMimallocConstructor(index)) << index;
  }
  EXPECT_FALSE(profile->AllowsNativeMimallocConstructor(3455U));

  EXPECT_TRUE(profile->HasValidNativePreJniBootstrap());
  EXPECT_EQ(profile->native_pre_jni_bootstrap.registry_initializer, 0x1f7b131U);
  EXPECT_EQ(profile->native_pre_jni_bootstrap.registry_slot, 0x70c7868U);
  EXPECT_TRUE(
      profile->ShouldInitializeNativeMimallocThreadAfterConstructor(2U));
  EXPECT_FALSE(
      profile->ShouldInitializeNativeMimallocThreadAfterConstructor(3U));
  EXPECT_EQ(profile->data_seeds.arena_initializer, 0x1bf2e5dU);
  EXPECT_EQ(profile->data_seeds.allocator_thread_initializer, 0x1fd61f3U);
  EXPECT_TRUE(profile->native_allocator.IsValid());
  EXPECT_EQ(profile->native_allocator.allocate, 0x1bf2b81U);
  EXPECT_EQ(profile->native_allocator.deallocate, 0x1bf7abaU);
  EXPECT_EQ(profile->bridge_entries[1].kind, HostBridgeKind::kUsableSize);
  EXPECT_EQ(profile->bridge_entries[5].kind, HostBridgeKind::kFree);
  EXPECT_EQ(profile->default_allocator_strategy,
            HostAllocatorStrategy::kNativeMimalloc);
  EXPECT_EQ(profile->ResolveAllocatorStrategy(false, false),
            HostAllocatorStrategy::kNativeMimalloc);
  EXPECT_EQ(profile->ResolveAllocatorStrategy(false, true),
            HostAllocatorStrategy::kNativeMimalloc);
  EXPECT_EQ(profile->ResolveAllocatorStrategy(true, true),
            HostAllocatorStrategy::kHostBridges);
  EXPECT_EQ(profile->ResolveAllocatorStrategy(true, false),
            HostAllocatorStrategy::kNativeMimalloc);
}

TEST(HostAbiProfileTest, ResearchedBaselineKeepsHostBridgeDefault) {
  const HostAbiProfile* profile =
      FindHostAbiProfile("50e1b0abd123350e794226062fe3a1ef360c5f0d");

  ASSERT_NE(profile, nullptr);
  EXPECT_EQ(profile->default_allocator_strategy,
            HostAllocatorStrategy::kHostBridges);
  EXPECT_EQ(profile->ResolveAllocatorStrategy(false, false),
            HostAllocatorStrategy::kHostBridges);
  EXPECT_EQ(profile->ResolveAllocatorStrategy(true, false),
            HostAllocatorStrategy::kNativeMimalloc);
}

TEST(HostAbiProfileTest, UnknownBuildHasNoNativeOffsets) {
  EXPECT_EQ(FindHostAbiProfile("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
            nullptr);
}

class ExternalHostAbiProfileLoaderTest : public ::testing::Test {
 protected:
  static std::string Hex(uintptr_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << value;
    return output.str();
  }

  static uintptr_t SymbolRva(const char* name) {
    void* symbol = dlsym(fixture_handle_, name);
    EXPECT_NE(symbol, nullptr) << dlerror();
    Dl_info info{};
    EXPECT_NE(dladdr(symbol, &info), 0);
    return reinterpret_cast<uintptr_t>(symbol) -
           reinterpret_cast<uintptr_t>(info.dli_fbase);
  }

  static std::pair<uintptr_t, std::size_t> ReadInitArray(
      const std::string& path) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    EXPECT_GE(descriptor, 0);
    Elf* elf = elf_begin(descriptor, ELF_C_READ, nullptr);
    EXPECT_NE(elf, nullptr);
    std::size_t section_count = 0;
    EXPECT_EQ(elf_getshdrnum(elf, &section_count), 0);
    std::pair<uintptr_t, std::size_t> result{};
    for (std::size_t index = 0; index < section_count; ++index) {
      Elf_Scn* section = elf_getscn(elf, index);
      GElf_Shdr header{};
      EXPECT_NE(gelf_getshdr(section, &header), nullptr);
      if (header.sh_type == SHT_INIT_ARRAY) {
        result = {
            static_cast<uintptr_t>(header.sh_addr),
            static_cast<std::size_t>(header.sh_size / sizeof(std::uint64_t))};
        break;
      }
    }
    elf_end(elf);
    close(descriptor);
    return result;
  }

  static void WriteJson(const std::filesystem::path& path,
                        const nlohmann::json& json, mode_t mode = 0600) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output);
    output << json.dump(2) << '\n';
    output.close();
    ASSERT_TRUE(output);
    ASSERT_EQ(chmod(path.c_str(), mode), 0);
  }

  static nlohmann::json ReadJson(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input);
    return nlohmann::json::parse(input, nullptr, false);
  }

  static std::string HashText(std::string_view value) {
    const std::filesystem::path path = temporary_root_ / "hash-input";
    {
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      EXPECT_TRUE(output);
      output << value;
    }
    const FileSha256Result hash = ComputeFileSha256(path.string());
    EXPECT_TRUE(hash) << hash.error;
    return hash.sha256;
  }

  static void CopyApprovalSet(const std::filesystem::path& directory,
                              std::filesystem::path* copied_receipt) {
    std::error_code code;
    ASSERT_TRUE(std::filesystem::create_directories(directory, code));
    ASSERT_FALSE(code) << code.message();
    const std::string prefix = payload_id_ + "-" + approval_generation_;
    for (const std::string& suffix :
         {std::string(".json"), std::string(".canary-1.json"),
          std::string(".canary-2.json")}) {
      const std::filesystem::path source =
          receipt_path_.parent_path() / (prefix + suffix);
      const std::filesystem::path destination = directory / (prefix + suffix);
      ASSERT_TRUE(std::filesystem::copy_file(
          source, destination,
          std::filesystem::copy_options::overwrite_existing, code));
      ASSERT_FALSE(code) << code.message();
      ASSERT_EQ(chmod(destination.c_str(), 0444), 0);
    }
    *copied_receipt = directory / (prefix + ".json");
  }

  static void CopyApprovalSetWithRuntimeSha256(
      const std::filesystem::path& directory, std::string_view runtime_sha256,
      std::filesystem::path* copied_receipt) {
    std::error_code code;
    ASSERT_TRUE(std::filesystem::create_directories(directory, code));
    ASSERT_FALSE(code) << code.message();

    std::array<std::filesystem::path, 2> staged_canaries;
    std::array<std::string, 2> canary_hashes;
    const std::string original_prefix =
        payload_id_ + "-" + approval_generation_;
    for (std::size_t index = 0; index < staged_canaries.size(); ++index) {
      const std::filesystem::path source =
          receipt_path_.parent_path() /
          (original_prefix + ".canary-" + std::to_string(index + 1) + ".json");
      nlohmann::json attestation = ReadJson(source);
      ASSERT_FALSE(attestation.is_discarded());
      attestation["runtime_sha256"] = runtime_sha256;
      staged_canaries[index] =
          directory / ("staged-canary-" + std::to_string(index + 1));
      WriteJson(staged_canaries[index], attestation, 0444);
      const FileSha256Result hash =
          ComputeFileSha256(staged_canaries[index].string());
      ASSERT_TRUE(hash) << hash.error;
      canary_hashes[index] = hash.sha256;
    }

    std::string generation_evidence;
    generation_evidence.append("runtime_build_id=")
        .append(runtime_build_id_)
        .append("\n");
    generation_evidence.append("payload=")
        .append(payload_fingerprint_)
        .append("\n");
    generation_evidence.append("profile=").append(profile_sha256_).append("\n");
    generation_evidence.append("compatibility=")
        .append(manifest_sha256_)
        .append("\n");
    generation_evidence.append("canary_1=")
        .append(canary_hashes[0])
        .append("\n");
    generation_evidence.append("canary_2=")
        .append(canary_hashes[1])
        .append("\n");
    const std::string generation = HashText(generation_evidence).substr(0, 40);
    const std::string prefix = payload_id_ + "-" + generation;
    for (std::size_t index = 0; index < staged_canaries.size(); ++index) {
      const std::filesystem::path destination =
          directory /
          (prefix + ".canary-" + std::to_string(index + 1) + ".json");
      std::filesystem::rename(staged_canaries[index], destination, code);
      ASSERT_FALSE(code) << code.message();
    }

    nlohmann::json receipt = ReadJson(receipt_path_);
    ASSERT_FALSE(receipt.is_discarded());
    receipt["generation"] = generation;
    receipt["canary_runtime_sha256"] = runtime_sha256;
    receipt["runtime_sha256"] = runtime_sha256;
    receipt["canary_attestation_sha256"] = canary_hashes;
    *copied_receipt = directory / (prefix + ".json");
    WriteJson(*copied_receipt, receipt, 0444);
  }

  static void SetUpTestSuite() {
    ASSERT_NE(elf_version(EV_CURRENT), EV_NONE);
    std::array<char, 64> temporary_template{};
    std::string encoded_template = "/tmp/mocktail-host-abi-loader-XXXXXX";
    std::copy(encoded_template.begin(), encoded_template.end(),
              temporary_template.begin());
    char* directory = mkdtemp(temporary_template.data());
    ASSERT_NE(directory, nullptr);
    temporary_root_ = directory;

    std::error_code code;
    const std::filesystem::path fixture_source =
        std::filesystem::canonical(MOCKTAIL_HOST_ABI_FIXTURE_PATH, code);
    ASSERT_FALSE(code) << code.message();
    const BuildIdResult build_id = ReadElfBuildId(fixture_source.string());
    ASSERT_TRUE(build_id) << build_id.error;
    build_id_ = build_id.build_id;
    payload_id_ = "9999-" + build_id_;
    const std::filesystem::path payload_directory =
        temporary_root_ / "payloads" / payload_id_;
    ASSERT_TRUE(std::filesystem::create_directories(payload_directory, code));
    ASSERT_FALSE(code) << code.message();
    const std::filesystem::path copied_payload =
        payload_directory / "libroblox.so";
    ASSERT_TRUE(std::filesystem::copy_file(
        fixture_source, copied_payload,
        std::filesystem::copy_options::overwrite_existing, code));
    ASSERT_FALSE(code) << code.message();
    payload_path_ = std::filesystem::canonical(copied_payload, code);
    ASSERT_FALSE(code) << code.message();
    fixture_handle_ = dlopen(payload_path_.c_str(), RTLD_NOW | RTLD_LOCAL);
    ASSERT_NE(fixture_handle_, nullptr) << dlerror();

    const FileSha256Result payload_hash =
        ComputeFileSha256(payload_path_.string());
    ASSERT_TRUE(payload_hash) << payload_hash.error;
    payload_sha256_ = payload_hash.sha256;

    const auto [init_array_offset, init_array_count] =
        ReadInitArray(payload_path_.string());
    ASSERT_NE(init_array_offset, 0U);
    ASSERT_GT(init_array_count, 0U);

    const nlohmann::json profile = {
        {"elf_build_id", build_id_},
        {"bridge_entries",
         {{{"rva", Hex(SymbolRva("MocktailFixtureSmallAllocate"))},
           {"kind", "allocate"},
           {"label", "small-allocate"}},
          {{"rva", Hex(SymbolRva("MocktailFixtureUsableSize"))},
           {"kind", "usable_size"},
           {"label", "usable-size"}},
          {{"rva", Hex(SymbolRva("MocktailFixtureReallocate"))},
           {"kind", "reallocate"},
           {"label", "reallocate"}},
          {{"rva", Hex(SymbolRva("MocktailFixtureAllocate"))},
           {"kind", "allocate"},
           {"label", "allocate"}},
          {{"rva", Hex(SymbolRva("MocktailFixtureAlignedAllocate"))},
           {"kind", "aligned_allocate"},
           {"label", "aligned-allocate-direct"}},
          {{"rva", Hex(SymbolRva("MocktailFixtureFree"))},
           {"kind", "free"},
           {"label", "free"}}}},
        {"data_seeds",
         {{"allocator_object_slot",
           Hex(SymbolRva("mocktail_fixture_allocator_slot"))},
          {"empty_string_slot",
           Hex(SymbolRva("mocktail_fixture_empty_string_slot"))},
          {"jni_singleton_slot", Hex(SymbolRva("mocktail_fixture_jni_slot"))},
          {"jni_singleton_bytes", 0x400},
          {"arena_initializer",
           Hex(SymbolRva("MocktailFixtureArenaInitialize"))},
          {"allocator_thread_initializer",
           Hex(SymbolRva("MocktailFixtureThreadInitialize"))},
          {"arena_guard_slot",
           Hex(SymbolRva("mocktail_fixture_arena_guard_slot"))},
          {"arena_table_slot",
           Hex(SymbolRva("mocktail_fixture_arena_table_slot"))},
          {"arena_table_slot_count", 0x400000}}},
        {"native_allocator",
         {{"allocate", Hex(SymbolRva("MocktailFixtureAllocate"))},
          {"deallocate", Hex(SymbolRva("MocktailFixtureFree"))}}},
        {"init_array_offset", Hex(init_array_offset)},
        {"init_array_count", init_array_count},
        {"constructor_run_ranges",
         {{{"begin", 2}, {"end_exclusive", 3}},
          {{"begin", 5}, {"end_exclusive", init_array_count}}}},
        {"native_mimalloc_constructor_run_ranges",
         {{{"begin", 2}, {"end_exclusive", init_array_count}}}},
        {"native_mimalloc_thread_initializer_after_constructor", 2},
        {"native_pre_jni_bootstrap",
         {{"registry_initializer",
           Hex(SymbolRva("MocktailFixtureRegistryInitialize"))},
          {"registry_slot", Hex(SymbolRva("mocktail_fixture_registry_slot"))}}},
        {"default_allocator_strategy", "native_mimalloc"},
    };
    const nlohmann::json sidecar = {
        {"schema_version", 1},
        {"elf_build_id", build_id_},
        {"payload_sha256", payload_sha256_},
        {"payload_id", payload_id_},
        {"payload_path", "payloads/" + payload_id_},
        {"reference",
         {{"elf_build_id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
          {"payload_sha256", std::string(64, 'b')}}},
        {"profile", profile},
        {"derivation_anchors",
         {{"signature_version", 1},
          {"allocator_object_initializer_rva",
           Hex(SymbolRva("MocktailFixtureArenaInitialize"))},
          {"empty_string_initializer_rva",
           Hex(SymbolRva("MocktailFixtureThreadInitialize"))},
          {"jni_singleton_initializer_rva",
           Hex(SymbolRva("MocktailFixtureRegistryInitialize"))},
          {"constructor_rvas",
           {{"2", Hex(SymbolRva("MocktailFixtureConstructorTwo"))},
            {"3", Hex(SymbolRva("MocktailFixtureConstructorThree"))},
            {"4", Hex(SymbolRva("MocktailFixtureConstructorFour"))},
            {"5", Hex(SymbolRva("MocktailFixtureConstructorFive"))}}}}},
    };
    ASSERT_TRUE(std::filesystem::create_directories(
        temporary_root_ / "host_abi_profiles", code));
    ASSERT_FALSE(code) << code.message();
    ASSERT_TRUE(std::filesystem::create_directories(
        temporary_root_ / "compatibility_profiles", code));
    ASSERT_FALSE(code) << code.message();
    ASSERT_TRUE(std::filesystem::create_directories(
        temporary_root_ / "approvals", code));
    ASSERT_FALSE(code) << code.message();

    const std::filesystem::path staged_profile =
        temporary_root_ / "host_abi_profiles/staged.json";
    WriteJson(staged_profile, sidecar, 0444);
    const FileSha256Result profile_hash =
        ComputeFileSha256(staged_profile.string());
    ASSERT_TRUE(profile_hash) << profile_hash.error;
    profile_sha256_ = profile_hash.sha256;

    const std::filesystem::path staged_manifest =
        temporary_root_ / "compatibility_profiles/staged.json";
    WriteJson(staged_manifest, {{"schema_version", 1}, {"profiles", {}}}, 0444);
    const FileSha256Result manifest_hash =
        ComputeFileSha256(staged_manifest.string());
    const FileSha256Result runtime_hash = ComputeFileSha256("/proc/self/exe");
    const BuildIdResult runtime_build_id = ReadElfBuildId("/proc/self/exe");
    ASSERT_TRUE(manifest_hash) << manifest_hash.error;
    ASSERT_TRUE(runtime_hash) << runtime_hash.error;
    ASSERT_TRUE(runtime_build_id) << runtime_build_id.error;
    manifest_sha256_ = manifest_hash.sha256;
    runtime_sha256_ = runtime_hash.sha256;
    runtime_build_id_ = runtime_build_id.build_id;
    payload_fingerprint_ = std::string(64, 'a');

    std::array<std::filesystem::path, 2> staged_canaries = {
        temporary_root_ / "approvals/staged-canary-1.json",
        temporary_root_ / "approvals/staged-canary-2.json",
    };
    std::array<std::string, 2> canary_hashes;
    for (std::size_t index = 0; index < staged_canaries.size(); ++index) {
      const std::size_t run = index + 1;
      const nlohmann::json attestation = {
          {"schema_version", 1},
          {"status", "passed"},
          {"canary_tier", "C"},
          {"run", run},
          {"run_id", payload_id_ + "-" + std::to_string(run) + "-100-200"},
          {"payload_id", payload_id_},
          {"payload_fingerprint", payload_fingerprint_},
          {"profile_sha256", profile_sha256_},
          {"compatibility_manifest_sha256", manifest_sha256_},
          {"runtime_sha256", runtime_sha256_},
          {"runtime_build_id", runtime_build_id_},
          {"readiness_log_sha256", std::string(64, 'c' + index)},
      };
      WriteJson(staged_canaries[index], attestation, 0444);
      const FileSha256Result canary_hash =
          ComputeFileSha256(staged_canaries[index].string());
      ASSERT_TRUE(canary_hash) << canary_hash.error;
      canary_hashes[index] = canary_hash.sha256;
    }

    std::string generation_evidence;
    generation_evidence.append("runtime_build_id=")
        .append(runtime_build_id_)
        .append("\n");
    generation_evidence.append("payload=")
        .append(payload_fingerprint_)
        .append("\n");
    generation_evidence.append("profile=").append(profile_sha256_).append("\n");
    generation_evidence.append("compatibility=")
        .append(manifest_sha256_)
        .append("\n");
    generation_evidence.append("canary_1=")
        .append(canary_hashes[0])
        .append("\n");
    generation_evidence.append("canary_2=")
        .append(canary_hashes[1])
        .append("\n");
    approval_generation_ = HashText(generation_evidence).substr(0, 40);
    const std::string approval_filename =
        payload_id_ + "-" + approval_generation_ + ".json";
    profile_path_ = temporary_root_ / "host_abi_profiles" / approval_filename;
    manifest_path_ =
        temporary_root_ / "compatibility_profiles" / approval_filename;
    std::filesystem::rename(staged_profile, profile_path_, code);
    ASSERT_FALSE(code) << code.message();
    std::filesystem::rename(staged_manifest, manifest_path_, code);
    ASSERT_FALSE(code) << code.message();
    for (std::size_t index = 0; index < staged_canaries.size(); ++index) {
      const std::filesystem::path destination =
          temporary_root_ / "approvals" /
          (payload_id_ + "-" + approval_generation_ + ".canary-" +
           std::to_string(index + 1) + ".json");
      std::filesystem::rename(staged_canaries[index], destination, code);
      ASSERT_FALSE(code) << code.message();
    }

    const nlohmann::json receipt = {
        {"schema_version", 1},
        {"status", "approved"},
        {"payload_id", payload_id_},
        {"generation", approval_generation_},
        {"elf_build_id", build_id_},
        {"payload_path", payload_path_.string()},
        {"payload_sha256", payload_sha256_},
        {"payload_fingerprint", payload_fingerprint_},
        {"profile_sha256", profile_sha256_},
        {"compatibility_manifest_sha256", manifest_sha256_},
        {"canary_runtime_sha256", runtime_sha256_},
        {"runtime_sha256", runtime_sha256_},
        {"runtime_build_id", runtime_build_id_},
        {"canary_tier", "C"},
        {"successful_runs", 2},
        {"canary_attestation_sha256", canary_hashes},
    };
    receipt_path_ = temporary_root_ / "approvals" / approval_filename;
    WriteJson(receipt_path_, receipt, 0444);

    const std::filesystem::path invalid_directory =
        temporary_root_ / "invalid-runtime";
    CopyApprovalSet(invalid_directory, &invalid_receipt_path_);
    nlohmann::json invalid_receipt = receipt;
    invalid_receipt["runtime_build_id"] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    ASSERT_EQ(chmod(invalid_receipt_path_.c_str(), 0600), 0);
    WriteJson(invalid_receipt_path_, invalid_receipt, 0444);
  }

  static void TearDownTestSuite() {
    if (fixture_handle_ != nullptr) {
      dlclose(fixture_handle_);
    }
    std::error_code code;
    std::filesystem::remove_all(temporary_root_, code);
  }

  static ExternalHostAbiProfileRequest BaseRequest() {
    ExternalHostAbiProfileRequest request;
    request.profile_file = profile_path_.string();
    request.payload_path = payload_path_.string();
    request.compatibility_manifest_path = manifest_path_.string();
    request.expected_build_id = build_id_;
    return request;
  }

  inline static void* fixture_handle_ = nullptr;
  inline static std::filesystem::path temporary_root_;
  inline static std::filesystem::path payload_path_;
  inline static std::filesystem::path profile_path_;
  inline static std::filesystem::path manifest_path_;
  inline static std::filesystem::path receipt_path_;
  inline static std::filesystem::path invalid_receipt_path_;
  inline static std::string payload_id_;
  inline static std::string approval_generation_;
  inline static std::string build_id_;
  inline static std::string payload_sha256_;
  inline static std::string payload_fingerprint_;
  inline static std::string profile_sha256_;
  inline static std::string manifest_sha256_;
  inline static std::string runtime_sha256_;
  inline static std::string runtime_build_id_;
};

TEST_F(ExternalHostAbiProfileLoaderTest, ComputesStandardSha256Vector) {
  const std::filesystem::path vector_path = temporary_root_ / "sha-vector";
  {
    std::ofstream output(vector_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output);
    output << "abc";
  }
  const FileSha256Result result = ComputeFileSha256(vector_path.string());

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.sha256,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_F(ExternalHostAbiProfileLoaderTest,
       RejectsCandidateWithoutResearchCommandLineMarker) {
  ExternalHostAbiProfileRequest request = BaseRequest();
  request.candidate_canary = true;
  request.candidate_process_authorization = true;
  request.explicit_unverified_authorization = false;

  const ExternalHostAbiProfileResult result =
      LoadExternalHostAbiProfile(request);

  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("canary authorization"), std::string::npos);
}

TEST_F(ExternalHostAbiProfileLoaderTest,
       RejectsCandidateWithoutDedicatedProcessMarker) {
  ExternalHostAbiProfileRequest request = BaseRequest();
  request.candidate_canary = true;
  request.explicit_unverified_authorization = true;

  const ExternalHostAbiProfileResult result =
      LoadExternalHostAbiProfile(request);

  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("canary authorization"), std::string::npos);
}

TEST_F(ExternalHostAbiProfileLoaderTest,
       LoadsExactCandidateIntoLookupRegistry) {
  ExternalHostAbiProfileRequest request = BaseRequest();
  request.candidate_canary = true;
  request.candidate_process_authorization = true;
  request.explicit_unverified_authorization = true;

  const ExternalHostAbiProfileResult result =
      LoadExternalHostAbiProfile(request);

  ASSERT_TRUE(result) << result.error;
  ASSERT_NE(result.profile, nullptr);
  EXPECT_EQ(result.profile, FindHostAbiProfile(build_id_));
  EXPECT_EQ(result.payload_sha256, payload_sha256_);
  EXPECT_EQ(result.profile_sha256, profile_sha256_);
}

TEST_F(ExternalHostAbiProfileLoaderTest, AcceptsExactRepeatedTierCReceipt) {
  ExternalHostAbiProfileRequest request = BaseRequest();
  request.approval_receipt_path = receipt_path_.string();

  const ExternalHostAbiProfileResult result =
      LoadExternalHostAbiProfile(request);

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.profile, FindHostAbiProfile(build_id_));
}

TEST_F(ExternalHostAbiProfileLoaderTest,
       RejectsReceiptWithMismatchedCanaryRuntimeEvidence) {
  ExternalHostAbiProfileRequest request = BaseRequest();
  request.approval_receipt_path = invalid_receipt_path_.string();

  const ExternalHostAbiProfileResult result =
      LoadExternalHostAbiProfile(request);

  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("canary attestation"), std::string::npos);
}

TEST_F(ExternalHostAbiProfileLoaderTest,
       AcceptsInternallyConsistentReceiptFromPreviousRuntime) {
  std::filesystem::path receipt;
  CopyApprovalSetWithRuntimeSha256(temporary_root_ / "wrong-runtime-sha",
                                   std::string(64, 'f'), &receipt);
  ExternalHostAbiProfileRequest request = BaseRequest();
  request.approval_receipt_path = receipt.string();

  const ExternalHostAbiProfileResult result =
      LoadExternalHostAbiProfile(request);

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.profile, FindHostAbiProfile(build_id_));
}

TEST_F(ExternalHostAbiProfileLoaderTest, RejectsMissingCanaryAttestation) {
  std::filesystem::path receipt;
  CopyApprovalSet(temporary_root_ / "missing-canary", &receipt);
  const std::filesystem::path second_canary =
      receipt.parent_path() /
      (payload_id_ + "-" + approval_generation_ + ".canary-2.json");
  ASSERT_TRUE(std::filesystem::remove(second_canary));
  ExternalHostAbiProfileRequest request = BaseRequest();
  request.approval_receipt_path = receipt.string();

  const ExternalHostAbiProfileResult result =
      LoadExternalHostAbiProfile(request);

  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("missing an adjacent canary"), std::string::npos);
}

TEST_F(ExternalHostAbiProfileLoaderTest, RejectsTamperedCanaryAttestation) {
  std::filesystem::path receipt;
  CopyApprovalSet(temporary_root_ / "tampered-canary", &receipt);
  const std::filesystem::path first_canary =
      receipt.parent_path() /
      (payload_id_ + "-" + approval_generation_ + ".canary-1.json");
  nlohmann::json tampered = ReadJson(first_canary);
  ASSERT_FALSE(tampered.is_discarded());
  tampered["readiness_log_sha256"] = std::string(64, 'e');
  ASSERT_EQ(chmod(first_canary.c_str(), 0600), 0);
  WriteJson(first_canary, tampered, 0444);
  ExternalHostAbiProfileRequest request = BaseRequest();
  request.approval_receipt_path = receipt.string();

  const ExternalHostAbiProfileResult result =
      LoadExternalHostAbiProfile(request);

  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("attestation hash"), std::string::npos);
}

TEST_F(ExternalHostAbiProfileLoaderTest, RejectsWrongCanaryRunIdentity) {
  std::filesystem::path receipt;
  CopyApprovalSet(temporary_root_ / "wrong-canary-run", &receipt);
  const std::filesystem::path second_canary =
      receipt.parent_path() /
      (payload_id_ + "-" + approval_generation_ + ".canary-2.json");
  nlohmann::json mismatched = ReadJson(second_canary);
  ASSERT_FALSE(mismatched.is_discarded());
  mismatched["run"] = 1;
  ASSERT_EQ(chmod(second_canary.c_str(), 0600), 0);
  WriteJson(second_canary, mismatched, 0444);
  ExternalHostAbiProfileRequest request = BaseRequest();
  request.approval_receipt_path = receipt.string();

  const ExternalHostAbiProfileResult result =
      LoadExternalHostAbiProfile(request);

  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("exact approval evidence"), std::string::npos);
}

TEST_F(ExternalHostAbiProfileLoaderTest, RejectsMismatchedApprovalGeneration) {
  std::filesystem::path receipt;
  CopyApprovalSet(temporary_root_ / "wrong-generation", &receipt);
  nlohmann::json mismatched = ReadJson(receipt);
  ASSERT_FALSE(mismatched.is_discarded());
  mismatched["generation"] = std::string(40, 'f');
  ASSERT_EQ(chmod(receipt.c_str(), 0600), 0);
  WriteJson(receipt, mismatched, 0444);
  ExternalHostAbiProfileRequest request = BaseRequest();
  request.approval_receipt_path = receipt.string();

  const ExternalHostAbiProfileResult result =
      LoadExternalHostAbiProfile(request);

  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("filename does not match"), std::string::npos);
}

TEST_F(ExternalHostAbiProfileLoaderTest, RejectsNonExactSuccessfulRunCount) {
  std::filesystem::path receipt;
  CopyApprovalSet(temporary_root_ / "wrong-run-count", &receipt);
  nlohmann::json mismatched = ReadJson(receipt);
  ASSERT_FALSE(mismatched.is_discarded());
  mismatched["successful_runs"] = 3;
  ASSERT_EQ(chmod(receipt.c_str(), 0600), 0);
  WriteJson(receipt, mismatched, 0444);
  ExternalHostAbiProfileRequest request = BaseRequest();
  request.approval_receipt_path = receipt.string();

  const ExternalHostAbiProfileResult result =
      LoadExternalHostAbiProfile(request);

  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("does not authorize"), std::string::npos);
}

TEST_F(ExternalHostAbiProfileLoaderTest, RejectsWritableCanaryAttestation) {
  std::filesystem::path receipt;
  CopyApprovalSet(temporary_root_ / "writable-canary", &receipt);
  const std::filesystem::path first_canary =
      receipt.parent_path() /
      (payload_id_ + "-" + approval_generation_ + ".canary-1.json");
  ASSERT_EQ(chmod(first_canary.c_str(), 0644), 0);
  ExternalHostAbiProfileRequest request = BaseRequest();
  request.approval_receipt_path = receipt.string();

  const ExternalHostAbiProfileResult result =
      LoadExternalHostAbiProfile(request);

  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("owner-writable"), std::string::npos);
}

}  // namespace
}  // namespace mocktail::compat
