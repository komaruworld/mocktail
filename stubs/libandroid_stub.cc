// Android NDK compatibility symbols used by libroblox.so on Linux. Asset calls
// can read from an extracted directory or directly from an APK.
//
// Symbols sourced from Android NDK r28 android/native_activity.h,
// android/asset_manager.h, android/looper.h, android/configuration.h.

#include "libc_shim/vulkan_etc1_sky_transcoder.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <minizip/unzip.h>
#if defined(MOCKTAIL_MINIZIP_HAS_STREAM_TELL)
#include <minizip/mz_strm.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unordered_map>
#include <vector>

// Read-only asset access

struct AAssetManager {};
struct AAsset {
  std::string path;
  std::string apk_entry;
  std::vector<unsigned char> data;
  size_t offset = 0;
};
struct AAssetDir {};

namespace {

AAssetManager g_asset_manager;

const char* GetEnvNonEmpty(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' ? value : nullptr;
}

bool TraceEnabled() {
  static const bool enabled =
      GetEnvNonEmpty("MOCKTAIL_ANDROID_STUB_TRACE") != nullptr ||
      GetEnvNonEmpty("MOCKTAIL_WINDOW_TRACE") != nullptr ||
      GetEnvNonEmpty("MOCKTAIL_FULL_TRACE") != nullptr;
  return enabled;
}

bool AssetTraceEnabled() {
  static const bool enabled =
      GetEnvNonEmpty("MOCKTAIL_ASSET_TRACE") != nullptr ||
      GetEnvNonEmpty("MOCKTAIL_TRACE_ALL") != nullptr ||
      GetEnvNonEmpty("MOCKTAIL_FULL_TRACE") != nullptr;
  return enabled;
}

std::string_view StripAssetUriPrefix(std::string_view path) {
  static constexpr const char* kPrefixes[] = {
      "file:///android_asset/",
      "rbxasset://",
  };
  for (const char* prefix : kPrefixes) {
    const size_t prefix_len = std::strlen(prefix);
    if (path.size() >= prefix_len && path.substr(0, prefix_len) == prefix) {
      return path.substr(prefix_len);
    }
  }
  return path;
}

bool HasUnsafePathSegment(std::string_view path) {
  if (path.empty() || path[0] == '/') {
    return true;
  }
  return path == "." || path == ".." ||
         path.find("/../") != std::string_view::npos || path.find("../") == 0 ||
         (path.size() >= 3 && path.rfind("/..") == path.size() - 3) ||
         path.find("//") != std::string_view::npos;
}

std::string_view StripAndroidAssetPrefix(std::string_view path) {
  static constexpr const char* kPrefixes[] = {
      "assets/",
      "content/",
  };
  for (const char* prefix : kPrefixes) {
    const size_t prefix_len = std::strlen(prefix);
    if (path.size() >= prefix_len && path.substr(0, prefix_len) == prefix) {
      return path.substr(prefix_len);
    }
  }
  return path;
}

bool FileExists(const std::string& path) {
  struct stat st;
  return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

struct AssetLookupCache {
  std::mutex mutex;
  std::unordered_map<std::string, std::string> resolved_paths;
};

AssetLookupCache g_asset_lookup_cache;
constexpr std::size_t kMaxCachedAssetPaths = 1024 * 5;

std::string AssetCacheKey(const std::string& root,
                          const std::string& requested) {
  std::string key;
  key.reserve(root.size() + 1 + requested.size());
  key.append(root);
  key.push_back('\0');
  key.append(requested);
  return key;
}

std::string ResolveAssetPath(const char* filename) {
  if (filename == nullptr) {
    return {};
  }
  const std::string_view requested_view = StripAssetUriPrefix(filename);
  if (HasUnsafePathSegment(requested_view)) {
    return {};
  }

  const std::string requested(requested_view);
  const char* root_env = GetEnvNonEmpty("MOCKTAIL_ASSET_ROOT");
  const std::string root = root_env != nullptr ? root_env : "rbx_bin/assets";
  const std::string cache_key = AssetCacheKey(root, requested);

  {
    std::scoped_lock lock(g_asset_lookup_cache.mutex);
    auto it = g_asset_lookup_cache.resolved_paths.find(cache_key);
    if (it != g_asset_lookup_cache.resolved_paths.end()) {
      if (FileExists(it->second)) {
        return it->second;
      }
      g_asset_lookup_cache.resolved_paths.erase(it);
    }
  }

  const std::string stripped(StripAndroidAssetPrefix(requested_view));

  const std::vector<std::string> candidates = {
      root + "/" + requested,
      root + "/" + stripped,
      root + "/ExtraContent/" + requested,
      root + "/ExtraContent/" + stripped,
  };
  for (const std::string& candidate : candidates) {
    if (FileExists(candidate)) {
      std::scoped_lock lock(g_asset_lookup_cache.mutex);
      if (g_asset_lookup_cache.resolved_paths.size() >=
              kMaxCachedAssetPaths &&
          g_asset_lookup_cache.resolved_paths.find(cache_key) ==
              g_asset_lookup_cache.resolved_paths.end()) {
        g_asset_lookup_cache.resolved_paths.erase(
            g_asset_lookup_cache.resolved_paths.begin());
      }
      g_asset_lookup_cache.resolved_paths.insert_or_assign(cache_key,
                                                           candidate);
      return candidate;
    }
  }

  return {};
}

bool LoadFile(const std::string& path, std::vector<unsigned char>* data) {
  if (data == nullptr) {
    return false;
  }
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return false;
  }
  long size = std::ftell(file);
  if (size < 0) {
    std::fclose(file);
    return false;
  }
  if (std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    return false;
  }
  data->resize(static_cast<size_t>(size));
  if (size > 0) {
    size_t read = std::fread(data->data(), 1, static_cast<size_t>(size), file);
    if (read != static_cast<size_t>(size)) {
      std::fclose(file);
      return false;
    }
  }
  std::fclose(file);
  return true;
}

std::string ApkEntryName(const char* filename) {
  std::string_view requested = StripAssetUriPrefix(filename);
  constexpr const char* kAssetsPrefix = "assets/";
  if (requested.rfind(kAssetsPrefix, 0) == 0) {
    requested.remove_prefix(std::strlen(kAssetsPrefix));
  }
  return "assets/" + std::string(requested);
}

bool OpenStoredApkEntryDescriptor(const std::string& entry_name, int* fd,
                                  off_t* start, off_t* length) {
  const char* apk_path = GetEnvNonEmpty("MOCKTAIL_ASSET_APK_PATH");
  if (apk_path == nullptr || fd == nullptr || start == nullptr ||
      length == nullptr) {
    return false;
  }
  unzFile archive = unzOpen64(apk_path);
  if (archive == nullptr ||
      unzLocateFile(archive, entry_name.c_str(), 1) != UNZ_OK) {
    if (archive != nullptr) {
      unzClose(archive);
    }
    return false;
  }
  unz_file_info64 info{};
  if (unzGetCurrentFileInfo64(archive, &info, nullptr, 0, nullptr, 0, nullptr,
                              0) != UNZ_OK ||
      info.compression_method != 0 || unzOpenCurrentFile(archive) != UNZ_OK) {
    unzClose(archive);
    return false;
  }
  ZPOS64_T data_offset = 0;
#if defined(MOCKTAIL_MINIZIP_HAS_ZSTREAM_POS)
  data_offset = unzGetCurrentFileZStreamPos64(archive);
#elif defined(MOCKTAIL_MINIZIP_HAS_STREAM_TELL)
  const int64_t stream_offset = mz_stream_tell(unzGetStream(archive));
  if (stream_offset < 0) {
    unzCloseCurrentFile(archive);
    unzClose(archive);
    return false;
  }
  data_offset = static_cast<ZPOS64_T>(stream_offset);
#endif
  unzCloseCurrentFile(archive);
  unzClose(archive);
  if (data_offset > static_cast<ZPOS64_T>(INT64_MAX) ||
      info.uncompressed_size > static_cast<ZPOS64_T>(INT64_MAX)) {
    return false;
  }
  const int apk_fd = ::open(apk_path, O_RDONLY | O_CLOEXEC);
  if (apk_fd < 0) {
    return false;
  }
  *fd = apk_fd;
  *start = static_cast<off_t>(data_offset);
  *length = static_cast<off_t>(info.uncompressed_size);
  return true;
}

template <typename Fn>
Fn ResolveCached(Fn* slot, const char* name) {
  Fn fn = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (__builtin_expect(fn == nullptr, 0)) {
    fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, name));
    if (fn != nullptr) {
      __atomic_store_n(slot, fn, __ATOMIC_RELEASE);
    }
  }
  return fn;
}

