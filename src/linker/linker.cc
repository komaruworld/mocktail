// Without the Bionic linker, loading falls back to host dlopen/dlsym.

#include "linker/linker.h"

#include "compat/bionic_atfork_runtime.h"
#include "compat/bionic_dns_runtime.h"
#include "compat/bionic_host_libc_runtime.h"
#include "compat/bionic_large_file_runtime.h"
#include "compat/bionic_pthread_key_runtime.h"
#include "compat/bionic_abi_exports.h"
#include "compat/bionic_rwlock_runtime.h"
#include "compat/bionic_semaphore_runtime.h"
#include "compat/bionic_signal_runtime.h"
#include "compat/bionic_stdio_runtime.h"
#include "compat/bionic_sysconf.h"
#include "libc_shim/libc_shim.h"

#include <dlfcn.h>
#include <elf.h>
#include <link.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <list>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#ifdef MOCKTAIL_USE_BIONIC_LINKER
#include <mcpelauncher/linker.h>

namespace linker::libdl {
std::unordered_map<std::string, void*> get_dl_symbols();
}  // namespace linker::libdl
#endif

namespace linker {

namespace {

std::unordered_map<std::string, LibraryHandle> g_symbol_table;

// Synthetic exports retain ownership per Android SONAME.
std::unordered_map<std::string, SymbolMap> g_synthetic_symbols;
std::unordered_map<std::string, LibraryHandle> g_synthetic_libraries;
std::unordered_set<LibraryHandle> g_synthetic_handles;

SymbolMap g_legacy_bionic_symbols;

// Keep string memory alive for Bionic linker sonames to avoid dangling pointers.
std::list<std::string> g_persistent_names;
std::mutex g_registry_mutex;
AndroidLibraryLoadObserver g_library_load_observer = nullptr;
void* g_library_load_observer_context = nullptr;

struct ProgramHeaderCapture {
  uintptr_t expected_base = 0;
  const ElfW(Phdr)* phdr = nullptr;
  size_t phnum = 0;
  bool found = false;
};

int CaptureProgramHeaders(dl_phdr_info* info, size_t, void* data) {
  auto* capture = static_cast<ProgramHeaderCapture*>(data);
  if (info == nullptr || capture == nullptr ||
      static_cast<uintptr_t>(info->dlpi_addr) != capture->expected_base) {
    return 0;
  }
  capture->phdr = info->dlpi_phdr;
  capture->phnum = info->dlpi_phnum;
  capture->found = true;
  return 1;
}

bool HasPrefix(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

#ifdef MOCKTAIL_USE_BIONIC_LINKER
bool LinkerTraceEnabled() {
  static const bool enabled = std::getenv("MOCKTAIL_TRACE_LINKER") != nullptr;
  return enabled;
}

void* BionicDlopenBoundary(const char* filename, int flags) {
  void* handle = ::linker::dlopen(filename, flags);
  if (__builtin_expect(LinkerTraceEnabled(), 0) && filename != nullptr &&
      std::strstr(filename, "vulkan") != nullptr) {
    std::cerr << "  [bionic-libdl] dlopen(" << filename << ") -> " << handle
              << '\n';
  }
  return handle;
}

void* BionicDlsymBoundary(void* handle, const char* symbol) {
  if (symbol != nullptr && IsBlockedRobloxCrashReportNativeSymbol(symbol)) {
    return nullptr;
  }
  void* address = ::linker::dlsym(handle, symbol);
  if (__builtin_expect(LinkerTraceEnabled(), 0) && symbol != nullptr &&
      std::strncmp(symbol, "vk", 2) == 0) {
    std::cerr << "  [bionic-libdl] dlsym(" << symbol << ") -> " << address
              << '\n';
  }
  return address;
}

int BionicDlcloseBoundary(void* handle) {
  return ::linker::dlclose(handle);
}

char* BionicDlerrorBoundary() { return ::linker::dlerror(); }
#endif

// Host-incompatible Bionic APIs belong only to synthetic libc.so. Exporting
// them from an earlier dependency could bind the guest to glibc instead.
bool IsBionicLibcOnlySymbol(std::string_view symbol) {
  static constexpr std::string_view kStdioSymbols[] = {
      "__fread_chk", "__fwrite_chk", "__sF",   "clearerr",
      "fclose",     "feof",         "ferror", "fflush",
      "fgetc",      "fgets",        "fileno", "fprintf",
      "fputc",      "fputs",        "fputwc", "fread",
      "fscanf",     "fseek",        "fseeko", "ftell",
      "ftello",     "fwrite",       "getc",   "setvbuf",
      "stderr",     "stdin",        "stdout", "ungetc",
      "vfprintf",   "vfscanf",
  };
  if (std::binary_search(std::begin(kStdioSymbols), std::end(kStdioSymbols),
                         symbol)) {
    return true;
  }
  return symbol == "__cmsg_nxthdr" || symbol == "__cxa_thread_atexit_impl" ||
         symbol == "__readlink_chk" || symbol == "__register_atfork" ||
         symbol == "arc4random_buf" || symbol == "geteuid" ||
         symbol == "getuid" || symbol == "lseek64" ||
         symbol == "mallinfo" || symbol == "pread64" || symbol == "pwrite64" ||
         symbol == "sigaction" || symbol == "strerror_r" ||
         symbol == "strtoll_l" || symbol == "strtoull_l" ||
         symbol == "sysconf" || symbol == "sysinfo" || symbol == "uname" ||
         HasPrefix(symbol, "pthread_") ||
         HasPrefix(symbol, "__pthread_") || HasPrefix(symbol, "sem_");
}

bool MayExposeLegacySymbol(std::string_view soname,
                           std::string_view symbol) {
  return soname == "libc.so" || !IsBionicLibcOnlySymbol(symbol);
}

SymbolMap LegacyExportsForSyntheticLibrary(const std::string& soname,
                                           const SymbolMap& legacy_exports) {
  SymbolMap filtered;
  filtered.reserve(legacy_exports.size());
  for (const auto& [name, address] : legacy_exports) {
    if (MayExposeLegacySymbol(soname, name)) {
      filtered.emplace(name, address);
    }
  }
  return filtered;
}

void RemoveHandleFromRegistry(LibraryHandle handle) {
  for (auto it = g_symbol_table.begin(); it != g_symbol_table.end();) {
    if (it->second == handle) {
      it = g_symbol_table.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = g_synthetic_libraries.begin();
       it != g_synthetic_libraries.end();) {
    if (it->second == handle) {
      it = g_synthetic_libraries.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

void RegisterSymbol(const std::string& name, void* addr) {
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  g_legacy_bionic_symbols[name] = addr;
}

const SymbolMap& GetBionicSymbols() {
  return g_legacy_bionic_symbols;
}

void* BionicDlIteratePhdrAddress() {
#ifdef MOCKTAIL_USE_BIONIC_LINKER
  using Callback = int (*)(dl_phdr_info*, size_t, void*);
  using Function = int (*)(Callback, void*);
  return reinterpret_cast<void*>(
      static_cast<Function>(&::linker::dl_iterate_phdr));
#else
  return reinterpret_cast<void*>(&::dl_iterate_phdr);
#endif
}

ProgramHeaderValidation ValidateBionicProgramHeaders(LibraryHandle handle) {
  if (handle == nullptr) {
    return {false, "null Android library handle"};
  }
#ifdef MOCKTAIL_USE_BIONIC_LINKER
  const uintptr_t base = ::linker::get_library_base(handle);
  if (base == 0) {
    return {false, "Bionic linker returned a zero load base"};
  }
  const auto* elf_header = reinterpret_cast<const ElfW(Ehdr)*>(base);
  if (std::memcmp(elf_header->e_ident, ELFMAG, SELFMAG) != 0 ||
      elf_header->e_phoff == 0 || elf_header->e_phnum == 0) {
    return {false, "loaded Android ELF has invalid program headers"};
  }

  ProgramHeaderCapture capture;
  capture.expected_base = base;
  ::linker::dl_iterate_phdr(&CaptureProgramHeaders, &capture);
  if (!capture.found) {
    return {false, "Bionic dl_iterate_phdr cannot see the loaded Android ELF"};
  }
  const auto* expected_phdr =
      reinterpret_cast<const ElfW(Phdr)*>(base + elf_header->e_phoff);
  if (capture.phdr != expected_phdr || capture.phnum != elf_header->e_phnum) {
    return {false, "Bionic dl_iterate_phdr returned inconsistent ELF metadata"};
  }
  bool has_unwind_header = false;
  for (size_t index = 0; index < capture.phnum; ++index) {
    if (capture.phdr[index].p_type == PT_GNU_EH_FRAME) {
      has_unwind_header = true;
      break;
    }
  }
  if (!has_unwind_header) {
    return {false, "loaded Android ELF has no PT_GNU_EH_FRAME"};
  }
  return {true, {}};
#else
  return {false, "Bionic linker support is disabled"};
#endif
}

std::uintptr_t FindLoadedAndroidLibraryBase(
    const std::string& logical_name) {
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  const auto loaded = g_symbol_table.find(logical_name);
  if (loaded == g_symbol_table.end() || loaded->second == nullptr) {
    return 0;
  }
#ifdef MOCKTAIL_USE_BIONIC_LINKER
  return static_cast<std::uintptr_t>(
      ::linker::get_library_base(loaded->second));
#else
  return 0;
#endif
}

void RegisterSyntheticSymbol(const std::string& soname,
                             const std::string& name, void* address) {
  LibraryHandle loaded_handle = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    g_synthetic_symbols[soname][name] = address;
    auto loaded = g_synthetic_libraries.find(soname);
    if (loaded != g_synthetic_libraries.end()) {
      loaded_handle = loaded->second;
    }
  }

#ifdef MOCKTAIL_USE_BIONIC_LINKER
  if (loaded_handle != nullptr) {
    ::linker::relocate(loaded_handle, SymbolMap{{name, address}});
  }
#else
  static_cast<void>(loaded_handle);
#endif
}

void RegisterBionicPthreadKeyRuntimeForLibc() {
  RegisterSyntheticSymbol(
      "libc.so", "pthread_key_create",
      reinterpret_cast<void*>(mocktail_bionic_pthread_key_create));
  RegisterSyntheticSymbol(
      "libc.so", "pthread_key_delete",
      reinterpret_cast<void*>(mocktail_bionic_pthread_key_delete));
  RegisterSyntheticSymbol(
      "libc.so", "pthread_getspecific",
      reinterpret_cast<void*>(mocktail_bionic_pthread_getspecific));
  RegisterSyntheticSymbol(
      "libc.so", "pthread_setspecific",
      reinterpret_cast<void*>(mocktail_bionic_pthread_setspecific));
  RegisterSyntheticSymbol("libc.so", "sem_init",
                          reinterpret_cast<void*>(mocktail_bionic_sem_init));
  RegisterSyntheticSymbol("libc.so", "sem_destroy",
                          reinterpret_cast<void*>(mocktail_bionic_sem_destroy));
  RegisterSyntheticSymbol("libc.so", "sem_wait",
                          reinterpret_cast<void*>(mocktail_bionic_sem_wait));
  RegisterSyntheticSymbol("libc.so", "sem_post",
                          reinterpret_cast<void*>(mocktail_bionic_sem_post));
  RegisterSyntheticSymbol(
      "libc.so", "pthread_rwlock_init",
      reinterpret_cast<void*>(mocktail_bionic_pthread_rwlock_init));
  RegisterSyntheticSymbol(
      "libc.so", "pthread_rwlock_destroy",
      reinterpret_cast<void*>(mocktail_bionic_pthread_rwlock_destroy));
  RegisterSyntheticSymbol(
      "libc.so", "pthread_rwlock_rdlock",
      reinterpret_cast<void*>(mocktail_bionic_pthread_rwlock_rdlock));
  RegisterSyntheticSymbol(
      "libc.so", "pthread_rwlock_wrlock",
      reinterpret_cast<void*>(mocktail_bionic_pthread_rwlock_wrlock));
  RegisterSyntheticSymbol(
      "libc.so", "pthread_rwlock_unlock",
      reinterpret_cast<void*>(mocktail_bionic_pthread_rwlock_unlock));
  RegisterSyntheticSymbol("libc.so", "pthread_once",
                          reinterpret_cast<void*>(mocktail_pthread_once));
  RegisterSyntheticSymbol("libc.so", "pthread_spin_init",
                          reinterpret_cast<void*>(mocktail_pthread_spin_init));
  RegisterSyntheticSymbol(
      "libc.so", "pthread_spin_destroy",
      reinterpret_cast<void*>(mocktail_pthread_spin_destroy));
  RegisterSyntheticSymbol("libc.so", "pthread_spin_lock",
                          reinterpret_cast<void*>(mocktail_pthread_spin_lock));
  RegisterSyntheticSymbol(
      "libc.so", "pthread_spin_trylock",
      reinterpret_cast<void*>(mocktail_pthread_spin_trylock));
  RegisterSyntheticSymbol(
      "libc.so", "pthread_spin_unlock",
      reinterpret_cast<void*>(mocktail_pthread_spin_unlock));
  RegisterSyntheticSymbol(
      "libc.so", "pthread_barrier_init",
      reinterpret_cast<void*>(mocktail_pthread_barrier_init));
  RegisterSyntheticSymbol(
      "libc.so", "pthread_barrier_destroy",
      reinterpret_cast<void*>(mocktail_pthread_barrier_destroy));
  RegisterSyntheticSymbol(
      "libc.so", "pthread_barrier_wait",
      reinterpret_cast<void*>(mocktail_pthread_barrier_wait));
}

void RegisterBionicAtForkRuntimeForLibc() {
  RegisterSyntheticSymbol(
      "libc.so", "__register_atfork",
      reinterpret_cast<void*>(mocktail_bionic_register_atfork));
}

void RegisterBionicLargeFileRuntimeForLibc() {
  RegisterSyntheticSymbol("libc.so", "pread64",
                          reinterpret_cast<void*>(mocktail_bionic_pread64));
  RegisterSyntheticSymbol("libc.so", "pwrite64",
                          reinterpret_cast<void*>(mocktail_bionic_pwrite64));
  RegisterSyntheticSymbol("libc.so", "lseek64",
                          reinterpret_cast<void*>(mocktail_bionic_lseek64));
}

void RegisterBionicHostLibcRuntimeForLibc() {
  RegisterSyntheticSymbol("libc.so", "getaddrinfo",
                          reinterpret_cast<void*>(mocktail_bionic_getaddrinfo));
  RegisterSyntheticSymbol(
      "libc.so", "freeaddrinfo",
      reinterpret_cast<void*>(mocktail_bionic_freeaddrinfo));
  RegisterSyntheticSymbol(
      "libc.so", "gethostbyname",
      reinterpret_cast<void*>(mocktail_bionic_gethostbyname));
  // Replace legacy DNS wrappers before libc.so snapshots the global map, so
  // every payload crosses the upload deny boundary.
  RegisterSymbol("getaddrinfo",
                 reinterpret_cast<void*>(mocktail_bionic_getaddrinfo));
  RegisterSymbol("freeaddrinfo",
                 reinterpret_cast<void*>(mocktail_bionic_freeaddrinfo));
  RegisterSymbol("gethostbyname",
                 reinterpret_cast<void*>(mocktail_bionic_gethostbyname));
  RegisterSyntheticSymbol("libc.so", "__cmsg_nxthdr",
                          reinterpret_cast<void*>(
                              mocktail_bionic_cmsg_nxthdr));
  RegisterSyntheticSymbol(
      "libc.so", "__cxa_thread_atexit_impl",
      reinterpret_cast<void*>(mocktail_bionic_cxa_thread_atexit_impl));
  RegisterSyntheticSymbol("libc.so", "__readlink_chk",
                          reinterpret_cast<void*>(mocktail___readlink_chk));
  RegisterSyntheticSymbol("libc.so", "arc4random_buf",
                          reinterpret_cast<void*>(
                              mocktail_bionic_arc4random_buf));
  RegisterSyntheticSymbol("libc.so", "mallinfo",
                          reinterpret_cast<void*>(mocktail_bionic_mallinfo));
  RegisterSyntheticSymbol("libc.so", "sysinfo",
                          reinterpret_cast<void*>(mocktail_bionic_sysinfo));
  RegisterSyntheticSymbol("libc.so", "uname",
                          reinterpret_cast<void*>(mocktail_bionic_uname));
  RegisterSyntheticSymbol("libc.so", "getuid",
                          reinterpret_cast<void*>(mocktail_bionic_getuid));
  RegisterSyntheticSymbol("libc.so", "geteuid",
                          reinterpret_cast<void*>(mocktail_bionic_geteuid));
  RegisterSymbol("sysinfo", reinterpret_cast<void*>(mocktail_bionic_sysinfo));
  RegisterSymbol("uname", reinterpret_cast<void*>(mocktail_bionic_uname));
  RegisterSymbol("getuid", reinterpret_cast<void*>(mocktail_bionic_getuid));
  RegisterSymbol("geteuid", reinterpret_cast<void*>(mocktail_bionic_geteuid));
  RegisterSyntheticSymbol("libc.so", "strerror_r",
                          reinterpret_cast<void*>(mocktail_bionic_strerror_r));
  // Publish this before relocation to prevent binding glibc's incompatible
  // GNU char*-returning strerror_r.
  RegisterSymbol("strerror_r",
                 reinterpret_cast<void*>(mocktail_bionic_strerror_r));
  RegisterSyntheticSymbol("libc.so", "strtoll_l",
                          reinterpret_cast<void*>(mocktail_bionic_strtoll_l));
  RegisterSyntheticSymbol("libc.so", "strtoull_l",
                          reinterpret_cast<void*>(mocktail_bionic_strtoull_l));
}

void RegisterBionicSysconfRuntimeForLibc() {
  RegisterSyntheticSymbol("libc.so", "sysconf",
                          reinterpret_cast<void*>(mocktail_bionic_sysconf));
}

void RegisterBionicSignalRuntimeForLibc() {
  RegisterSyntheticSymbol("libc.so", "sigaction",
                          reinterpret_cast<void*>(mocktail_bionic_sigaction));
}

void RegisterBionicStdioRuntimeForLibc() {
  RegisterSyntheticSymbol("libc.so", "__sF",
                          mocktail::compat::BionicFileArraySymbolAddress());
  RegisterSyntheticSymbol("libc.so", "stdin",
                          mocktail::compat::BionicStdinSymbolAddress());
  RegisterSyntheticSymbol("libc.so", "stdout",
                          mocktail::compat::BionicStdoutSymbolAddress());
  RegisterSyntheticSymbol("libc.so", "stderr",
                          mocktail::compat::BionicStderrSymbolAddress());
  RegisterSyntheticSymbol("libc.so", "__fread_chk",
                          reinterpret_cast<void*>(mocktail___fread_chk));
  RegisterSyntheticSymbol("libc.so", "__fwrite_chk",
                          reinterpret_cast<void*>(mocktail___fwrite_chk));
  RegisterSyntheticSymbol("libc.so", "clearerr",
                          reinterpret_cast<void*>(mocktail_clearerr));
  RegisterSyntheticSymbol("libc.so", "fclose",
                          reinterpret_cast<void*>(mocktail_fclose));
  RegisterSyntheticSymbol("libc.so", "feof",
                          reinterpret_cast<void*>(mocktail_feof));
  RegisterSyntheticSymbol("libc.so", "ferror",
                          reinterpret_cast<void*>(mocktail_ferror));
  RegisterSyntheticSymbol("libc.so", "fflush",
                          reinterpret_cast<void*>(mocktail_fflush));
  RegisterSyntheticSymbol("libc.so", "fgetc",
                          reinterpret_cast<void*>(mocktail_getc));
  RegisterSyntheticSymbol("libc.so", "fgets",
                          reinterpret_cast<void*>(mocktail_fgets));
  RegisterSyntheticSymbol("libc.so", "fileno",
                          reinterpret_cast<void*>(mocktail_fileno));
  RegisterSyntheticSymbol("libc.so", "fprintf",
                          reinterpret_cast<void*>(mocktail_fprintf));
  RegisterSyntheticSymbol("libc.so", "fputc",
                          reinterpret_cast<void*>(mocktail_fputc));
  RegisterSyntheticSymbol("libc.so", "fputs",
                          reinterpret_cast<void*>(mocktail_fputs));
  RegisterSyntheticSymbol("libc.so", "fputwc",
                          reinterpret_cast<void*>(mocktail_fputwc));
  RegisterSyntheticSymbol("libc.so", "fread",
                          reinterpret_cast<void*>(mocktail_fread));
  RegisterSyntheticSymbol("libc.so", "fscanf",
                          reinterpret_cast<void*>(mocktail_fscanf));
  RegisterSyntheticSymbol("libc.so", "fseek",
                          reinterpret_cast<void*>(mocktail_fseek));
  RegisterSyntheticSymbol("libc.so", "fseeko",
                          reinterpret_cast<void*>(mocktail_fseeko));
  RegisterSyntheticSymbol("libc.so", "ftell",
                          reinterpret_cast<void*>(mocktail_ftell));
  RegisterSyntheticSymbol("libc.so", "ftello",
                          reinterpret_cast<void*>(mocktail_ftello));
  RegisterSyntheticSymbol("libc.so", "fwrite",
                          reinterpret_cast<void*>(mocktail_fwrite));
  RegisterSyntheticSymbol("libc.so", "getc",
                          reinterpret_cast<void*>(mocktail_getc));
  RegisterSyntheticSymbol("libc.so", "setvbuf",
                          reinterpret_cast<void*>(mocktail_setvbuf));
  RegisterSyntheticSymbol("libc.so", "ungetc",
                          reinterpret_cast<void*>(mocktail_ungetc));
  RegisterSyntheticSymbol("libc.so", "vfprintf",
                          reinterpret_cast<void*>(mocktail_vfprintf));
  RegisterSyntheticSymbol("libc.so", "vfscanf",
                          reinterpret_cast<void*>(mocktail_vfscanf));
}

void RegisterBionicDynamicLoaderForLibdl() {
#ifdef MOCKTAIL_USE_BIONIC_LINKER
  for (const auto& [name, address] : libdl::get_dl_symbols()) {
    RegisterSyntheticSymbol("libdl.so", name, address);
  }
  RegisterSyntheticSymbol("libdl.so", "dlopen",
                          reinterpret_cast<void*>(BionicDlopenBoundary));
  RegisterSymbol("dlopen", reinterpret_cast<void*>(BionicDlopenBoundary));
  RegisterSyntheticSymbol("libdl.so", "dlsym",
                          reinterpret_cast<void*>(BionicDlsymBoundary));
  RegisterSymbol("dlsym", reinterpret_cast<void*>(BionicDlsymBoundary));
  RegisterSyntheticSymbol("libdl.so", "dlclose",
                          reinterpret_cast<void*>(BionicDlcloseBoundary));
  RegisterSymbol("dlclose", reinterpret_cast<void*>(BionicDlcloseBoundary));
  RegisterSyntheticSymbol("libdl.so", "dlerror",
                          reinterpret_cast<void*>(BionicDlerrorBoundary));
  RegisterSymbol("dlerror", reinterpret_cast<void*>(BionicDlerrorBoundary));
#endif
}

SymbolMap GetSyntheticLibrarySymbols(const std::string& soname) {
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  auto found = g_synthetic_symbols.find(soname);
  return found == g_synthetic_symbols.end() ? SymbolMap{} : found->second;
}

LibraryHandle AddSyntheticLibrary(const std::string& soname,
                                  const SymbolMap& exports) {
  LibraryHandle existing_handle = nullptr;
  SymbolMap owned_exports;
  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    auto& registered_exports = g_synthetic_symbols[soname];
    for (const auto& [name, address] : exports) {
      registered_exports[name] = address;
    }
    owned_exports = registered_exports;
    auto existing = g_synthetic_libraries.find(soname);
    if (existing != g_synthetic_libraries.end()) {
      existing_handle = existing->second;
    }
  }

  if (existing_handle != nullptr) {
#ifdef MOCKTAIL_USE_BIONIC_LINKER
    if (!exports.empty()) {
      ::linker::relocate(existing_handle, exports);
    }
#endif
    return existing_handle;
  }

#ifdef MOCKTAIL_USE_BIONIC_LINKER
  const char* persistent_soname = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    g_persistent_names.push_back(soname);
    persistent_soname = g_persistent_names.back().c_str();
  }
  LibraryHandle handle =
      ::linker::load_library(persistent_soname, owned_exports);
#else
  LibraryHandle handle = ::dlopen(soname.c_str(), RTLD_LAZY | RTLD_GLOBAL);
#endif
  if (!handle) {
#ifdef MOCKTAIL_USE_BIONIC_LINKER
    std::cerr << "  [bionic-linker] failed to register synthetic library '"
              << soname << "': " << ::linker::dlerror() << '\n';
#else
    std::cerr << "  [linker] failed to load synthetic library '" << soname
              << "': " << ::dlerror() << '\n';
#endif
    return nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    g_synthetic_libraries[soname] = handle;
    g_synthetic_handles.insert(handle);
    g_symbol_table[soname] = handle;
  }
  std::cout << "  [linker] Registered synthetic library '" << soname
            << "' with " << owned_exports.size() << " export(s)\n";
  return handle;
}

void UpdateAndroidLibraryPath(const std::string& search_path) {
#ifdef MOCKTAIL_USE_BIONIC_LINKER
  if (!search_path.empty()) {
    ::linker::update_LD_LIBRARY_PATH(search_path.c_str());
  }
#else
  static_cast<void>(search_path);
#endif
}

bool SetAndroidLibraryLoadObserver(AndroidLibraryLoadObserver observer,
                                   void* context) {
  if (observer == nullptr || context == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  if (g_library_load_observer != nullptr) {
    return false;
  }
  g_library_load_observer = observer;
  g_library_load_observer_context = context;
  return true;
}

void ClearAndroidLibraryLoadObserver(void* context) {
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  if (context == nullptr || context != g_library_load_observer_context) {
    return;
  }
  g_library_load_observer = nullptr;
  g_library_load_observer_context = nullptr;
}

LibraryHandle OpenAndroidLibrary(const std::string& real_path,
                                 const std::string& logical_name) {
  const std::size_t separator = real_path.find_last_of('/');
  if (separator != std::string::npos) {
    UpdateAndroidLibraryPath(separator == 0 ? "/"
                                             : real_path.substr(0, separator));
  }

#ifdef MOCKTAIL_USE_BIONIC_LINKER
  LibraryHandle handle = ::linker::dlopen(real_path.c_str(), RTLD_NOW);
#else
  LibraryHandle handle = ::dlopen(real_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
#endif
  if (!handle) {
#ifdef MOCKTAIL_USE_BIONIC_LINKER
    std::cerr << "  [bionic-linker] failed to load '" << real_path
              << "': " << ::linker::dlerror() << '\n';
#else
    std::cerr << "  [linker] dlopen failed for '" << real_path
              << "': " << ::dlerror() << '\n';
#endif
    return nullptr;
  }

  const std::string& registry_name =
      logical_name.empty() ? real_path : logical_name;
  AndroidLibraryLoadObserver observer = nullptr;
  void* observer_context = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    g_symbol_table[registry_name] = handle;
    observer = g_library_load_observer;
    observer_context = g_library_load_observer_context;
  }

  std::uintptr_t image_base = 0;
#ifdef MOCKTAIL_USE_BIONIC_LINKER
  image_base = static_cast<std::uintptr_t>(::linker::get_library_base(handle));
#endif
  if (observer != nullptr &&
      !observer(observer_context, registry_name, image_base)) {
    {
      std::lock_guard<std::mutex> lock(g_registry_mutex);
      RemoveHandleFromRegistry(handle);
    }
#ifdef MOCKTAIL_USE_BIONIC_LINKER
    ::linker::dlclose(handle);
#else
    ::dlclose(handle);
#endif
    std::cerr << "  [linker] Android image observer rejected '" << real_path
              << "'\n";
    return nullptr;
  }
  std::cout << "  [linker] Loaded '" << real_path
            << "' as '" << registry_name << "'\n";
  return handle;
}

LibraryHandle LoadLibrary(const std::string& real_path,
                          const std::string& host_alias) {
  if (host_alias == "libroblox") {
    return OpenAndroidLibrary(real_path, host_alias);
  }

  // Register adapters here so libc.so owns the guest ABI per SONAME.
  if (real_path == "libc.so") {
    RegisterBionicAtForkRuntimeForLibc();
    RegisterBionicHostLibcRuntimeForLibc();
    RegisterBionicLargeFileRuntimeForLibc();
  }

  SymbolMap legacy_exports;
  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    legacy_exports =
        LegacyExportsForSyntheticLibrary(real_path, g_legacy_bionic_symbols);
    auto specific = g_synthetic_symbols.find(real_path);
    if (specific != g_synthetic_symbols.end()) {
      for (const auto& [name, address] : specific->second) {
        legacy_exports[name] = address;
      }
    }
  }
  LibraryHandle handle = AddSyntheticLibrary(real_path, legacy_exports);
  if (handle != nullptr && host_alias != real_path) {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    g_symbol_table[host_alias] = handle;
  }
  return handle;
}

bool IsBlockedRobloxCrashReportNativeSymbol(
    std::string_view symbol_name) noexcept {
  constexpr std::string_view kPrefix =
      "Java_com_roblox_engine_jni_NativeSettingsInterface_";
  constexpr std::string_view kBlockedMethods[] = {
      "nativeInitCrashpad",
      "nativeInitAppCrashpadReporter",
      "nativeRunCrashpadHandler",
  };
  if (symbol_name.size() <= kPrefix.size() ||
      symbol_name.substr(0, kPrefix.size()) != kPrefix) {
    return false;
  }
  const std::string_view method = symbol_name.substr(kPrefix.size());
  for (const std::string_view blocked : kBlockedMethods) {
    if (method == blocked || (method.size() > blocked.size() &&
                              method.substr(0, blocked.size()) == blocked &&
                              method.substr(blocked.size(), 2) == "__")) {
      return true;
    }
  }
  return false;
}

void* ResolveSymbol(LibraryHandle handle, const std::string& symbol_name) {
  if (!handle || IsBlockedRobloxCrashReportNativeSymbol(symbol_name)) {
    return nullptr;
  }

#ifdef MOCKTAIL_USE_BIONIC_LINKER
  void* sym = ::linker::dlsym(handle, symbol_name.c_str());
  if (!sym) {
    char* err = ::linker::dlerror();
    std::cerr << "  [linker] symbol '" << symbol_name << "' not found. dlerror: "
              << (err ? err : "unknown error") << '\n';
  }
#else
  void* sym = ::dlsym(handle, symbol_name.c_str());
  if (!sym) {
    char* err = ::dlerror();
    std::cerr << "  [linker] symbol '" << symbol_name << "' not found. dlerror: "
              << (err ? err : "unknown error") << '\n';
  }
#endif

  return sym;
}

void UnloadLibrary(LibraryHandle handle) {
  if (!handle) {
    return;
  }

  bool synthetic = false;
  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    synthetic = g_synthetic_handles.erase(handle) != 0;
    RemoveHandleFromRegistry(handle);
  }

#ifdef MOCKTAIL_USE_BIONIC_LINKER
  if (synthetic) {
    ::linker::unload_library(handle);
  } else {
    ::linker::dlclose(handle);
  }
#else
  static_cast<void>(synthetic);
  ::dlclose(handle);
#endif
}

const std::unordered_map<std::string, LibraryHandle>& GetSymbolTable() {
  return g_symbol_table;
}

}  // namespace linker
