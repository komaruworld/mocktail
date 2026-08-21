#include "runtime/discord_rpc.h"

#include <curl/curl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "runtime/owned_pthread.h"

namespace mocktail {
namespace runtime {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumDiscordFrameBytes = 1024U * 1024U;
constexpr std::size_t kMaximumRobloxApiBytes = 512U * 1024U;
constexpr std::size_t kDiscordWorkerStackBytes = 16U * 1024U * 1024U;
constexpr std::chrono::milliseconds kReconnectDelay{5000};
constexpr std::chrono::seconds kDiscordActivityRateWindow{20};
constexpr std::size_t kDiscordActivityRateLimit = 5;
constexpr std::chrono::seconds kPlaceMetadataRetryBase{10};
constexpr std::chrono::seconds kPlaceMetadataRetryMaximum{300};
constexpr char kJoinPage[] = "https://komaruworld.github.io/mocktail/join.html";

const char* PresencePhaseName(RobloxExperiencePresencePhase phase) {
  switch (phase) {
    case RobloxExperiencePresencePhase::kBrowsing:
      return "browsing";
    case RobloxExperiencePresencePhase::kJoining:
      return "joining";
    case RobloxExperiencePresencePhase::kPlaying:
      return "playing";
  }
  return "unknown";
}

enum class DiscordOpcode : uint32_t {
  kHandshake = 0,
  kFrame = 1,
  kClose = 2,
  kPing = 3,
  kPong = 4,
};

struct DiscordFrame {
  DiscordOpcode opcode = DiscordOpcode::kClose;
  std::string payload;
};

std::string TruncateUtf8(std::string value, std::size_t maximum) {
  if (value.size() <= maximum) {
    return value;
  }
  std::size_t boundary = maximum;
  while (boundary != 0 &&
         (static_cast<unsigned char>(value[boundary]) & 0xc0U) == 0x80U) {
    --boundary;
  }
  value.resize(boundary);
  return value;
}

std::string DumpJson(const Json& value) {
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string RenderPlaceTemplate(std::string text, std::string_view place_name) {
  constexpr std::string_view placeholder = "{place_name}";
  std::size_t offset = 0;
  while ((offset = text.find(placeholder, offset)) != std::string::npos) {
    text.replace(offset, placeholder.size(), place_name);
    offset += place_name.size();
  }
  return TruncateUtf8(std::move(text), 128);
}

std::string PercentEncode(std::string_view value) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size() * 3);
  for (const unsigned char byte : value) {
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
        byte == '.' || byte == '~') {
      encoded.push_back(static_cast<char>(byte));
    } else {
      encoded.push_back('%');
      encoded.push_back(hex[byte >> 4U]);
      encoded.push_back(hex[byte & 0x0fU]);
    }
  }
  return encoded;
}

bool HasDiscordJoinTarget(const RobloxExperienceLaunchRequest& request) {
  return request.place_id > 0 &&
         (request.game_instance_id.empty() ||
          (request.game_instance_id.size() <= 128 &&
           std::none_of(request.game_instance_id.begin(),
                        request.game_instance_id.end(),
                        [](unsigned char byte) {
                          return byte < 0x20 || byte == 0x7f;
                        })));
}

