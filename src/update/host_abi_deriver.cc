#include "update/host_abi_deriver.h"

#define JSON_NOEXCEPTION 1
#include <capstone.h>
#include <elf.h>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compat/elf_build_id.h"
#include "update/payload_integrity.h"

namespace mocktail::update {
namespace {

using Json = nlohmann::json;

constexpr std::uint64_t kMaximumElfBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumJsonBytes = 1024ULL * 1024ULL;
constexpr std::size_t kMaximumRelocations = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumInitArrayEntries = 64U * 1024U;
constexpr std::size_t kRegistryInitializerMaximumBytes = 0x1000;
constexpr std::uint32_t kAndroidRelaSection = 0x60000002U;
constexpr std::uint32_t kRelativeRelocation = 8U;
constexpr std::int64_t kGroupedByInfo = 1;
constexpr std::int64_t kGroupedByOffsetDelta = 2;
constexpr std::int64_t kGroupedByAddend = 4;
constexpr std::int64_t kGroupHasAddend = 8;

constexpr std::array<std::string_view, 7> kRequiredExports = {
    "JNI_OnLoad",
    "Java_com_roblox_engine_jni_NativeGLInterface_nativeGameGlobalInit",
    "Java_com_roblox_engine_jni_NativeGLInterface_nativeUpdateAdapterInit",
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2InitWithParams",
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeStartLuaAppDM",
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2StartAppWithParams",
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeCallMessagesFromMainThread",
};

constexpr std::array<std::string_view, 2> kToolchainMarkers = {
    "Android (13624864, +pgo, +bolt, +lto, +mlgo, based on r530567e) "
    "clang version 19.0.1",
    "Linker: LLD 19.0.1",
};

bool IsLowerHex(std::string_view value, std::size_t size) {
  return value.size() == size &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

bool CheckedRange(std::uint64_t offset, std::uint64_t size,
                  std::uint64_t limit) {
  return offset <= limit && size <= limit - offset;
}

std::string ReadSmallRegular(const std::filesystem::path& path,
                             std::uint64_t maximum, std::string* error) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    *error = "cannot open JSON input: " + path.string();
    return {};
  }
  struct stat metadata = {};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size <= 0 ||
      static_cast<std::uint64_t>(metadata.st_size) > maximum) {
    close(descriptor);
    *error = "JSON input is not a bounded regular file";
    return {};
  }
  std::string contents(static_cast<std::size_t>(metadata.st_size), '\0');
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t read_size =
        read(descriptor, contents.data() + offset, contents.size() - offset);
    if (read_size < 0 && errno == EINTR) continue;
    if (read_size <= 0) {
      close(descriptor);
      *error = "cannot read JSON input";
      return {};
    }
    offset += static_cast<std::size_t>(read_size);
  }
  close(descriptor);
  return contents;
}

std::optional<Json> ReadJson(const std::filesystem::path& path,
                             std::string* error) {
  const std::string contents = ReadSmallRegular(path, kMaximumJsonBytes, error);
  if (!error->empty()) return std::nullopt;
  Json document = Json::parse(contents, nullptr, false, true);
  if (document.is_discarded() || !document.is_object()) {
    *error = "JSON input must contain one object: " + path.string();
    return std::nullopt;
  }
  return document;
}

