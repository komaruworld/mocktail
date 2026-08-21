#include "compat/host_abi_profile_loader.h"

#include <elf.h>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <sys/stat.h>
#include <unistd.h>

#define JSON_NOEXCEPTION 1
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compat/elf_build_id.h"
#include "mocktail/sha256.h"

namespace mocktail::compat {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumProfileBytes = 1024 * 1024;
constexpr std::size_t kMaximumReceiptBytes = 64 * 1024;
constexpr std::size_t kMaximumInitArrayEntries = 1024 * 1024;
constexpr std::size_t kMaximumSeedAllocationBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumArenaTableSlots = 16 * 1024 * 1024;

class FileDescriptor {
 public:
  explicit FileDescriptor(int value) noexcept : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      close(value_);
    }
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  int get() const noexcept { return value_; }

 private:
  int value_;
};

struct ElfDeleter {
  void operator()(Elf* elf) const noexcept {
    if (elf != nullptr) {
      elf_end(elf);
    }
  }
};

struct LoadSegment {
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
  unsigned int flags = 0;
};

struct OwnedHostAbiProfile {
  HostAbiProfile profile;
  std::string build_id;
  std::array<std::string, kMaxHostBridgeEntries> labels;
  std::string canonical_payload_path;
  std::string payload_sha256;
  std::string profile_sha256;
  std::string reference_build_id;
  std::string reference_payload_sha256;
  std::vector<uintptr_t> derivation_code_rvas;

  void BindViews() {
    profile.elf_build_id = build_id;
    for (std::size_t index = 0; index < profile.bridge_entry_count; ++index) {
      profile.bridge_entries[index].label = labels[index];
    }
  }
};

std::mutex g_external_profiles_mutex;
std::vector<std::unique_ptr<OwnedHostAbiProfile>> g_external_profiles;

ExternalHostAbiProfileResult Failure(std::string error) {
  ExternalHostAbiProfileResult result;
  result.error = std::move(error);
  return result;
}

std::string Lowercase(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char character) {
                   if (character >= 'A' && character <= 'F') {
                     return static_cast<char>(character - 'A' + 'a');
                   }
                   return static_cast<char>(character);
                 });
  return result;
}