void* MocktailNativeWindow() {
  using Fn = void* (*)();
  static Fn cached_fn = nullptr;
  Fn fn = ResolveCached(&cached_fn, "mocktail_native_window");
  return fn != nullptr ? fn() : nullptr;
}

int MocktailWindowWidth() {
  using Fn = int (*)();
  static Fn cached_fn = nullptr;
  Fn fn = ResolveCached(&cached_fn, "mocktail_window_width");
  return fn != nullptr ? fn() : 1280;
}

int MocktailWindowHeight() {
  using Fn = int (*)();
  static Fn cached_fn = nullptr;
  Fn fn = ResolveCached(&cached_fn, "mocktail_window_height");
  return fn != nullptr ? fn() : 720;
}

bool MocktailUsesDirectVulkan() {
  using Fn = bool (*)();
  static Fn cached_fn = nullptr;
  Fn fn = ResolveCached(&cached_fn, "mocktail_window_uses_direct_vulkan");
  if (fn != nullptr) {
    return fn();
  }
  const char* backend = GetEnvNonEmpty("MOCKTAIL_GRAPHICS_BACKEND");
  return backend != nullptr && std::strcmp(backend, "direct-vulkan") == 0;
}

}  // namespace

extern "C" {

AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename,
                           int) {
  if (mgr == nullptr) {
    return nullptr;
  }
  std::string path = ResolveAssetPath(filename);
  if (path.empty()) {
    return nullptr;
  }
  auto* asset = new AAsset;
  asset->path = path;
  asset->apk_entry = ApkEntryName(filename);
  if (!LoadFile(path, &asset->data)) {
    delete asset;
    return nullptr;
  }
  if (MocktailUsesDirectVulkan()) {
    libc_shim::TranscodeEtc1SkyTextureForVulkan(path.c_str(), &asset->data);
  }
  if (AssetTraceEnabled()) {
    std::fprintf(stderr, "[asset] open %s -> %s (%zu bytes)\n", filename,
                 path.c_str(), asset->data.size());
  }
  return asset;
}

