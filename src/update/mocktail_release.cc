#include "update/mocktail_release.h"

#define JSON_NOEXCEPTION 1
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <nlohmann/json.hpp>
#include <utility>

#include "update/compatibility_catalog.h"

namespace mocktail::update {
namespace {

constexpr std::size_t kMaximumReleaseDocumentBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaximumCatalogBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumStateBytes = 256U * 1024U;
constexpr std::size_t kMaximumSupportedCodes = 256;

HttpTransferRequest QuickRequest(std::string url,
                                 std::vector<std::string> allowed_hosts,
                                 std::size_t maximum_bytes) {
  HttpTransferRequest request;
  request.url = std::move(url);
  request.allowed_hosts = std::move(allowed_hosts);
  request.maximum_bytes = maximum_bytes;
  request.connect_timeout_ms = 4000;
  request.transfer_timeout_ms = 8000;
  request.low_speed_seconds = 5;
  request.maximum_attempts = 1;
  request.maximum_redirects = 2;
  return request;
}

bool ValidTagCharacters(std::string_view tag) {
  return !tag.empty() && tag.size() <= 32 &&
         std::all_of(tag.begin(), tag.end(), [](unsigned char character) {
           return std::isdigit(character) || character == '.' ||
                  character == 'v';
         });
}

std::vector<std::uint64_t> SupportedCodes(
    const std::vector<SupportedPayloadProfile>& profiles) {
  std::vector<std::uint64_t> codes;
  for (const SupportedPayloadProfile& profile : profiles) {
    if (codes.size() == kMaximumSupportedCodes) break;
    codes.push_back(profile.version_code);
  }
  std::sort(codes.begin(), codes.end());
  codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
  return codes;
}

bool ReadBoundedFile(const std::filesystem::path& path, std::string* contents) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return false;
  struct stat metadata = {};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 ||
      static_cast<std::uintmax_t>(metadata.st_size) > kMaximumStateBytes) {
    close(descriptor);
    return false;
  }
  contents->clear();
  std::array<char, 16U * 1024U> buffer{};
  while (true) {
    const ssize_t bytes = read(descriptor, buffer.data(), buffer.size());
    if (bytes == 0) break;
    if (bytes < 0) {
      if (errno == EINTR) continue;
      close(descriptor);
      return false;
    }
    contents->append(buffer.data(), static_cast<std::size_t>(bytes));
    if (contents->size() > kMaximumStateBytes) {
      close(descriptor);
      return false;
    }
  }
  close(descriptor);
  return true;
}

bool WriteFileAtomically(const std::filesystem::path& path,
                         const std::string& contents, std::string* error) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(path.parent_path(), filesystem_error);
  if (filesystem_error) {
    *error = "cannot create release check state directory";
    return false;
  }
  const std::filesystem::path temporary =
      path.parent_path() /
      ("." + path.filename().string() + ".tmp-" + std::to_string(getpid()));
  const int descriptor =
      open(temporary.c_str(),
           O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor < 0) {
    *error = "cannot write release check state";
    return false;
  }
  std::size_t offset = 0;
  bool written = true;
  while (offset < contents.size()) {
    const ssize_t bytes =
        write(descriptor, contents.data() + offset, contents.size() - offset);
    if (bytes < 0 && errno == EINTR) continue;
    if (bytes <= 0) {
      written = false;
      break;
    }
    offset += static_cast<std::size_t>(bytes);
  }
  written = written && fsync(descriptor) == 0;
  written = close(descriptor) == 0 && written;
  if (!written) {
    std::filesystem::remove(temporary, filesystem_error);
    *error = "cannot write release check state";
    return false;
  }
  std::filesystem::rename(temporary, path, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary, filesystem_error);
    *error = "cannot publish release check state";
    return false;
  }
  return true;
}

struct PersistedState {
  std::string repository;
  std::time_t checked_at = 0;
  bool succeeded = false;
  std::optional<MocktailRelease> release;
  std::string notified_key;
};

