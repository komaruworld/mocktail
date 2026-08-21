#include "compat/bionic_stdio_runtime.h"

#include <cerrno>
#include <cstdint>

extern "C" {

alignas(sizeof(void*)) __attribute__((visibility("default"))) unsigned char
    __sF[mocktail::compat::kBionicStandardStreamCount]
        [mocktail::compat::kBionicFileSize] = {};

}

namespace {

FILE* g_bionic_stdin = reinterpret_cast<FILE*>(&__sF[0]);
FILE* g_bionic_stdout = reinterpret_cast<FILE*>(&__sF[1]);
FILE* g_bionic_stderr = reinterpret_cast<FILE*>(&__sF[2]);

FILE* RequireHostStream(FILE* stream) {
  FILE* host_stream = mocktail::compat::TranslateBionicFile(stream);
  if (host_stream != nullptr) {
    return host_stream;
  }
  errno = EINVAL;
  return nullptr;
}

}  // namespace

namespace mocktail::compat {

FILE* TranslateBionicFile(FILE* stream) noexcept {
  const uintptr_t address = reinterpret_cast<uintptr_t>(stream);
  if (address == reinterpret_cast<uintptr_t>(&__sF[0])) {
    return stdin;
  }
  if (address == reinterpret_cast<uintptr_t>(&__sF[1])) {
    return stdout;
  }
  if (address == reinterpret_cast<uintptr_t>(&__sF[2])) {
    return stderr;
  }
  return stream;
}

void* BionicFileArraySymbolAddress() noexcept { return &__sF[0]; }

void* BionicStdinSymbolAddress() noexcept { return &g_bionic_stdin; }

void* BionicStdoutSymbolAddress() noexcept { return &g_bionic_stdout; }

void* BionicStderrSymbolAddress() noexcept { return &g_bionic_stderr; }

}  // namespace mocktail::compat

