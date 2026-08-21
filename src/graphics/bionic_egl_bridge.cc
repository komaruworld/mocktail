#include "mocktail/graphics/bionic_egl_bridge.h"

#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>

#include <cstdlib>

namespace mocktail::graphics {
namespace {

constexpr const char *kEglExportNames[] = {
    "eglBindAPI",
    "eglChooseConfig",
    "eglCreateContext",
    "eglCreatePbufferSurface",
    "eglCreateWindowSurface",
    "eglDestroyContext",
    "eglDestroySurface",
    "eglGetConfigAttrib",
    "eglGetCurrentContext",
    "eglGetCurrentDisplay",
    "eglGetCurrentSurface",
    "eglGetDisplay",
    "eglGetError",
    "eglGetProcAddress",
    "eglInitialize",
    "eglMakeCurrent",
    "eglQueryString",
    "eglQuerySurface",
    "eglSwapBuffers",
    "eglSwapInterval",
    "eglTerminate",
};

std::string CanonicalPath(const std::string &path) {
  char resolved[PATH_MAX + 1] = {};
  if (::realpath(path.c_str(), resolved) == nullptr) {
    return path;
  }
  return resolved;
}

std::string ExecutableDirectory() {
  char executable_path[PATH_MAX + 1] = {};
  const ssize_t length =
      ::readlink("/proc/self/exe", executable_path, PATH_MAX);
  if (length <= 0) {
    return {};
  }
  executable_path[length] = '\0';
  std::string path(executable_path);
  const std::string::size_type separator = path.find_last_of('/');
  if (separator == std::string::npos) {
    return {};
  }
  return separator == 0 ? "/" : path.substr(0, separator);
}

} // namespace

bool BionicEglBridge::Load() {
  if (handle_ != nullptr) {
    return true;
  }

  exports_.clear();
  error_.clear();
  library_path_.clear();

  const char *directory_override =
      std::getenv("MOCKTAIL_RUNTIME_LIBRARY_DIR");
  const std::string directory =
      directory_override != nullptr && directory_override[0] != '\0'
          ? directory_override
          : ExecutableDirectory();
  if (directory.empty()) {
    error_ = "could not determine the executable directory";
    return false;
  }
  library_path_ = CanonicalPath(directory + "/libEGL.so");

  ::dlerror();
  void *loaded_handle = ::dlopen(library_path_.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (loaded_handle == nullptr) {
    const char *loader_error = ::dlerror();
    error_ = "could not load exact EGL bridge '" + library_path_ + "'";
    if (loader_error != nullptr) {
      error_ += ": ";
      error_ += loader_error;
    }
    return false;
  }

  for (const char *name : kEglExportNames) {
    ::dlerror();
    void *address = ::dlsym(loaded_handle, name);
    const char *loader_error = ::dlerror();
    if (address == nullptr || loader_error != nullptr) {
      error_ = "exact EGL bridge is missing export '";
      error_ += name;
      error_ += "'";
      ::dlclose(loaded_handle);
      exports_.clear();
      return false;
    }

    Dl_info origin = {};
    if (::dladdr(address, &origin) == 0 || origin.dli_fname == nullptr ||
        CanonicalPath(origin.dli_fname) != library_path_) {
      error_ = "EGL export '";
      error_ += name;
      error_ += "' did not resolve from the exact Mocktail bridge";
      ::dlclose(loaded_handle);
      exports_.clear();
      return false;
    }
    exports_.emplace(name, address);
  }

  handle_ = loaded_handle;
  return true;
}

} // namespace mocktail::graphics
