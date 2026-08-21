#include "services/browser_tracker_service.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <random>
#include <system_error>

namespace mocktail {
namespace services {
namespace {

constexpr char kInitializeUrl[] =
    "https://apis.roblox.com/browser-tracker-api/device/initialize"
    "?suggestedBrowserTrackerId=";
constexpr std::size_t kMaximumStorageBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaximumCookieBytes = 64 * 1024;
constexpr std::string_view kTrackerCookieName = "RBXEventTrackerV2";

bool IsValidId(const std::string& value) {
  if (value.empty() || value.size() > 20 || value.front() == '0') return false;
  std::uint64_t parsed = 0;
  const auto conversion =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  return conversion.ec == std::errc() &&
         conversion.ptr == value.data() + value.size() && parsed != 0;
}

std::string SuggestedId() {
  std::random_device entropy;
  std::mt19937_64 random(entropy());
  std::uniform_int_distribution<std::uint64_t> distribution(
      1, std::numeric_limits<std::uint64_t>::max());
  return std::to_string(distribution(random));
}

bool ReadStorage(const std::filesystem::path& path, nlohmann::json* storage,
                 std::string* error) {
  std::error_code filesystem_error;
  if (!std::filesystem::exists(path, filesystem_error)) {
    if (filesystem_error) {
      *error = "appStorage is unavailable";
    }
    *storage = nlohmann::json::object();
    return !filesystem_error;
  }
  const auto size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error || size > kMaximumStorageBytes) {
    *error = "appStorage is unavailable or exceeds the size limit";
    return false;
  }
  std::ifstream input(path);
  if (!input ||
      !(*storage = nlohmann::json::parse(input, nullptr, false)).is_object()) {
    *error = "appStorage is not a valid JSON object";
    return false;
  }
  return true;
}

bool ReadSmallFile(const std::filesystem::path& path, std::string* contents,
                   std::string* error) {
  std::error_code filesystem_error;
  if (!std::filesystem::exists(path, filesystem_error)) {
    if (filesystem_error) *error = "cookie file is unavailable";
    return !filesystem_error;
  }
  const auto size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error || size > kMaximumCookieBytes) {
    *error = "cookie file is unavailable or exceeds the size limit";
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *error = "cookie file could not be opened";
    return false;
  }
  contents->assign(std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>());
  if (input.bad()) {
    error->assign("cookie file could not be read");
    return false;
  }
  return true;
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                            value.back() == '\r' || value.back() == '\n'))
    value.remove_suffix(1);
  return value;
}

std::string ExtractCookieValue(std::string_view cookies,
                               std::string_view name) {
  size_t begin = 0;
  while (begin < cookies.size()) {
    while (begin < cookies.size() &&
           (cookies[begin] == ';' || cookies[begin] == '\r' ||
            cookies[begin] == '\n' || cookies[begin] == ' '))
      ++begin;
    size_t end = cookies.find_first_of(";\r\n", begin);
    if (end == std::string_view::npos) end = cookies.size();
    const std::string_view segment = Trim(cookies.substr(begin, end - begin));
    const size_t equals = segment.find('=');
    if (equals != std::string_view::npos &&
        Trim(segment.substr(0, equals)) == name) {
      return std::string(Trim(segment.substr(equals + 1)));
    }
    begin = end + (end < cookies.size());
  }
  return {};
}

bool HasCookie(std::string_view cookies, std::string_view name) {
  size_t begin = 0;
  while (begin < cookies.size()) {
    while (begin < cookies.size() &&
           (cookies[begin] == ';' || cookies[begin] == '\r' ||
            cookies[begin] == '\n' || cookies[begin] == ' ')) {
      ++begin;
    }
    size_t end = cookies.find_first_of(";\r\n", begin);
    if (end == std::string_view::npos) end = cookies.size();
    const std::string_view segment = Trim(cookies.substr(begin, end - begin));
    const size_t equals = segment.find('=');
    if (equals != std::string_view::npos &&
        Trim(segment.substr(0, equals)) == name) {
      return true;
    }
    begin = end + (end < cookies.size());
  }
  return false;
}