PersistedState LoadState(const std::filesystem::path& path) {
  PersistedState state;
  std::string contents;
  if (!ReadBoundedFile(path, &contents)) return state;
  const nlohmann::json document =
      nlohmann::json::parse(contents, nullptr, false);
  if (document.is_discarded() || !document.is_object() ||
      document.value("schema_version", 0) != 1) {
    return state;
  }
  state.repository = document.value("repository", "");
  const auto checked_at = document.find("checked_at");
  if (checked_at != document.end() && checked_at->is_number_integer()) {
    state.checked_at = checked_at->get<std::time_t>();
  }
  state.succeeded = document.value("succeeded", false);
  state.notified_key = document.value("notified_key", "");
  const auto release = document.find("release");
  if (release != document.end() && release->is_object()) {
    MocktailRelease parsed;
    parsed.version = release->value("version", "");
    parsed.tag = release->value("tag", parsed.version);
    parsed.url = release->value("url", "");
    parsed.catalog_known = release->value("catalog_known", false);
    const auto codes = release->find("supported_roblox_codes");
    if (codes != release->end() && codes->is_array()) {
      for (const nlohmann::json& code : *codes) {
        if (!code.is_number_unsigned() ||
            parsed.supported_roblox_codes.size() == kMaximumSupportedCodes) {
          break;
        }
        parsed.supported_roblox_codes.push_back(code.get<std::uint64_t>());
      }
    }
    if (ParseReleaseVersion(parsed.version).has_value()) {
      state.release = std::move(parsed);
    }
  }
  return state;
}

bool SaveState(const std::filesystem::path& path, const PersistedState& state,
               std::string* error) {
  nlohmann::json document = {
      {"schema_version", 1},
      {"repository", state.repository},
      {"checked_at", state.checked_at},
      {"succeeded", state.succeeded},
      {"notified_key", state.notified_key},
  };
  if (state.release.has_value()) {
    document["release"] = {
        {"tag", state.release->tag},
        {"version", state.release->version},
        {"url", state.release->url},
        {"catalog_known", state.release->catalog_known},
        {"supported_roblox_codes", state.release->supported_roblox_codes},
    };
  }
  return WriteFileAtomically(path, document.dump(2) + "\n", error);
}

std::string RobloxLabel(std::string_view name, std::uint64_t code) {
  if (name.empty()) return "Roblox (" + std::to_string(code) + ")";
  return "Roblox " + std::string(name);
}

bool Supports(const MocktailRelease& release, std::uint64_t code) {
  return release.catalog_known &&
         std::binary_search(release.supported_roblox_codes.begin(),
                            release.supported_roblox_codes.end(), code);
}

}  // namespace

std::optional<ReleaseVersion> ParseReleaseVersion(std::string_view text) {
  if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
    text.remove_prefix(1);
  }
  if (text.empty() || text.size() > 32) return std::nullopt;
  std::array<std::uint32_t, 3> parts{};
  std::size_t index = 0;
  const char* cursor = text.data();
  const char* end = text.data() + text.size();
  while (true) {
    if (index == parts.size() || cursor == end ||
        !std::isdigit(static_cast<unsigned char>(*cursor))) {
      return std::nullopt;
    }
    const auto parsed = std::from_chars(cursor, end, parts[index]);
    if (parsed.ec != std::errc()) return std::nullopt;
    cursor = parsed.ptr;
    ++index;
    if (cursor == end) break;
    if (*cursor != '.') return std::nullopt;
    ++cursor;
  }
  if (index < 2) return std::nullopt;
  return ReleaseVersion{parts[0], parts[1], parts[2]};
}

int CompareReleaseVersions(const ReleaseVersion& left,
                           const ReleaseVersion& right) {
  const std::array<std::uint32_t, 3> a = {left.major, left.minor, left.patch};
  const std::array<std::uint32_t, 3> b = {right.major, right.minor,
                                          right.patch};
  return a < b ? -1 : (b < a ? 1 : 0);
}