bool WriteAll(int descriptor, const void* bytes, std::size_t size) {
  const auto* next = static_cast<const unsigned char*>(bytes);
  while (size != 0) {
    const ssize_t written =
        send(descriptor, next, size, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (written > 0) {
      next += written;
      size -= static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd event{descriptor, POLLOUT, 0};
      if (poll(&event, 1, 1500) > 0 && (event.revents & POLLOUT) != 0) {
        continue;
      }
    }
    return false;
  }
  return true;
}

bool ReadAll(int descriptor, void* bytes, std::size_t size) {
  auto* next = static_cast<unsigned char*>(bytes);
  while (size != 0) {
    const ssize_t received = recv(descriptor, next, size, 0);
    if (received > 0) {
      next += received;
      size -= static_cast<std::size_t>(received);
      continue;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

void EncodeLittleEndian(uint32_t value, unsigned char* destination) {
  destination[0] = static_cast<unsigned char>(value & 0xffU);
  destination[1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
  destination[2] = static_cast<unsigned char>((value >> 16U) & 0xffU);
  destination[3] = static_cast<unsigned char>((value >> 24U) & 0xffU);
}

uint32_t DecodeLittleEndian(const unsigned char* source) {
  return static_cast<uint32_t>(source[0]) |
         (static_cast<uint32_t>(source[1]) << 8U) |
         (static_cast<uint32_t>(source[2]) << 16U) |
         (static_cast<uint32_t>(source[3]) << 24U);
}

bool WriteFrame(int descriptor, DiscordOpcode opcode,
                std::string_view payload) {
  if (payload.size() > kMaximumDiscordFrameBytes ||
      payload.size() > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  std::array<unsigned char, 8> header{};
  EncodeLittleEndian(static_cast<uint32_t>(opcode), header.data());
  EncodeLittleEndian(static_cast<uint32_t>(payload.size()), header.data() + 4);
  return WriteAll(descriptor, header.data(), header.size()) &&
         WriteAll(descriptor, payload.data(), payload.size());
}

bool ReadFrame(int descriptor, DiscordFrame* frame) {
  std::array<unsigned char, 8> header{};
  if (frame == nullptr || !ReadAll(descriptor, header.data(), header.size())) {
    return false;
  }
  const uint32_t length = DecodeLittleEndian(header.data() + 4);
  if (length > kMaximumDiscordFrameBytes) {
    return false;
  }
  frame->opcode = static_cast<DiscordOpcode>(DecodeLittleEndian(header.data()));
  frame->payload.resize(length);
  return length == 0 ||
         ReadAll(descriptor, frame->payload.data(), frame->payload.size());
}

void LogDiscordRpcError(const DiscordFrame& frame) {
  if (frame.opcode != DiscordOpcode::kFrame) {
    return;
  }
  const Json response = Json::parse(frame.payload, nullptr, false);
  if (!response.is_object()) {
    return;
  }
  const auto event = response.find("evt");
  if (event == response.end() || !event->is_string() ||
      event->get_ref<const std::string&>() != "ERROR") {
    return;
  }
  int64_t code = 0;
  std::string message = "Discord rejected an RPC request";
  const auto data = response.find("data");
  if (data != response.end() && data->is_object()) {
    const auto code_value = data->find("code");
    if (code_value != data->end() && code_value->is_number_integer()) {
      code = code_value->get<int64_t>();
    }
    const auto message_value = data->find("message");
    if (message_value != data->end() && message_value->is_string()) {
      message = TruncateUtf8(message_value->get<std::string>(), 256);
      for (char& byte : message) {
        if (static_cast<unsigned char>(byte) < 0x20U || byte == 0x7f) {
          byte = ' ';
        }
      }
    }
  }
  std::fprintf(stderr, "  [discord-rpc] request rejected code=%lld: %s\n",
               static_cast<long long>(code), message.c_str());
}

std::vector<std::string> DiscordSocketDirectories() {
  std::vector<std::string> directories;
  const auto append = [&directories](const char* path) {
    if (path != nullptr && path[0] != '\0') {
      directories.emplace_back(path);
    }
  };
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  append(runtime);
  if (runtime != nullptr && runtime[0] != '\0') {
    directories.emplace_back(std::string(runtime) +
                             "/app/com.discordapp.Discord");
    directories.emplace_back(std::string(runtime) +
                             "/.flatpak/com.discordapp.Discord/xdg-run");
    directories.emplace_back(std::string(runtime) + "/snap.discord");
  }
  append(std::getenv("TMPDIR"));
  directories.emplace_back("/tmp");
  directories.emplace_back("/run/user/" + std::to_string(getuid()));
  std::unordered_set<std::string> unique;
  std::vector<std::string> result;
  for (std::string& directory : directories) {
    if (unique.insert(directory).second) {
      result.push_back(std::move(directory));
    }
  }
  return result;
}

int ConnectDiscord() {
  for (const std::string& directory : DiscordSocketDirectories()) {
    for (int index = 0; index < 10; ++index) {
      const std::string path =
          directory + "/discord-ipc-" + std::to_string(index);
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      if (path.size() >= sizeof(address.sun_path)) {
        continue;
      }
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      const int descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (descriptor < 0) {
        continue;
      }
      timeval timeout{2, 0};
      (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof(timeout));
      (void)setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                       sizeof(timeout));
      if (connect(descriptor, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) == 0) {
        return descriptor;
      }
      (void)close(descriptor);
    }
  }
  return -1;
}

bool HandshakeDiscord(int descriptor, std::string_view application_id) {
  Json handshake = {{"v", 1}, {"client_id", application_id}};
  if (!WriteFrame(descriptor, DiscordOpcode::kHandshake, DumpJson(handshake))) {
    return false;
  }
  for (int attempt = 0; attempt < 3; ++attempt) {
    DiscordFrame frame;
    if (!ReadFrame(descriptor, &frame)) {
      return false;
    }
    if (frame.opcode == DiscordOpcode::kPing) {
      if (!WriteFrame(descriptor, DiscordOpcode::kPong, frame.payload)) {
        return false;
      }
      continue;
    }
    if (frame.opcode != DiscordOpcode::kFrame) {
      return false;
    }
    const Json response = Json::parse(frame.payload, nullptr, false);
    const auto event =
        response.is_object() ? response.find("evt") : response.end();
    return event != response.end() && event->is_string() &&
           event->get_ref<const std::string&>() == "READY";
  }
  return false;
}

Json ActivityJson(const DiscordRpcActivity& activity) {
  Json result = {
      {"type", 0}, {"details", activity.details}, {"instance", true}};
  if (!activity.state.empty()) {
    result["state"] = activity.state;
  }
  if (activity.start_timestamp.has_value()) {
    result["timestamps"] = {{"start", *activity.start_timestamp}};
  }
  if (!activity.large_image.empty()) {
    result["assets"] = {{"large_image", activity.large_image}};
    if (!activity.large_text.empty()) {
      result["assets"]["large_text"] = activity.large_text;
    }
  }
  if (!activity.button_label.empty() && !activity.button_url.empty()) {
    result["buttons"] = Json::array(
        {{{"label", activity.button_label}, {"url", activity.button_url}}});
  }
  return result;
}

bool SetDiscordActivity(int descriptor, const DiscordRpcActivity* activity,
                        uint64_t nonce) {
  Json args = {{"pid", getpid()}};
  args["activity"] = activity != nullptr ? ActivityJson(*activity) : Json();
  const Json request = {{"cmd", "SET_ACTIVITY"},
                        {"args", std::move(args)},
                        {"nonce", std::to_string(nonce)}};
  return WriteFrame(descriptor, DiscordOpcode::kFrame, DumpJson(request));
}

struct BoundedResponse {
  std::string body;
  bool exceeded = false;
};

std::once_flag g_curl_init_once;
CURLcode g_curl_init_status = CURLE_FAILED_INIT;

void InitializeCurl() {
  g_curl_init_status = curl_global_init(CURL_GLOBAL_DEFAULT);
}

size_t WriteHttpResponse(char* data, size_t size, size_t count,
                         void* user_data) {
  auto* response = static_cast<BoundedResponse*>(user_data);
  if (response == nullptr || data == nullptr ||
      (count != 0 && size > std::numeric_limits<size_t>::max() / count)) {
    return 0;
  }
  const std::size_t bytes = size * count;
  if (bytes > kMaximumRobloxApiBytes ||
      response->body.size() > kMaximumRobloxApiBytes - bytes) {
    response->exceeded = true;
    return 0;
  }
  response->body.append(data, bytes);
  return bytes;
}

std::optional<Json> GetRobloxJson(std::string url) {
  std::call_once(g_curl_init_once, InitializeCurl);
  if (g_curl_init_status != CURLE_OK) {
    return std::nullopt;
  }
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return std::nullopt;
  }
  BoundedResponse response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3500L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mocktail/0.1");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteHttpResponse);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  std::string proxy_url;
  const char* proxy_host = std::getenv("MOCKTAIL_HTTP_PROXY_HOST");
  const char* proxy_port = std::getenv("MOCKTAIL_HTTP_PROXY_PORT");
  const char* proxy_scheme = std::getenv("MOCKTAIL_HTTP_PROXY_SCHEME");
  if (proxy_host != nullptr && proxy_port != nullptr) {
    const std::optional<NetworkProxyConfig> proxy =
        ParseNetworkProxyConfig(proxy_host, proxy_port,
                                proxy_scheme != nullptr ? proxy_scheme
                                                        : "http");
    if (proxy.has_value()) {
      proxy_url = BuildNetworkProxyUrl(*proxy);
      curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url.c_str());
    }
  }
  const CURLcode status = curl_easy_perform(curl);
  long http_status = 0;
  if (status == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  }
  curl_easy_cleanup(curl);
  if (status != CURLE_OK || http_status != 200 || response.exceeded) {
    return std::nullopt;
  }
  Json parsed = Json::parse(response.body, nullptr, false);
  if (parsed.is_discarded()) {
    return std::nullopt;
  }
  return parsed;
}

struct ResolvedPlaceMetadata {
  std::string name;
  std::string icon_url;
};

bool IsSafeExternalImageUrl(std::string_view url) {
  return url.size() <= 512 && url.substr(0, 8) == "https://" &&
         std::none_of(url.begin(), url.end(), [](unsigned char byte) {
           return byte < 0x20U || byte == 0x7fU;
         });
}

ResolvedPlaceMetadata ResolvePlaceMetadata(int64_t place_id) {
  ResolvedPlaceMetadata metadata;
  if (place_id <= 0) {
    return metadata;
  }
  const std::optional<Json> universe =
      GetRobloxJson("https://apis.roblox.com/universes/v1/places/" +
                    std::to_string(place_id) + "/universe");
  if (!universe.has_value() || !universe->contains("universeId")) {
    return metadata;
  }
  const Json& identifier = (*universe)["universeId"];
  int64_t universe_id = 0;
  if (identifier.is_number_unsigned()) {
    const uint64_t unsigned_id = identifier.get<uint64_t>();
    if (unsigned_id >
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return metadata;
    }
    universe_id = static_cast<int64_t>(unsigned_id);
  } else if (identifier.is_number_integer()) {
    universe_id = identifier.get<int64_t>();
  } else {
    return metadata;
  }
  if (universe_id <= 0) {
    return metadata;
  }
  const std::optional<Json> game =
      GetRobloxJson("https://games.roblox.com/v1/games?universeIds=" +
                    std::to_string(universe_id));
  if (game.has_value() && game->contains("data") &&
      (*game)["data"].is_array() && !(*game)["data"].empty()) {
    const Json& first = (*game)["data"][0];
    if (first.contains("name") && first["name"].is_string()) {
      metadata.name =
          TruncateUtf8(first["name"].get<std::string>(), 128);
    }
  }

  const std::optional<Json> thumbnail = GetRobloxJson(
      "https://thumbnails.roblox.com/v1/games/icons?universeIds=" +
      std::to_string(universe_id) +
      "&returnPolicy=PlaceHolder&size=512x512&format=Png&isCircular=false");
  if (thumbnail.has_value() && thumbnail->contains("data") &&
      (*thumbnail)["data"].is_array() && !(*thumbnail)["data"].empty()) {
    const Json& first = (*thumbnail)["data"][0];
    if (first.contains("imageUrl") && first["imageUrl"].is_string()) {
      std::string icon_url = first["imageUrl"].get<std::string>();
      if (IsSafeExternalImageUrl(icon_url)) {
        metadata.icon_url = std::move(icon_url);
      }
    }
  }
  return metadata;
}

std::chrono::seconds PlaceMetadataRetryDelay(unsigned failures) {
  const unsigned shift = std::min(failures, 5U);
  return std::min(kPlaceMetadataRetryBase * (1U << shift),
                  kPlaceMetadataRetryMaximum);
}

}  // namespace

bool IsPublicDiscordJoin(const RobloxExperienceLaunchRequest& request) {
  return HasDiscordJoinTarget(request) &&
         request.reserved_server_access_code.empty() &&
         request.access_code.empty() && request.link_code.empty();
}

std::string BuildDiscordJoinUrl(const RobloxExperienceLaunchRequest& request) {
  if (!HasDiscordJoinTarget(request)) {
    return {};
  }
  std::string url =
      std::string(kJoinPage) + "#placeId=" + std::to_string(request.place_id);
  if (!request.game_instance_id.empty()) {
    url += "&gameInstanceId=" + PercentEncode(request.game_instance_id);
  }
  return url.size() <= 512 ? std::move(url) : std::string();
}

DiscordRpcActivity BuildDiscordRpcActivity(
    const DiscordRpcConfig& config, RobloxExperiencePresencePhase phase,
    const RobloxExperienceLaunchRequest* request, std::string place_name,
    int64_t session_started_at, std::string place_icon_url) {
  DiscordRpcActivity activity;
  activity.state = TruncateUtf8(config.text.state, 128);
  switch (phase) {
    case RobloxExperiencePresencePhase::kBrowsing:
      activity.details = TruncateUtf8(config.text.browsing, 128);
      break;
    case RobloxExperiencePresencePhase::kJoining:
    case RobloxExperiencePresencePhase::kPlaying: {
      if (place_name.empty()) {
        if (phase == RobloxExperiencePresencePhase::kJoining) {
          activity.details = TruncateUtf8(config.text.joining, 128);
          break;
        }
        place_name = config.text.unknown_place;
      }
      if (config.show_place_name) {
        activity.details = RenderPlaceTemplate(config.text.playing, place_name);
      } else {
        activity.details = activity.state;
        activity.state.clear();
      }
      if (phase == RobloxExperiencePresencePhase::kPlaying &&
          config.show_elapsed_time && session_started_at > 0) {
        activity.start_timestamp = session_started_at;
      }
      if (IsSafeExternalImageUrl(place_icon_url)) {
        activity.large_image = std::move(place_icon_url);
        activity.large_text = TruncateUtf8(place_name, 128);
      }
      if (phase == RobloxExperiencePresencePhase::kPlaying &&
          config.join_enabled && request != nullptr &&
          (!config.public_servers_only || IsPublicDiscordJoin(*request))) {
        activity.button_url = BuildDiscordJoinUrl(*request);
        if (!activity.button_url.empty()) {
          activity.button_label = config.join_button_label;
        }
      }
      break;
    }
  }
  return activity;
}

class DiscordRpcSession::Impl final {
 public:
  explicit Impl(DiscordRpcConfig config) : config_(std::move(config)) {}

  ~Impl() { Stop(); }

  bool Start(std::string* detail) {
    if (!config_.enabled) {
      if (detail != nullptr) {
        *detail = "disabled by runtime policy";
      }
      return true;
    }
    if (config_.application_id.empty()) {
      if (detail != nullptr) {
        *detail = "no Discord application ID is bundled";
      }
      return false;
    }
    std::call_once(g_curl_init_once, InitializeCurl);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (started_) {
        return true;
      }
      phase_ = RobloxExperiencePresencePhase::kBrowsing;
      request_ = {};
      revision_ = 1;
      stopping_ = false;
    }
    const int error =
        worker_.Start(&Impl::RunWorker, this, kDiscordWorkerStackBytes);
    if (error != 0) {
      if (detail != nullptr) {
        *detail =
            "could not start Discord RPC worker: " + std::to_string(error);
      }
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = true;
    return true;
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!started_ && !worker_.joinable()) {
        return;
      }
      stopping_ = true;
      condition_.notify_all();
    }
    if (worker_.joinable()) {
      (void)worker_.WaitFor(-1, 1);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
  }

  void Update(RobloxExperiencePresencePhase phase,
              const RobloxExperienceLaunchRequest* request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || stopping_) {
      return;
    }
    phase_ = phase;
    request_ = {};
    if (request != nullptr) {
      // Never pass access codes or canonical launch JSON to the Discord worker.
      request_.place_id = request->place_id;
      request_.game_instance_id = request->game_instance_id;
      if (!request->reserved_server_access_code.empty()) {
        request_.reserved_server_access_code = "present";
      }
      if (!request->access_code.empty()) {
        request_.access_code = "present";
      }
      if (!request->link_code.empty()) {
        request_.link_code = "present";
      }
    }
    if (phase == RobloxExperiencePresencePhase::kPlaying) {
      session_started_at_ =
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
    } else {
      session_started_at_ = 0;
    }
    ++revision_;
    std::fprintf(stderr,
                 "  [discord-rpc] queued phase=%s revision=%llu "
                 "place_id=%lld\n",
                 PresencePhaseName(phase_),
                 static_cast<unsigned long long>(revision_),
                 static_cast<long long>(request_.place_id));
    condition_.notify_all();
  }

 private:
  struct Desired {
    RobloxExperiencePresencePhase phase =
        RobloxExperiencePresencePhase::kBrowsing;
    RobloxExperienceLaunchRequest request;
    int64_t started_at = 0;
    uint64_t revision = 0;
  };

  struct CachedPlaceMetadata {
    std::string name;
    std::string icon_url;
    std::chrono::steady_clock::time_point next_attempt{};
    unsigned failures = 0;
  };

  static void* RunWorker(void* context) {
    static_cast<Impl*>(context)->Run();
    return nullptr;
  }

  bool IsStopping() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_;
  }

  Desired Snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return {phase_, request_, session_started_at_, revision_};
  }

  bool WaitForChange(uint64_t revision, std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait_for(lock, duration, [this, revision]() {
      return stopping_ || revision_ != revision;
    });
    return !stopping_;
  }

  void Run() {
    int descriptor = -1;
    uint64_t published_revision = 0;
    uint64_t nonce = 1;
    std::deque<std::chrono::steady_clock::time_point> activity_updates;
    while (!IsStopping()) {
      Desired desired = Snapshot();
      std::string place_name;
      std::string place_icon_url;
      bool resolve_metadata_after_publish = false;
      if ((desired.phase == RobloxExperiencePresencePhase::kJoining ||
           desired.phase == RobloxExperiencePresencePhase::kPlaying) &&
          desired.request.place_id > 0) {
        CachedPlaceMetadata& cached =
            place_metadata_[desired.request.place_id];
        place_name = cached.name;
        place_icon_url = cached.icon_url;
        if ((cached.name.empty() || cached.icon_url.empty()) &&
            std::chrono::steady_clock::now() >= cached.next_attempt) {
          resolve_metadata_after_publish = true;
        }
      }
      if (descriptor < 0) {
        descriptor = ConnectDiscord();
        if (descriptor < 0 ||
            !HandshakeDiscord(descriptor, config_.application_id)) {
          if (descriptor >= 0) {
            (void)close(descriptor);
            descriptor = -1;
          }
          if (!WaitForChange(desired.revision, kReconnectDelay)) {
            break;
          }
          continue;
        }
        std::fprintf(stderr, "  [discord-rpc] connected to Discord Desktop\n");
        published_revision = 0;
      }
      const bool wait_for_place_metadata =
          (desired.phase == RobloxExperiencePresencePhase::kJoining ||
           desired.phase == RobloxExperiencePresencePhase::kPlaying) &&
          desired.request.place_id > 0 && place_name.empty();
      if (published_revision != desired.revision &&
          !wait_for_place_metadata) {
        const auto now = std::chrono::steady_clock::now();
        while (!activity_updates.empty() &&
               now - activity_updates.front() >= kDiscordActivityRateWindow) {
          activity_updates.pop_front();
        }
        if (activity_updates.size() >= kDiscordActivityRateLimit) {
          const auto delay =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  activity_updates.front() + kDiscordActivityRateWindow - now);
          (void)WaitForChange(desired.revision, delay);
          continue;
        }
        const DiscordRpcActivity activity = BuildDiscordRpcActivity(
            config_, desired.phase,
            desired.phase == RobloxExperiencePresencePhase::kBrowsing
                ? nullptr
                : &desired.request,
            std::move(place_name), desired.started_at,
            std::move(place_icon_url));
        if (!SetDiscordActivity(descriptor, &activity, nonce++)) {
          (void)close(descriptor);
          descriptor = -1;
          continue;
        }
        std::fprintf(stderr,
                     "  [discord-rpc] sent phase=%s revision=%llu\n",
                     PresencePhaseName(desired.phase),
                     static_cast<unsigned long long>(desired.revision));
        activity_updates.push_back(std::chrono::steady_clock::now());
        published_revision = desired.revision;
      }

      if (resolve_metadata_after_publish) {
        const ResolvedPlaceMetadata resolved =
            ResolvePlaceMetadata(desired.request.place_id);
        CachedPlaceMetadata& cached =
            place_metadata_[desired.request.place_id];
        bool changed = false;
        if (!resolved.name.empty() && resolved.name != cached.name) {
          cached.name = resolved.name;
          changed = true;
        }
        if (!resolved.icon_url.empty() &&
            resolved.icon_url != cached.icon_url) {
          cached.icon_url = resolved.icon_url;
          changed = true;
        }
        if (cached.name.empty() || cached.icon_url.empty()) {
          const auto delay = PlaceMetadataRetryDelay(cached.failures++);
          cached.next_attempt = std::chrono::steady_clock::now() + delay;
          std::fprintf(stderr,
                       "  [discord-rpc] place metadata incomplete; retry in "
                       "%llds place_id=%lld\n",
                       static_cast<long long>(delay.count()),
                       static_cast<long long>(desired.request.place_id));
        } else {
          cached.failures = 0;
          cached.next_attempt = {};
        }
        if (Snapshot().revision != desired.revision) {
          continue;
        }
        if (changed &&
            (desired.phase == RobloxExperiencePresencePhase::kJoining ||
             desired.phase == RobloxExperiencePresencePhase::kPlaying)) {
          // Publish cached metadata through the normal rate limiter.
          published_revision = 0;
          continue;
        }
      }

      pollfd event{descriptor, POLLIN, 0};
      const int ready = poll(&event, 1, 500);
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready < 0 || (ready > 0 && (event.revents &
                                      (POLLERR | POLLHUP | POLLNVAL)) != 0)) {
        (void)close(descriptor);
        descriptor = -1;
        continue;
      }
      if (ready > 0 && (event.revents & POLLIN) != 0) {
        DiscordFrame frame;
        if (!ReadFrame(descriptor, &frame)) {
          (void)close(descriptor);
          descriptor = -1;
        } else if (frame.opcode == DiscordOpcode::kPing) {
          if (!WriteFrame(descriptor, DiscordOpcode::kPong, frame.payload)) {
            (void)close(descriptor);
            descriptor = -1;
          }
        } else if (frame.opcode == DiscordOpcode::kClose) {
          (void)close(descriptor);
          descriptor = -1;
        } else {
          LogDiscordRpcError(frame);
        }
      }
    }
    if (descriptor >= 0) {
      (void)SetDiscordActivity(descriptor, nullptr, nonce);
      (void)close(descriptor);
    }
  }

  const DiscordRpcConfig config_;
  std::mutex mutex_;
  std::condition_variable condition_;
  OwnedPthread worker_;
  RobloxExperiencePresencePhase phase_ =
      RobloxExperiencePresencePhase::kBrowsing;
  RobloxExperienceLaunchRequest request_;
  std::unordered_map<int64_t, CachedPlaceMetadata> place_metadata_;
  int64_t session_started_at_ = 0;
  uint64_t revision_ = 0;
  bool started_ = false;
  bool stopping_ = false;
};

DiscordRpcSession::DiscordRpcSession(DiscordRpcConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

DiscordRpcSession::~DiscordRpcSession() { Stop(); }

bool DiscordRpcSession::Start(std::string* detail) {
  return impl_->Start(detail);
}

void DiscordRpcSession::Stop() { impl_->Stop(); }

RobloxExperiencePresenceObserver DiscordRpcSession::observer() {
  return {this, &DiscordRpcSession::Notify};
}

void DiscordRpcSession::Notify(void* context,
                               RobloxExperiencePresencePhase phase,
                               const RobloxExperienceLaunchRequest* request) {
  if (context != nullptr) {
    static_cast<DiscordRpcSession*>(context)->impl_->Update(phase, request);
  }
}

}  // namespace runtime
}  // namespace mocktail