bool IsLowerHex(std::string_view value, std::size_t size) {
  if (value.size() != size) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

bool ReadSmallRegularFile(const std::string& path, std::size_t maximum_bytes,
                          bool require_immutable, std::string* contents,
                          std::string* error) {
  struct stat status{};
  if (lstat(path.c_str(), &status) != 0) {
    *error = "lstat(" + path + "): " + std::strerror(errno);
    return false;
  }
  if (!S_ISREG(status.st_mode)) {
    *error = "not a regular non-symlink file: " + path;
    return false;
  }
  if (status.st_uid != geteuid()) {
    *error = "file is not owned by the current user: " + path;
    return false;
  }
  if ((status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    *error = "file is group- or world-writable: " + path;
    return false;
  }
  if (require_immutable && (status.st_mode & S_IWUSR) != 0) {
    *error = "approval evidence is owner-writable: " + path;
    return false;
  }
  if (status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > maximum_bytes) {
    *error = "file exceeds the accepted size limit: " + path;
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *error = "cannot open file: " + path;
    return false;
  }
  contents->assign(std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>());
  if (input.bad()) {
    *error = "cannot read file: " + path;
    return false;
  }
  return true;
}

std::optional<std::string> CanonicalRegularPath(const std::string& path,
                                                std::string* error) {
  if (path.empty() || !std::filesystem::path(path).is_absolute()) {
    *error = "path must be absolute: " + path;
    return std::nullopt;
  }
  struct stat link_status{};
  if (lstat(path.c_str(), &link_status) != 0) {
    *error = "lstat(" + path + "): " + std::strerror(errno);
    return std::nullopt;
  }
  if (S_ISLNK(link_status.st_mode)) {
    *error = "symlink paths are not accepted: " + path;
    return std::nullopt;
  }
  std::error_code code;
  const std::filesystem::path canonical =
      std::filesystem::canonical(path, code);
  if (code) {
    *error = "cannot canonicalize " + path + ": " + code.message();
    return std::nullopt;
  }
  const std::filesystem::file_status status =
      std::filesystem::status(canonical, code);
  if (code || !std::filesystem::is_regular_file(status)) {
    *error = "canonical path is not a regular file: " + canonical.string();
    return std::nullopt;
  }
  return canonical.string();
}

const Json* RequiredField(const Json& object, const char* name) {
  const auto field = object.find(name);
  return field != object.end() ? &*field : nullptr;
}

bool ReadRequiredString(const Json& object, const char* name,
                        std::string* value) {
  const Json* field = RequiredField(object, name);
  if (field == nullptr || !field->is_string()) {
    return false;
  }
  *value = field->get_ref<const std::string&>();
  return true;
}

bool ReadSize(const Json& value, std::size_t maximum, std::size_t* result) {
  std::uint64_t parsed = 0;
  if (value.is_number_unsigned()) {
    parsed = value.get<std::uint64_t>();
  } else if (value.is_number_integer()) {
    const std::int64_t signed_value = value.get<std::int64_t>();
    if (signed_value < 0) {
      return false;
    }
    parsed = static_cast<std::uint64_t>(signed_value);
  } else {
    return false;
  }
  if (parsed > maximum || parsed > std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  *result = static_cast<std::size_t>(parsed);
  return true;
}

bool ReadRequiredSize(const Json& object, const char* name, std::size_t maximum,
                      std::size_t* result) {
  const Json* field = RequiredField(object, name);
  return field != nullptr && ReadSize(*field, maximum, result);
}

bool ReadHexValue(const Json& object, const char* name, std::uint64_t maximum,
                  std::uint64_t* result) {
  const Json* field = RequiredField(object, name);
  if (field == nullptr || !field->is_string()) {
    return false;
  }
  const std::string& encoded = field->get_ref<const std::string&>();
  if (encoded.size() <= 2 || encoded[0] != '0' || encoded[1] != 'x') {
    return false;
  }
  std::uint64_t value = 0;
  const std::from_chars_result parsed = std::from_chars(
      encoded.data() + 2, encoded.data() + encoded.size(), value, 16);
  if (parsed.ec != std::errc() ||
      parsed.ptr != encoded.data() + encoded.size() || value == 0 ||
      value > maximum) {
    return false;
  }
  *result = value;
  return true;
}

bool ReadRva(const Json& object, const char* name, uintptr_t* result) {
  std::uint64_t value = 0;
  if (!ReadHexValue(object, name, std::numeric_limits<std::uintptr_t>::max(),
                    &value)) {
    return false;
  }
  *result = static_cast<std::uintptr_t>(value);
  return true;
}

std::optional<HostBridgeKind> ParseBridgeKind(std::string_view kind) {
  if (kind == "allocate") {
    return HostBridgeKind::kAllocate;
  }
  if (kind == "reallocate") {
    return HostBridgeKind::kReallocate;
  }
  if (kind == "aligned_allocate") {
    return HostBridgeKind::kAlignedAllocate;
  }
  if (kind == "free") {
    return HostBridgeKind::kFree;
  }
  if (kind == "usable_size") {
    return HostBridgeKind::kUsableSize;
  }
  return std::nullopt;
}

std::optional<HostBridgeKind> ExpectedKindForLabel(std::string_view label) {
  if (label == "small-allocate" || label == "allocate") {
    return HostBridgeKind::kAllocate;
  }
  if (label == "usable-size") {
    return HostBridgeKind::kUsableSize;
  }
  if (label == "reallocate") {
    return HostBridgeKind::kReallocate;
  }
  if (label == "aligned-allocate-direct") {
    return HostBridgeKind::kAlignedAllocate;
  }
  if (label == "free") {
    return HostBridgeKind::kFree;
  }
  return std::nullopt;
}

bool IsSafeLabel(std::string_view label) {
  return !label.empty() && label.size() <= 64 &&
         std::all_of(label.begin(), label.end(), [](char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') || character == '-' ||
                  character == '_';
         });
}

bool ParseRanges(const Json& object, const char* name, std::size_t init_count,
                 std::array<ConstructorRange, kMaxConstructorRunRanges>* ranges,
                 std::size_t* count) {
  const Json* encoded = RequiredField(object, name);
  if (encoded == nullptr || !encoded->is_array() || encoded->empty() ||
      encoded->size() > ranges->size()) {
    return false;
  }
  *count = 0;
  for (const Json& item : *encoded) {
    if (!item.is_object()) {
      return false;
    }
    ConstructorRange range;
    if (!ReadRequiredSize(item, "begin", init_count, &range.begin) ||
        !ReadRequiredSize(item, "end_exclusive", init_count,
                          &range.end_exclusive)) {
      return false;
    }
    (*ranges)[(*count)++] = range;
  }
  return true;
}

std::unique_ptr<OwnedHostAbiProfile> ParseProfile(const Json& root,
                                                  std::string* error) {
  const Json* schema = RequiredField(root, "schema_version");
  const Json* reference = RequiredField(root, "reference");
  const Json* derivation = RequiredField(root, "derivation_anchors");
  const Json* profile_json = RequiredField(root, "profile");
  std::size_t schema_version = 0;
  if (!root.is_object() || schema == nullptr ||
      !ReadSize(*schema, 1, &schema_version) || schema_version != 1 ||
      reference == nullptr || !reference->is_object() || reference->empty() ||
      derivation == nullptr || !derivation->is_object() ||
      derivation->empty() || profile_json == nullptr ||
      !profile_json->is_object()) {
    *error = "unsupported external host ABI profile schema";
    return nullptr;
  }

  auto owned = std::make_unique<OwnedHostAbiProfile>();
  std::string nested_build_id;
  if (!ReadRequiredString(root, "elf_build_id", &owned->build_id) ||
      !ReadRequiredString(*profile_json, "elf_build_id", &nested_build_id) ||
      !IsValidBuildId(owned->build_id) ||
      owned->build_id != Lowercase(owned->build_id) ||
      nested_build_id != owned->build_id) {
    *error = "external host ABI profile has an invalid exact Build ID";
    return nullptr;
  }

  if (!ReadRequiredString(*reference, "elf_build_id",
                          &owned->reference_build_id) ||
      !ReadRequiredString(*reference, "payload_sha256",
                          &owned->reference_payload_sha256) ||
      !IsValidBuildId(owned->reference_build_id) ||
      owned->reference_build_id != Lowercase(owned->reference_build_id) ||
      !IsLowerHex(owned->reference_payload_sha256, 64)) {
    *error = "external host ABI profile has invalid reference identity";
    return nullptr;
  }
  std::size_t signature_version = 0;
  const Json* encoded_signature_version =
      RequiredField(*derivation, "signature_version");
  if (encoded_signature_version == nullptr ||
      !ReadSize(*encoded_signature_version, 1, &signature_version) ||
      signature_version != 1) {
    *error = "external host ABI profile has unsupported derivation anchors";
    return nullptr;
  }
  for (const char* name :
       {"allocator_object_initializer_rva", "empty_string_initializer_rva",
        "jni_singleton_initializer_rva"}) {
    uintptr_t rva = 0;
    if (!ReadRva(*derivation, name, &rva)) {
      *error = "external host ABI profile is missing a derivation anchor";
      return nullptr;
    }
    owned->derivation_code_rvas.push_back(rva);
  }
  const Json* constructor_anchors =
      RequiredField(*derivation, "constructor_rvas");
  if (constructor_anchors == nullptr || !constructor_anchors->is_object()) {
    *error = "external host ABI profile has invalid constructor anchors";
    return nullptr;
  }
  for (const char* index : {"2", "3", "4", "5"}) {
    uintptr_t rva = 0;
    if (!ReadRva(*constructor_anchors, index, &rva)) {
      *error = "external host ABI profile is missing a constructor anchor";
      return nullptr;
    }
    owned->derivation_code_rvas.push_back(rva);
  }

  const Json* bridge_entries = RequiredField(*profile_json, "bridge_entries");
  if (bridge_entries == nullptr || !bridge_entries->is_array() ||
      bridge_entries->size() != 6) {
    *error = "external host ABI profile has an invalid bridge table";
    return nullptr;
  }
  std::array<bool, 5> observed_kinds{};
  std::vector<uintptr_t> observed_rvas;
  std::vector<std::string> observed_labels;
  uintptr_t primary_allocate_rva = 0;
  uintptr_t free_rva = 0;
  for (const Json& entry : *bridge_entries) {
    std::string kind_name;
    std::string label;
    uintptr_t rva = 0;
    if (!entry.is_object() || !ReadRva(entry, "rva", &rva) ||
        !ReadRequiredString(entry, "kind", &kind_name) ||
        !ReadRequiredString(entry, "label", &label) || !IsSafeLabel(label)) {
      *error = "external host ABI profile has an invalid bridge entry";
      return nullptr;
    }
    const std::optional<HostBridgeKind> kind = ParseBridgeKind(kind_name);
    const std::optional<HostBridgeKind> expected_kind =
        ExpectedKindForLabel(label);
    if (!kind.has_value() || !expected_kind.has_value() ||
        *kind != *expected_kind ||
        std::find(observed_rvas.begin(), observed_rvas.end(), rva) !=
            observed_rvas.end() ||
        std::find(observed_labels.begin(), observed_labels.end(), label) !=
            observed_labels.end()) {
      *error = "external host ABI profile has an unknown or duplicate bridge";
      return nullptr;
    }
    const std::size_t index = owned->profile.bridge_entry_count++;
    owned->labels[index] = std::move(label);
    owned->profile.bridge_entries[index] = {rva, *kind, {}};
    observed_rvas.push_back(rva);
    observed_labels.push_back(owned->labels[index]);
    observed_kinds[static_cast<std::size_t>(*kind)] = true;
    if (owned->labels[index] == "allocate") {
      primary_allocate_rva = rva;
    } else if (owned->labels[index] == "free") {
      free_rva = rva;
    }
  }
  if (!std::all_of(observed_kinds.begin(), observed_kinds.end(),
                   [](bool observed) { return observed; })) {
    *error = "external host ABI profile is missing a required bridge kind";
    return nullptr;
  }

  const Json* seeds = RequiredField(*profile_json, "data_seeds");
  if (seeds == nullptr || !seeds->is_object() ||
      !ReadRva(*seeds, "allocator_object_slot",
               &owned->profile.data_seeds.allocator_object_slot) ||
      !ReadRva(*seeds, "empty_string_slot",
               &owned->profile.data_seeds.empty_string_slot) ||
      !ReadRva(*seeds, "jni_singleton_slot",
               &owned->profile.data_seeds.jni_singleton_slot) ||
      !ReadRequiredSize(*seeds, "jni_singleton_bytes",
                        kMaximumSeedAllocationBytes,
                        &owned->profile.data_seeds.jni_singleton_bytes) ||
      !ReadRva(*seeds, "arena_initializer",
               &owned->profile.data_seeds.arena_initializer) ||
      !ReadRva(*seeds, "allocator_thread_initializer",
               &owned->profile.data_seeds.allocator_thread_initializer) ||
      !ReadRva(*seeds, "arena_guard_slot",
               &owned->profile.data_seeds.arena_guard_slot) ||
      !ReadRva(*seeds, "arena_table_slot",
               &owned->profile.data_seeds.arena_table_slot) ||
      !ReadRequiredSize(*seeds, "arena_table_slot_count",
                        kMaximumArenaTableSlots,
                        &owned->profile.data_seeds.arena_table_slot_count)) {
    *error = "external host ABI profile has invalid data seeds";
    return nullptr;
  }
  if (owned->profile.data_seeds.jni_singleton_bytes != 0x400 ||
      owned->profile.data_seeds.arena_table_slot_count != 0x400000) {
    *error = "external host ABI profile changed fixed seed allocation sizes";
    return nullptr;
  }

  const Json* allocator = RequiredField(*profile_json, "native_allocator");
  const Json* bootstrap =
      RequiredField(*profile_json, "native_pre_jni_bootstrap");
  if (allocator == nullptr || !allocator->is_object() ||
      !ReadRva(*allocator, "allocate",
               &owned->profile.native_allocator.allocate) ||
      !ReadRva(*allocator, "deallocate",
               &owned->profile.native_allocator.deallocate) ||
      bootstrap == nullptr || !bootstrap->is_object() ||
      !ReadRva(*bootstrap, "registry_initializer",
               &owned->profile.native_pre_jni_bootstrap.registry_initializer) ||
      !ReadRva(*bootstrap, "registry_slot",
               &owned->profile.native_pre_jni_bootstrap.registry_slot) ||
      !ReadRva(*profile_json, "init_array_offset",
               &owned->profile.init_array_offset) ||
      !ReadRequiredSize(*profile_json, "init_array_count",
                        kMaximumInitArrayEntries,
                        &owned->profile.init_array_count) ||
      owned->profile.init_array_count == 0 ||
      !ParseRanges(*profile_json, "constructor_run_ranges",
                   owned->profile.init_array_count,
                   &owned->profile.constructor_run_ranges,
                   &owned->profile.constructor_run_range_count) ||
      !ParseRanges(
          *profile_json, "native_mimalloc_constructor_run_ranges",
          owned->profile.init_array_count,
          &owned->profile.native_mimalloc_constructor_run_ranges,
          &owned->profile.native_mimalloc_constructor_run_range_count) ||
      !ReadRequiredSize(
          *profile_json, "native_mimalloc_thread_initializer_after_constructor",
          owned->profile.init_array_count - 1,
          &owned->profile
               .native_mimalloc_thread_initializer_after_constructor)) {
    *error = "external host ABI profile has invalid native bootstrap fields";
    return nullptr;
  }
  if (owned->profile.native_allocator.allocate != primary_allocate_rva ||
      owned->profile.native_allocator.deallocate != free_rva) {
    *error =
        "external host ABI profile native allocator disagrees with bridges";
    return nullptr;
  }
  std::array<uintptr_t, 6> writable_slots = {
      owned->profile.data_seeds.allocator_object_slot,
      owned->profile.data_seeds.empty_string_slot,
      owned->profile.data_seeds.jni_singleton_slot,
      owned->profile.data_seeds.arena_guard_slot,
      owned->profile.data_seeds.arena_table_slot,
      owned->profile.native_pre_jni_bootstrap.registry_slot,
  };
  if (std::any_of(writable_slots.begin(), writable_slots.end(),
                  [](uintptr_t rva) { return rva % alignof(void*) != 0; })) {
    *error = "external host ABI profile contains an unaligned writable slot";
    return nullptr;
  }
  std::sort(writable_slots.begin(), writable_slots.end());
  if (std::adjacent_find(writable_slots.begin(), writable_slots.end()) !=
      writable_slots.end()) {
    *error = "external host ABI profile aliases writable state slots";
    return nullptr;
  }

  std::string strategy;
  if (!ReadRequiredString(*profile_json, "default_allocator_strategy",
                          &strategy) ||
      strategy != "native_mimalloc") {
    *error = "external host ABI profile must retain native mimalloc";
    return nullptr;
  }
  owned->profile.default_allocator_strategy =
      HostAllocatorStrategy::kNativeMimalloc;
  owned->BindViews();
  if (!owned->profile.native_allocator.IsValid() ||
      !owned->profile.HasValidConstructorRanges() ||
      !owned->profile.HasValidNativeMimallocConstructorRanges() ||
      !owned->profile.HasValidNativePreJniBootstrap() ||
      !owned->profile.ShouldInitializeNativeMimallocThreadAfterConstructor(
          owned->profile
              .native_mimalloc_thread_initializer_after_constructor)) {
    *error = "external host ABI profile has inconsistent constructor policy";
    return nullptr;
  }
  if (owned->profile.init_array_count <= 5 ||
      owned->profile.constructor_run_range_count != 2 ||
      owned->profile.constructor_run_ranges[0].begin != 2 ||
      owned->profile.constructor_run_ranges[0].end_exclusive != 3 ||
      owned->profile.constructor_run_ranges[1].begin != 5 ||
      owned->profile.constructor_run_ranges[1].end_exclusive !=
          owned->profile.init_array_count ||
      owned->profile.native_mimalloc_constructor_run_range_count != 1 ||
      owned->profile.native_mimalloc_constructor_run_ranges[0].begin != 2 ||
      owned->profile.native_mimalloc_constructor_run_ranges[0].end_exclusive !=
          owned->profile.init_array_count ||
      owned->profile.native_mimalloc_thread_initializer_after_constructor !=
          2) {
    *error =
        "external host ABI profile changed the verified constructor policy";
    return nullptr;
  }
  return owned;
}

bool RangeInSegment(const std::vector<LoadSegment>& segments,
                    std::uint64_t begin, std::uint64_t size,
                    unsigned int required_flags, unsigned int forbidden_flags) {
  if (size == 0 || begin > std::numeric_limits<std::uint64_t>::max() - size) {
    return false;
  }
  const std::uint64_t end = begin + size;
  return std::any_of(segments.begin(), segments.end(),
                     [begin, end, required_flags,
                      forbidden_flags](const LoadSegment& segment) {
                       return (segment.flags & required_flags) ==
                                  required_flags &&
                              (segment.flags & forbidden_flags) == 0 &&
                              segment.begin <= begin && end <= segment.end;
                     });
}

bool ValidateProfileAgainstElf(const std::string& payload_path,
                               const HostAbiProfile& profile,
                               const std::vector<uintptr_t>& derivation_rvas,
                               std::string* error) {
  if (elf_version(EV_CURRENT) == EV_NONE) {
    *error = "libelf initialization failed";
    return false;
  }
  FileDescriptor descriptor(open(payload_path.c_str(), O_RDONLY | O_CLOEXEC));
  if (descriptor.get() < 0) {
    *error = "cannot open payload ELF: " + std::string(std::strerror(errno));
    return false;
  }
  std::unique_ptr<Elf, ElfDeleter> elf(
      elf_begin(descriptor.get(), ELF_C_READ, nullptr));
  if (!elf || elf_kind(elf.get()) != ELF_K_ELF) {
    *error = "payload is not a readable ELF object";
    return false;
  }
  GElf_Ehdr header{};
  if (gelf_getehdr(elf.get(), &header) == nullptr ||
      gelf_getclass(elf.get()) != ELFCLASS64 || header.e_machine != EM_X86_64 ||
      header.e_type != ET_DYN) {
    *error = "payload must be an x86-64 ET_DYN ELF object";
    return false;
  }

  std::size_t program_count = 0;
  if (elf_getphdrnum(elf.get(), &program_count) != 0) {
    *error = "cannot enumerate payload ELF program headers";
    return false;
  }
  std::vector<LoadSegment> segments;
  for (std::size_t index = 0; index < program_count; ++index) {
    GElf_Phdr program{};
    if (gelf_getphdr(elf.get(), static_cast<int>(index), &program) == nullptr) {
      *error = "cannot read payload ELF program header";
      return false;
    }
    if (program.p_type == PT_LOAD && program.p_memsz > 0 &&
        program.p_vaddr <=
            std::numeric_limits<std::uint64_t>::max() - program.p_memsz) {
      segments.push_back({program.p_vaddr, program.p_vaddr + program.p_memsz,
                          program.p_flags});
    }
  }

  std::vector<uintptr_t> code_rvas = {
      profile.data_seeds.arena_initializer,
      profile.data_seeds.allocator_thread_initializer,
      profile.native_allocator.allocate,
      profile.native_allocator.deallocate,
      profile.native_pre_jni_bootstrap.registry_initializer,
  };
  for (std::size_t index = 0; index < profile.bridge_entry_count; ++index) {
    code_rvas.push_back(profile.bridge_entries[index].rva);
  }
  code_rvas.insert(code_rvas.end(), derivation_rvas.begin(),
                   derivation_rvas.end());
  for (const uintptr_t rva : code_rvas) {
    if (!RangeInSegment(segments, rva, 1, PF_R | PF_X, PF_W)) {
      *error =
          "host ABI profile contains a code RVA outside executable ELF "
          "segments";
      return false;
    }
  }

  const std::array<uintptr_t, 6> data_rvas = {
      profile.data_seeds.allocator_object_slot,
      profile.data_seeds.empty_string_slot,
      profile.data_seeds.jni_singleton_slot,
      profile.data_seeds.arena_guard_slot,
      profile.data_seeds.arena_table_slot,
      profile.native_pre_jni_bootstrap.registry_slot,
  };
  for (const uintptr_t rva : data_rvas) {
    if (!RangeInSegment(segments, rva, sizeof(void*), PF_R | PF_W, PF_X)) {
      *error =
          "host ABI profile contains a data RVA outside writable ELF segments";
      return false;
    }
  }

  std::size_t section_count = 0;
  if (elf_getshdrnum(elf.get(), &section_count) != 0) {
    *error = "cannot enumerate payload ELF sections";
    return false;
  }
  bool exact_init_array = false;
  for (std::size_t index = 0; index < section_count; ++index) {
    Elf_Scn* section = elf_getscn(elf.get(), index);
    GElf_Shdr section_header{};
    if (section == nullptr ||
        gelf_getshdr(section, &section_header) == nullptr) {
      *error = "cannot read payload ELF section";
      return false;
    }
    if (section_header.sh_type == SHT_INIT_ARRAY &&
        section_header.sh_addr == profile.init_array_offset &&
        (section_header.sh_entsize == 0 ||
         section_header.sh_entsize == sizeof(std::uint64_t)) &&
        section_header.sh_size ==
            profile.init_array_count * sizeof(std::uint64_t) &&
        (section_header.sh_flags & (SHF_ALLOC | SHF_WRITE)) ==
            (SHF_ALLOC | SHF_WRITE) &&
        (section_header.sh_flags & SHF_EXECINSTR) == 0) {
      exact_init_array = true;
      break;
    }
  }
  if (!exact_init_array) {
    *error = "host ABI profile does not match the payload .init_array";
    return false;
  }
  return true;
}

bool ParseJsonFile(const std::string& path, std::size_t maximum_bytes,
                   bool require_immutable, Json* json, std::string* bytes,
                   std::string* error) {
  if (!ReadSmallRegularFile(path, maximum_bytes, require_immutable, bytes,
                            error)) {
    return false;
  }
  *json = Json::parse(*bytes, nullptr, false);
  if (json->is_discarded() || !json->is_object()) {
    *error = "invalid JSON object: " + path;
    return false;
  }
  return true;
}

bool ValidatePayloadId(std::string_view payload_id, std::string_view build_id) {
  const std::size_t separator = payload_id.find('-');
  if (!IsLowerHex(build_id, 40) || separator == 0 ||
      separator == std::string_view::npos ||
      payload_id.substr(separator + 1) != build_id) {
    return false;
  }
  return std::all_of(
      payload_id.begin(), payload_id.begin() + separator,
      [](char character) { return character >= '0' && character <= '9'; });
}

bool ValidateStoredPayloadPath(std::string_view encoded_path,
                               std::string_view payload_id,
                               const std::filesystem::path& canonical_library) {
  if (encoded_path != "payloads/" + std::string(payload_id) ||
      canonical_library.filename() != "libroblox.so") {
    return false;
  }
  const std::filesystem::path payload_directory =
      canonical_library.parent_path();
  return payload_directory.filename() == std::string(payload_id) &&
         payload_directory.parent_path().filename() == "payloads";
}

bool IsDecimal(std::string_view value) {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](char character) {
           return character >= '0' && character <= '9';
         });
}

bool ValidateCanaryRunId(std::string_view run_id, std::string_view payload_id,
                         std::size_t run) {
  const std::string prefix =
      std::string(payload_id) + "-" + std::to_string(run) + "-";
  if (run_id.size() <= prefix.size() ||
      run_id.substr(0, prefix.size()) != prefix) {
    return false;
  }
  const std::string_view suffix = run_id.substr(prefix.size());
  const std::size_t separator = suffix.find('-');
  return separator != std::string_view::npos &&
         suffix.find('-', separator + 1) == std::string_view::npos &&
         IsDecimal(suffix.substr(0, separator)) &&
         IsDecimal(suffix.substr(separator + 1));
}

struct CanaryEvidence {
  std::string sha256;
  std::string run_id;
};

bool ValidateCanaryAttestation(const std::string& attestation_path,
                               std::size_t expected_run,
                               std::string_view payload_id,
                               std::string_view payload_fingerprint,
                               std::string_view profile_sha256,
                               std::string_view compatibility_manifest_sha256,
                               std::string_view runtime_sha256,
                               std::string_view runtime_build_id,
                               CanaryEvidence* evidence, std::string* error) {
  Json attestation;
  std::string attestation_bytes;
  if (!ParseJsonFile(attestation_path, kMaximumReceiptBytes, true, &attestation,
                     &attestation_bytes, error)) {
    return false;
  }

  std::size_t schema_version = 0;
  std::size_t run = 0;
  std::string status;
  std::string canary_tier;
  std::string run_id;
  std::string attestation_payload_id;
  std::string attestation_payload_fingerprint;
  std::string attestation_profile_sha256;
  std::string attestation_manifest_sha256;
  std::string attestation_runtime_sha256;
  std::string attestation_runtime_build_id;
  std::string readiness_log_sha256;
  const Json* schema = RequiredField(attestation, "schema_version");
  if (schema == nullptr || !ReadSize(*schema, 1, &schema_version) ||
      schema_version != 1 ||
      !ReadRequiredString(attestation, "status", &status) ||
      status != "passed" ||
      !ReadRequiredString(attestation, "canary_tier", &canary_tier) ||
      canary_tier != "C" || !ReadRequiredSize(attestation, "run", 2, &run) ||
      run != expected_run ||
      !ReadRequiredString(attestation, "run_id", &run_id) ||
      !ValidateCanaryRunId(run_id, payload_id, expected_run) ||
      !ReadRequiredString(attestation, "payload_id", &attestation_payload_id) ||
      attestation_payload_id != payload_id ||
      !ReadRequiredString(attestation, "payload_fingerprint",
                          &attestation_payload_fingerprint) ||
      attestation_payload_fingerprint != payload_fingerprint ||
      !ReadRequiredString(attestation, "profile_sha256",
                          &attestation_profile_sha256) ||
      attestation_profile_sha256 != profile_sha256 ||
      !ReadRequiredString(attestation, "compatibility_manifest_sha256",
                          &attestation_manifest_sha256) ||
      attestation_manifest_sha256 != compatibility_manifest_sha256 ||
      !ReadRequiredString(attestation, "runtime_sha256",
                          &attestation_runtime_sha256) ||
      attestation_runtime_sha256 != runtime_sha256 ||
      !ReadRequiredString(attestation, "runtime_build_id",
                          &attestation_runtime_build_id) ||
      attestation_runtime_build_id != runtime_build_id ||
      !ReadRequiredString(attestation, "readiness_log_sha256",
                          &readiness_log_sha256) ||
      !IsLowerHex(readiness_log_sha256, 64)) {
    *error = "canary attestation does not match the exact approval evidence";
    return false;
  }

  evidence->sha256 = foundation::ComputeSha256Hex(attestation_bytes);
  evidence->run_id = std::move(run_id);
  return true;
}

bool ValidateReceipt(const std::string& receipt_path,
                     const std::string& canonical_payload_path,
                     const std::string& payload_sha256,
                     const std::string& profile_sha256,
                     const std::string& payload_id, const std::string& build_id,
                     const std::string& compatibility_manifest_path,
                     std::string* error) {
  Json receipt;
  std::string receipt_bytes;
  if (!ParseJsonFile(receipt_path, kMaximumReceiptBytes, true, &receipt,
                     &receipt_bytes, error)) {
    return false;
  }
  std::size_t schema_version = 0;
  std::size_t successful_runs = 0;
  std::string status;
  std::string receipt_payload_id;
  std::string generation;
  std::string receipt_build_id;
  std::string receipt_payload_path;
  std::string receipt_payload_sha256;
  std::string receipt_profile_sha256;
  std::string receipt_runtime_build_id;
  std::string canary_runtime_sha256;
  std::string receipt_runtime_sha256;
  std::string receipt_manifest_sha256;
  std::string payload_fingerprint;
  std::string canary_tier;
  const Json* schema = RequiredField(receipt, "schema_version");
  if (schema == nullptr || !ReadSize(*schema, 1, &schema_version) ||
      schema_version != 1 || !ReadRequiredString(receipt, "status", &status) ||
      status != "approved" ||
      !ReadRequiredString(receipt, "payload_id", &receipt_payload_id) ||
      receipt_payload_id != payload_id ||
      !ReadRequiredString(receipt, "generation", &generation) ||
      !IsLowerHex(generation, 40) ||
      !ReadRequiredString(receipt, "elf_build_id", &receipt_build_id) ||
      !ReadRequiredString(receipt, "payload_path", &receipt_payload_path) ||
      !ReadRequiredString(receipt, "payload_sha256", &receipt_payload_sha256) ||
      !ReadRequiredString(receipt, "profile_sha256", &receipt_profile_sha256) ||
      !ReadRequiredString(receipt, "runtime_build_id",
                          &receipt_runtime_build_id) ||
      !ReadRequiredString(receipt, "canary_runtime_sha256",
                          &canary_runtime_sha256) ||
      !ReadRequiredString(receipt, "runtime_sha256", &receipt_runtime_sha256) ||
      !ReadRequiredString(receipt, "compatibility_manifest_sha256",
                          &receipt_manifest_sha256) ||
      !ReadRequiredString(receipt, "payload_fingerprint",
                          &payload_fingerprint) ||
      !ReadRequiredString(receipt, "canary_tier", &canary_tier) ||
      !ReadRequiredSize(receipt, "successful_runs", 2, &successful_runs) ||
      successful_runs != 2 || canary_tier != "C" ||
      receipt_build_id != build_id ||
      receipt_payload_path != canonical_payload_path ||
      receipt_payload_sha256 != payload_sha256 ||
      receipt_profile_sha256 != profile_sha256 ||
      !IsLowerHex(receipt_runtime_build_id, 40) ||
      !IsLowerHex(canary_runtime_sha256, 64) ||
      receipt_runtime_sha256 != canary_runtime_sha256 ||
      !IsLowerHex(payload_fingerprint, 64)) {
    *error =
        "approval receipt does not authorize the exact ABI profile and payload";
    return false;
  }

  const std::filesystem::path receipt_file(receipt_path);
  const std::string evidence_prefix = payload_id + "-" + generation;
  if (receipt_file.filename() != evidence_prefix + ".json") {
    *error = "approval receipt filename does not match its evidence generation";
    return false;
  }

  const FileSha256Result manifest_hash =
      ComputeFileSha256(compatibility_manifest_path);
  if (!manifest_hash) {
    *error = "cannot hash compatibility manifest for approval receipt";
    return false;
  }
  if (receipt_manifest_sha256 != manifest_hash.sha256) {
    *error = "approval receipt belongs to different compatibility bytes";
    return false;
  }

  std::array<CanaryEvidence, 2> canary_evidence;
  for (std::size_t index = 0; index < canary_evidence.size(); ++index) {
    const std::string filename =
        evidence_prefix + ".canary-" + std::to_string(index + 1) + ".json";
    const std::filesystem::path candidate =
        receipt_file.parent_path() / filename;
    std::string path_error;
    const std::optional<std::string> canonical_attestation =
        CanonicalRegularPath(candidate.string(), &path_error);
    if (!canonical_attestation.has_value() ||
        std::filesystem::path(*canonical_attestation).parent_path() !=
            receipt_file.parent_path() ||
        std::filesystem::path(*canonical_attestation).filename() != filename) {
      *error = "approval receipt is missing an adjacent canary attestation";
      return false;
    }
    if (!ValidateCanaryAttestation(
            *canonical_attestation, index + 1, payload_id, payload_fingerprint,
            profile_sha256, manifest_hash.sha256, canary_runtime_sha256,
            receipt_runtime_build_id, &canary_evidence[index], error)) {
      return false;
    }
  }
  if (canary_evidence[0].run_id == canary_evidence[1].run_id) {
    *error = "approval receipt canary attestations reuse one run ID";
    return false;
  }

  const Json* attestation_hashes =
      RequiredField(receipt, "canary_attestation_sha256");
  if (attestation_hashes == nullptr || !attestation_hashes->is_array() ||
      attestation_hashes->size() != canary_evidence.size()) {
    *error = "approval receipt must bind exactly two canary attestations";
    return false;
  }
  for (std::size_t index = 0; index < canary_evidence.size(); ++index) {
    const Json& encoded_hash = (*attestation_hashes)[index];
    if (!encoded_hash.is_string() ||
        encoded_hash.get_ref<const std::string&>() !=
            canary_evidence[index].sha256) {
      *error = "approval receipt canary attestation hash does not match";
      return false;
    }
  }

  std::string generation_evidence;
  generation_evidence.append("runtime_build_id=")
      .append(receipt_runtime_build_id)
      .append("\n");
  generation_evidence.append("payload=")
      .append(payload_fingerprint)
      .append("\n");
  generation_evidence.append("profile=").append(profile_sha256).append("\n");
  generation_evidence.append("compatibility=")
      .append(manifest_hash.sha256)
      .append("\n");
  generation_evidence.append("canary_1=")
      .append(canary_evidence[0].sha256)
      .append("\n");
  generation_evidence.append("canary_2=")
      .append(canary_evidence[1].sha256)
      .append("\n");
  const std::string expected_generation =
      foundation::ComputeSha256Hex(generation_evidence).substr(0, 40);
  if (generation != expected_generation) {
    *error = "approval receipt evidence generation does not match";
    return false;
  }
  return true;
}

const OwnedHostAbiProfile* FindLoadedProfile(std::string_view build_id) {
  std::lock_guard<std::mutex> lock(g_external_profiles_mutex);
  for (const std::unique_ptr<OwnedHostAbiProfile>& profile :
       g_external_profiles) {
    if (profile->build_id == build_id) {
      return profile.get();
    }
  }
  return nullptr;
}

ExternalHostAbiProfileResult RegisterProfile(
    std::unique_ptr<OwnedHostAbiProfile> profile) {
  std::lock_guard<std::mutex> lock(g_external_profiles_mutex);
  for (const std::unique_ptr<OwnedHostAbiProfile>& existing :
       g_external_profiles) {
    if (existing->build_id != profile->build_id) {
      continue;
    }
    if (existing->canonical_payload_path == profile->canonical_payload_path &&
        existing->payload_sha256 == profile->payload_sha256 &&
        existing->profile_sha256 == profile->profile_sha256) {
      return {&existing->profile,
              existing->payload_sha256,
              existing->profile_sha256,
              {}};
    }
    return Failure(
        "a different external ABI profile is already loaded for this Build ID");
  }
  profile->BindViews();
  const HostAbiProfile* registered = &profile->profile;
  const std::string payload_sha256 = profile->payload_sha256;
  const std::string profile_sha256 = profile->profile_sha256;
  g_external_profiles.push_back(std::move(profile));
  return {registered, payload_sha256, profile_sha256, {}};
}

}  // namespace