void AAsset_close(AAsset* asset) {
  if (asset != nullptr && AssetTraceEnabled() &&
      asset->path.find("shader") != std::string::npos) {
    std::fprintf(stderr, "[asset] close %s buffer=%p size=%zu\n",
                 asset->path.c_str(), asset->data.data(), asset->data.size());
  }
  delete asset;
}

int AAsset_read(AAsset* asset, void* buf, size_t count) {
  if (asset == nullptr || buf == nullptr) {
    return -1;
  }
  const size_t remaining = asset->offset <= asset->data.size()
                               ? asset->data.size() - asset->offset
                               : 0;
  const size_t to_read = std::min(count, remaining);
  if (to_read > 0) {
    std::memcpy(buf, asset->data.data() + asset->offset, to_read);
    asset->offset += to_read;
  }
  if (AssetTraceEnabled() && asset->path.find("shader") != std::string::npos) {
    std::fprintf(stderr, "[asset] read %s count=%zu -> %zu offset=%zu\n",
                 asset->path.c_str(), count, to_read, asset->offset);
  }
  return static_cast<int>(to_read);
}

const void* AAsset_getBuffer(AAsset* asset) {
  if (asset == nullptr) {
    return nullptr;
  }
  if (AssetTraceEnabled() && asset->path.find("shader") != std::string::npos) {
    std::fprintf(stderr, "[asset] getBuffer %s -> %zu bytes\n",
                 asset->path.c_str(), asset->data.size());
  }
  return asset->data.empty() ? static_cast<const void*>("")
                             : static_cast<const void*>(asset->data.data());
}

off_t AAsset_getLength(AAsset* asset) {
  if (asset != nullptr && AssetTraceEnabled() &&
      asset->path.find("shader") != std::string::npos) {
    std::fprintf(stderr, "[asset] getLength %s -> %zu bytes\n",
                 asset->path.c_str(), asset->data.size());
  }
  return asset == nullptr ? 0 : static_cast<off_t>(asset->data.size());
}

off_t AAsset_getLength64(AAsset* asset) { return AAsset_getLength(asset); }