bool IsValidRepositorySlug(std::string_view repository) {
  const std::size_t slash = repository.find('/');
  if (slash == std::string_view::npos || slash == 0 ||
      slash + 1 == repository.size() ||
      repository.find('/', slash + 1) != std::string_view::npos ||
      repository.size() > 140) {
    return false;
  }
  const auto valid = [](std::string_view part, bool allow_dot) {
    return part != "." && part != ".." &&
           std::all_of(part.begin(), part.end(),
                       [allow_dot](unsigned char character) {
                         return std::isalnum(character) || character == '-' ||
                                character == '_' ||
                                (allow_dot && character == '.');
                       });
  };
  return valid(repository.substr(0, slash), false) &&
         valid(repository.substr(slash + 1), true);
}

MocktailReleaseResult ParseLatestReleaseDocument(std::string_view document,
                                                 std::string_view repository) {
  MocktailReleaseResult result;
  const nlohmann::json parsed = nlohmann::json::parse(document, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    result.error = "GitHub release document is not a JSON object";
    return result;
  }
  const auto tag = parsed.find("tag_name");
  const auto url = parsed.find("html_url");
  if (tag == parsed.end() || !tag->is_string() || url == parsed.end() ||
      !url->is_string()) {
    result.error = "GitHub release document has no tag or page";
    return result;
  }
  if (parsed.value("draft", false) || parsed.value("prerelease", false)) {
    result.error = "GitHub latest release is a draft or prerelease";
    return result;
  }
  const std::string tag_name = tag->get<std::string>();
  const std::optional<ReleaseVersion> version = ParseReleaseVersion(tag_name);
  if (!ValidTagCharacters(tag_name) || !version.has_value()) {
    result.error = "GitHub latest release tag is not a version: " +
                   tag_name.substr(0, 64);
    return result;
  }
  const std::string page = url->get<std::string>();
  const std::string expected_prefix =
      "https://github.com/" + std::string(repository) + "/releases/";
  if (page.rfind(expected_prefix, 0) != 0 || page.size() > 512) {
    result.error = "GitHub release page is outside the project repository";
    return result;
  }
  result.release.tag = tag_name;
  result.release.version =
      tag_name.front() == 'v' ? tag_name.substr(1) : tag_name;
  result.release.url = page;
  return result;
}

MocktailReleaseResult FetchLatestMocktailRelease(std::string_view repository,
                                                 const HttpFetcher& fetch) {
  MocktailReleaseResult result;
  if (!IsValidRepositorySlug(repository)) {
    result.error = "release repository is not OWNER/REPO";
    return result;
  }
  HttpTransferRequest request = QuickRequest(
      "https://api.github.com/repos/" + std::string(repository) +
          "/releases/latest",
      {"api.github.com"}, kMaximumReleaseDocumentBytes);
  request.headers = {"Accept: application/vnd.github+json",
                     "X-GitHub-Api-Version: 2022-11-28"};
  const HttpBytesResult document = fetch(request);
  if (!document) {
    result.error = "cannot check the latest Mocktail release: " +
                   document.error;
    return result;
  }
  result = ParseLatestReleaseDocument(document.bytes, repository);
  if (!result) return result;

  const HttpBytesResult catalog = fetch(QuickRequest(
      "https://raw.githubusercontent.com/" + std::string(repository) + "/" +
          result.release.tag + "/config/roblox_compatibility.json",
      {"raw.githubusercontent.com"}, kMaximumCatalogBytes));
  if (!catalog) return result;
  const CompatibilityCatalogResult parsed_catalog =
      ParseCompatibilityCatalog(catalog.bytes);
  if (parsed_catalog) {
    result.release.supported_roblox_codes =
        SupportedCodes(parsed_catalog.profiles);
    result.release.catalog_known = true;
  }
  return result;
}