FileSha256Result ComputeFileSha256(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {{}, "cannot open file for SHA-256: " + path};
  }
  foundation::Sha256 sha256;
  std::array<unsigned char, 128 * 1024> buffer{};
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
    const std::streamsize read = input.gcount();
    if (read > 0) {
      sha256.Update(buffer.data(), static_cast<std::size_t>(read));
    }
  }
  if (!input.eof()) {
    return {{}, "cannot read file for SHA-256: " + path};
  }
  return {sha256.FinalHex(), {}};
}

const HostAbiProfile* FindLoadedExternalHostAbiProfile(
    std::string_view build_id) noexcept {
  const OwnedHostAbiProfile* loaded = FindLoadedProfile(build_id);
  return loaded != nullptr ? &loaded->profile : nullptr;
}

ExternalHostAbiProfileResult LoadExternalHostAbiProfile(
    const ExternalHostAbiProfileRequest& request) {
  if (request.profile_file.empty() || request.payload_path.empty() ||
      request.expected_build_id.empty()) {
    return Failure("external host ABI profile request is incomplete");
  }
  if (request.candidate_canary) {
    if (!request.candidate_process_authorization ||
        !request.explicit_unverified_authorization ||
        !request.approval_receipt_path.empty()) {
      return Failure(
          "candidate ABI profile requires isolated canary authorization");
    }
  } else if (request.candidate_process_authorization ||
             request.approval_receipt_path.empty()) {
    return Failure("normal ABI profile loading requires an approval receipt");
  }

  std::string path_error;
  const std::optional<std::string> canonical_profile =
      CanonicalRegularPath(request.profile_file, &path_error);
  if (!canonical_profile.has_value()) {
    return Failure(path_error);
  }
  const std::optional<std::string> canonical_payload =
      CanonicalRegularPath(request.payload_path, &path_error);
  if (!canonical_payload.has_value()) {
    return Failure(path_error);
  }
  const std::optional<std::string> canonical_manifest =
      CanonicalRegularPath(request.compatibility_manifest_path, &path_error);
  if (!canonical_manifest.has_value()) {
    return Failure(path_error);
  }

  Json root;
  std::string profile_bytes;
  if (!ParseJsonFile(*canonical_profile, kMaximumProfileBytes, false, &root,
                     &profile_bytes, &path_error)) {
    return Failure(path_error);
  }
  std::unique_ptr<OwnedHostAbiProfile> profile =
      ParseProfile(root, &path_error);
  if (!profile) {
    return Failure(path_error);
  }

  std::string encoded_payload_sha256;
  std::string encoded_payload_path;
  std::string payload_id;
  if (!ReadRequiredString(root, "payload_sha256", &encoded_payload_sha256) ||
      !ReadRequiredString(root, "payload_path", &encoded_payload_path) ||
      !ReadRequiredString(root, "payload_id", &payload_id) ||
      !IsLowerHex(encoded_payload_sha256, 64) ||
      !ValidatePayloadId(payload_id, profile->build_id) ||
      !ValidateStoredPayloadPath(encoded_payload_path, payload_id,
                                 *canonical_payload) ||
      profile->reference_build_id == profile->build_id ||
      profile->reference_payload_sha256 == encoded_payload_sha256 ||
      profile->build_id != Lowercase(request.expected_build_id)) {
    return Failure(
        "external host ABI profile payload identity does not match the launch");
  }
  const BuildIdResult actual_build_id = ReadElfBuildId(*canonical_payload);
  if (!actual_build_id || actual_build_id.build_id != profile->build_id) {
    return Failure(
        "external host ABI profile Build ID does not match payload ELF");
  }

  const FileSha256Result payload_hash = ComputeFileSha256(*canonical_payload);
  if (!payload_hash || payload_hash.sha256 != encoded_payload_sha256) {
    return Failure(
        "external host ABI profile SHA-256 does not match payload bytes");
  }
  foundation::Sha256 profile_hasher;
  profile_hasher.Update(
      reinterpret_cast<const unsigned char*>(profile_bytes.data()),
      profile_bytes.size());
  const std::string profile_sha256 = profile_hasher.FinalHex();
  if (!ValidateProfileAgainstElf(*canonical_payload, profile->profile,
                                 profile->derivation_code_rvas, &path_error)) {
    return Failure(path_error);
  }

  profile->canonical_payload_path = *canonical_payload;
  profile->payload_sha256 = payload_hash.sha256;
  profile->profile_sha256 = profile_sha256;

  if (FindHostAbiProfile(profile->build_id) != nullptr) {
    const OwnedHostAbiProfile* loaded = FindLoadedProfile(profile->build_id);
    if (loaded == nullptr) {
      return Failure(
          "external host ABI profile cannot override a built-in Build ID");
    }
  }

  if (!request.candidate_canary) {
    const std::optional<std::string> canonical_receipt =
        CanonicalRegularPath(request.approval_receipt_path, &path_error);
    if (!canonical_receipt.has_value() ||
        !ValidateReceipt(*canonical_receipt, *canonical_payload,
                         payload_hash.sha256, profile_sha256, payload_id,
                         profile->build_id, *canonical_manifest, &path_error)) {
      return Failure(path_error);
    }
  }
  return RegisterProfile(std::move(profile));
}