off_t AAsset_getRemainingLength(AAsset* asset) {
  if (asset == nullptr || asset->offset > asset->data.size()) {
    return 0;
  }
  if (AssetTraceEnabled() && asset->path.find("shader") != std::string::npos) {
    std::fprintf(stderr, "[asset] getRemainingLength %s -> %zu bytes\n",
                 asset->path.c_str(), asset->data.size() - asset->offset);
  }
  return static_cast<off_t>(asset->data.size() - asset->offset);
}

off_t AAsset_getRemainingLength64(AAsset* asset) {
  return AAsset_getRemainingLength(asset);
}

off_t AAsset_seek(AAsset* asset, off_t offset, int whence) {
  if (asset == nullptr) {
    return -1;
  }
  off_t base = 0;
  if (whence == SEEK_SET) {
    base = 0;
  } else if (whence == SEEK_CUR) {
    base = static_cast<off_t>(asset->offset);
  } else if (whence == SEEK_END) {
    base = static_cast<off_t>(asset->data.size());
  } else {
    errno = EINVAL;
    return -1;
  }
  off_t next = base + offset;
  if (next < 0) {
    errno = EINVAL;
    return -1;
  }
  const auto clamped = std::min(static_cast<size_t>(next), asset->data.size());
  asset->offset = clamped;
  if (AssetTraceEnabled() && asset->path.find("shader") != std::string::npos) {
    std::fprintf(stderr, "[asset] seek %s offset=%lld whence=%d -> %zu\n",
                 asset->path.c_str(), static_cast<long long>(offset), whence,
                 asset->offset);
  }
  return static_cast<off_t>(asset->offset);
}

off_t AAsset_seek64(AAsset* asset, off_t offset, int whence) {
  return AAsset_seek(asset, offset, whence);
}

int AAsset_openFileDescriptor(AAsset* asset, off_t* outStart,
                              off_t* outLength) {
  if (asset == nullptr || asset->path.empty()) {
    return -1;
  }
  int fd = -1;
  off_t start = 0;
  off_t descriptor_length = static_cast<off_t>(asset->data.size());
  if (!OpenStoredApkEntryDescriptor(asset->apk_entry, &fd, &start,
                                    &descriptor_length)) {
    fd = ::open(asset->path.c_str(), O_RDONLY | O_CLOEXEC);
  }
  if (fd < 0) {
    return -1;
  }
  if (outStart != nullptr) {
    *outStart = start;
  }
  if (outLength != nullptr) {
    *outLength = descriptor_length;
  }
  if (AssetTraceEnabled() && asset->path.find("shader") != std::string::npos) {
    std::fprintf(stderr,
                 "[asset] openFileDescriptor %s -> fd=%d start=%lld "
                 "length=%zu\n",
                 asset->path.c_str(), fd, static_cast<long long>(start),
                 static_cast<size_t>(descriptor_length));
  }
  return fd;
}

int AAsset_openFileDescriptor64(AAsset* asset, off_t* outStart,
                                off_t* outLength) {
  return AAsset_openFileDescriptor(asset, outStart, outLength);
}

AAssetManager* AAssetManager_fromJava(void*, void*) {
  return &g_asset_manager;
}

// AConfiguration — device configuration query

struct AConfiguration {};

AConfiguration* AConfiguration_new() {
  return static_cast<AConfiguration*>(calloc(1, sizeof(AConfiguration)));
}

void AConfiguration_delete(AConfiguration* config) { free(config); }

void AConfiguration_fromAssetManager(AConfiguration*,
                                     AAssetManager*) {}

void AConfiguration_getLanguage(AConfiguration*, char* out) {
  if (out) {
    out[0] = 'e';
    out[1] = 'n';
  }
}

void AConfiguration_getCountry(AConfiguration*, char* out) {
  if (out) {
    out[0] = 'U';
    out[1] = 'S';
  }
}

int32_t AConfiguration_getScreenSize(AConfiguration*) {
  return 2;  // ACONFIGURATION_SCREENSIZE_NORMAL
}

int32_t AConfiguration_getScreenWidthDp(AConfiguration*) {
  return 360;
}

int32_t AConfiguration_getScreenHeightDp(AConfiguration*) {
  return 640;
}