std::string BrowserIdFromTracker(std::string_view tracker) {
  size_t begin = 0;
  while (begin < tracker.size()) {
    size_t end = tracker.find('&', begin);
    if (end == std::string_view::npos) end = tracker.size();
    const std::string_view field = tracker.substr(begin, end - begin);
    const size_t equals = field.find('=');
    if (equals != std::string_view::npos &&
        field.substr(0, equals) == "browserid") {
      const std::string id(field.substr(equals + 1));
      return IsValidId(id) ? id : std::string();
    }
    begin = end + (end < tracker.size());
  }
  return {};
}

std::string TrackerFromResponse(const HttpResponse& response) {
  for (const std::string& header : response.headers) {
    constexpr std::string_view prefix = "set-cookie:";
    if (header.size() < prefix.size()) continue;
    std::string lowered = header.substr(0, prefix.size());
    for (char& ch : lowered) ch = static_cast<char>(std::tolower(ch));
    if (lowered != prefix) continue;
    const std::string value =
        ExtractCookieValue(Trim(std::string_view(header).substr(prefix.size())),
                           kTrackerCookieName);
    if (!value.empty()) return value;
  }
  return {};
}

bool WriteAll(int descriptor, const std::string& contents) {
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t written =
        write(descriptor, contents.data() + offset, contents.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

bool IsPrivateDirectory(const struct stat& metadata) {
  return S_ISDIR(metadata.st_mode) && metadata.st_uid == geteuid() &&
         (metadata.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

bool FindExistingDirectoryAncestor(const std::filesystem::path& directory,
                                   std::filesystem::path* ancestor) {
  if (directory.empty() || ancestor == nullptr) return false;
  std::filesystem::path candidate = directory;
  while (true) {
    struct stat metadata = {};
    if (lstat(candidate.c_str(), &metadata) == 0) {
      if (!S_ISDIR(metadata.st_mode)) return false;
      *ancestor = candidate;
      return true;
    }
    if (errno != ENOENT) return false;
    std::filesystem::path parent = candidate.parent_path();
    if (parent.empty() && candidate != ".") parent = ".";
    if (parent.empty() || parent == candidate) return false;
    candidate = parent;
  }
}

bool FsyncDirectoryChain(const std::filesystem::path& first,
                         const std::filesystem::path& last) {
  std::filesystem::path current = first;
  while (true) {
    const int descriptor =
        open(current.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return false;
    struct stat metadata = {};
    const bool synchronized = fstat(descriptor, &metadata) == 0 &&
                              S_ISDIR(metadata.st_mode) &&
                              fsync(descriptor) == 0;
    close(descriptor);
    if (!synchronized) return false;
    if (current == last) return true;
    std::filesystem::path parent = current.parent_path();
    if (parent.empty() && current != ".") parent = ".";
    if (parent.empty() || parent == current) return false;
    current = parent;
  }
}

bool IsSafeDestinationAt(int directory, const std::string& name) {
  struct stat metadata = {};
  if (fstatat(directory, name.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) == 0) {
    return S_ISREG(metadata.st_mode) && metadata.st_uid == geteuid() &&
           metadata.st_nlink == 1;
  }
  return errno == ENOENT;
}

bool IsOpenedFileAt(int directory, const std::string& name, int descriptor) {
  struct stat opened = {};
  struct stat linked = {};
  return fstat(descriptor, &opened) == 0 &&
         fstatat(directory, name.c_str(), &linked, AT_SYMLINK_NOFOLLOW) == 0 &&
         S_ISREG(opened.st_mode) && S_ISREG(linked.st_mode) &&
         opened.st_uid == geteuid() && linked.st_uid == geteuid() &&
         opened.st_nlink == 1 && linked.st_nlink == 1 &&
         opened.st_dev == linked.st_dev && opened.st_ino == linked.st_ino;
}

void UnlinkOpenedFileAt(int directory, const std::string& name,
                        int descriptor) {
  if (IsOpenedFileAt(directory, name, descriptor)) {
    (void)unlinkat(directory, name.c_str(), 0);
  }
}

int OpenCookieWriterLock(const std::filesystem::path& path) {
  std::error_code filesystem_error;
  const std::filesystem::path resolved =
      std::filesystem::absolute(path, filesystem_error).lexically_normal();
  if (filesystem_error || resolved.empty()) return -1;
  const std::filesystem::path parent = resolved.parent_path();
  struct stat parent_status = {};
  if (parent.empty() || lstat(parent.c_str(), &parent_status) != 0 ||
      !IsPrivateDirectory(parent_status)) {
    return -1;
  }
  const int directory =
      open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0) return -1;
  struct stat opened_parent_status = {};
  if (fstat(directory, &opened_parent_status) != 0 ||
      opened_parent_status.st_dev != parent_status.st_dev ||
      opened_parent_status.st_ino != parent_status.st_ino ||
      !IsPrivateDirectory(opened_parent_status)) {
    close(directory);
    return -1;
  }
  const std::string filename = resolved.filename().string();
  if (filename.empty() || filename == "." || filename == "..") {
    close(directory);
    return -1;
  }
  const std::string lock_name = "." + filename + ".mocktail-writer.lock";
  const int lock =
      openat(directory, lock_name.c_str(),
             O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
  close(directory);
  if (lock < 0) return -1;
  struct stat lock_status = {};
  if (fstat(lock, &lock_status) != 0 || !S_ISREG(lock_status.st_mode) ||
      lock_status.st_uid != geteuid() || lock_status.st_nlink != 1 ||
      (lock_status.st_mode & (S_IRWXG | S_IRWXO)) != 0 ||
      flock(lock, LOCK_EX | LOCK_NB) != 0) {
    close(lock);
    return -1;
  }
  return lock;
}

bool AtomicWriteContents(const std::filesystem::path& path,
                         const std::string& contents,
                         std::string_view directory_error,
                         std::string_view replacement_error,
                         std::string* error) {
  std::error_code filesystem_error;
  const std::filesystem::path resolved =
      std::filesystem::absolute(path, filesystem_error).lexically_normal();
  if (filesystem_error || resolved.empty()) {
    error->assign(directory_error);
    return false;
  }
  const std::filesystem::path parent = resolved.parent_path();
  std::filesystem::path existing_ancestor;
  if (!FindExistingDirectoryAncestor(parent, &existing_ancestor)) {
    error->assign(directory_error);
    return false;
  }
  const bool parent_existed = existing_ancestor == parent;
  std::filesystem::create_directories(parent, filesystem_error);
  if (filesystem_error) {
    error->assign(directory_error);
    return false;
  }

  struct stat parent_status = {};
  if (lstat(parent.c_str(), &parent_status) != 0 ||
      !IsPrivateDirectory(parent_status)) {
    error->assign(directory_error);
    return false;
  }

  const int directory =
      open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory < 0) {
    error->assign(directory_error);
    return false;
  }
  struct stat opened_parent_status = {};
  if (fstat(directory, &opened_parent_status) != 0 ||
      opened_parent_status.st_dev != parent_status.st_dev ||
      opened_parent_status.st_ino != parent_status.st_ino ||
      !IsPrivateDirectory(opened_parent_status)) {
    close(directory);
    error->assign(directory_error);
    return false;
  }

  const std::string filename = resolved.filename().string();
  if (filename.empty() || filename == "." || filename == ".." ||
      !IsSafeDestinationAt(directory, filename)) {
    close(directory);
    error->assign(replacement_error);
    return false;
  }

  static std::atomic<std::uint64_t> serial{0};
  std::string temporary;
  int descriptor = -1;
  for (int attempt = 0; attempt < 16; ++attempt) {
    temporary = "." + filename + ".mocktail.tmp-" + std::to_string(getpid()) +
                "-" + std::to_string(serial.fetch_add(1));
    descriptor = openat(directory, temporary.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                        S_IRUSR | S_IWUSR);
    if (descriptor >= 0 || errno != EEXIST) break;
  }
  if (descriptor < 0) {
    close(directory);
    error->assign(replacement_error);
    return false;
  }
  struct stat temporary_status = {};
  const bool written = fstat(descriptor, &temporary_status) == 0 &&
                       S_ISREG(temporary_status.st_mode) &&
                       temporary_status.st_uid == geteuid() &&
                       temporary_status.st_nlink == 1 &&
                       fchmod(descriptor, S_IRUSR | S_IWUSR) == 0 &&
                       WriteAll(descriptor, contents) && fsync(descriptor) == 0;
  const bool linked =
      written && IsSafeDestinationAt(directory, filename) &&
      IsOpenedFileAt(directory, temporary, descriptor) &&
      renameat(directory, temporary.c_str(), directory, filename.c_str()) == 0;
  if (!linked) {
    UnlinkOpenedFileAt(directory, temporary, descriptor);
  }
  const bool synchronized = linked && fsync(directory) == 0;
  close(descriptor);
  close(directory);
  if (synchronized && !parent_existed) {
    const std::filesystem::path first_created_parent =
        parent.parent_path().empty() ? std::filesystem::path(".")
                                     : parent.parent_path();
    if (!FsyncDirectoryChain(first_created_parent, existing_ancestor)) {
      error->assign(replacement_error);
      return false;
    }
  }
  if (!synchronized) {
    error->assign(replacement_error);
  }
  return synchronized;
}

bool AtomicWrite(const std::filesystem::path& path,
                 const nlohmann::json& storage, std::string* error) {
  return AtomicWriteContents(path, storage.dump(),
                             "cannot create appStorage directory",
                             "cannot atomically replace appStorage", error);
}

bool AtomicWriteText(const std::filesystem::path& path,
                     const std::string& contents, std::string* error) {
  return AtomicWriteContents(path, contents, "cannot create cookie directory",
                             "cannot atomically replace cookie file", error);
}

}  // namespace

BrowserTrackerResult BrowserTrackerService::EnsureInitialized(
    const std::filesystem::path& app_storage_file,
    const std::filesystem::path& cookie_file, bool allow_cookie_update) {
  BrowserTrackerResult result;
  if (app_storage_file.empty() || cookie_file.empty()) {
    result.error = "BrowserTracker storage path is empty";
    return result;
  }
  std::error_code filesystem_error;
  const std::filesystem::path resolved_app_storage =
      std::filesystem::absolute(app_storage_file, filesystem_error)
          .lexically_normal();
  if (filesystem_error || resolved_app_storage.empty()) {
    result.error = "cannot lock appStorage";
    return result;
  }
  const std::filesystem::path lock_path =
      resolved_app_storage.string() + ".lock";
  const std::filesystem::path lock_parent = lock_path.parent_path().empty()
                                                ? std::filesystem::path(".")
                                                : lock_path.parent_path();
  std::filesystem::path existing_lock_ancestor;
  if (!FindExistingDirectoryAncestor(lock_parent, &existing_lock_ancestor)) {
    result.error = "cannot lock appStorage";
    return result;
  }
  const bool lock_parent_existed = existing_lock_ancestor == lock_parent;
  std::filesystem::create_directories(lock_parent, filesystem_error);
  struct stat lock_parent_status = {};
  const bool safe_lock_parent =
      !filesystem_error &&
      lstat(lock_parent.c_str(), &lock_parent_status) == 0 &&
      IsPrivateDirectory(lock_parent_status);
  const int lock = safe_lock_parent
                       ? open(lock_path.c_str(),
                              O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600)
                       : -1;
  struct stat lock_status = {};
  if (lock < 0 || fstat(lock, &lock_status) != 0 ||
      !S_ISREG(lock_status.st_mode) || lock_status.st_uid != geteuid() ||
      lock_status.st_nlink != 1 ||
      (lock_status.st_mode & (S_IRWXG | S_IRWXO)) != 0 ||
      flock(lock, LOCK_EX) != 0 ||
      (!lock_parent_existed &&
       !FsyncDirectoryChain(lock_parent, existing_lock_ancestor))) {
    if (lock >= 0) close(lock);
    result.error = "cannot lock appStorage";
    return result;
  }

  nlohmann::json storage;
  if (!ReadStorage(app_storage_file, &storage, &result.error)) {
    close(lock);
    return result;
  }
  std::string cookie_contents;
  if (!ReadSmallFile(cookie_file, &cookie_contents, &result.error)) {
    close(lock);
    return result;
  }

  std::string tracker = ExtractCookieValue(cookie_contents, kTrackerCookieName);
  std::string authoritative_id = BrowserIdFromTracker(tracker);
  if (HasCookie(cookie_contents, kTrackerCookieName) &&
      authoritative_id.empty()) {
    result.error = "RBXEventTrackerV2 cookie has an invalid browserid";
    close(lock);
    return result;
  }

  const auto existing = storage.find("BrowserTrackerId");
  std::string stored_id;
  if (existing != storage.end() && existing->is_string() &&
      IsValidId(existing->get_ref<const std::string&>())) {
    stored_id = existing->get<std::string>();
  } else if (existing != storage.end()) {
    result.error = "appStorage BrowserTrackerId is invalid";
    close(lock);
    return result;
  }
  if (!authoritative_id.empty()) {
    result.browser_tracker_id = authoritative_id;
    if (stored_id == authoritative_id) {
      close(lock);
      return result;
    }
    storage["BrowserTrackerId"] = authoritative_id;
    if (AtomicWrite(app_storage_file, storage, &result.error))
      result.status = BrowserTrackerStatus::kCreated;
    close(lock);
    return result;
  }
  if (!allow_cookie_update) {
    result.error =
        "external cookie file has no RBXEventTrackerV2 browser identity";
    close(lock);
    return result;
  }

  const std::string suggested = SuggestedId();
  HttpRequest request;
  request.url = std::string(kInitializeUrl) + suggested;
  request.headers = {"Accept: application/json",
                     "Content-Type: application/json"};
  request.maximum_body_bytes = 4096;
  const HttpResponse response = http_client_.Post(request, "{}");
  result.http_status = response.status_code;
  if (!response.transport_ok) {
    result.error = "BrowserTracker initialization transport failed";
  } else if (response.status_code != 200) {
    result.error = "BrowserTracker initialization was rejected";
  } else {
    const nlohmann::json body =
        nlohmann::json::parse(response.body, nullptr, false);
    std::string id;
    if (body.is_number_unsigned()) {
      id = std::to_string(body.get<std::uint64_t>());
    } else if (body.is_string()) {
      id = body.get<std::string>();
    } else if (body.is_object() && body.contains("browserTrackerId")) {
      const auto& value = body.at("browserTrackerId");
      if (value.is_number_unsigned())
        id = std::to_string(value.get<std::uint64_t>());
      if (value.is_string()) id = value.get<std::string>();
    }
    tracker = TrackerFromResponse(response);
    authoritative_id = BrowserIdFromTracker(tracker);
    if (!IsValidId(id) || authoritative_id.empty() || id != authoritative_id) {
      result.error =
          "BrowserTracker response does not contain a valid numeric ID";
    } else {
      const std::string expected_cookie_contents = cookie_contents;
      if (!cookie_contents.empty() && cookie_contents.back() != '\n' &&
          cookie_contents.back() != ';')
        cookie_contents.push_back(';');
      cookie_contents.append("RBXEventTrackerV2=").append(tracker).append("\n");
      const int cookie_writer_lock = OpenCookieWriterLock(cookie_file);
      if (cookie_writer_lock < 0) {
        result.error = "cannot lock cookie file for BrowserTracker update";
      } else {
        std::string current_cookie_contents;
        if (!ReadSmallFile(cookie_file, &current_cookie_contents,
                           &result.error)) {
          result.error = "cookie file changed during BrowserTracker update";
        } else if (current_cookie_contents != expected_cookie_contents) {
          result.error = "cookie file changed during BrowserTracker update";
        } else if (AtomicWriteText(cookie_file, cookie_contents,
                                   &result.error)) {
          storage["BrowserTrackerId"] = authoritative_id;
        }
        close(cookie_writer_lock);
      }
      if (result.error.empty() &&
          AtomicWrite(app_storage_file, storage, &result.error)) {
        result.status = BrowserTrackerStatus::kCreated;
        result.browser_tracker_id = std::move(authoritative_id);
      }
    }
  }
  close(lock);
  return result;
}

}  // namespace services
}  // namespace mocktail
