#include <dlfcn.h>

#include <cstring>

namespace {

bool UsesHostCheckedMemoryAbi() {
  void* symbol = dlsym(RTLD_DEFAULT, "__memcpy_chk");
  Dl_info info{};
  return symbol != nullptr && dladdr(symbol, &info) != 0 &&
         info.dli_fname != nullptr &&
         std::strstr(info.dli_fname, "libc.so") != nullptr;
}

}  // namespace

extern "C" int real_gamemode_query_status() { return 0; }

extern "C" int real_gamemode_request_start() {
  return UsesHostCheckedMemoryAbi() ? 0 : -1;
}

extern "C" int real_gamemode_request_end() { return 0; }

extern "C" const char* real_gamemode_error_string() {
  return "GameMode client resolved a checked-memory symbol from the app";
}