bool WritePrivate(const std::filesystem::path& path, std::string_view contents,
                  std::string* error) {
  const int descriptor = open(
      path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor < 0) {
    *error = "cannot create derived HostAbi artifact";
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t written =
        write(descriptor, contents.data() + offset, contents.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      close(descriptor);
      *error = "cannot write derived HostAbi artifact";
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  const bool durable = fsync(descriptor) == 0;
  close(descriptor);
  if (!durable) *error = "cannot persist derived HostAbi artifact";
  return durable;
}

struct Segment {
  std::uint64_t offset = 0;
  std::uint64_t address = 0;
  std::uint64_t file_size = 0;
  std::uint64_t memory_size = 0;
  std::uint32_t flags = 0;

  bool ContainsMemory(std::uint64_t rva, std::uint64_t size) const {
    return rva >= address && CheckedRange(rva - address, size, memory_size);
  }

  bool ContainsFile(std::uint64_t rva, std::uint64_t size) const {
    return rva >= address && CheckedRange(rva - address, size, file_size);
  }
};

struct Section {
  std::size_t index = 0;
  std::string name;
  std::uint32_t type = 0;
  std::uint64_t flags = 0;
  std::uint64_t address = 0;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::size_t link = 0;
  std::uint64_t entry_size = 0;
};

class ElfImage final {
 public:
  ~ElfImage() {
    if (elf_ != nullptr) elf_end(elf_);
    if (mapping_ != MAP_FAILED) munmap(mapping_, file_size_);
    if (descriptor_ >= 0) close(descriptor_);
  }

  ElfImage(const ElfImage&) = delete;
  ElfImage& operator=(const ElfImage&) = delete;
  ElfImage() = default;

  bool Open(const std::filesystem::path& path, std::string* error) {
    path_ = path;
    descriptor_ = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor_ < 0) {
      *error = "cannot open ELF input: " + path.string();
      return false;
    }
    struct stat metadata = {};
    if (fstat(descriptor_, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size < static_cast<off_t>(sizeof(Elf64_Ehdr)) ||
        static_cast<std::uint64_t>(metadata.st_size) > kMaximumElfBytes) {
      *error = "ELF input is not a bounded regular file";
      return false;
    }
    file_size_ = static_cast<std::size_t>(metadata.st_size);
    mapping_ =
        mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, descriptor_, 0);
    if (mapping_ == MAP_FAILED) {
      *error = "cannot map ELF input";
      return false;
    }
    if (elf_version(EV_CURRENT) == EV_NONE) {
      *error = "libelf cannot initialise";
      return false;
    }
    elf_ = elf_begin(descriptor_, ELF_C_READ, nullptr);
    GElf_Ehdr header{};
    if (elf_ == nullptr || elf_kind(elf_) != ELF_K_ELF ||
        gelf_getehdr(elf_, &header) == nullptr ||
        gelf_getclass(elf_) != ELFCLASS64 || header.e_type != ET_DYN ||
        header.e_machine != EM_X86_64 || header.e_version != EV_CURRENT ||
        header.e_ident[EI_DATA] != ELFDATA2LSB) {
      *error = "candidate must be a little-endian x86-64 ET_DYN ELF";
      return false;
    }
    std::size_t program_count = 0;
    if (elf_getphdrnum(elf_, &program_count) != 0 || program_count == 0) {
      *error = "ELF has no program headers";
      return false;
    }
    for (std::size_t index = 0; index < program_count; ++index) {
      GElf_Phdr program{};
      if (gelf_getphdr(elf_, index, &program) == nullptr) {
        *error = "cannot inspect ELF program header";
        return false;
      }
      if (program.p_type != PT_LOAD) continue;
      if (program.p_filesz > program.p_memsz ||
          !CheckedRange(program.p_offset, program.p_filesz, file_size_)) {
        *error = "ELF PT_LOAD exceeds file bounds";
        return false;
      }
      segments_.push_back({program.p_offset, program.p_vaddr, program.p_filesz,
                           program.p_memsz, program.p_flags});
    }
    if (segments_.empty()) {
      *error = "ELF has no PT_LOAD segments";
      return false;
    }
    std::size_t section_count = 0;
    std::size_t names_index = 0;
    if (elf_getshdrnum(elf_, &section_count) != 0 || section_count == 0 ||
        elf_getshdrstrndx(elf_, &names_index) != 0) {
      *error = "cannot enumerate ELF sections";
      return false;
    }
    sections_.reserve(section_count);
    for (std::size_t index = 0; index < section_count; ++index) {
      Elf_Scn* section = elf_getscn(elf_, index);
      GElf_Shdr header_value{};
      if (section == nullptr ||
          gelf_getshdr(section, &header_value) == nullptr) {
        *error = "cannot inspect ELF section";
        return false;
      }
      const char* name = elf_strptr(elf_, names_index, header_value.sh_name);
      if (name == nullptr ||
          (header_value.sh_type != SHT_NOBITS &&
           !CheckedRange(header_value.sh_offset, header_value.sh_size,
                         file_size_))) {
        *error = "ELF section exceeds file bounds";
        return false;
      }
      Section parsed{index,
                     name,
                     header_value.sh_type,
                     header_value.sh_flags,
                     header_value.sh_addr,
                     header_value.sh_offset,
                     header_value.sh_size,
                     header_value.sh_link,
                     header_value.sh_entsize};
      if (!parsed.name.empty() &&
          by_name_.find(parsed.name) != by_name_.end()) {
        *error = "ELF contains duplicate section " + parsed.name;
        return false;
      }
      if (!parsed.name.empty()) by_name_[parsed.name] = sections_.size();
      sections_.push_back(std::move(parsed));
    }
    if (!ValidateSections(error)) return false;
    const compat::BuildIdResult identity =
        compat::ReadElfBuildId(path.string());
    if (!identity || !IsLowerHex(identity.build_id, 40)) {
      *error = identity ? "ELF Build ID is not 20 bytes" : identity.error;
      return false;
    }
    build_id_ = identity.build_id;
    sha256_ = HashRegularFile(path, error);
    return error->empty();
  }

  const std::string& build_id() const { return build_id_; }
  const std::string& sha256() const { return sha256_; }
  std::size_t file_size() const { return file_size_; }

  const Section* FindSection(std::string_view name) const {
    const auto found = by_name_.find(std::string(name));
    return found == by_name_.end() ? nullptr : &sections_[found->second];
  }

  std::string_view Bytes(std::uint64_t offset, std::uint64_t size,
                         std::string* error) const {
    if (!CheckedRange(offset, size, file_size_)) {
      *error = "ELF byte range exceeds file bounds";
      return {};
    }
    return {static_cast<const char*>(mapping_) + offset,
            static_cast<std::size_t>(size)};
  }

  std::string_view SectionBytes(const Section& section,
                                std::string* error) const {
    return Bytes(section.offset, section.size, error);
  }

  const Segment* SegmentForMemory(std::uint64_t rva, std::uint64_t size) const {
    const Segment* result = nullptr;
    for (const Segment& segment : segments_) {
      if (!segment.ContainsMemory(rva, size)) continue;
      if (result != nullptr) return nullptr;
      result = &segment;
    }
    return result;
  }

  bool RequireCode(std::uint64_t rva, std::uint64_t size,
                   std::string* error) const {
    const Segment* segment = SegmentForMemory(rva, size);
    if (segment == nullptr || !segment->ContainsFile(rva, size) ||
        (segment->flags & (PF_R | PF_X)) != (PF_R | PF_X) ||
        (segment->flags & PF_W) != 0) {
      *error = "RVA is not in a read/execute ELF segment";
      return false;
    }
    return true;
  }

  bool RequireWritable(std::uint64_t rva, std::uint64_t size,
                       std::string* error) const {
    const Segment* segment = SegmentForMemory(rva, size);
    if (segment == nullptr ||
        (segment->flags & (PF_R | PF_W)) != (PF_R | PF_W) ||
        (segment->flags & PF_X) != 0) {
      *error = "RVA is not in a non-executable read/write ELF segment";
      return false;
    }
    return true;
  }

  std::optional<std::uint64_t> OffsetForRva(std::uint64_t rva,
                                            std::uint64_t size,
                                            std::string* error) const {
    const Segment* selected = nullptr;
    for (const Segment& segment : segments_) {
      if (!segment.ContainsFile(rva, size)) continue;
      if (selected != nullptr) {
        *error = "RVA has no unique file mapping";
        return std::nullopt;
      }
      selected = &segment;
    }
    if (selected == nullptr) {
      *error = "RVA has no file mapping";
      return std::nullopt;
    }
    return selected->offset + rva - selected->address;
  }

  bool ValidateToolchain(std::string* error) const {
    const Section* comment = FindSection(".comment");
    if (comment == nullptr) {
      *error = "ELF is missing .comment";
      return false;
    }
    const std::string_view bytes = SectionBytes(*comment, error);
    if (!error->empty()) return false;
    for (const std::string_view marker : kToolchainMarkers) {
      if (bytes.find(marker) == bytes.npos) {
        *error = "ELF compiler/linker family differs from approved reference";
        return false;
      }
    }
    return true;
  }

  bool HasRequiredExports(std::string* error) const {
    const Section* symbols = FindSection(".dynsym");
    if (symbols == nullptr || symbols->link >= sections_.size()) {
      *error = "ELF dynamic symbols are invalid";
      return false;
    }
    const Section& strings = sections_[symbols->link];
    if (strings.name != ".dynstr") {
      *error = "ELF .dynsym does not reference .dynstr";
      return false;
    }
    std::set<std::string> exported;
    Elf_Scn* section = elf_getscn(elf_, symbols->index);
    Elf_Data* data =
        section == nullptr ? nullptr : elf_getdata(section, nullptr);
    if (data == nullptr || symbols->entry_size == 0) {
      *error = "cannot read ELF dynamic symbols";
      return false;
    }
    const std::size_t count = symbols->size / symbols->entry_size;
    for (std::size_t index = 0; index < count; ++index) {
      GElf_Sym symbol{};
      if (gelf_getsym(data, index, &symbol) == nullptr) {
        *error = "cannot decode ELF dynamic symbol";
        return false;
      }
      if (GELF_ST_TYPE(symbol.st_info) != STT_FUNC ||
          (GELF_ST_BIND(symbol.st_info) != STB_GLOBAL &&
           GELF_ST_BIND(symbol.st_info) != STB_WEAK) ||
          symbol.st_shndx == SHN_UNDEF || symbol.st_name == 0) {
        continue;
      }
      const char* name = elf_strptr(elf_, symbols->link, symbol.st_name);
      if (name != nullptr) exported.insert(name);
    }
    for (const std::string_view required : kRequiredExports) {
      if (exported.find(std::string(required)) == exported.end()) {
        *error = "candidate is missing required JNI export: " +
                 std::string(required);
        return false;
      }
    }
    return true;
  }

  std::optional<std::vector<std::uint64_t>> InitArray(std::string* error) const;

 private:
  bool ValidateSections(std::string* error) const {
    const Section* text = FindSection(".text");
    const Section* init = FindSection(".init_array");
    const Section* rela = FindSection(".rela.dyn");
    const Section* dynsym = FindSection(".dynsym");
    const Section* dynstr = FindSection(".dynstr");
    const Section* note = FindSection(".note.gnu.build-id");
    const Section* comment = FindSection(".comment");
    if (text == nullptr || text->type != SHT_PROGBITS ||
        (text->flags & (SHF_ALLOC | SHF_EXECINSTR)) !=
            (SHF_ALLOC | SHF_EXECINSTR) ||
        (text->flags & SHF_WRITE) != 0 || init == nullptr ||
        init->type != SHT_INIT_ARRAY ||
        (init->flags & (SHF_ALLOC | SHF_WRITE)) != (SHF_ALLOC | SHF_WRITE) ||
        (init->flags & SHF_EXECINSTR) != 0 || init->size == 0 ||
        init->size % sizeof(std::uint64_t) != 0 ||
        init->size / sizeof(std::uint64_t) > kMaximumInitArrayEntries ||
        (init->entry_size != 0 && init->entry_size != sizeof(std::uint64_t)) ||
        rela == nullptr || rela->type != kAndroidRelaSection ||
        dynsym == nullptr || dynsym->type != SHT_DYNSYM || dynstr == nullptr ||
        dynstr->type != SHT_STRTAB || note == nullptr ||
        note->type != SHT_NOTE || comment == nullptr ||
        comment->type != SHT_PROGBITS) {
      *error = "ELF required section layout is unsupported";
      return false;
    }
    return RequireCode(text->address, text->size, error) &&
           RequireWritable(init->address, init->size, error);
  }

  std::filesystem::path path_;
  int descriptor_ = -1;
  std::size_t file_size_ = 0;
  void* mapping_ = MAP_FAILED;
  Elf* elf_ = nullptr;
  std::vector<Segment> segments_;
  std::vector<Section> sections_;
  std::map<std::string, std::size_t> by_name_;
  std::string build_id_;
  std::string sha256_;
};

class SlebReader final {
 public:
  explicit SlebReader(std::string_view bytes) : bytes_(bytes) {}

  std::optional<std::int64_t> Pop(std::string* error) {
    std::uint64_t value = 0;
    unsigned shift = 0;
    unsigned char byte = 0;
    do {
      if (position_ >= bytes_.size() || shift >= 70) {
        *error = "invalid or truncated APS2 SLEB128 value";
        return std::nullopt;
      }
      byte = static_cast<unsigned char>(bytes_[position_++]);
      value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
      shift += 7;
    } while ((byte & 0x80U) != 0);
    if (shift < 64 && (byte & 0x40U) != 0) value |= ~0ULL << shift;
    return static_cast<std::int64_t>(value);
  }

  bool at_end() const { return position_ == bytes_.size(); }

 private:
  std::string_view bytes_;
  std::size_t position_ = 0;
};

struct Relocation {
  std::uint64_t offset = 0;
  std::uint64_t info = 0;
  std::uint64_t addend = 0;
};

std::optional<std::vector<Relocation>> DecodeAps2(std::string_view encoded,
                                                  std::string* error) {
  if (encoded.size() < 4 || encoded.substr(0, 4) != "APS2") {
    *error = ".rela.dyn does not use APS2 encoding";
    return std::nullopt;
  }
  SlebReader reader(encoded.substr(4));
  const auto count_value = reader.Pop(error);
  const auto first_offset = reader.Pop(error);
  if (!count_value.has_value() || !first_offset.has_value() ||
      *count_value < 0 ||
      static_cast<std::uint64_t>(*count_value) > kMaximumRelocations ||
      *first_offset < 0) {
    if (error->empty()) *error = "APS2 relocation header is invalid";
    return std::nullopt;
  }
  const std::size_t count = static_cast<std::size_t>(*count_value);
  std::int64_t relocation_offset = *first_offset;
  std::int64_t relocation_info = 0;
  std::int64_t relocation_addend = 0;
  std::vector<Relocation> result;
  result.reserve(count);
  while (result.size() < count) {
    const auto group_size_value = reader.Pop(error);
    const auto flags_value = reader.Pop(error);
    if (!group_size_value.has_value() || !flags_value.has_value() ||
        *group_size_value <= 0 ||
        static_cast<std::uint64_t>(*group_size_value) > count - result.size() ||
        *flags_value < 0 || (*flags_value & ~0x0fLL) != 0) {
      if (error->empty()) *error = "APS2 relocation group is invalid";
      return std::nullopt;
    }
    const std::int64_t flags = *flags_value;
    std::int64_t offset_delta = 0;
    if ((flags & kGroupedByOffsetDelta) != 0) {
      const auto value = reader.Pop(error);
      if (!value.has_value()) return std::nullopt;
      offset_delta = *value;
    }
    if ((flags & kGroupedByInfo) != 0) {
      const auto value = reader.Pop(error);
      if (!value.has_value()) return std::nullopt;
      relocation_info = *value;
    }
    const std::int64_t addend_mode =
        flags & (kGroupHasAddend | kGroupedByAddend);
    if (addend_mode == (kGroupHasAddend | kGroupedByAddend)) {
      const auto value = reader.Pop(error);
      if (!value.has_value()) return std::nullopt;
      relocation_addend += *value;
    } else if (addend_mode != kGroupHasAddend) {
      relocation_addend = 0;
    }
    for (std::int64_t index = 0; index < *group_size_value; ++index) {
      if ((flags & kGroupedByOffsetDelta) != 0) {
        relocation_offset += offset_delta;
      } else {
        const auto value = reader.Pop(error);
        if (!value.has_value()) return std::nullopt;
        relocation_offset += *value;
      }
      if ((flags & kGroupedByInfo) == 0) {
        const auto value = reader.Pop(error);
        if (!value.has_value()) return std::nullopt;
        relocation_info = *value;
      }
      if (addend_mode == kGroupHasAddend) {
        const auto value = reader.Pop(error);
        if (!value.has_value()) return std::nullopt;
        relocation_addend += *value;
      }
      if (relocation_offset < 0 || relocation_info < 0 ||
          relocation_addend < 0) {
        *error = "APS2 relocation exceeds unsigned bounds";
        return std::nullopt;
      }
      result.push_back({static_cast<std::uint64_t>(relocation_offset),
                        static_cast<std::uint64_t>(relocation_info),
                        static_cast<std::uint64_t>(relocation_addend)});
    }
  }
  if (!reader.at_end()) {
    *error = "APS2 relocation stream contains trailing bytes";
    return std::nullopt;
  }
  return result;
}

std::optional<std::vector<std::uint64_t>> ElfImage::InitArray(
    std::string* error) const {
  const Section* init = FindSection(".init_array");
  const Section* rela = FindSection(".rela.dyn");
  if (init == nullptr || rela == nullptr) {
    *error = "ELF init-array metadata is unavailable";
    return std::nullopt;
  }
  const auto decoded = DecodeAps2(SectionBytes(*rela, error), error);
  if (!decoded.has_value()) return std::nullopt;
  const std::size_t count = init->size / sizeof(std::uint64_t);
  std::vector<std::optional<std::uint64_t>> selected(count);
  for (const Relocation& relocation : *decoded) {
    if (relocation.offset < init->address ||
        relocation.offset >= init->address + init->size) {
      continue;
    }
    if (relocation.offset % sizeof(std::uint64_t) != 0 ||
        (relocation.info & 0xffffffffULL) != kRelativeRelocation ||
        (relocation.info >> 32U) != 0) {
      *error = ".init_array contains a non-relative relocation";
      return std::nullopt;
    }
    const std::size_t index = static_cast<std::size_t>(
        (relocation.offset - init->address) / sizeof(std::uint64_t));
    if (selected[index].has_value() ||
        !RequireCode(relocation.addend, 1, error)) {
      if (error->empty()) *error = ".init_array relocation is duplicated";
      return std::nullopt;
    }
    selected[index] = relocation.addend;
  }
  std::vector<std::uint64_t> result;
  result.reserve(count);
  for (const auto& entry : selected) {
    if (!entry.has_value()) {
      *error = ".init_array is not fully covered by APS2 relocations";
      return std::nullopt;
    }
    result.push_back(*entry);
  }
  return result;
}

std::optional<std::uint64_t> ParseRva(const Json& value,
                                      std::string_view description,
                                      std::string* error) {
  if (!value.is_string()) {
    *error = std::string(description) + " must be a nonzero hex RVA";
    return std::nullopt;
  }
  const std::string encoded = value.get<std::string>();
  if (encoded.size() < 3 || encoded.substr(0, 2) != "0x" || encoded[2] == '0' ||
      !std::all_of(encoded.begin() + 2, encoded.end(),
                   [](unsigned char character) {
                     return (character >= '0' && character <= '9') ||
                            (character >= 'a' && character <= 'f');
                   })) {
    *error = std::string(description) + " must be a canonical nonzero hex RVA";
    return std::nullopt;
  }
  std::uint64_t result = 0;
  for (const unsigned char character : std::string_view(encoded).substr(2)) {
    const unsigned value =
        character <= '9' ? character - '0' : character - 'a' + 10;
    if (result > (UINT64_MAX - value) / 16U) {
      *error = std::string(description) + " RVA exceeds 64 bits";
      return std::nullopt;
    }
    result = result * 16U + value;
  }
  return result;
}

std::string FormatRva(std::uint64_t value) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string reversed;
  while (value != 0) {
    reversed.push_back(kDigits[value & 0xfU]);
    value >>= 4U;
  }
  std::reverse(reversed.begin(), reversed.end());
  return "0x" + reversed;
}