int32_t AConfiguration_getNavHidden(AConfiguration*) {
  return 1;  // ACONFIGURATION_NAVHIDDEN_YES
}

}  // extern "C"

// ALooper — high-performance Linux epoll + eventfd event loop

namespace {

constexpr int kPollWake = -1;
constexpr int kPollCallback = -2;
constexpr int kPollTimeout = -3;
constexpr int kPollError = -4;
constexpr int kPrepareAllowNonCallbacks = 1 << 0;
constexpr uint64_t kWakeToken = 0;

using LooperCallback = int (*)(int, int, void*);

struct LooperRegistration {
  int fd = -1;
  int ident = 0;
  int events = 0;
  LooperCallback callback = nullptr;
  void* data = nullptr;
  uint64_t token = 0;
};

struct LooperResponse {
  LooperRegistration registration;
  int events = 0;
};

struct ALooper {
  explicit ALooper(int requested_opts)
      : opts(requested_opts),
        allow_non_callbacks(
            (requested_opts & kPrepareAllowNonCallbacks) != 0),
        epoll_fd(::epoll_create1(EPOLL_CLOEXEC)),
        wake_fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (epoll_fd < 0 || wake_fd < 0) {
      if (epoll_fd >= 0) ::close(epoll_fd);
      if (wake_fd >= 0) ::close(wake_fd);
      epoll_fd = -1;
      wake_fd = -1;
      return;
    }
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.u64 = kWakeToken;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_fd, &event) != 0) {
      ::close(epoll_fd);
      ::close(wake_fd);
      epoll_fd = -1;
      wake_fd = -1;
    }
  }

  ~ALooper() {
    if (epoll_fd >= 0) ::close(epoll_fd);
    if (wake_fd >= 0) ::close(wake_fd);
  }

  ALooper(const ALooper&) = delete;
  ALooper& operator=(const ALooper&) = delete;

  bool valid() const { return epoll_fd >= 0 && wake_fd >= 0; }

  const int opts;
  const bool allow_non_callbacks;
  int epoll_fd = -1;
  int wake_fd = -1;
  std::atomic<uint32_t> refs{1};
  std::atomic<bool> polling{false};
  std::mutex mutex;
  std::unordered_map<int, uint64_t> token_by_fd;
  std::unordered_map<uint64_t, LooperRegistration> registrations_by_token;
  uint64_t next_token = 1;
  std::vector<epoll_event> epoll_scratch;

  // Only the associated thread accesses pending_responses.  Entries are
  // generation-checked before being returned so a concurrent remove/re-add
  // cannot surface an event for a reused descriptor.
  std::deque<LooperResponse> pending_responses;
};

void ReleaseLooperReference(ALooper* looper);

struct ThreadLooperState {
  ~ThreadLooperState() { ReleaseLooperReference(looper); }

  ALooper* looper = nullptr;
};

thread_local ThreadLooperState g_thread_looper;

void ReleaseLooperReference(ALooper* looper) {
  if (looper != nullptr &&
      looper->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete looper;
  }
}

uint32_t EpollEventsFromAndroid(int events) {
  uint32_t result = 0;
  if ((events & (1 << 0)) != 0) result |= EPOLLIN;
  if ((events & (1 << 1)) != 0) result |= EPOLLOUT;
  return result;
}

int AndroidEventsFromEpoll(uint32_t events) {
  int result = 0;
  if ((events & EPOLLIN) != 0) result |= 1 << 0;
  if ((events & EPOLLOUT) != 0) result |= 1 << 1;
  if ((events & EPOLLERR) != 0) result |= 1 << 2;
  if ((events & EPOLLHUP) != 0) result |= 1 << 3;
  return result;
}

uint64_t NextRegistrationTokenLocked(ALooper* looper) {
  for (;;) {
    const uint64_t token = looper->next_token++;
    if (looper->next_token == kWakeToken) {
      looper->next_token = 1;
    }
    if (token != kWakeToken &&
        looper->registrations_by_token.find(token) ==
            looper->registrations_by_token.end()) {
      return token;
    }
  }
}

bool RegistrationIsCurrent(ALooper* looper,
                           const LooperRegistration& registration) {
  std::scoped_lock lock(looper->mutex);
  const auto fd_it = looper->token_by_fd.find(registration.fd);
  if (fd_it == looper->token_by_fd.end() ||
      fd_it->second != registration.token) {
    return false;
  }
  return looper->registrations_by_token.find(registration.token) !=
         looper->registrations_by_token.end();
}

