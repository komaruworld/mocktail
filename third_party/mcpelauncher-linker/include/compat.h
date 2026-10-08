#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE 1
#endif

#include <sys/stat.h>

#include <android/versioning.h>
#include <string.h>

#ifdef __cplusplus
#include <atomic>
#ifndef _Atomic
#define _Atomic(T) std::atomic<T>
#endif
typedef std::atomic_int atomic_int;
typedef std::atomic_uint atomic_uint;
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#ifndef PAGE_MASK
#define PAGE_MASK (~(PAGE_SIZE - 1))
#endif
#if !defined(__APPLE__) && !defined(__FreeBSD__)
#define DEF_WEAK(a)
#define __BIONIC_ALIGN(__value, __alignment) (((__value) + (__alignment)-1) & ~((__alignment)-1))
#define	__predict_true(exp)	__builtin_expect((exp) != 0, 1)
#define	__predict_false(exp)	__builtin_expect((exp) != 0, 0)
#include <features.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(exp) ({         \
    __typeof__(exp) _rc;                   \
    do {                                   \
        _rc = (exp);                       \
    } while (_rc == -1 && errno == EINTR); \
    _rc; })
#endif
#if !defined(__GLIBC__)
#include <libgen.h>
#ifndef mmap64
#define mmap64 mmap
#endif
#ifndef lseek64
#define lseek64 lseek
#endif
#ifndef pread64
#define pread64 pread
#endif
#ifndef basename
#define basename(name) basename((char*)(name))
#endif
static inline const char* getprogname(void) {
  return program_invocation_short_name != 0
             ? program_invocation_short_name
             : "";
}
#endif
#if defined(__GLIBC__) && (__GLIBC__ < 2 || __GLIBC__ == 2 && __GLIBC_MINOR__ < 30)
#include <sys/syscall.h>
#ifdef SYS_gettid
#define gettid() syscall(SYS_gettid)
#else
#define gettid() 0
#endif
#endif
extern
#ifdef __cplusplus
"C"
#endif
size_t strlcpy(char *dst, const char *src, size_t dsize);
extern
#ifdef __cplusplus
"C"
#endif
size_t strlcat(char *dst, const char *src, size_t dsize);

#else /* APPLE || FreeBSD */
#define gettid() 0
#define __assert __loader_assert
extern 
#ifdef __cplusplus
"C"
#endif
void __loader_assert(const char* file, int line, const char* msg);
/* Used to retry syscalls that can return EINTR. */
#define TEMP_FAILURE_RETRY(exp) ({         \
    __typeof__(exp) _rc;                   \
    do {                                   \
        _rc = (exp);                       \
    } while (_rc == -1 && errno == EINTR); \
    _rc; })

#include <stdio.h>
#include <sys/types.h>
#include "../core/base/include/android-base/off64_t.h"
#  define fopen64 fopen
#  define mmap64 mmap
#  define pread64 pread
#ifdef __FreeBSD__
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_MUTEX_INITIALIZER
#define MAP_NORESERVE 0
#define DEF_WEAK(a)
#else
#define __LIBC_HIDDEN__
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
// macos-14 arm64 VM of GitHub error marks them as non existent
#define	__predict_true(exp)	__builtin_expect((exp) != 0, 1)
#define	__predict_false(exp)	__builtin_expect((exp) != 0, 0)
#endif
#include <libgen.h>
#ifndef basename
#define basename(name) basename((char*)name)
#endif
// Returns the address of the page containing address 'x'.
#define PAGE_START(x) ((x) & PAGE_MASK)

// Returns the offset of address 'x' in its page.
#define PAGE_OFFSET(x) ((x) & ~PAGE_MASK)

// Returns the address of the next page after address 'x', unless 'x' is
// itself at the start of a page.
#define PAGE_END(x) PAGE_START((x) + (PAGE_SIZE-1))
#endif