ExternalHostAbiProfileResult InitializeHostAbiProfileFromEnvironment(
    const std::string& payload_path,
    const std::string& compatibility_manifest_path,
    const std::string& expected_build_id,
    bool explicit_unverified_authorization) {
  const char* profile_file = std::getenv("MOCKTAIL_HOST_ABI_PROFILE_FILE");
  const char* canary = std::getenv("MOCKTAIL_HOST_ABI_CANARY");
  const char* candidate_process =
      std::getenv("MOCKTAIL_ALLOW_CANDIDATE_HOST_ABI");
  const char* receipt = std::getenv("MOCKTAIL_HOST_ABI_APPROVAL_RECEIPT");
  const bool has_profile = profile_file != nullptr && profile_file[0] != '\0';
  const bool has_canary_marker = canary != nullptr && canary[0] != '\0';
  const bool candidate_canary =
      has_canary_marker && std::string_view(canary) == "1";
  const bool has_candidate_process_marker =
      candidate_process != nullptr && candidate_process[0] != '\0';
  const bool candidate_process_authorization =
      has_candidate_process_marker &&
      std::string_view(candidate_process) == "1";
  const bool has_receipt = receipt != nullptr && receipt[0] != '\0';
  if (!has_profile) {
    if (has_canary_marker || has_candidate_process_marker || has_receipt) {
      return Failure("host ABI authorization was set without a profile file");
    }
    return {};
  }
  if (has_canary_marker != candidate_canary ||
      has_candidate_process_marker != candidate_process_authorization) {
    return Failure("host ABI canary markers must use the exact value 1");
  }
  if (candidate_canary == has_receipt ||
      candidate_canary != candidate_process_authorization) {
    return Failure(
        "external host ABI profile requires exactly one authorization mode");
  }
  ExternalHostAbiProfileRequest request;
  request.profile_file = profile_file;
  request.payload_path = payload_path;
  request.compatibility_manifest_path = compatibility_manifest_path;
  request.expected_build_id = expected_build_id;
  request.approval_receipt_path = has_receipt ? receipt : "";
  request.candidate_canary = candidate_canary;
  request.candidate_process_authorization = candidate_process_authorization;
  request.explicit_unverified_authorization = explicit_unverified_authorization;
  return LoadExternalHostAbiProfile(request);
}

}  // namespace mocktail::compat