int RemoveFdLocked(ALooper* looper, int fd, uint64_t expected_token) {
  const auto fd_it = looper->token_by_fd.find(fd);
  if (fd_it == looper->token_by_fd.end() ||
      (expected_token != 0 && fd_it->second != expected_token)) {
    return 0;
  }

  const uint64_t token = fd_it->second;
  const int ctl_result =
      ::epoll_ctl(looper->epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
  const int ctl_errno = errno;

  // Closing an fd automatically removes it from epoll.  Treat that as a
  // successful logical removal.  On a real internal failure, preserve the
  // registration so poll and a later removal still agree with the kernel.
  if (ctl_result != 0 && ctl_errno != ENOENT && ctl_errno != EBADF) {
    errno = ctl_errno;
    return -1;
  }
  looper->token_by_fd.erase(fd_it);
  looper->registrations_by_token.erase(token);
  return 1;
}

void ClearPollOutputs(int* out_fd, int* out_events, void** out_data) {
  if (out_fd != nullptr) *out_fd = 0;
  if (out_events != nullptr) *out_events = 0;
  if (out_data != nullptr) *out_data = nullptr;
}

bool TakePendingIdent(ALooper* looper, int* out_fd, int* out_events,
                      void** out_data, int* result) {
  while (!looper->pending_responses.empty()) {
    const LooperResponse response = looper->pending_responses.front();
    looper->pending_responses.pop_front();
    if (!RegistrationIsCurrent(looper, response.registration) ||
        response.registration.callback != nullptr) {
      continue;
    }
    if (out_fd != nullptr) *out_fd = response.registration.fd;
    if (out_events != nullptr) *out_events = response.events;
    if (out_data != nullptr) *out_data = response.registration.data;
    *result = response.registration.ident;
    return true;
  }
  return false;
}

int RemainingPollTimeout(
    int original_timeout,
    const std::chrono::steady_clock::time_point& deadline) {
  if (original_timeout < 0) return -1;
  if (original_timeout == 0) return 0;

  const auto remaining = deadline - std::chrono::steady_clock::now();
  if (remaining <= std::chrono::steady_clock::duration::zero()) return 0;
  auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
  if (millis < remaining) {
    millis += std::chrono::milliseconds(1);
  }
  return static_cast<int>(std::min<int64_t>(
      millis.count(), std::numeric_limits<int>::max()));
}

}  // namespace