struct Operand {
  enum class Type { kRegister, kImmediate, kMemory };
  Type type = Type::kRegister;
  std::string register_name;
  std::string segment_name;
  std::string base_name;
  std::string index_name;
  int scale = 0;
  std::int64_t value = 0;
  std::uint8_t size = 0;
  bool rip_relative = false;
};

struct Instruction {
  std::uint64_t address = 0;
  std::uint16_t size = 0;
  std::string mnemonic;
  std::vector<Operand> operands;
  std::uint8_t immediate_offset = 0;
  std::uint8_t immediate_size = 0;
  std::uint8_t displacement_offset = 0;
  std::uint8_t displacement_size = 0;
  bool control_flow = false;
  bool returns = false;
};

class Disassembler final {
 public:
  ~Disassembler() {
    if (handle_ != 0) cs_close(&handle_);
  }

  bool Open(std::string* error) {
    int major = 0;
    int minor = 0;
    cs_version(&major, &minor);
    if (major != 5 || cs_open(CS_ARCH_X86, CS_MODE_64, &handle_) != CS_ERR_OK ||
        cs_option(handle_, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) {
      *error = "native Capstone major version 5 is required";
      return false;
    }
    return true;
  }

  std::optional<std::vector<Instruction>> Decode(const ElfImage& image,
                                                 std::uint64_t rva,
                                                 std::size_t count,
                                                 std::size_t maximum_bytes,
                                                 std::string* error) const {
    if (count == 0 || count > 256 || !image.RequireCode(rva, 1, error)) {
      if (error->empty()) *error = "signature instruction count is invalid";
      return std::nullopt;
    }
    const auto offset = image.OffsetForRva(rva, 1, error);
    if (!offset.has_value()) return std::nullopt;
    const std::size_t available =
        std::min(maximum_bytes, image.file_size() - *offset);
    const std::string_view bytes = image.Bytes(*offset, available, error);
    if (!error->empty()) return std::nullopt;
    cs_insn* decoded = nullptr;
    const std::size_t decoded_count =
        cs_disasm(handle_, reinterpret_cast<const std::uint8_t*>(bytes.data()),
                  bytes.size(), rva, count, &decoded);
    if (decoded_count != count || decoded == nullptr) {
      if (decoded != nullptr) cs_free(decoded, decoded_count);
      *error = "cannot decode normalized instruction signature";
      return std::nullopt;
    }
    std::vector<Instruction> result;
    result.reserve(count);
    std::uint64_t expected_address = rva;
    for (std::size_t index = 0; index < count; ++index) {
      const cs_insn& source = decoded[index];
      if (source.address != expected_address || source.size == 0 ||
          source.detail == nullptr) {
        cs_free(decoded, decoded_count);
        *error = "instruction signature is not contiguous";
        return std::nullopt;
      }
      Instruction target;
      target.address = source.address;
      target.size = source.size;
      target.mnemonic = source.mnemonic;
      target.control_flow = cs_insn_group(handle_, &source, CS_GRP_CALL) ||
                            cs_insn_group(handle_, &source, CS_GRP_JUMP);
      target.returns = cs_insn_group(handle_, &source, CS_GRP_RET);
      const cs_x86& x86 = source.detail->x86;
      target.immediate_offset = x86.encoding.imm_offset;
      target.immediate_size = x86.encoding.imm_size;
      target.displacement_offset = x86.encoding.disp_offset;
      target.displacement_size = x86.encoding.disp_size;
      for (std::uint8_t operand_index = 0; operand_index < x86.op_count;
           ++operand_index) {
        const cs_x86_op& source_operand = x86.operands[operand_index];
        Operand operand;
        operand.size = source_operand.size;
        if (source_operand.type == X86_OP_REG) {
          operand.type = Operand::Type::kRegister;
          const char* name = cs_reg_name(handle_, source_operand.reg);
          operand.register_name = name == nullptr ? "" : name;
        } else if (source_operand.type == X86_OP_IMM) {
          operand.type = Operand::Type::kImmediate;
          operand.value = source_operand.imm;
        } else if (source_operand.type == X86_OP_MEM) {
          operand.type = Operand::Type::kMemory;
          const x86_op_mem& memory = source_operand.mem;
          const char* segment = cs_reg_name(handle_, memory.segment);
          const char* base = cs_reg_name(handle_, memory.base);
          const char* memory_index = cs_reg_name(handle_, memory.index);
          operand.segment_name = segment == nullptr ? "" : segment;
          operand.base_name = base == nullptr ? "" : base;
          operand.index_name = memory_index == nullptr ? "" : memory_index;
          operand.scale = memory.scale;
          operand.value = memory.disp;
          operand.rip_relative = memory.base == X86_REG_RIP;
        } else {
          cs_free(decoded, decoded_count);
          *error = "signature contains an unsupported x86 operand";
          return std::nullopt;
        }
        target.operands.push_back(std::move(operand));
      }
      result.push_back(std::move(target));
      expected_address += source.size;
    }
    cs_free(decoded, decoded_count);
    if (!image.RequireCode(rva, expected_address - rva, error)) {
      return std::nullopt;
    }
    return result;
  }

 private:
  csh handle_ = 0;
};

struct SignatureSpec {
  std::string name;
  std::uint64_t reference_rva = 0;
  std::size_t instruction_count = 0;
  bool registry_size_may_change = false;
  std::size_t minimum_anchor_bytes = 6;
};

struct SignatureMatch {
  std::uint64_t rva = 0;
  std::vector<Instruction> reference;
  std::vector<Instruction> candidate;
};

bool SameSemanticOperand(const Operand& left, const Operand& right,
                         bool control_flow, bool wildcard_size) {
  if (left.type != right.type || left.size != right.size) return false;
  if (left.type == Operand::Type::kRegister) {
    return left.register_name == right.register_name;
  }
  if (left.type == Operand::Type::kImmediate) {
    return control_flow || wildcard_size || left.value == right.value;
  }
  return left.segment_name == right.segment_name &&
         left.base_name == right.base_name &&
         left.index_name == right.index_name && left.scale == right.scale &&
         left.rip_relative == right.rip_relative &&
         (left.rip_relative || left.value == right.value);
}

bool ValidateSemantics(const std::vector<Instruction>& reference,
                       const std::vector<Instruction>& candidate,
                       bool allow_registry_size_change, std::string* error) {
  if (reference.size() != candidate.size()) {
    *error = "normalized signature instruction count changed";
    return false;
  }
  for (std::size_t index = 0; index < reference.size(); ++index) {
    const Instruction& left = reference[index];
    const Instruction& right = candidate[index];
    const bool wildcard =
        allow_registry_size_change && (index == 8 || index == 12);
    if (left.mnemonic != right.mnemonic ||
        left.operands.size() != right.operands.size()) {
      *error = "normalized instruction semantics changed at index " +
               std::to_string(index);
      return false;
    }
    for (std::size_t operand = 0; operand < left.operands.size(); ++operand) {
      if (!SameSemanticOperand(left.operands[operand], right.operands[operand],
                               left.control_flow, wildcard)) {
        *error = "normalized instruction operand changed at index " +
                 std::to_string(index);
        return false;
      }
    }
  }
  if (allow_registry_size_change) {
    std::array<std::int64_t, 2> sizes{};
    for (std::size_t output = 0; output < sizes.size(); ++output) {
      const Instruction& instruction = candidate[output == 0 ? 8 : 12];
      if (instruction.operands.size() != 2 ||
          instruction.operands[1].type != Operand::Type::kImmediate) {
        *error = "registry initializer size operands changed";
        return false;
      }
      sizes[output] = instruction.operands[1].value;
    }
    if (sizes[0] != sizes[1] || sizes[0] < 0x100 || sizes[0] > 0x1000 ||
        sizes[0] % 8 != 0) {
      *error = "registry initializer object size is inconsistent";
      return false;
    }
  }
  return true;
}

std::optional<std::vector<SignatureMatch>> FindSignatureMatches(
    const ElfImage& reference, const ElfImage& candidate,
    const Disassembler& disassembler, const SignatureSpec& spec,
    bool allow_multiple_candidates, std::string* error) {
  const auto reference_instructions = disassembler.Decode(
      reference, spec.reference_rva, spec.instruction_count, 4096, error);
  if (!reference_instructions.has_value()) return std::nullopt;
  const std::uint64_t signature_size = reference_instructions->back().address +
                                       reference_instructions->back().size -
                                       spec.reference_rva;
  const auto reference_offset =
      reference.OffsetForRva(spec.reference_rva, signature_size, error);
  if (!reference_offset.has_value()) return std::nullopt;
  const std::string_view encoded =
      reference.Bytes(*reference_offset, signature_size, error);
  if (!error->empty()) return std::nullopt;
  std::vector<bool> mask(encoded.size(), true);
  for (const Instruction& instruction : *reference_instructions) {
    const std::size_t base =
        static_cast<std::size_t>(instruction.address - spec.reference_rva);
    for (const auto [offset, size] :
         {std::pair{instruction.immediate_offset, instruction.immediate_size},
          std::pair{instruction.displacement_offset,
                    instruction.displacement_size}}) {
      if (size == 0) continue;
      if (offset == 0 ||
          static_cast<std::size_t>(offset) + size > instruction.size) {
        *error = "Capstone returned an invalid operand encoding range";
        return std::nullopt;
      }
      for (std::size_t index = offset; index < offset + size; ++index) {
        mask[base + index] = false;
      }
    }
  }
  std::size_t anchor_offset = 0;
  std::size_t anchor_size = 0;
  std::size_t run_start = 0;
  bool running = false;
  for (std::size_t index = 0; index <= mask.size(); ++index) {
    const bool enabled = index < mask.size() && mask[index];
    if (enabled && !running) {
      run_start = index;
      running = true;
    } else if (!enabled && running) {
      if (index - run_start > anchor_size) {
        anchor_offset = run_start;
        anchor_size = index - run_start;
      }
      running = false;
    }
  }
  if (anchor_size < spec.minimum_anchor_bytes) {
    *error = "normalized signature has no selective fixed anchor";
    return std::nullopt;
  }
  const Section* text = candidate.FindSection(".text");
  if (text == nullptr) {
    *error = "candidate has no .text section";
    return std::nullopt;
  }
  const std::string_view candidate_text = candidate.SectionBytes(*text, error);
  if (!error->empty()) return std::nullopt;
  const std::string_view anchor = encoded.substr(anchor_offset, anchor_size);
  std::vector<std::size_t> matches;
  auto search = candidate_text.begin();
  while (search != candidate_text.end()) {
    const auto found =
        std::search(search, candidate_text.end(), anchor.begin(), anchor.end());
    if (found == candidate_text.end()) break;
    const std::size_t anchor_position =
        static_cast<std::size_t>(found - candidate_text.begin());
    if (anchor_position >= anchor_offset) {
      const std::size_t signature_offset = anchor_position - anchor_offset;
      if (CheckedRange(signature_offset, encoded.size(),
                       candidate_text.size())) {
        bool same = true;
        for (std::size_t index = 0; index < encoded.size(); ++index) {
          if (mask[index] &&
              candidate_text[signature_offset + index] != encoded[index]) {
            same = false;
            break;
          }
        }
        if (same) matches.push_back(signature_offset);
      }
    }
    search = found + 1;
  }
  if (matches.empty()) {
    *error = "signature " + spec.name + " matched " +
             "0 candidate locations";
    return std::nullopt;
  }
  if (!allow_multiple_candidates && matches.size() != 1U) {
    *error = "signature " + spec.name + " matched " +
             std::to_string(matches.size()) + " candidate locations";
    return std::nullopt;
  }
  std::vector<SignatureMatch> semantic_matches;
  std::string first_semantic_error;
  for (const std::size_t match : matches) {
    const std::uint64_t match_rva = text->address + match;
    std::string semantic_error;
    const auto candidate_instructions = disassembler.Decode(
        candidate, match_rva, spec.instruction_count, 4096, &semantic_error);
    if (!candidate_instructions.has_value() ||
        !ValidateSemantics(*reference_instructions, *candidate_instructions,
                           spec.registry_size_may_change, &semantic_error)) {
      if (first_semantic_error.empty()) {
        first_semantic_error = std::move(semantic_error);
      }
      continue;
    }
    semantic_matches.push_back(SignatureMatch{
        match_rva, *reference_instructions, *candidate_instructions});
  }
  if (semantic_matches.empty() && matches.size() == 1U &&
      !first_semantic_error.empty()) {
    *error = std::move(first_semantic_error);
    return std::nullopt;
  }
  return semantic_matches;
}

std::optional<SignatureMatch> FindSignatureMatch(
    const ElfImage& reference, const ElfImage& candidate,
    const Disassembler& disassembler, const SignatureSpec& spec,
    std::string* error) {
  auto matches =
      FindSignatureMatches(reference, candidate, disassembler, spec, false,
                           error);
  if (!matches.has_value()) return std::nullopt;
  if (matches->size() != 1U) {
    *error = "signature " + spec.name + " matched " +
             std::to_string(matches->size()) + " candidate locations";
    return std::nullopt;
  }
  return std::move(matches->front());
}

std::optional<std::uint64_t> DirectCallTarget(
    const Instruction& instruction, std::string_view description,
    std::string* error) {
  if (instruction.mnemonic != "call" || instruction.operands.size() != 1U ||
      instruction.operands[0].type != Operand::Type::kImmediate ||
      instruction.operands[0].value < 0) {
    *error = std::string(description) + " is not one direct call";
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(instruction.operands[0].value);
}

std::vector<std::uint64_t> RipTargets(const Instruction& instruction) {
  std::vector<std::uint64_t> result;
  for (const Operand& operand : instruction.operands) {
    if (operand.type == Operand::Type::kMemory && operand.rip_relative) {
      result.push_back(instruction.address + instruction.size + operand.value);
    }
  }
  return result;
}

std::optional<std::uint64_t> MappedRipTarget(const SignatureMatch& match,
                                             std::uint64_t reference_target,
                                             std::string_view description,
                                             std::string* error) {
  std::set<std::uint64_t> candidates;
  for (std::size_t instruction = 0; instruction < match.reference.size();
       ++instruction) {
    const auto reference_targets = RipTargets(match.reference[instruction]);
    const auto candidate_targets = RipTargets(match.candidate[instruction]);
    if (reference_targets.size() != candidate_targets.size()) {
      *error = std::string(description) + " RIP-relative operand count changed";
      return std::nullopt;
    }
    for (std::size_t operand = 0; operand < reference_targets.size();
         ++operand) {
      if (reference_targets[operand] == reference_target) {
        candidates.insert(candidate_targets[operand]);
      }
    }
  }
  if (candidates.size() != 1) {
    *error = std::string(description) + " has no unique mapped RIP target";
    return std::nullopt;
  }
  return *candidates.begin();
}

std::optional<std::uint64_t> DeriveRegistrySlot(
    const ElfImage& image, const Disassembler& disassembler,
    std::uint64_t initializer, std::string* error) {
  // A 256-instruction cap is safely inside the bounded 0x1000 byte window and
  // includes the first return for every accepted reference/candidate to date.
  const auto instructions = disassembler.Decode(
      image, initializer, 256, kRegistryInitializerMaximumBytes, error);
  if (!instructions.has_value()) return std::nullopt;
  std::vector<std::uint64_t> stores;
  bool found_return = false;
  for (const Instruction& instruction : *instructions) {
    if (instruction.mnemonic == "mov" && instruction.operands.size() == 2 &&
        instruction.operands[0].type == Operand::Type::kMemory &&
        instruction.operands[0].rip_relative &&
        instruction.operands[0].size == 8 &&
        instruction.operands[1].type == Operand::Type::kRegister &&
        instruction.operands[1].register_name == "rbx") {
      stores.push_back(instruction.address + instruction.size +
                       instruction.operands[0].value);
    }
    if (instruction.returns) {
      found_return = true;
      break;
    }
  }
  if (!found_return || stores.size() != 1 || stores[0] % 8 != 0 ||
      !image.RequireWritable(stores[0], 8, error)) {
    if (error->empty()) {
      *error = "pre-JNI registry initializer does not publish one slot";
    }
    return std::nullopt;
  }
  return stores[0];
}

std::optional<std::size_t> PositiveSize(const Json& value,
                                        std::string_view description,
                                        std::string* error) {
  if (!value.is_number_unsigned()) {
    *error = std::string(description) + " must be a positive integer";
    return std::nullopt;
  }
  const std::uint64_t number = value.get<std::uint64_t>();
  if (number == 0 || number > SIZE_MAX) {
    *error = std::string(description) + " is outside native size bounds";
    return std::nullopt;
  }
  return static_cast<std::size_t>(number);
}

std::optional<Json> AdjustRanges(const Json& ranges,
                                 std::size_t reference_count,
                                 std::size_t candidate_count,
                                 std::string* error) {
  if (!ranges.is_array() || ranges.empty()) {
    *error = "constructor ranges are missing";
    return std::nullopt;
  }
  Json result = Json::array();
  std::size_t previous_end = 0;
  for (const Json& range : ranges) {
    if (!range.is_object() || !range.contains("begin") ||
        !range.contains("end_exclusive") ||
        !range["begin"].is_number_unsigned() ||
        !range["end_exclusive"].is_number_unsigned()) {
      *error = "constructor range is invalid";
      return std::nullopt;
    }
    const std::size_t begin = range["begin"].get<std::size_t>();
    std::size_t end = range["end_exclusive"].get<std::size_t>();
    if (end == reference_count) end = candidate_count;
    if (begin >= end || begin < previous_end || end > candidate_count) {
      *error = "constructor range cannot fit candidate init-array";
      return std::nullopt;
    }
    result.push_back({{"begin", begin}, {"end_exclusive", end}});
    previous_end = end;
  }
  return result;
}

std::optional<std::vector<SignatureSpec>> SignatureSpecs(const Json& sidecar,
                                                         std::string* error) {
  const Json& profile = sidecar["profile"];
  const Json& anchors = sidecar["derivation_anchors"];
  if (!profile.is_object() || !anchors.is_object() ||
      !profile.contains("bridge_entries") ||
      !profile["bridge_entries"].is_array() ||
      !profile.contains("data_seeds") || !profile["data_seeds"].is_object() ||
      !profile.contains("native_pre_jni_bootstrap") ||
      !profile["native_pre_jni_bootstrap"].is_object() ||
      !anchors.contains("constructor_rvas") ||
      !anchors["constructor_rvas"].is_object()) {
    *error = "reference HostAbi sidecar shape is incomplete";
    return std::nullopt;
  }
  std::map<std::string, std::uint64_t> bridges;
  for (const Json& entry : profile["bridge_entries"]) {
    if (!entry.is_object() || !entry.contains("label") ||
        !entry["label"].is_string() || !entry.contains("rva")) {
      *error = "reference HostAbi bridge is incomplete";
      return std::nullopt;
    }
    const std::string label = entry["label"].get<std::string>();
    const auto rva = ParseRva(entry["rva"], "reference bridge", error);
    if (!rva.has_value() || !bridges.emplace(label, *rva).second) {
      if (error->empty()) *error = "reference HostAbi bridge is duplicated";
      return std::nullopt;
    }
  }
  const auto bridge =
      [&](std::string_view label) -> std::optional<std::uint64_t> {
    const auto found = bridges.find(std::string(label));
    if (found == bridges.end()) {
      *error = "reference HostAbi bridge is missing: " + std::string(label);
      return std::nullopt;
    }
    return found->second;
  };
  const Json& seeds = profile["data_seeds"];
  const Json& bootstrap = profile["native_pre_jni_bootstrap"];
  const Json& constructors = anchors["constructor_rvas"];
  std::vector<SignatureSpec> result;
  const auto add = [&](std::string name, const Json& encoded, std::size_t count,
                       bool registry = false, std::size_t minimum = 6) -> bool {
    const auto rva = ParseRva(encoded, name, error);
    if (!rva.has_value()) return false;
    result.push_back({std::move(name), *rva, count, registry, minimum});
    return true;
  };
  for (const auto& [label, count] :
       std::array<std::pair<std::string_view, std::size_t>, 6>{
           {{"small-allocate", 24},
            // usable-size is an 18-instruction leaf. Do not include bytes from
            // the adjacent function or alignment padding in its signature.
            {"usable-size", 18},
            {"reallocate", 24},
            {"allocate", 24},
            {"aligned-allocate-direct", 24},
            {"free", 24}}}) {
    const auto rva = bridge(label);
    if (!rva.has_value()) return std::nullopt;
    result.push_back({std::string(label), *rva, count, false, 6});
  }
  if (!add("arena-initializer", seeds["arena_initializer"], 24) ||
      !add("allocator-thread-initializer",
           seeds["allocator_thread_initializer"], 19) ||
      !add("registry-initializer", bootstrap["registry_initializer"], 20,
           true) ||
      !add("allocator-object-initializer",
           anchors["allocator_object_initializer_rva"], 19) ||
      !add("empty-string-initializer", anchors["empty_string_initializer_rva"],
           23) ||
      !add("jni-singleton-initializer",
           anchors["jni_singleton_initializer_rva"], 11) ||
      !add("constructor-2", constructors["2"], 24) ||
      !add("constructor-3", constructors["3"], 24) ||
      !add("constructor-4", constructors["4"], 24, false, 3)) {
    return std::nullopt;
  }
  return result;
}

bool ValidateReferenceIdentity(const Json& sidecar, const ElfImage& reference,
                               std::string* error) {
  if (sidecar.value("schema_version", 0) != 1 ||
      sidecar.value("elf_build_id", "") != reference.build_id() ||
      sidecar.value("payload_sha256", "") != reference.sha256() ||
      !sidecar.contains("profile") || !sidecar["profile"].is_object() ||
      sidecar["profile"].value("elf_build_id", "") != reference.build_id() ||
      !sidecar.contains("derivation_anchors") ||
      !sidecar["derivation_anchors"].is_object() ||
      sidecar["derivation_anchors"].value("signature_version", 0) != 1) {
    *error = "reference ELF bytes do not match exact HostAbi sidecar";
    return false;
  }
  const std::string payload_id = sidecar.value("payload_id", "");
  if (payload_id.empty() ||
      sidecar.value("payload_path", "") != "payloads/" + payload_id ||
      payload_id.size() <= 41 ||
      payload_id.substr(payload_id.size() - 40) != reference.build_id()) {
    *error = "reference HostAbi sidecar payload identity is inconsistent";
    return false;
  }
  return true;
}

std::optional<Json> LoadCandidateMetadata(const std::filesystem::path& root,
                                          const ElfImage& candidate,
                                          std::string* error) {
  const auto metadata = ReadJson(root / "roblox_payload.json", error);
  if (!metadata.has_value()) return std::nullopt;
  const std::string version_name = metadata->value("version_name", "");
  const std::uint64_t version_code = metadata->value("version_code", 0ULL);
  const Json* hashes =
      metadata->contains("sha256") ? &(*metadata)["sha256"] : nullptr;
  if (metadata->value("schema_version", 0) != 1 ||
      metadata->value("package", "") != "com.roblox.client" ||
      metadata->value("abi", "") != "x86_64" || version_name.empty() ||
      version_code == 0 ||
      metadata->value("elf_build_id", "") != candidate.build_id() ||
      hashes == nullptr || !hashes->is_object() ||
      hashes->value("libroblox", "") != candidate.sha256()) {
    *error = "payload metadata does not match candidate ELF bytes";
    return std::nullopt;
  }
  const std::string payload_id =
      std::to_string(version_code) + "-" + candidate.build_id();
  if (root.filename() != payload_id) {
    *error = "candidate ELF parent is not the canonical payload ID";
    return std::nullopt;
  }
  return metadata;
}

std::optional<std::uint64_t> ProfileRva(const Json& object,
                                        std::string_view field,
                                        std::string* error) {
  if (!object.contains(field)) {
    *error = "reference profile is missing " + std::string(field);
    return std::nullopt;
  }
  return ParseRva(object[field], field, error);
}

std::optional<std::pair<Json, Json>> DeriveDocuments(const ElfImage& reference,
                                                     const ElfImage& candidate,
                                                     const Json& sidecar,
                                                     const Json& metadata,
                                                     std::string* error) {
  if (!ValidateReferenceIdentity(sidecar, reference, error) ||
      reference.build_id() == candidate.build_id() ||
      reference.sha256() == candidate.sha256() ||
      !reference.ValidateToolchain(error) ||
      !candidate.ValidateToolchain(error) ||
      !candidate.HasRequiredExports(error)) {
    return std::nullopt;
  }
  Disassembler disassembler;
  if (!disassembler.Open(error)) return std::nullopt;
  const auto specs = SignatureSpecs(sidecar, error);
  if (!specs.has_value()) return std::nullopt;
  std::map<std::string, SignatureMatch> matches;
  std::vector<SignatureMatch> allocate_matches;
  for (const SignatureSpec& spec : *specs) {
    if (spec.name == "allocate") {
      auto candidates = FindSignatureMatches(reference, candidate,
                                             disassembler, spec, true, error);
      if (!candidates.has_value()) return std::nullopt;
      allocate_matches = std::move(*candidates);
      continue;
    }
    auto match =
        FindSignatureMatch(reference, candidate, disassembler, spec, error);
    if (!match.has_value()) return std::nullopt;
    matches.emplace(spec.name, std::move(*match));
  }
  // 2904 contains two semantically equivalent allocate wrappers. Select the
  // one independently referenced by the registry initializer instead of
  // weakening signature uniqueness globally.
  const SignatureMatch& registry_match = matches.at("registry-initializer");
  if (allocate_matches.empty() || registry_match.reference.size() <= 9U ||
      registry_match.candidate.size() <= 9U) {
    *error = "allocator signatures are incomplete";
    return std::nullopt;
  }
  const auto reference_allocate = DirectCallTarget(
      registry_match.reference[9], "reference registry allocator call", error);
  if (!reference_allocate.has_value() ||
      *reference_allocate !=
          allocate_matches.front().reference.front().address) {
    if (error->empty()) {
      *error =
          "reference registry initializer does not call its allocator bridge";
    }
    return std::nullopt;
  }
  const auto candidate_allocate = DirectCallTarget(
      registry_match.candidate[9], "candidate registry allocator call", error);
  if (!candidate_allocate.has_value()) return std::nullopt;
  auto selected_allocate = std::find_if(
      allocate_matches.begin(), allocate_matches.end(),
      [&](const SignatureMatch& match) {
        return match.rva == *candidate_allocate;
      });
  if (selected_allocate == allocate_matches.end() ||
      std::find_if(std::next(selected_allocate), allocate_matches.end(),
                   [&](const SignatureMatch& match) {
                     return match.rva == *candidate_allocate;
                   }) != allocate_matches.end()) {
    *error = "signature allocate candidates do not identify exactly one "
             "registry allocator call";
    return std::nullopt;
  }
  matches.emplace("allocate", std::move(*selected_allocate));
  const auto reference_init = reference.InitArray(error);
  const auto candidate_init = candidate.InitArray(error);
  if (!reference_init.has_value() || !candidate_init.has_value()) {
    return std::nullopt;
  }
  const Json& reference_profile = sidecar["profile"];
  const auto reference_count =
      PositiveSize(reference_profile["init_array_count"],
                   "reference init-array count", error);
  if (!reference_count.has_value() ||
      reference_init->size() != *reference_count ||
      candidate_init->size() < 8) {
    if (error->empty()) *error = "init-array shape is not derivable";
    return std::nullopt;
  }
  for (const std::size_t index : {2U, 3U, 4U}) {
    const SignatureMatch& match =
        matches["constructor-" + std::to_string(index)];
    if ((*reference_init)[index] != match.reference[0].address ||
        (*candidate_init)[index] != match.rva) {
      *error = "constructor moved outside verified init-array boundary";
      return std::nullopt;
    }
  }
  const SignatureMatch& singleton = matches["jni-singleton-initializer"];
  if ((*reference_init)[5] != singleton.reference[0].address ||
      (*candidate_init)[5] != singleton.rva) {
    *error = "JNI singleton initializer is not constructor index 5";
    return std::nullopt;
  }

  Json candidate_bridges = Json::array();
  std::map<std::string, std::uint64_t> bridge_rvas;
  for (const Json& entry : reference_profile["bridge_entries"]) {
    const std::string label = entry.value("label", "");
    const std::uint64_t rva = matches[label].rva;
    candidate_bridges.push_back({{"rva", FormatRva(rva)},
                                 {"kind", entry.value("kind", "")},
                                 {"label", label}});
    bridge_rvas[label] = rva;
  }

  const Json& seeds = reference_profile["data_seeds"];
  const auto mapped =
      [&](const std::string& match_name,
          std::string_view field) -> std::optional<std::uint64_t> {
    const auto old_target = ProfileRva(seeds, field, error);
    return old_target.has_value()
               ? MappedRipTarget(matches[match_name], *old_target, field, error)
               : std::nullopt;
  };
  const auto allocator_slot =
      mapped("allocator-object-initializer", "allocator_object_slot");
  const auto empty_slot =
      mapped("empty-string-initializer", "empty_string_slot");
  const auto jni_slot =
      mapped("jni-singleton-initializer", "jni_singleton_slot");
  const auto arena_guard = mapped("arena-initializer", "arena_guard_slot");
  const auto arena_table = mapped("usable-size", "arena_table_slot");
  if (!allocator_slot.has_value() || !empty_slot.has_value() ||
      !jni_slot.has_value() || !arena_guard.has_value() ||
      !arena_table.has_value()) {
    return std::nullopt;
  }
  for (const std::uint64_t slot :
       {*allocator_slot, *empty_slot, *jni_slot, *arena_guard, *arena_table}) {
    if (!candidate.RequireWritable(slot, 8, error)) return std::nullopt;
  }

  const Json& old_bootstrap = reference_profile["native_pre_jni_bootstrap"];
  const auto old_registry_initializer =
      ProfileRva(old_bootstrap, "registry_initializer", error);
  const auto old_registry_slot =
      ProfileRva(old_bootstrap, "registry_slot", error);
  if (!old_registry_initializer.has_value() || !old_registry_slot.has_value()) {
    return std::nullopt;
  }
  const auto verified_old_slot = DeriveRegistrySlot(
      reference, disassembler, *old_registry_initializer, error);
  if (!verified_old_slot.has_value() ||
      *verified_old_slot != *old_registry_slot) {
    if (error->empty()) *error = "reference registry slot does not match ELF";
    return std::nullopt;
  }
  const std::uint64_t registry_initializer =
      matches["registry-initializer"].rva;
  const auto registry_slot =
      DeriveRegistrySlot(candidate, disassembler, registry_initializer, error);
  if (!registry_slot.has_value()) return std::nullopt;
  const Instruction& registry_call =
      matches["registry-initializer"].candidate[9];
  if (registry_call.operands.size() != 1 ||
      registry_call.operands[0].type != Operand::Type::kImmediate ||
      static_cast<std::uint64_t>(registry_call.operands[0].value) !=
          bridge_rvas["allocate"]) {
    *error = "registry initializer no longer calls native allocator";
    return std::nullopt;
  }

  const auto constructor_ranges =
      AdjustRanges(reference_profile["constructor_run_ranges"],
                   *reference_count, candidate_init->size(), error);
  const auto native_ranges =
      AdjustRanges(reference_profile["native_mimalloc_constructor_run_ranges"],
                   *reference_count, candidate_init->size(), error);
  if (!constructor_ranges.has_value() || !native_ranges.has_value()) {
    return std::nullopt;
  }
  const std::size_t thread_boundary = reference_profile.value(
      "native_mimalloc_thread_initializer_after_constructor", SIZE_MAX);
  if (thread_boundary != 2) {
    *error = "native allocator TLS boundary is no longer verified";
    return std::nullopt;
  }
  const auto singleton_bytes =
      PositiveSize(seeds["jni_singleton_bytes"], "JNI singleton size", error);
  const auto arena_slots = PositiveSize(seeds["arena_table_slot_count"],
                                        "arena table slot count", error);
  if (!singleton_bytes.has_value() || !arena_slots.has_value()) {
    return std::nullopt;
  }
  const Section* init_section = candidate.FindSection(".init_array");
  if (init_section == nullptr) {
    *error = "candidate init-array section is unavailable";
    return std::nullopt;
  }

  Json profile = {
      {"elf_build_id", candidate.build_id()},
      {"bridge_entries", candidate_bridges},
      {"data_seeds",
       {{"allocator_object_slot", FormatRva(*allocator_slot)},
        {"empty_string_slot", FormatRva(*empty_slot)},
        {"jni_singleton_slot", FormatRva(*jni_slot)},
        {"jni_singleton_bytes", *singleton_bytes},
        {"arena_initializer", FormatRva(matches["arena-initializer"].rva)},
        {"allocator_thread_initializer",
         FormatRva(matches["allocator-thread-initializer"].rva)},
        {"arena_guard_slot", FormatRva(*arena_guard)},
        {"arena_table_slot", FormatRva(*arena_table)},
        {"arena_table_slot_count", *arena_slots}}},
      {"native_allocator",
       {{"allocate", FormatRva(bridge_rvas["allocate"])},
        {"deallocate", FormatRva(bridge_rvas["free"])}}},
      {"init_array_offset", FormatRva(init_section->address)},
      {"init_array_count", candidate_init->size()},
      {"constructor_run_ranges", *constructor_ranges},
      {"native_mimalloc_constructor_run_ranges", *native_ranges},
      {"native_mimalloc_thread_initializer_after_constructor", thread_boundary},
      {"native_pre_jni_bootstrap",
       {{"registry_initializer", FormatRva(registry_initializer)},
        {"registry_slot", FormatRva(*registry_slot)}}},
      {"default_allocator_strategy", "native_mimalloc"},
  };
  Json anchors = {
      {"signature_version", 1},
      {"allocator_object_initializer_rva",
       FormatRva(matches["allocator-object-initializer"].rva)},
      {"empty_string_initializer_rva",
       FormatRva(matches["empty-string-initializer"].rva)},
      {"jni_singleton_initializer_rva", FormatRva(singleton.rva)},
      {"constructor_rvas",
       {{"2", FormatRva((*candidate_init)[2])},
        {"3", FormatRva((*candidate_init)[3])},
        {"4", FormatRva((*candidate_init)[4])},
        {"5", FormatRva((*candidate_init)[5])}}},
  };
  const std::uint64_t version_code =
      metadata["version_code"].get<std::uint64_t>();
  const std::string payload_id =
      std::to_string(version_code) + "-" + candidate.build_id();
  Json derived = {
      {"schema_version", 1},
      {"elf_build_id", candidate.build_id()},
      {"payload_sha256", candidate.sha256()},
      {"payload_id", payload_id},
      {"payload_path", "payloads/" + payload_id},
      {"reference",
       {{"elf_build_id", reference.build_id()},
        {"payload_sha256", reference.sha256()}}},
      {"profile", std::move(profile)},
      {"derivation_anchors", std::move(anchors)},
  };
  Json compatibility = {
      {"schema_version", 1},
      {"profiles",
       Json::array({{{"version_name", metadata["version_name"]},
                     {"version_code", version_code},
                     {"elf_build_id", candidate.build_id()},
                     {"status", "experimental"},
                     {"default_allowed", true},
                     {"allow_legacy_binary_patches", false},
                     {"allow_host_abi_bridges", true},
                     {"allow_host_constructor_replay", true},
                     {"reason",
                      "Machine-derived exact-Build-ID profile for isolated "
                      "probation only; normal activation requires two "
                      "successful no-recovery Tier C canaries."}}})},
  };
  return std::pair<Json, Json>{std::move(derived), std::move(compatibility)};
}

}  // namespace

HostAbiDerivationResult DeriveHostAbiProfile(
    const HostAbiDerivationOptions& options) {
  HostAbiDerivationResult result;
  std::error_code filesystem_error;
  std::filesystem::create_directories(options.output_directory,
                                      filesystem_error);
  if (filesystem_error ||
      !std::filesystem::is_empty(options.output_directory, filesystem_error)) {
    result.error = "HostAbi derivation output directory must be empty";
    return result;
  }
  ElfImage reference;
  ElfImage candidate;
  if (!reference.Open(options.reference_library, &result.error) ||
      !candidate.Open(options.candidate_payload_directory / "libroblox.so",
                      &result.error)) {
    return result;
  }
  const auto sidecar = ReadJson(options.reference_profile, &result.error);
  const auto metadata = LoadCandidateMetadata(
      options.candidate_payload_directory, candidate, &result.error);
  if (!sidecar.has_value() || !metadata.has_value()) return result;
  const auto documents =
      DeriveDocuments(reference, candidate, *sidecar, *metadata, &result.error);
  if (!documents.has_value()) return result;
  result.profile = options.output_directory / "host_abi_profile.json";
  result.compatibility_manifest =
      options.output_directory / "compatibility.json";
  if (!WritePrivate(result.profile, documents->first.dump(2) + "\n",
                    &result.error) ||
      !WritePrivate(result.compatibility_manifest,
                    documents->second.dump(2) + "\n", &result.error)) {
    return result;
  }
  return result;
}

}  // namespace mocktail::update
