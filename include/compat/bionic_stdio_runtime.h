#ifndef MOCKTAIL_COMPAT_BIONIC_STDIO_RUNTIME_H_
#define MOCKTAIL_COMPAT_BIONIC_STDIO_RUNTIME_H_

#include <sys/types.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cwchar>

namespace mocktail::compat {

// Android's public LP64 FILE ABI is opaque but has a fixed 152-byte stride.
// Old NDK clients derive stdin/stdout/stderr by indexing the exported __sF
// array, so its size and alignment are part of the binary interface even when
// all actual I/O is delegated to host stdio.
constexpr size_t kBionicFileSize = 152;
constexpr size_t kBionicStandardStreamCount = 3;

FILE* TranslateBionicFile(FILE* stream) noexcept;
void* BionicFileArraySymbolAddress() noexcept;
void* BionicStdinSymbolAddress() noexcept;
void* BionicStdoutSymbolAddress() noexcept;
void* BionicStderrSymbolAddress() noexcept;

}  // namespace mocktail::compat

extern "C" {

alignas(sizeof(void*)) extern unsigned char
    __sF[mocktail::compat::kBionicStandardStreamCount]
        [mocktail::compat::kBionicFileSize];

}

// FILE values returned by host fopen/fdopen pass through unchanged. Pointers
// into Bionic's ABI-sized __sF array are translated to the corresponding host
// standard stream at this single boundary.
extern "C" {

size_t mocktail_fwrite(const void* buffer, size_t size, size_t count,
                       FILE* stream);
size_t mocktail_fread(void* buffer, size_t size, size_t count, FILE* stream);
int mocktail_fflush(FILE* stream);
int mocktail_fclose(FILE* stream);
int mocktail_feof(FILE* stream);
int mocktail_ferror(FILE* stream);
void mocktail_clearerr(FILE* stream);
int mocktail_fileno(FILE* stream);
int mocktail_fseek(FILE* stream, long offset, int whence);
long mocktail_ftell(FILE* stream);
int mocktail_fseeko(FILE* stream, off_t offset, int whence);
off_t mocktail_ftello(FILE* stream);
char* mocktail_fgets(char* string, int count, FILE* stream);
int mocktail_fputc(int character, FILE* stream);
int mocktail_fputs(const char* string, FILE* stream);
int mocktail_getc(FILE* stream);
wint_t mocktail_fputwc(wchar_t character, FILE* stream);
int mocktail_setvbuf(FILE* stream, char* buffer, int mode, size_t size);
int mocktail_ungetc(int character, FILE* stream);
int mocktail_vfprintf(FILE* stream, const char* format, va_list arguments);
int mocktail_fprintf(FILE* stream, const char* format, ...);
int mocktail_vfscanf(FILE* stream, const char* format, va_list arguments);
int mocktail_fscanf(FILE* stream, const char* format, ...);
size_t mocktail___fread_chk(void* buffer, size_t buffer_size, size_t size,
                            size_t count, FILE* stream);
size_t mocktail___fwrite_chk(const void* buffer, size_t buffer_size,
                             size_t size, size_t count, FILE* stream);

}  // extern "C"

#endif  // MOCKTAIL_COMPAT_BIONIC_STDIO_RUNTIME_H_