extern "C" {

ALooper* ALooper_forThread() { return g_thread_looper.looper; }

ALooper* ALooper_prepare(int opts) {
  if (g_thread_looper.looper != nullptr) {
    return g_thread_looper.looper;
  }

  auto* looper = new (std::nothrow) ALooper(opts);
  if (looper == nullptr) {
    errno = ENOMEM;
    return nullptr;
  }
  if (!looper->valid()) {
    delete looper;
    return nullptr;
  }
  g_thread_looper.looper = looper;
  return looper;
}

void ALooper_acquire(ALooper* looper) {
  if (looper != nullptr) {
    looper->refs.fetch_add(1, std::memory_order_relaxed);
  }
}

void ALooper_release(ALooper* looper) {
  ReleaseLooperReference(looper);
}

int ALooper_addFd(ALooper* looper, int fd, int ident, int events,
                  int (*callback)(int, int, void*), void* data) {
  const bool callback_ident_valid = ident >= 0 || ident == kPollCallback;
  if (looper == nullptr || !looper->valid() || fd < 0 ||
      (callback == nullptr &&
       (!looper->allow_non_callbacks || ident < 0)) ||
      (callback != nullptr && !callback_ident_valid)) {
    errno = EINVAL;
    return -1;
  }

  std::scoped_lock lock(looper->mutex);
  auto fd_it = looper->token_by_fd.find(fd);
  const bool update = fd_it != looper->token_by_fd.end();
  const uint64_t old_token = update ? fd_it->second : 0;
  const uint64_t token = NextRegistrationTokenLocked(looper);
  const LooperRegistration registration = {
      fd, callback != nullptr ? kPollCallback : ident, events, callback, data,
      token};

  const auto inserted =
      looper->registrations_by_token.emplace(token, registration);
  if (!inserted.second) {
    errno = EAGAIN;
    return -1;
  }
  if (update) {
    fd_it->second = token;
  } else {
    const auto fd_inserted = looper->token_by_fd.emplace(fd, token);
    if (!fd_inserted.second) {
      looper->registrations_by_token.erase(token);
      errno = EAGAIN;
      return -1;
    }
    fd_it = fd_inserted.first;
  }

  epoll_event event{};
  event.events = EpollEventsFromAndroid(events);
  event.data.u64 = token;
  int operation = update ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
  int ctl_result = ::epoll_ctl(looper->epoll_fd, operation, fd, &event);
  if (ctl_result != 0 && update && errno == ENOENT) {
    operation = EPOLL_CTL_ADD;
    ctl_result = ::epoll_ctl(looper->epoll_fd, operation, fd, &event);
  } else if (ctl_result != 0 && !update && errno == EEXIST) {
    operation = EPOLL_CTL_MOD;
    ctl_result = ::epoll_ctl(looper->epoll_fd, operation, fd, &event);
  }

  if (ctl_result != 0) {
    const int ctl_errno = errno;
    looper->registrations_by_token.erase(token);
    if (update) {
      fd_it->second = old_token;
    } else {
      looper->token_by_fd.erase(fd_it);
    }
    errno = ctl_errno;
    return -1;
  }

  if (update) {
    looper->registrations_by_token.erase(old_token);
  }
  return 1;
}

int ALooper_removeFd(ALooper* looper, int fd) {
  if (looper == nullptr || !looper->valid() || fd < 0) {
    errno = EINVAL;
    return -1;
  }
  std::scoped_lock lock(looper->mutex);
  return RemoveFdLocked(looper, fd, 0);
}

void ALooper_wake(ALooper* looper) {
  if (looper == nullptr || looper->wake_fd < 0) return;
  const uint64_t value = 1;
  ssize_t written;
  do {
    written = ::write(looper->wake_fd, &value, sizeof(value));
  } while (written < 0 && errno == EINTR);
}

bool ALooper_isPolling(ALooper* looper) {
  return looper != nullptr &&
         looper->polling.load(std::memory_order_acquire);
}

int ALooper_pollOnce(int timeout_ms, int* out_fd, int* out_events,
                     void** out_data) {
  ClearPollOutputs(out_fd, out_events, out_data);
  auto* looper = ALooper_forThread();
  if (looper == nullptr || !looper->valid()) return kPollError;

  int pending_result = 0;
  if (TakePendingIdent(looper, out_fd, out_events, out_data,
                       &pending_result)) {
    return pending_result;
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(
                            timeout_ms > 0 ? timeout_ms : 0);
  int wait_timeout = timeout_ms;

  for (;;) {
    int max_events = 0;
    {
      std::scoped_lock lock(looper->mutex);
      const std::size_t event_capacity =
          1 + looper->registrations_by_token.size();
      if (event_capacity >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        errno = EOVERFLOW;
        return kPollError;
      }
      if (looper->epoll_scratch.size() < event_capacity) {
        looper->epoll_scratch.resize(event_capacity);
      }
      max_events = static_cast<int>(looper->epoll_scratch.size());
    }

    looper->polling.store(true, std::memory_order_release);
    const int ready = ::epoll_wait(looper->epoll_fd,
                                   looper->epoll_scratch.data(), max_events,
                                   wait_timeout);
    const int wait_errno = errno;
    looper->polling.store(false, std::memory_order_release);

    if (ready == 0) return kPollTimeout;
    if (ready < 0) {
      if (wait_errno != EINTR) {
        errno = wait_errno;
        return kPollError;
      }
      wait_timeout = RemainingPollTimeout(timeout_ms, deadline);
      continue;
    }

    bool had_wake = false;
    std::vector<LooperResponse> responses;
    responses.reserve(static_cast<std::size_t>(ready));

    for (int i = 0; i < ready; ++i) {
      const epoll_event& event =
          looper->epoll_scratch[static_cast<std::size_t>(i)];
      if (event.data.u64 != kWakeToken) {
        continue;
      }
      uint64_t value = 0;
      ssize_t read_result;
      do {
        read_result = ::read(looper->wake_fd, &value, sizeof(value));
      } while (read_result < 0 && errno == EINTR);
      had_wake = true;
    }

    {
      std::scoped_lock lock(looper->mutex);
      for (int i = 0; i < ready; ++i) {
        const epoll_event& event =
            looper->epoll_scratch[static_cast<std::size_t>(i)];
        const uint64_t token = event.data.u64;
        if (token == kWakeToken) {
          continue;
        }
        const auto registration_it =
            looper->registrations_by_token.find(token);
        if (registration_it == looper->registrations_by_token.end()) {
          continue;
        }
        responses.push_back({registration_it->second,
                             AndroidEventsFromEpoll(event.events)});
      }
    }

    bool invoked_callback = false;
    for (const LooperResponse& response : responses) {
      if (response.registration.callback == nullptr) continue;
      const int keep = response.registration.callback(
          response.registration.fd, response.events,
          response.registration.data);
      invoked_callback = true;
      if (keep == 0) {
        std::scoped_lock lock(looper->mutex);
        (void)RemoveFdLocked(looper, response.registration.fd,
                             response.registration.token);
      }
    }

    for (const LooperResponse& response : responses) {
      if (response.registration.callback == nullptr) {
        looper->pending_responses.push_back(response);
      }
    }

    if (TakePendingIdent(looper, out_fd, out_events, out_data,
                         &pending_result)) {
      return pending_result;
    }

    if (invoked_callback) return kPollCallback;
    if (had_wake) return kPollWake;

    // Every event in the batch belonged to a removed or replaced
    // registration.  Keep waiting without extending the caller's timeout.
    wait_timeout = RemainingPollTimeout(timeout_ms, deadline);
  }
}

int ALooper_pollAll(int timeout_ms, int* out_fd, int* out_events,
                    void** out_data) {
  int current_timeout = timeout_ms;
  for (;;) {
    const int result =
        ALooper_pollOnce(current_timeout, out_fd, out_events, out_data);
    if (result != kPollCallback) return result;
    current_timeout = 0;
  }
}

// ANativeWindow — surface / window handle

struct ANativeWindow {};

void ANativeWindow_acquire(ANativeWindow*) {}
void ANativeWindow_release(ANativeWindow*) {}

ANativeWindow* ANativeWindow_fromSurface(void* env, void* surface) {
  void* native_window = MocktailNativeWindow();
  // The direct-Vulkan WSI adapter ignores the Android window
  // member and creates VkSurfaceKHR from SDL_Window. Preserve the Java Surface
  // token as its opaque ANativeWindow identity so a recreated Surface is not
  // collapsed back onto the same SDL_Window pointer inside Roblox.
  if (surface != nullptr && MocktailUsesDirectVulkan()) {
    if (TraceEnabled()) {
      std::fprintf(stderr,
                   "[androidstub] ANativeWindow_fromSurface env=%p surface=%p "
                   "-> surface_identity=%p backend_window=%p\n",
                   env, surface, surface, native_window);
    }
    return reinterpret_cast<ANativeWindow*>(surface);
  }
  if (native_window != nullptr) {
    if (TraceEnabled()) {
      std::fprintf(stderr,
                   "[androidstub] ANativeWindow_fromSurface env=%p surface=%p "
                   "-> native_window=%p\n",
                   env, surface, native_window);
    }
    return reinterpret_cast<ANativeWindow*>(native_window);
  }
  static ANativeWindow fallback_window;
  if (TraceEnabled()) {
    std::fprintf(stderr,
                 "[androidstub] ANativeWindow_fromSurface env=%p surface=%p "
                 "-> fallback_window=%p\n",
                 env, surface, static_cast<void*>(&fallback_window));
  }
  return &fallback_window;
}

int32_t ANativeWindow_getWidth(ANativeWindow* window) {
  int width = MocktailWindowWidth();
  if (TraceEnabled()) {
    std::fprintf(stderr,
                 "[androidstub] ANativeWindow_getWidth window=%p -> %d\n",
                 static_cast<void*>(window), width);
  }
  return width;
}

int32_t ANativeWindow_getHeight(ANativeWindow* window) {
  int height = MocktailWindowHeight();
  if (TraceEnabled()) {
    std::fprintf(stderr,
                 "[androidstub] ANativeWindow_getHeight window=%p -> %d\n",
                 static_cast<void*>(window), height);
  }
  return height;
}

}  // extern "C"