MocktailReleaseCheck CheckMocktailRelease(
    const MocktailReleaseCheckOptions& options) {
  MocktailReleaseCheck check;
  PersistedState state = LoadState(options.state_file);
  if (state.repository != options.repository) {
    state = PersistedState{};
    state.repository = options.repository;
  }
  check.notified_key = state.notified_key;
  const std::time_t age = options.now - state.checked_at;
  const std::time_t lifetime =
      state.succeeded ? options.refresh_seconds : options.retry_seconds;
  if (!options.force_refresh && state.checked_at > 0 && age >= 0 &&
      age < lifetime) {
    check.latest = state.release;
    return check;
  }
  if (!options.fetch) {
    check.error = "no HTTP client is available for the release check";
    check.latest = state.release;
    return check;
  }
  MocktailReleaseResult fetched =
      FetchLatestMocktailRelease(options.repository, options.fetch);
  check.refreshed = true;
  state.checked_at = options.now;
  state.succeeded = static_cast<bool>(fetched);
  if (fetched) {
    state.release = std::move(fetched.release);
  } else {
    check.error = fetched.error;
  }
  check.latest = state.release;
  std::string save_error;
  if (!SaveState(options.state_file, state, &save_error) &&
      check.error.empty()) {
    check.error = save_error;
  }
  return check;
}

std::string ReadNotifiedKey(const std::filesystem::path& state_file) {
  return LoadState(state_file).notified_key;
}

bool RecordNotifiedKey(const std::filesystem::path& state_file,
                       std::string_view key, std::string* error) {
  PersistedState state = LoadState(state_file);
  state.notified_key = std::string(key);
  return SaveState(state_file, state, error);
}

UpdateNotice ComposeUpdateNotice(std::string_view installed_version,
                                 const std::optional<MocktailRelease>& latest,
                                 const RobloxUpdateState& roblox) {
  UpdateNotice notice;
  const std::optional<ReleaseVersion> installed =
      ParseReleaseVersion(installed_version);
  std::optional<ReleaseVersion> available;
  if (latest.has_value()) available = ParseReleaseVersion(latest->version);
  const bool newer_release = installed.has_value() && available.has_value() &&
                             CompareReleaseVersions(*available, *installed) > 0;
  const bool roblox_blocked =
      roblox.latest_rejected && roblox.latest_version_code > 0 &&
      roblox.active_version_code < roblox.latest_version_code;
  if (!newer_release && !roblox_blocked) return notice;

  const std::string mocktail = "Mocktail " + std::string(installed_version);
  const std::string latest_roblox =
      RobloxLabel(roblox.latest_version_name, roblox.latest_version_code);
  std::string blocked_sentence;
  if (roblox_blocked) {
    blocked_sentence = latest_roblox + " is out, but it did not pass " +
                       mocktail + "'s compatibility check on this computer";
    blocked_sentence +=
        roblox.active_version_code > 0
            ? ", so Mocktail keeps " +
                  RobloxLabel(roblox.active_version_name,
                              roblox.active_version_code) +
                  ". Roblox can refuse to join experiences with that older "
                  "version."
            : ".";
  }

  if (newer_release) {
    const bool release_fixes_roblox =
        roblox_blocked && Supports(*latest, roblox.latest_version_code);
    notice.key = "release:" + latest->version;
    if (roblox_blocked) {
      notice.key += ":roblox:" + std::to_string(roblox.latest_version_code);
    }
    notice.heading = release_fixes_roblox ? "Update Mocktail for new Roblox"
                                          : "Mocktail update available";
    if (release_fixes_roblox) {
      notice.body = blocked_sentence + "\n\nMocktail " + latest->version +
                    " supports " + latest_roblox + ".";
    } else {
      notice.body = "Mocktail " + latest->version + " is available (" +
                    mocktail + " is installed).";
      if (roblox_blocked) notice.body += "\n\n" + blocked_sentence;
    }
    notice.body += "\n\nDownload it from GitHub:";
    notice.command = latest->url;
    return notice;
  }

  notice.key = "roblox:" + std::to_string(roblox.latest_version_code) +
               ":mocktail:" + std::string(installed_version);
  notice.heading = "Roblox update pending";
  notice.body = blocked_sentence +
                "\n\nA newer Mocktail release usually adds support for new "
                "Roblox versions.";
  if (latest.has_value()) {
    notice.body += " None is published yet (latest: " + latest->version +
                   ").";
  } else {
    notice.body += " Update Mocktail the same way you installed it.";
  }
  return notice;
}

}  // namespace mocktail::update
