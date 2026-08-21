#include "runtime/game_mode.h"

#include <dlfcn.h>
#if defined(__GLIBC__)
#include <link.h>
#endif

#include <cstring>
#include <string>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

template <typename Function>
bool ResolveFunction(void* library, const char* name, Function* function,
                     std::string* error) {
  static_assert(sizeof(Function) == sizeof(void*),
                "GameMode function pointers must match dlsym pointers");
  dlerror();
  void* address = dlsym(library, name);
  const char* lookup_error = dlerror();
  if (lookup_error != nullptr || address == nullptr) {
    if (error != nullptr) {
      *error = lookup_error != nullptr ? lookup_error
                                       : std::string("missing symbol ") + name;
    }
    return false;
  }
  std::memcpy(function, &address, sizeof(address));
  return true;
}

std::string ClientError(const GameModeClientApi& api,
                        std::string_view fallback) {
  if (api.error_string != nullptr) {
    const char* error = api.error_string();
    if (error != nullptr && *error != '\0') {
      return error;
    }
  }
  return std::string(fallback);
}

void* OpenClientLibrary(const char* name) {
#if defined(__GLIBC__)
  // Mocktail intentionally exports Bionic compatibility symbols for the
  // Android payload. A normal dlopen() lets host libraries such as libdbus
  // bind to those ABI-compatible-looking exports instead of glibc, which can
  // corrupt the GameMode portal exchange. A new link-map namespace keeps the
  // host client and all of its dependencies on the host ABI.
  return dlmopen(LM_ID_NEWLM, name, RTLD_NOW | RTLD_LOCAL);
#else
  return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

}  // namespace

bool ParseGameModePolicy(std::string_view value, GameModePolicy* policy) {
  if (policy == nullptr) {
    return false;
  }
  if (value == "auto") {
    *policy = GameModePolicy::kAuto;
    return true;
  }
  if (value == "on" || value == "1" || value == "true") {
    *policy = GameModePolicy::kOn;
    return true;
  }
  if (value == "off" || value == "0" || value == "false") {
    *policy = GameModePolicy::kOff;
    return true;
  }
  return false;
}

const char* GameModePolicyName(GameModePolicy policy) {
  switch (policy) {
    case GameModePolicy::kAuto:
      return "auto";
    case GameModePolicy::kOn:
      return "on";
    case GameModePolicy::kOff:
      return "off";
  }
  return "invalid";
}

const char* GameModeSessionStateName(GameModeSessionState state) {
  switch (state) {
    case GameModeSessionState::kDisabled:
      return "disabled";
    case GameModeSessionState::kUnavailable:
      return "unavailable";
    case GameModeSessionState::kRequestFailed:
      return "request-failed";
    case GameModeSessionState::kAlreadyActive:
      return "already-active";
    case GameModeSessionState::kActive:
      return "active";
    case GameModeSessionState::kStopped:
      return "stopped";
    case GameModeSessionState::kStopFailed:
      return "stop-failed";
  }
  return "invalid";
}

GameModeSession::~GameModeSession() { (void)Stop(); }

GameModeSession::GameModeSession(GameModeSession&& other) noexcept
    : api_(other.api_),
      library_handle_(std::exchange(other.library_handle_, nullptr)),
      state_(other.state_),
      owns_request_(std::exchange(other.owns_request_, false)),
      detail_(std::move(other.detail_)) {
  other.api_ = {};
  other.state_ = GameModeSessionState::kStopped;
}

GameModeSession GameModeSession::Start(GameModePolicy policy) {
  if (policy == GameModePolicy::kOff) {
    return StartBound(policy, {}, nullptr);
  }

  void* library = OpenClientLibrary("libgamemode.so.0");
  if (library == nullptr) {
    library = OpenClientLibrary("libgamemode.so");
  }
  if (library == nullptr) {
    GameModeSession session;
    session.state_ = GameModeSessionState::kUnavailable;
    const char* error = dlerror();
    session.detail_ = error != nullptr ? error : "libgamemode is unavailable";
    return session;
  }

  GameModeClientApi api;
  std::string error;
  if (!ResolveFunction(library, "real_gamemode_request_start",
                       &api.request_start, &error) ||
      !ResolveFunction(library, "real_gamemode_request_end", &api.request_end,
                       &error) ||
      !ResolveFunction(library, "real_gamemode_error_string",
                       &api.error_string, &error)) {
    dlclose(library);
    GameModeSession session;
    session.state_ = GameModeSessionState::kUnavailable;
    session.detail_ = std::move(error);
    return session;
  }
  (void)ResolveFunction(library, "real_gamemode_query_status",
                        &api.query_status, nullptr);

  GameModeSession session = StartBound(policy, api, library);
  if (!session.owns_request_) {
    session.CloseLibrary();
  }
  return session;
}

GameModeSession GameModeSession::StartWithClientForTesting(
    GameModePolicy policy, GameModeClientApi api) {
  return StartBound(policy, api, nullptr);
}

GameModeSession GameModeSession::StartBound(GameModePolicy policy,
                                            GameModeClientApi api,
                                            void* library_handle) {
  GameModeSession session;
  if (policy == GameModePolicy::kOff) {
    return session;
  }
  session.api_ = api;
  session.library_handle_ = library_handle;
  if (api.request_start == nullptr || api.request_end == nullptr) {
    session.state_ = GameModeSessionState::kUnavailable;
    session.detail_ = "libgamemode client entry points are unavailable";
    return session;
  }
  if (api.query_status != nullptr && api.query_status() == 2) {
    session.state_ = GameModeSessionState::kAlreadyActive;
    return session;
  }
  if (api.request_start() != 0) {
    session.state_ = GameModeSessionState::kRequestFailed;
    session.detail_ = ClientError(api, "GameMode request was rejected");
    return session;
  }
  session.state_ = GameModeSessionState::kActive;
  session.owns_request_ = true;
  return session;
}

Status GameModeSession::Stop() {
  if (!owns_request_) {
    CloseLibrary();
    return Status::Ok();
  }

  owns_request_ = false;
  const int result = api_.request_end != nullptr ? api_.request_end() : -1;
  if (result == 0) {
    state_ = GameModeSessionState::kStopped;
    detail_.clear();
    CloseLibrary();
    return Status::Ok();
  }

  state_ = GameModeSessionState::kStopFailed;
  detail_ = ClientError(api_, "GameMode release request failed");
  const std::string detail = detail_;
  CloseLibrary();
  return Status::Error(StatusCode::kPlatformError, detail);
}

void GameModeSession::CloseLibrary() {
  if (library_handle_ != nullptr) {
    dlclose(library_handle_);
    library_handle_ = nullptr;
  }
}

}  // namespace runtime
}  // namespace mocktail