extern "C" {

size_t mocktail_fwrite(const void* buffer, size_t size, size_t count,
                       FILE* stream) {
  FILE* host_stream = RequireHostStream(stream);
  if (buffer == nullptr || host_stream == nullptr) {
    return 0;
  }
  return std::fwrite(buffer, size, count, host_stream);
}

size_t mocktail_fread(void* buffer, size_t size, size_t count, FILE* stream) {
  FILE* host_stream = RequireHostStream(stream);
  if (buffer == nullptr || host_stream == nullptr) {
    return 0;
  }
  return std::fread(buffer, size, count, host_stream);
}

int mocktail_fflush(FILE* stream) {
  // Bionic's fflush(nullptr) walks its own FILE registry. The guest and host
  // registries are intentionally distinct, so asking glibc to traverse every
  // stream can include payload-owned entries with a different layout. Explicit
  // host FILE streams remain fully flushable.
  if (stream == nullptr) {
    return 0;
  }
  FILE* host_stream = RequireHostStream(stream);
  return host_stream == nullptr ? EOF : std::fflush(host_stream);
}

int mocktail_fclose(FILE* stream) {
  FILE* host_stream = RequireHostStream(stream);
  return host_stream == nullptr ? EOF : std::fclose(host_stream);
}

int mocktail_feof(FILE* stream) {
  FILE* host_stream = RequireHostStream(stream);
  return host_stream == nullptr ? 0 : std::feof(host_stream);
}

int mocktail_ferror(FILE* stream) {
  FILE* host_stream = RequireHostStream(stream);
  return host_stream == nullptr ? 0 : std::ferror(host_stream);
}

void mocktail_clearerr(FILE* stream) {
  FILE* host_stream = RequireHostStream(stream);
  if (host_stream != nullptr) {
    std::clearerr(host_stream);
  }
}

int mocktail_fileno(FILE* stream) {
  FILE* host_stream = RequireHostStream(stream);
  return host_stream == nullptr ? -1 : ::fileno(host_stream);
}

int mocktail_fseek(FILE* stream, long offset, int whence) {
  FILE* host_stream = RequireHostStream(stream);
  return host_stream == nullptr ? -1 : std::fseek(host_stream, offset, whence);
}

long mocktail_ftell(FILE* stream) {
  FILE* host_stream = RequireHostStream(stream);
  return host_stream == nullptr ? -1L : std::ftell(host_stream);
}

int mocktail_fseeko(FILE* stream, off_t offset, int whence) {
  FILE* host_stream = RequireHostStream(stream);
  return host_stream == nullptr ? -1 : ::fseeko(host_stream, offset, whence);
}

off_t mocktail_ftello(FILE* stream) {
  FILE* host_stream = RequireHostStream(stream);
  return host_stream == nullptr ? static_cast<off_t>(-1)
                                : ::ftello(host_stream);
}

char* mocktail_fgets(char* string, int count, FILE* stream) {
  FILE* host_stream = RequireHostStream(stream);
  if (string == nullptr || count <= 0 || host_stream == nullptr) {
    return nullptr;
  }
  return std::fgets(string, count, host_stream);
}

int mocktail_fputc(int character, FILE* stream) {
  FILE* host_stream = RequireHostStream(stream);
  return host_stream == nullptr ? EOF : std::fputc(character, host_stream);
}

int mocktail_fputs(const char* string, FILE* stream) {
  FILE* host_stream = RequireHostStream(stream);
  if (string == nullptr || host_stream == nullptr) {
    return EOF;
  }
  return std::fputs(string, host_stream);
}

int mocktail_getc(FILE* stream) {
  FILE* host_stream = RequireHostStream(stream);
  return host_stream == nullptr ? EOF : std::getc(host_stream);
}

wint_t mocktail_fputwc(wchar_t character, FILE* stream) {
  FILE* host_stream = RequireHostStream(stream);
  return host_stream == nullptr ? WEOF : std::fputwc(character, host_stream);
}

int mocktail_setvbuf(FILE* stream, char* buffer, int mode, size_t size) {
  FILE* host_stream = RequireHostStream(stream);
  return host_stream == nullptr ? -1
                                : std::setvbuf(host_stream, buffer, mode, size);
}

int mocktail_ungetc(int character, FILE* stream) {
  FILE* host_stream = RequireHostStream(stream);
  return host_stream == nullptr ? EOF : std::ungetc(character, host_stream);
}

int mocktail_vfprintf(FILE* stream, const char* format, va_list arguments) {
  FILE* host_stream = RequireHostStream(stream);
  if (format == nullptr || host_stream == nullptr) {
    return -1;
  }
  return std::vfprintf(host_stream, format, arguments);
}

int mocktail_fprintf(FILE* stream, const char* format, ...) {
  FILE* host_stream = RequireHostStream(stream);
  if (format == nullptr || host_stream == nullptr) {
    return -1;
  }
  va_list arguments;
  va_start(arguments, format);
  const int result = std::vfprintf(host_stream, format, arguments);
  va_end(arguments);
  return result;
}

int mocktail_vfscanf(FILE* stream, const char* format, va_list arguments) {
  FILE* host_stream = RequireHostStream(stream);
  if (format == nullptr || host_stream == nullptr) {
    return EOF;
  }
  return std::vfscanf(host_stream, format, arguments);
}

int mocktail_fscanf(FILE* stream, const char* format, ...) {
  FILE* host_stream = RequireHostStream(stream);
  if (format == nullptr || host_stream == nullptr) {
    return EOF;
  }
  va_list arguments;
  va_start(arguments, format);
  const int result = std::vfscanf(host_stream, format, arguments);
  va_end(arguments);
  return result;
}

size_t mocktail___fread_chk(void* buffer, size_t buffer_size, size_t size,
                            size_t count, FILE* stream) {
  if (buffer == nullptr) {
    return 0;
  }
  if (size != 0 && count > buffer_size / size) {
    errno = ERANGE;
    count = buffer_size / size;
  }
  return mocktail_fread(buffer, size, count, stream);
}

size_t mocktail___fwrite_chk(const void* buffer, size_t buffer_size,
                             size_t size, size_t count, FILE* stream) {
  if (buffer == nullptr) {
    return 0;
  }
  if (size != 0 && count > buffer_size / size) {
    errno = ERANGE;
    count = buffer_size / size;
  }
  return mocktail_fwrite(buffer, size, count, stream);
}

}  // extern "C"
