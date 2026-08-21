#ifndef MOCKTAIL_LINKER_LINKER_H_
#define MOCKTAIL_LINKER_LINKER_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace linker {

// Opaque; valid only with this linker's resolve/unload functions.
using LibraryHandle = void*;
using SymbolMap = std::unordered_map<std::string, void*>;
using AndroidLibraryLoadObserver = bool (*)(void* context,
                                            std::string_view logical_name,
                                            std::uintptr_t image_base);

struct ProgramHeaderValidation {
  bool valid = false;
  std::string error;

  explicit operator bool() const noexcept { return valid; }
};

LibraryHandle AddSyntheticLibrary(const std::string& soname,
                                  const SymbolMap& exports);

// Updates an already loaded library in place.
void RegisterSyntheticSymbol(const std::string& soname,
                             const std::string& name, void* address);

// These adapters belong to their synthetic SONAME, never the host namespace.
void RegisterBionicPthreadKeyRuntimeForLibc();
void RegisterBionicAtForkRuntimeForLibc();
void RegisterBionicLargeFileRuntimeForLibc();
void RegisterBionicHostLibcRuntimeForLibc();
void RegisterBionicSysconfRuntimeForLibc();
void RegisterBionicSignalRuntimeForLibc();
void RegisterBionicStdioRuntimeForLibc();

// Android dlopen/dlsym must not escape into glibc.
void RegisterBionicDynamicLoaderForLibdl();

SymbolMap GetSyntheticLibrarySymbols(const std::string& soname);

void UpdateAndroidLibraryPath(const std::string& search_path);

// Installs the single process owner that validates or adapts a real Android
// image immediately after relocation and registration. Returning false from
// the observer rejects and unloads that image before startup can call it.
bool SetAndroidLibraryLoadObserver(AndroidLibraryLoadObserver observer,
                                   void* context);
void ClearAndroidLibraryLoadObserver(void* context);

LibraryHandle OpenAndroidLibrary(const std::string& real_path,
                                 const std::string& logical_name);

LibraryHandle LoadLibrary(const std::string& real_path,
                          const std::string& host_alias);

void* ResolveSymbol(LibraryHandle handle, const std::string& symbol_name);

// Crashpad execution is intentionally outside Mocktail's supported JNI
// surface. These exports are never returned to the host composition even when
// a Roblox payload contains them.
bool IsBlockedRobloxCrashReportNativeSymbol(
    std::string_view symbol_name) noexcept;

void UnloadLibrary(LibraryHandle handle);

void RegisterSymbol(const std::string& name, void* addr);

const SymbolMap& GetBionicSymbols();

// Android unwinders must not receive glibc's implementation or layout.
void* BionicDlIteratePhdrAddress();

ProgramHeaderValidation ValidateBionicProgramHeaders(LibraryHandle handle);

// Returns the mapped ELF base for a loaded Android library alias. The lookup
// is synchronized with library registration and returns zero for an unknown
// alias or a host-dlopen library without Bionic image metadata.
std::uintptr_t FindLoadedAndroidLibraryBase(const std::string& logical_name);

const std::unordered_map<std::string, LibraryHandle>& GetSymbolTable();

}  // namespace linker

#endif  // MOCKTAIL_LINKER_LINKER_H_
