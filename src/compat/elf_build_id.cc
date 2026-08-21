#include "compat/elf_build_id.h"

#include <elf.h>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace mocktail::compat {
namespace {

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

BuildIdResult Failure(std::string error) {
  return BuildIdResult{{}, std::move(error)};
}

std::string ElfError(const char* operation) {
  const char* detail = elf_errmsg(-1);
  std::string message(operation);
  message.append(": ");
  message.append(detail != nullptr ? detail : "unknown libelf error");
  return message;
}

std::string BytesToHex(const std::byte* bytes, std::size_t size) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string value(size * 2, '0');
  for (std::size_t index = 0; index < size; ++index) {
    const auto byte = std::to_integer<unsigned int>(bytes[index]);
    value[index * 2] = kHex[(byte >> 4U) & 0x0fU];
    value[index * 2 + 1] = kHex[byte & 0x0fU];
  }
  return value;
}

std::string FindBuildId(Elf_Data* data) {
  if (data == nullptr || data->d_buf == nullptr) {
    return {};
  }

  std::size_t offset = 0;
  while (offset < data->d_size) {
    GElf_Nhdr header{};
    std::size_t name_offset = 0;
    std::size_t descriptor_offset = 0;
    const std::size_t next =
        gelf_getnote(data, offset, &header, &name_offset, &descriptor_offset);
    if (next == 0 || next <= offset) {
      break;
    }

    const bool name_in_bounds =
        name_offset <= data->d_size &&
        header.n_namesz <= data->d_size - name_offset;
    const bool descriptor_in_bounds =
        descriptor_offset <= data->d_size &&
        header.n_descsz <= data->d_size - descriptor_offset;
    if (header.n_type == NT_GNU_BUILD_ID && header.n_namesz >= 3 &&
        header.n_descsz > 0 && name_in_bounds && descriptor_in_bounds) {
      const auto* raw = static_cast<const std::byte*>(data->d_buf);
      if (std::memcmp(raw + name_offset, "GNU", 3) == 0) {
        return BytesToHex(raw + descriptor_offset, header.n_descsz);
      }
    }
    offset = next;
  }
  return {};
}

}  // namespace

bool IsValidBuildId(std::string_view build_id) noexcept {
  if (build_id.size() < 8 || build_id.size() > 128 ||
      build_id.size() % 2 != 0) {
    return false;
  }
  for (const char character : build_id) {
    const bool digit = character >= '0' && character <= '9';
    const bool lowercase = character >= 'a' && character <= 'f';
    const bool uppercase = character >= 'A' && character <= 'F';
    if (!digit && !lowercase && !uppercase) {
      return false;
    }
  }
  return true;
}

BuildIdResult ReadElfBuildId(const std::string& path) {
  if (path.empty()) {
    return Failure("ELF path is empty");
  }
  if (elf_version(EV_CURRENT) == EV_NONE) {
    return Failure(ElfError("elf_version"));
  }

  FileDescriptor descriptor(open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (descriptor.get() < 0) {
    return Failure("open(" + path + "): " + std::strerror(errno));
  }

  std::unique_ptr<Elf, ElfDeleter> elf(
      elf_begin(descriptor.get(), ELF_C_READ, nullptr));
  if (!elf) {
    return Failure(ElfError("elf_begin"));
  }
  if (elf_kind(elf.get()) != ELF_K_ELF) {
    return Failure(path + " is not an ELF object");
  }

  std::size_t section_count = 0;
  if (elf_getshdrnum(elf.get(), &section_count) != 0) {
    return Failure(ElfError("elf_getshdrnum"));
  }

  for (std::size_t index = 0; index < section_count; ++index) {
    Elf_Scn* section = elf_getscn(elf.get(), index);
    if (section == nullptr) {
      return Failure(ElfError("elf_getscn"));
    }

    GElf_Shdr section_header{};
    if (gelf_getshdr(section, &section_header) == nullptr) {
      return Failure(ElfError("gelf_getshdr"));
    }
    if (section_header.sh_type != SHT_NOTE) {
      continue;
    }

    Elf_Data* data = nullptr;
    while ((data = elf_getdata(section, data)) != nullptr) {
      std::string build_id = FindBuildId(data);
      if (!build_id.empty()) {
        return BuildIdResult{std::move(build_id), {}};
      }
    }
  }

  return Failure(path + " has no GNU build ID");
}

}  // namespace mocktail::compat
