#include <cstdio>
#include <cstdlib>

namespace {

// This fixture must only be inspected as an ELF file. If a regression lets an
// unknown Build ID reach the native loader, the test fails before JNI_OnLoad.
__attribute__((constructor)) void FailIfLoaded() {
  std::fputs("unknown Roblox fixture reached native loading\n", stderr);
  std::abort();
}

}  // namespace

extern "C" int JNI_OnLoad(void*, void*) { return 0x10006; }
