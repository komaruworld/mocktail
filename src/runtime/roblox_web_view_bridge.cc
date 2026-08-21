#include "runtime/roblox_web_view_bridge.h"

#include <curl/curl.h>

#include <charconv>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

void ClearSensitiveString(std::string* value) {
  volatile char* bytes = value->empty() ? nullptr : value->data();
  for (std::size_t index = 0; index < value->size(); ++index) {
    bytes[index] = '\0';
  }
  value->clear();
}

constexpr char kWebViewProtocolClass[] =
    "com/roblox/protocols/webview/WebViewProtocol";
constexpr char kMessageBusClass[] =
    "com/roblox/universalapp/messagebus/MessageBus";
constexpr char kCookieProtocolClass[] =
    "com/roblox/universalapp/cookie/JNICookieProtocol";
constexpr char kCookieSetHandlerClass[] =
    "com/roblox/universalapp/cookie/"
    "JNICookieProtocol$OnSetCookieHandler";
constexpr char kNativeGlInterfaceClass[] =
    "com/roblox/engine/jni/NativeGLInterface";
constexpr char kConnectionPointerField[] = "a";
constexpr std::size_t kMaximumPendingHostWindowEvents = 256;

Status Invalid(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status Unavailable(std::string message) {
  return Status::Error(StatusCode::kUnavailable, std::move(message));
}

Status FailedPrecondition(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

Status CopyJniString(JNIEnv* env, jstring value, std::size_t maximum_bytes,
                     const char* field_name, std::string* output) {
  if (env == nullptr || value == nullptr || output == nullptr) {
    return Invalid(std::string(field_name) + " is null");
  }
  const jsize size = env->GetStringUTFLength(value);
  if (size < 0 || static_cast<std::size_t>(size) > maximum_bytes) {
    return Invalid(std::string(field_name) + " size is invalid");
  }
  const char* chars = env->GetStringUTFChars(value, nullptr);
  if (chars == nullptr) {
    return Unavailable(std::string("could not copy ") + field_name);
  }
  output->assign(chars, static_cast<std::size_t>(size));
  env->ReleaseStringUTFChars(value, chars);
  return Status::Ok();
}

bool HasControlBytes(const std::string& value) {
  for (const unsigned char byte : value) {
    if (byte <= 0x1f || byte == 0x7f) {
      return true;
    }
  }
  return false;
}

Status EscapeCaptchaQueryValue(const std::string& value, std::string* escaped) {
  if (escaped == nullptr) {
    return Invalid("captcha query output is null");
  }
  static std::once_flag curl_initialization;
  static CURLcode curl_initialization_status = CURLE_FAILED_INIT;
  std::call_once(curl_initialization, [] {
    curl_initialization_status = curl_global_init(CURL_GLOBAL_DEFAULT);
  });
  if (curl_initialization_status != CURLE_OK) {
    return Unavailable("could not initialize captcha URL encoder");
  }
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return Unavailable("could not initialize captcha URL encoder");
  }
  char* encoded =
      curl_easy_escape(curl, value.data(), static_cast<int>(value.size()));
  if (encoded == nullptr) {
    curl_easy_cleanup(curl);
    return Unavailable("could not encode captcha credential value");
  }
  escaped->assign(encoded);
  curl_free(encoded);
  curl_easy_cleanup(curl);
  return Status::Ok();
}

Status ParseOptionalWebViewString(const nlohmann::json& document,
                                  const char* key, std::size_t maximum_bytes,
                                  std::optional<std::string>* value) {
  if (key == nullptr || value == nullptr) {
    return Invalid("WebView optional string output is null");
  }
  const auto found = document.find(key);
  if (found == document.end()) {
    value->reset();
    return Status::Ok();
  }
  if (!found->is_string()) {
    return Invalid(std::string("WebView mutation field ") + key +
                   " must be a string");
  }
  const std::string& parsed = found->get_ref<const std::string&>();
  if (parsed.size() > maximum_bytes || HasControlBytes(parsed)) {
    return Invalid(std::string("WebView mutation field ") + key +
                   " is invalid");
  }
  *value = parsed;
  return Status::Ok();
}

Status ParseOptionalWebViewBoolean(const nlohmann::json& document,
                                   const char* key,
                                   std::optional<bool>* value) {
  if (key == nullptr || value == nullptr) {
    return Invalid("WebView optional boolean output is null");
  }
  const auto found = document.find(key);
  if (found == document.end()) {
    value->reset();
    return Status::Ok();
  }
  if (!found->is_boolean()) {
    return Invalid(std::string("WebView mutation field ") + key +
                   " must be a boolean");
  }
  *value = found->get<bool>();
  return Status::Ok();
}

} // namespace

struct RobloxWebViewBridge::RawCallbackTarget {
  std::mutex mutex;
  RobloxWebViewBridge *bridge = nullptr;
  SubscriptionKind kind = SubscriptionKind::kOpenWindow;
};

struct RobloxWebViewBridge::HostWindowCloseTarget {
  std::mutex mutex;
  RobloxWebViewBridge *bridge = nullptr;
  uint64_t active_generation = 0;
  OpenSource active_source = OpenSource::kMessageBus;
  bool close_queued = false;
};

struct RobloxWebViewBridge::HostWindowExitContext {
  std::shared_ptr<HostWindowCloseTarget> target;
  uint64_t generation = 0;
};

Status ParseRobloxWebViewOpenJson(const std::string &json,
                                  RobloxWebViewOpenRequest *request) {
  if (request == nullptr) {
    return Invalid("WebView open output is null");
  }
  *request = {};
  if (json.empty() || json.size() > kMaximumRobloxWebViewJsonBytes) {
    return Invalid("WebView open JSON size is invalid");
  }

  const nlohmann::json document =
      nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return Invalid("WebView open payload is not a JSON object");
  }
  const auto url = document.find("url");
  if (url == document.end() || !url->is_string()) {
    return Invalid("WebView open payload requires a string url");
  }
  const std::string &parsed_url = url->get_ref<const std::string &>();
  if (parsed_url.empty() || parsed_url.size() > kMaximumRobloxWebViewUrlBytes) {
    return Invalid("WebView open url size is invalid");
  }

  request->url = parsed_url;
  const auto title = document.find("title");
  if (title != document.end()) {
    if (!title->is_string() ||
        title->get_ref<const std::string&>().size() > 512 ||
        HasControlBytes(title->get_ref<const std::string&>())) {
      return Invalid("WebView open title is invalid");
    }
    request->title = title->get_ref<const std::string&>();
  }
  Status status =
      ParseOptionalWebViewBoolean(document, "isVisible", &request->is_visible);
  if (status.ok()) {
    status = ParseOptionalWebViewString(document, "windowType", 512,
                                        &request->window_type);
  }
  if (status.ok()) {
    status = ParseOptionalWebViewBoolean(document, "showDomainAsTitle",
                                         &request->show_domain_as_title);
  }
  if (status.ok()) {
    status = ParseOptionalWebViewBoolean(document, "backButtonVisible",
                                         &request->back_button_visible);
  }
  if (status.ok()) {
    status = ParseOptionalWebViewBoolean(document, "hideHeader",
                                         &request->hide_header);
  }
  if (status.ok()) {
    const auto search_params = document.find("searchParams");
    if (search_params != document.end()) {
      if (!search_params->is_object()) {
        status = Invalid("WebView open field searchParams must be an object");
      } else {
        status = ParseOptionalWebViewString(*search_params, "searchType", 512,
                                            &request->search_type);
      }
    }
  }
  if (!status.ok()) {
    *request = {};
  }
  return status;
}

Status ParseRobloxWebViewMutationJson(const std::string& json,
                                      RobloxWebViewMutationRequest* request) {
  if (request == nullptr) {
    return Invalid("WebView mutation output is null");
  }
  *request = {};
  if (json.empty() || json.size() > kMaximumRobloxWebViewJsonBytes) {
    return Invalid("WebView mutation JSON size is invalid");
  }

  const nlohmann::json document =
      nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return Invalid("WebView mutation payload is not a JSON object");
  }

  Status status = ParseOptionalWebViewString(
      document, "url", kMaximumRobloxWebViewUrlBytes, &request->url);
  if (status.ok()) {
    status =
        ParseOptionalWebViewString(document, "title", 512, &request->title);
  }
  if (status.ok()) {
    status = ParseOptionalWebViewBoolean(document, "isVisible",
                                         &request->is_visible);
  }
  if (status.ok()) {
    status = ParseOptionalWebViewString(document, "windowType", 512,
                                        &request->window_type);
  }
  if (status.ok()) {
    status = ParseOptionalWebViewBoolean(document, "showDomainAsTitle",
                                         &request->show_domain_as_title);
  }
  if (status.ok()) {
    const auto search_params = document.find("searchParams");
    if (search_params != document.end()) {
      if (!search_params->is_object()) {
        status =
            Invalid("WebView mutation field searchParams must be an object");
      } else {
        status = ParseOptionalWebViewString(*search_params, "searchType", 512,
                                            &request->search_type);
      }
    }
  }
  if (!status.ok()) {
    *request = {};
  }
  return status;
}

Status ParseRobloxCaptchaOpenJson(const std::string& json,
                                  RobloxWebViewOpenRequest* request) {
  if (request == nullptr) {
    return Invalid("captcha open output is null");
  }
  *request = {};
  if (json.empty() || json.size() > kMaximumRobloxWebViewJsonBytes) {
    return Invalid("captcha open JSON size is invalid");
  }
  const nlohmann::json document =
      nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return Invalid("captcha open payload is not a JSON object");
  }
  const auto captcha_type = document.find("captchaType");
  if (captcha_type == document.end() || !captcha_type->is_string()) {
    return Invalid("captcha open payload requires captchaType");
  }

  request->title = "Roblox verification";
  if (captcha_type->get_ref<const std::string&>() != "login") {
    request->url =
        "https://www.roblox.com/captcha/app/signup?hybrid-return-token=1";
    return Status::Ok();
  }

  const auto credential_type = document.find("cvalueType");
  const auto credential_value = document.find("cvalue");
  if (credential_type == document.end() || !credential_type->is_string() ||
      credential_value == document.end() || !credential_value->is_string()) {
    return Invalid("login captcha requires credential type and value");
  }
  std::string normalized_type = credential_type->get_ref<const std::string&>();
  if (normalized_type == "phone_number") {
    normalized_type = "phonenumber";
  } else if (normalized_type != "username" && normalized_type != "email") {
    return Invalid("login captcha credential type is unsupported");
  }
  const std::string& value = credential_value->get_ref<const std::string&>();
  if (value.empty() || value.size() > 4096 || HasControlBytes(value)) {
    return Invalid("login captcha credential value is invalid");
  }
  std::string escaped_value;
  Status status = EscapeCaptchaQueryValue(value, &escaped_value);
  if (!status.ok()) {
    return status;
  }
  request->url = "https://www.roblox.com/captcha/app/login?credentialsType=" +
                 normalized_type + "&credentialsValue=" + escaped_value +
                 "&hybrid-return-token=1";
  return Status::Ok();
}

Status ParseRobloxProfileViewUrl(const std::string& url,
                                 RobloxWebViewOpenRequest* request) {
  if (request == nullptr) {
    return Invalid("profile WebView output is null");
  }
  *request = {};
  if (url.empty() || url.size() > kMaximumRobloxWebViewUrlBytes ||
      HasControlBytes(url)) {
    return Invalid("profile WebView URL size is invalid");
  }

  constexpr std::string_view kPrefix = "https://www.roblox.com/users/";
  constexpr std::string_view kSuffix = "/profile";
  std::string_view value(url);
  if (value.size() < kPrefix.size() ||
      value.substr(0, kPrefix.size()) != kPrefix) {
    return Invalid("profile WebView URL origin is invalid");
  }
  value.remove_prefix(kPrefix.size());
  if (!value.empty() && value.back() == '/') {
    value.remove_suffix(1);
  }
  if (value.size() < kSuffix.size() ||
      value.substr(value.size() - kSuffix.size()) != kSuffix) {
    return Invalid("profile WebView URL path is invalid");
  }
  value.remove_suffix(kSuffix.size());
  if (value.empty() || value.front() == '0') {
    return Invalid("profile WebView user ID is invalid");
  }
  std::uint64_t user_id = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), user_id);
  if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
      user_id == 0) {
    return Invalid("profile WebView user ID is invalid");
  }

  request->url = url;
  request->title = "Profile";
  return Status::Ok();
}

Status ParseRobloxPurchaseRobuxNotification(const std::string& json,
                                            RobloxWebViewOpenRequest* request) {
  if (request == nullptr) {
    return Invalid("Robux WebView output is null");
  }
  *request = {};
  if (json.size() > kMaximumRobloxWebViewJsonBytes || HasControlBytes(json)) {
    return Invalid("Robux notification size is invalid");
  }
  // The APK ignores this notification's data and always opens the canonical
  // GlobalDist Robux page. Keeping the payload opaque avoids coupling this
  // route to incidental JSON fields in one Roblox release.
  request->url = "https://www.roblox.com/upgrades/robux?showHeader=true";
  request->title = "Robux";
  return Status::Ok();
}

RobloxWebViewBridge::RobloxWebViewBridge(JniEnvironmentProvider environment,
                                         RobloxWebViewMessageBusSymbols symbols,
                                         RobloxWebViewMessageBusObjects objects,
                                         RobloxWebViewSink sink)
    : environment_(environment),
      symbols_(symbols),
      source_objects_(objects),
      sink_(sink) {}

RobloxWebViewBridge::~RobloxWebViewBridge() { Shutdown(); }

Status RobloxWebViewBridge::Initialize() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initializing_ || shutting_down_ || initialized_) {
      return FailedPrecondition("WebView bridge is already initialized");
    }
    if (!environment_.valid() || !symbols_.complete() ||
        !source_objects_.complete() || !sink_.valid()) {
      return Unavailable("WebView MessageBus prerequisites are incomplete");
    }
    initializing_ = true;
    accepting_messages_ = true;
  }

  auto finish_failed_initialization = [this](Status status) {
    std::shared_ptr<HostWindowCloseTarget> host_window_close_target;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      accepting_messages_ = false;
      initializing_ = false;
      host_window_close_target = std::move(host_window_close_target_);
      {
        std::lock_guard<std::mutex> event_lock(host_window_event_mutex_);
        pending_host_window_events_.clear();
      }
      dispatch_drained_.notify_all();
    }
    if (host_window_close_target != nullptr) {
      std::lock_guard<std::mutex> target_lock(host_window_close_target->mutex);
      host_window_close_target->bridge = nullptr;
      host_window_close_target->active_generation = 0;
      host_window_close_target->close_queued = false;
    }
    return status;
  };

  JNIEnv *env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    return finish_failed_initialization(status);
  }
  jclass protocol_class = env->FindClass(kWebViewProtocolClass);
  if (protocol_class == nullptr) {
    return finish_failed_initialization(
        Unavailable("WebViewProtocol class is unavailable"));
  }
  std::array<jstring, kSubscriptionCount> method_ids = {
      symbols_.get_open_window_id(env, protocol_class),
      symbols_.get_mutate_window_id(env, protocol_class),
      symbols_.get_close_window_id(env, protocol_class),
  };
  jstring handle_window_close_id =
      symbols_.get_handle_window_close_id(env, protocol_class);
  jstring protocol_name = symbols_.get_protocol_name(env, protocol_class);
  jstring is_available_id = symbols_.get_is_available_id(env, protocol_class);
  env->DeleteLocalRef(protocol_class);
  status = CheckJniException(env, "resolve WebViewProtocol method ids");
  bool ids_complete = handle_window_close_id != nullptr &&
                      protocol_name != nullptr && is_available_id != nullptr;
  for (jstring method_id : method_ids) {
    ids_complete = ids_complete && method_id != nullptr;
  }
  if (!status.ok() || !ids_complete) {
    for (jstring method_id : method_ids) {
      if (method_id != nullptr) {
        env->DeleteLocalRef(method_id);
      }
    }
    if (handle_window_close_id != nullptr) {
      env->DeleteLocalRef(handle_window_close_id);
    }
    if (protocol_name != nullptr) {
      env->DeleteLocalRef(protocol_name);
    }
    if (is_available_id != nullptr) {
      env->DeleteLocalRef(is_available_id);
    }
    return finish_failed_initialization(
        status.ok() ? Unavailable("WebViewProtocol method id is null")
                    : status);
  }

  jclass message_bus_class = env->FindClass(kMessageBusClass);
  std::array<jstring, kSubscriptionCount> message_ids{};
  if (message_bus_class != nullptr) {
    for (std::size_t index = 0; index < message_ids.size(); ++index) {
      message_ids[index] = symbols_.get_message_id(
          env, message_bus_class, protocol_name, method_ids[index]);
    }
  }
  jstring handle_window_close_message_id =
      message_bus_class != nullptr
          ? symbols_.get_message_id(env, message_bus_class, protocol_name,
                                    handle_window_close_id)
          : nullptr;
  if (message_bus_class != nullptr) {
    env->DeleteLocalRef(message_bus_class);
  }
  for (jstring method_id : method_ids) {
    env->DeleteLocalRef(method_id);
  }
  env->DeleteLocalRef(handle_window_close_id);
  status = CheckJniException(env, "compose WebViewProtocol message ids");
  bool message_ids_complete = handle_window_close_message_id != nullptr;
  for (jstring message_id : message_ids) {
    message_ids_complete = message_ids_complete && message_id != nullptr;
  }
  if (!status.ok() || !message_ids_complete) {
    for (jstring message_id : message_ids) {
      if (message_id != nullptr) {
        env->DeleteLocalRef(message_id);
      }
    }
    if (handle_window_close_message_id != nullptr) {
      env->DeleteLocalRef(handle_window_close_message_id);
    }
    env->DeleteLocalRef(protocol_name);
    env->DeleteLocalRef(is_available_id);
    return finish_failed_initialization(
        status.ok() ? Unavailable("WebViewProtocol composite id is null")
                    : status);
  }

  auto host_window_close_target = std::make_shared<HostWindowCloseTarget>();
  host_window_close_target->bridge = this;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    host_window_close_target_ = host_window_close_target;
  }

  std::array<jobject, kSubscriptionCount> raw_callbacks{};
  std::array<jobject, kSubscriptionCount> connections{};
  std::array<jlong, kSubscriptionCount> connection_handles{};
  std::array<std::shared_ptr<RawCallbackTarget>, kSubscriptionCount>
      raw_callback_targets{};
  const std::array<SubscriptionKind, kSubscriptionCount> subscription_kinds = {
      SubscriptionKind::kOpenWindow,
      SubscriptionKind::kMutateWindow,
      SubscriptionKind::kCloseWindow,
  };
  for (std::size_t index = 0; index < raw_callback_targets.size(); ++index) {
    raw_callback_targets[index] = std::make_shared<RawCallbackTarget>();
    raw_callback_targets[index]->bridge = this;
    raw_callback_targets[index]->kind = subscription_kinds[index];
  }

  jobject message_bus = env->NewGlobalRef(source_objects_.message_bus);
  jobject local_availability_handler = nullptr;
  jobject availability_handler = nullptr;
  jobject cookie_protocol = nullptr;
  jobject cookie_set_handler = nullptr;
  jstring retained_protocol_name =
      static_cast<jstring>(env->NewGlobalRef(protocol_name));
  jstring retained_is_available_id =
      static_cast<jstring>(env->NewGlobalRef(is_available_id));
  jstring retained_handle_window_close_message_id =
      static_cast<jstring>(env->NewGlobalRef(handle_window_close_message_id));
  bool availability_registered = false;
  bool platform_web_callbacks_registered = false;
  bool cookie_set_handler_registered = false;

  auto make_targets_inert = [&]() {
    for (const std::shared_ptr<RawCallbackTarget>& target :
         raw_callback_targets) {
      if (target != nullptr) {
        std::lock_guard<std::mutex> target_lock(target->mutex);
        target->bridge = nullptr;
      }
    }
  };
  auto release_resources = [&]() {
    make_targets_inert();
    {
      std::unique_lock<std::mutex> lock(mutex_);
      accepting_messages_ = false;
      dispatch_drained_.wait(lock,
                             [this] { return in_flight_dispatches_ == 0; });
    }
    if (cookie_set_handler_registered) {
      symbols_.update_cookie_set_handler(env, cookie_protocol, nullptr);
      (void)CheckJniException(
          env, "clear failed Roblox cookie handler initialization");
      cookie_set_handler_registered = false;
    }
    if (platform_web_callbacks_registered) {
      source_objects_.clear_platform_web_callbacks(
          source_objects_.callback_factory_context);
      platform_web_callbacks_registered = false;
    }
    for (std::size_t index = 0; index < connections.size(); ++index) {
      if (connections[index] != nullptr && connection_handles[index] != 0) {
        symbols_.delete_connection(env, connections[index],
                                   connection_handles[index]);
        (void)CheckJniException(
            env, "disconnect failed WebViewProtocol initialization");
      }
    }
    if (availability_registered && message_bus != nullptr &&
        retained_protocol_name != nullptr &&
        retained_is_available_id != nullptr) {
      symbols_.clear_request_handler(env, message_bus, retained_protocol_name,
                                     retained_is_available_id);
      (void)CheckJniException(
          env, "clear failed WebViewProtocol availability initialization");
    }
    for (jobject callback : raw_callbacks) {
      if (callback != nullptr) {
        source_objects_.clear_raw_callback(
            source_objects_.callback_factory_context, callback);
      }
    }
    if (availability_handler != nullptr) {
      source_objects_.clear_request_handler(
          source_objects_.callback_factory_context, availability_handler);
    } else if (local_availability_handler != nullptr) {
      source_objects_.clear_request_handler(
          source_objects_.callback_factory_context, local_availability_handler);
    }
    for (jobject connection : connections) {
      if (connection != nullptr) {
        env->DeleteGlobalRef(connection);
      }
    }
    for (jobject callback : raw_callbacks) {
      if (callback != nullptr) {
        env->DeleteGlobalRef(callback);
      }
    }
    if (availability_handler != nullptr) {
      env->DeleteGlobalRef(availability_handler);
    }
    if (cookie_set_handler != nullptr) {
      env->DeleteGlobalRef(cookie_set_handler);
    }
    if (cookie_protocol != nullptr) {
      env->DeleteGlobalRef(cookie_protocol);
    }
    if (retained_protocol_name != nullptr) {
      env->DeleteGlobalRef(retained_protocol_name);
    }
    if (retained_is_available_id != nullptr) {
      env->DeleteGlobalRef(retained_is_available_id);
    }
    if (retained_handle_window_close_message_id != nullptr) {
      env->DeleteGlobalRef(retained_handle_window_close_message_id);
    }
    if (message_bus != nullptr) {
      env->DeleteGlobalRef(message_bus);
    }
  };

  local_availability_handler = source_objects_.create_request_handler(
      source_objects_.callback_factory_context, raw_callback_targets[0],
      &RobloxWebViewBridge::AvailabilityCallback);
  availability_handler = local_availability_handler != nullptr
                             ? env->NewGlobalRef(local_availability_handler)
                             : nullptr;
  if (local_availability_handler != nullptr) {
    env->DeleteLocalRef(local_availability_handler);
    local_availability_handler = nullptr;
  }
  status = CheckJniException(env, "retain WebViewProtocol common objects");
  if (!status.ok() || message_bus == nullptr ||
      availability_handler == nullptr || retained_protocol_name == nullptr ||
      retained_is_available_id == nullptr ||
      retained_handle_window_close_message_id == nullptr) {
    for (jstring message_id : message_ids) {
      env->DeleteLocalRef(message_id);
    }
    env->DeleteLocalRef(handle_window_close_message_id);
    env->DeleteLocalRef(protocol_name);
    env->DeleteLocalRef(is_available_id);
    release_resources();
    return finish_failed_initialization(
        status.ok()
            ? Unavailable("could not retain WebViewProtocol common objects")
            : status);
  }

  symbols_.set_request_handler_raw(env, message_bus, protocol_name,
                                   is_available_id, availability_handler);
  status = CheckJniException(env, "register WebViewProtocol availability");
  if (status.ok()) {
    availability_registered = true;
  }
  env->DeleteLocalRef(handle_window_close_message_id);
  env->DeleteLocalRef(protocol_name);
  env->DeleteLocalRef(is_available_id);
  if (!status.ok()) {
    for (jstring message_id : message_ids) {
      env->DeleteLocalRef(message_id);
    }
    release_resources();
    return finish_failed_initialization(status);
  }

  jclass connection_class = nullptr;
  jfieldID pointer_field = nullptr;
  for (std::size_t index = 0; index < message_ids.size(); ++index) {
    jobject local_callback = source_objects_.create_raw_callback(
        source_objects_.callback_factory_context, raw_callback_targets[index],
        &RobloxWebViewBridge::RawMessageCallback);
    raw_callbacks[index] =
        local_callback != nullptr ? env->NewGlobalRef(local_callback) : nullptr;
    if (local_callback != nullptr) {
      env->DeleteLocalRef(local_callback);
    }
    status = CheckJniException(env, "retain WebViewProtocol callback");
    if (!status.ok() || raw_callbacks[index] == nullptr) {
      if (status.ok()) {
        status = Unavailable("could not retain WebViewProtocol callback");
      }
      break;
    }

    jobject local_connection = symbols_.subscribe_raw(
        env, message_bus, message_ids[index], raw_callbacks[index], JNI_FALSE);
    status =
        CheckJniException(env, "subscribe to WebViewProtocol window event");
    if (status.ok() && local_connection != nullptr) {
      if (connection_class == nullptr) {
        connection_class = env->GetObjectClass(local_connection);
        pointer_field = connection_class != nullptr
                            ? env->GetFieldID(connection_class,
                                              kConnectionPointerField, "J")
                            : nullptr;
        status =
            CheckJniException(env, "resolve WebViewProtocol connection handle");
      }
      if (status.ok() && pointer_field != nullptr) {
        connection_handles[index] =
            env->GetLongField(local_connection, pointer_field);
        status =
            CheckJniException(env, "read WebViewProtocol connection handle");
      }
      if (status.ok() && connection_handles[index] != 0) {
        connections[index] = env->NewGlobalRef(local_connection);
        status = CheckJniException(env, "retain WebViewProtocol connection");
      } else if (status.ok()) {
        status = FailedPrecondition(
            "MessageBus Connection.<init>(J) handle was not preserved");
      }
      if ((!status.ok() || connections[index] == nullptr) &&
          connection_handles[index] != 0) {
        symbols_.delete_connection(env, local_connection,
                                   connection_handles[index]);
        (void)CheckJniException(
            env, "disconnect failed WebViewProtocol subscription");
        connection_handles[index] = 0;
      }
    } else if (status.ok()) {
      status = Unavailable("WebViewProtocol subscription failed");
    }
    if (local_connection != nullptr) {
      env->DeleteLocalRef(local_connection);
    }
    if (!status.ok() || connections[index] == nullptr) {
      if (status.ok()) {
        status = Unavailable("could not retain WebViewProtocol connection");
      }
      break;
    }
  }
  for (jstring message_id : message_ids) {
    env->DeleteLocalRef(message_id);
  }
  if (connection_class != nullptr) {
    env->DeleteLocalRef(connection_class);
  }
  if (!status.ok()) {
    release_resources();
    return finish_failed_initialization(status);
  }

  platform_web_callbacks_registered =
      source_objects_.set_platform_web_callbacks(
          source_objects_.callback_factory_context, raw_callback_targets[0],
          &RobloxWebViewBridge::DataModelNotificationCallback,
          &RobloxWebViewBridge::AppBridgeNotificationCallback,
          &RobloxWebViewBridge::NativeOverlayCallback,
          &RobloxWebViewBridge::OpenWebActivityCallback,
          &RobloxWebViewBridge::SyncCookiesCallback,
          &RobloxWebViewBridge::SetCookieCallback);
  if (!platform_web_callbacks_registered) {
    release_resources();
    return finish_failed_initialization(
        Unavailable("could not register Roblox platform web callbacks"));
  }

  jclass cookie_protocol_class = env->FindClass(kCookieProtocolClass);
  jclass cookie_set_handler_class = env->FindClass(kCookieSetHandlerClass);
  jobject local_cookie_protocol =
      cookie_protocol_class != nullptr ? env->AllocObject(cookie_protocol_class)
                                       : nullptr;
  jobject local_cookie_set_handler =
      cookie_set_handler_class != nullptr
          ? env->AllocObject(cookie_set_handler_class)
          : nullptr;
  cookie_protocol =
      local_cookie_protocol != nullptr
          ? env->NewGlobalRef(local_cookie_protocol)
          : nullptr;
  cookie_set_handler =
      local_cookie_set_handler != nullptr
          ? env->NewGlobalRef(local_cookie_set_handler)
          : nullptr;
  if (local_cookie_protocol != nullptr) {
    env->DeleteLocalRef(local_cookie_protocol);
  }
  if (local_cookie_set_handler != nullptr) {
    env->DeleteLocalRef(local_cookie_set_handler);
  }
  if (cookie_protocol_class != nullptr) {
    env->DeleteLocalRef(cookie_protocol_class);
  }
  if (cookie_set_handler_class != nullptr) {
    env->DeleteLocalRef(cookie_set_handler_class);
  }
  status = CheckJniException(env, "create Roblox cookie set handler");
  if (!status.ok() || cookie_protocol == nullptr ||
      cookie_set_handler == nullptr) {
    release_resources();
    return finish_failed_initialization(
        status.ok() ? Unavailable("could not create Roblox cookie set handler")
                    : status);
  }
  symbols_.update_cookie_set_handler(env, cookie_protocol,
                                     cookie_set_handler);
  cookie_set_handler_registered = true;
  status = CheckJniException(env, "register Roblox cookie set handler");
  if (!status.ok()) {
    release_resources();
    return finish_failed_initialization(status);
  }

  // This mirrors the APK WebViewProtocol constructor: its MessageBus request
  // handler and window subscriptions are installed before the native Android
  // protocol is initialized. Calling the native initializer earlier loses
  // messages that it can publish synchronously during startup.
  jclass initialization_class = env->FindClass(kWebViewProtocolClass);
  if (initialization_class != nullptr) {
    symbols_.initialize_android_web_view_protocol(env, initialization_class);
    env->DeleteLocalRef(initialization_class);
  }
  status = CheckJniException(env, "initialize Android WebViewProtocol");
  if (!status.ok() || initialization_class == nullptr) {
    if (status.ok()) {
      status = Unavailable(
          "WebViewProtocol class is unavailable during native "
          "initialization");
    }
    release_resources();
    return finish_failed_initialization(status);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    message_bus_ = message_bus;
    raw_callbacks_ = raw_callbacks;
    availability_handler_ = availability_handler;
    cookie_protocol_ = cookie_protocol;
    cookie_set_handler_ = cookie_set_handler;
    protocol_name_ = retained_protocol_name;
    is_available_id_ = retained_is_available_id;
    handle_window_close_message_id_ = retained_handle_window_close_message_id;
    connections_ = connections;
    connection_handles_ = connection_handles;
    raw_callback_targets_ = std::move(raw_callback_targets);
    platform_web_callbacks_registered_ = true;
    cookie_set_handler_registered_ = true;
    initialized_ = true;
    initializing_ = false;
    dispatch_drained_.notify_all();
  }
  return Status::Ok();
}

Status RobloxWebViewBridge::Shutdown() {
  std::unique_lock<std::mutex> lock(mutex_);
  accepting_messages_ = false;
  dispatch_drained_.wait(lock,
                         [this] { return !initializing_ && !shutting_down_; });
  bool has_subscription_resources = false;
  for (std::size_t index = 0; index < kSubscriptionCount; ++index) {
    has_subscription_resources = has_subscription_resources ||
                                 connections_[index] != nullptr ||
                                 raw_callbacks_[index] != nullptr;
  }
  if (!initialized_ && !has_subscription_resources &&
      availability_handler_ == nullptr && message_bus_ == nullptr &&
      cookie_protocol_ == nullptr && cookie_set_handler_ == nullptr) {
    return Status::Ok();
  }
  shutting_down_ = true;
  const std::array<jobject, kSubscriptionCount> connections = connections_;
  const std::array<jobject, kSubscriptionCount> raw_callbacks = raw_callbacks_;
  const std::array<jlong, kSubscriptionCount> connection_handles =
      connection_handles_;
  const bool platform_web_callbacks_registered =
      platform_web_callbacks_registered_;
  const bool cookie_set_handler_registered =
      cookie_set_handler_registered_;
  const jobject cookie_protocol = cookie_protocol_;
  const jobject cookie_set_handler = cookie_set_handler_;
  const std::array<std::shared_ptr<RawCallbackTarget>, kSubscriptionCount>
      raw_callback_targets = raw_callback_targets_;
  const std::shared_ptr<HostWindowCloseTarget> host_window_close_target =
      host_window_close_target_;
  platform_web_callbacks_registered_ = false;
  {
    std::lock_guard<std::mutex> event_lock(host_window_event_mutex_);
    pending_host_window_events_.clear();
  }
  lock.unlock();

  if (platform_web_callbacks_registered) {
    source_objects_.clear_platform_web_callbacks(
        source_objects_.callback_factory_context);
  }
  for (const std::shared_ptr<RawCallbackTarget>& target :
       raw_callback_targets) {
    if (target != nullptr) {
      std::lock_guard<std::mutex> target_lock(target->mutex);
      target->bridge = nullptr;
    }
  }
  if (host_window_close_target != nullptr) {
    std::lock_guard<std::mutex> target_lock(host_window_close_target->mutex);
    host_window_close_target->bridge = nullptr;
    host_window_close_target->active_generation = 0;
    host_window_close_target->close_queued = false;
  }

  lock.lock();
  dispatch_drained_.wait(lock, [this] { return in_flight_dispatches_ == 0; });
  lock.unlock();

  JNIEnv *env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    lock.lock();
    shutting_down_ = false;
    dispatch_drained_.notify_all();
    return status;
  }
  if (cookie_set_handler_registered) {
    symbols_.update_cookie_set_handler(env, cookie_protocol, nullptr);
    const Status clear_cookie_status =
        CheckJniException(env, "clear Roblox cookie set handler");
    if (status.ok()) {
      status = clear_cookie_status;
    }
  }
  for (std::size_t index = 0; index < connections.size(); ++index) {
    if (connections[index] == nullptr || connection_handles[index] == 0) {
      continue;
    }
    symbols_.delete_connection(env, connections[index],
                               connection_handles[index]);
    const Status disconnect_status =
        CheckJniException(env, "disconnect WebViewProtocol window event");
    if (status.ok()) {
      status = disconnect_status;
    }
  }
  if (message_bus_ != nullptr && protocol_name_ != nullptr &&
      is_available_id_ != nullptr) {
    symbols_.clear_request_handler(env, message_bus_, protocol_name_,
                                   is_available_id_);
    const Status clear_status =
        CheckJniException(env, "clear WebViewProtocol availability");
    if (status.ok()) {
      status = clear_status;
    }
  }
  for (jobject callback : raw_callbacks) {
    if (callback != nullptr) {
      source_objects_.clear_raw_callback(
          source_objects_.callback_factory_context, callback);
    }
  }
  if (availability_handler_ != nullptr) {
    source_objects_.clear_request_handler(
        source_objects_.callback_factory_context, availability_handler_);
  }
  for (jobject connection : connections) {
    if (connection != nullptr) {
      env->DeleteGlobalRef(connection);
    }
  }
  for (jobject callback : raw_callbacks) {
    if (callback != nullptr) {
      env->DeleteGlobalRef(callback);
    }
  }
  if (availability_handler_ != nullptr) {
    env->DeleteGlobalRef(availability_handler_);
  }
  if (cookie_set_handler != nullptr) {
    env->DeleteGlobalRef(cookie_set_handler);
  }
  if (cookie_protocol != nullptr) {
    env->DeleteGlobalRef(cookie_protocol);
  }
  if (protocol_name_ != nullptr) {
    env->DeleteGlobalRef(protocol_name_);
  }
  if (is_available_id_ != nullptr) {
    env->DeleteGlobalRef(is_available_id_);
  }
  if (handle_window_close_message_id_ != nullptr) {
    env->DeleteGlobalRef(handle_window_close_message_id_);
  }
  if (message_bus_ != nullptr) {
    env->DeleteGlobalRef(message_bus_);
  }
  lock.lock();
  connections_ = {};
  connection_handles_ = {};
  raw_callbacks_ = {};
  availability_handler_ = nullptr;
  cookie_protocol_ = nullptr;
  cookie_set_handler_ = nullptr;
  protocol_name_ = nullptr;
  is_available_id_ = nullptr;
  handle_window_close_message_id_ = nullptr;
  message_bus_ = nullptr;
  raw_callback_targets_ = {};
  host_window_close_target_.reset();
  cookie_set_handler_registered_ = false;
  initialized_ = false;
  shutting_down_ = false;
  dispatch_drained_.notify_all();
  return status;
}

Status RobloxWebViewBridge::HandleRawMessage(JNIEnv *env, jstring message) {
  if (env == nullptr || message == nullptr) {
    return Invalid("WebViewProtocol RawCallback message is null");
  }
  const jsize size = env->GetStringUTFLength(message);
  if (size <= 0 ||
      static_cast<std::size_t>(size) > kMaximumRobloxWebViewJsonBytes) {
    return Invalid("WebViewProtocol RawCallback message size is invalid");
  }
  const char *chars = env->GetStringUTFChars(message, nullptr);
  if (chars == nullptr) {
    return Unavailable("could not copy WebViewProtocol openWindow JSON");
  }
  std::string owned(chars, static_cast<std::size_t>(size));
  env->ReleaseStringUTFChars(message, chars);
  Status status =
      CheckJniException(env, "copy WebViewProtocol openWindow JSON");
  return status.ok() ? HandleOwnedMessage(std::move(owned)) : status;
}

Status RobloxWebViewBridge::HandleOwnedMessage(std::string message) {
  return HandleOwnedMessageFromSource(std::move(message),
                                      OpenSource::kMessageBus);
}

Status RobloxWebViewBridge::HandleMutateRawMessage(JNIEnv* env,
                                                   jstring message) {
  if (env == nullptr || message == nullptr) {
    return Invalid("WebViewProtocol mutateWindow message is null");
  }
  const jsize size = env->GetStringUTFLength(message);
  if (size <= 0 ||
      static_cast<std::size_t>(size) > kMaximumRobloxWebViewJsonBytes) {
    return Invalid("WebViewProtocol mutateWindow message size is invalid");
  }
  const char* chars = env->GetStringUTFChars(message, nullptr);
  if (chars == nullptr) {
    return Unavailable("could not copy WebViewProtocol mutateWindow JSON");
  }
  std::string owned(chars, static_cast<std::size_t>(size));
  env->ReleaseStringUTFChars(message, chars);
  Status status =
      CheckJniException(env, "copy WebViewProtocol mutateWindow JSON");
  return status.ok() ? HandleOwnedMutation(std::move(owned)) : status;
}

Status RobloxWebViewBridge::HandleOwnedMutation(std::string message) {
  RobloxWebViewMutationRequest request;
  Status status = ParseRobloxWebViewMutationJson(message, &request);
  if (!status.ok()) {
    return status;
  }
  status = BeginDispatch();
  if (!status.ok()) {
    return status;
  }
  status = sink_.dispatch_mutate(sink_.context, request);
  EndDispatch();
  return status;
}

Status RobloxWebViewBridge::HandleCloseWindow() {
  Status status = BeginDispatch();
  if (!status.ok()) {
    return status;
  }
  status = sink_.dispatch_close(sink_.context);
  EndDispatch();
  return status;
}

Status RobloxWebViewBridge::HandleOwnedMessageFromSource(std::string message,
                                                         OpenSource source) {
  RobloxWebViewOpenRequest request;
  Status status = ParseRobloxWebViewOpenJson(message, &request);
  if (!status.ok()) {
    return status;
  }
  return DispatchOpenRequest(std::move(request), source);
}

Status RobloxWebViewBridge::DispatchOpenRequest(
    RobloxWebViewOpenRequest request, OpenSource source) {
  Status status = BeginDispatch();
  if (!status.ok()) {
    return status;
  }
  if (request.url.empty() ||
      request.url.size() > kMaximumRobloxWebViewUrlBytes) {
    status = Invalid("WebView host-window URL is invalid");
  } else {
    const uint64_t generation =
        next_host_window_generation_.fetch_add(1, std::memory_order_relaxed);
    auto exit_context = std::make_shared<HostWindowExitContext>();
    exit_context->target = host_window_close_target_;
    exit_context->generation = generation;
    if (generation == 0 || exit_context->target == nullptr) {
      status =
          FailedPrecondition("WebView host-window generation is unavailable");
    } else {
      uint64_t previous_generation = 0;
      OpenSource previous_source = OpenSource::kMessageBus;
      bool previous_close_queued = false;
      {
        std::lock_guard<std::mutex> target_lock(exit_context->target->mutex);
        if (exit_context->target->bridge != this) {
          status =
              FailedPrecondition("WebView host-window observer is inactive");
        } else {
          previous_generation = exit_context->target->active_generation;
          previous_source = exit_context->target->active_source;
          previous_close_queued = exit_context->target->close_queued;
          exit_context->target->active_generation = generation;
          exit_context->target->active_source = source;
          exit_context->target->close_queued = false;
        }
      }
      if (status.ok()) {
        status = sink_.dispatch_open(
            sink_.context, request,
            WebViewHelperExitObserver{exit_context,
                                      &RobloxWebViewBridge::HostWindowExited});
      }
      if (status.ok() && (source == OpenSource::kDataModelNotification ||
                          source == OpenSource::kWebActivity ||
                          source == OpenSource::kCaptcha)) {
        QueueHostWindowEvent(HostWindowEventType::kDataModelUnfocused,
                             generation);
      }
      if (!status.ok()) {
        std::lock_guard<std::mutex> target_lock(exit_context->target->mutex);
        if (exit_context->target->active_generation == generation) {
          exit_context->target->active_generation = previous_generation;
          exit_context->target->active_source = previous_source;
          exit_context->target->close_queued = previous_close_queued;
        }
      }
    }
  }
  EndDispatch();
  return status;
}

void RobloxWebViewBridge::QueueHostWindowEvent(HostWindowEventType type,
                                               uint64_t generation) {
  if (generation == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(host_window_event_mutex_);
  if (pending_host_window_events_.size() == kMaximumPendingHostWindowEvents) {
    pending_host_window_events_.pop_front();
    std::fprintf(stderr,
                 "  [webview] pending host event queue overflow; preserving "
                 "newest lifecycle state\n");
  }
  pending_host_window_events_.push_back({type, generation});
}

Status RobloxWebViewBridge::DrainHostWindowEvents() {
  std::deque<PendingHostWindowEvent> events;
  {
    std::lock_guard<std::mutex> lock(host_window_event_mutex_);
    events.swap(pending_host_window_events_);
  }
  if (events.empty()) {
    return Status::Ok();
  }
  Status status = BeginDispatch();
  if (!status.ok()) {
    return Status::Ok();
  }
  JNIEnv *env = nullptr;
  status = environment_.Acquire(&env);
  if (!status.ok()) {
    std::lock_guard<std::mutex> lock(host_window_event_mutex_);
    pending_host_window_events_.insert(pending_host_window_events_.begin(),
                                       events.begin(), events.end());
    EndDispatch();
    return status;
  }

  std::size_t processed = 0;
  for (; processed < events.size() && status.ok();) {
    const PendingHostWindowEvent& event = events[processed];
    switch (event.type) {
      case HostWindowEventType::kDataModelUnfocused:
        status = BroadcastDataModelFocus(env, "Unfocused");
        if (status.ok()) {
          std::fprintf(stderr,
                       "  [webview] host window focus transferred to Roblox "
                       "web surface\n");
        }
        break;
      case HostWindowEventType::kDataModelFocused:
        status = BroadcastDataModelFocus(env, "Focused");
        if (status.ok()) {
          std::fprintf(stderr,
                       "  [webview] DataModel web surface focus returned to "
                       "Roblox\n");
        }
        break;
      case HostWindowEventType::kMessageBusWindowClose: {
        jstring payload = env->NewStringUTF("{}");
        if (payload == nullptr) {
          status = Unavailable("could not allocate WebView close payload");
        } else {
          symbols_.publish_raw(env, message_bus_,
                               handle_window_close_message_id_, payload);
          env->DeleteLocalRef(payload);
          status = CheckJniException(env, "publish WebView.handleWindowClose");
        }
        if (status.ok()) {
          std::fprintf(stderr,
                       "  [webview] MessageBus web surface close delivered "
                       "to Roblox\n");
        }
        break;
      }
    }
    if (status.ok() && event.type != HostWindowEventType::kDataModelUnfocused) {
      const std::shared_ptr<HostWindowCloseTarget> target =
          host_window_close_target_;
      if (target != nullptr) {
        std::lock_guard<std::mutex> target_lock(target->mutex);
        if (target->active_generation == event.generation) {
          target->active_generation = 0;
          target->close_queued = false;
        }
      }
    }
    if (status.ok()) {
      ++processed;
    }
  }
  if (processed < events.size()) {
    std::lock_guard<std::mutex> lock(host_window_event_mutex_);
    pending_host_window_events_.insert(pending_host_window_events_.begin(),
                                       events.begin() + processed,
                                       events.end());
  }
  EndDispatch();
  return status;
}

Status RobloxWebViewBridge::HandleDataModelNotification(JNIEnv* env,
                                                        jstring type,
                                                        jstring data) {
  if (env == nullptr || type == nullptr || data == nullptr) {
    return Invalid("DataModel notification arguments are null");
  }
  std::string owned_type;
  Status status =
      CopyJniString(env, type, kMaximumRobloxDataModelNotificationTypeBytes,
                    "DataModel notification type", &owned_type);
  if (!status.ok()) {
    return status;
  }

  if (owned_type == kRobloxOpenCustomWebViewNotification) {
    std::string owned_data;
    status = CopyJniString(env, data, kMaximumRobloxWebViewJsonBytes,
                           "DataModel WebView payload", &owned_data);
    if (!status.ok()) {
      return status;
    }
    std::fprintf(stderr,
                 "  [webview] OPEN_CUSTOM_WEBVIEW notification received\n");
    return HandleOwnedMessageFromSource(std::move(owned_data),
                                        OpenSource::kDataModelNotification);
  }

  RobloxWebViewOpenRequest request;
  if (owned_type == kRobloxViewProfileNotification) {
    std::string owned_url;
    status = CopyJniString(env, data, kMaximumRobloxWebViewUrlBytes,
                           "profile WebView URL", &owned_url);
    if (status.ok()) {
      status = ParseRobloxProfileViewUrl(owned_url, &request);
    }
    if (!status.ok()) {
      return status;
    }
    std::fprintf(stderr, "  [webview] VIEW_PROFILE notification received\n");
    return DispatchOpenRequest(std::move(request),
                               OpenSource::kDataModelNotification);
  }

  if (owned_type == kRobloxPurchaseRobuxNotification) {
    std::string owned_data;
    status = CopyJniString(env, data, kMaximumRobloxWebViewJsonBytes,
                           "Robux notification payload", &owned_data);
    if (status.ok()) {
      status = ParseRobloxPurchaseRobuxNotification(owned_data, &request);
    }
    if (!status.ok()) {
      return status;
    }
    std::fprintf(stderr, "  [webview] PURCHASE_ROBUX notification received\n");
    return DispatchOpenRequest(std::move(request),
                               OpenSource::kDataModelNotification);
  }

  return Status::Ok();
}

Status RobloxWebViewBridge::HandleAppBridgeNotification(JNIEnv* env,
                                                        jstring type,
                                                        jstring data) {
  std::string owned_type;
  Status status =
      CopyJniString(env, type, kMaximumRobloxDataModelNotificationTypeBytes,
                    "AppBridge notification type", &owned_type);
  if (!status.ok() || owned_type != kRobloxOpenCaptchaViewNotification) {
    return status;
  }
  std::string owned_data;
  status = CopyJniString(env, data, kMaximumRobloxWebViewJsonBytes,
                         "captcha notification payload", &owned_data);
  if (!status.ok()) {
    return status;
  }
  RobloxWebViewOpenRequest request;
  status = ParseRobloxCaptchaOpenJson(owned_data, &request);
  if (!status.ok()) {
    return status;
  }
  std::fprintf(stderr, "  [webview] OPEN_CAPTCHA_VIEW notification received\n");
  return DispatchOpenRequest(std::move(request), OpenSource::kCaptcha);
}

Status RobloxWebViewBridge::HandleNativeOverlay(JNIEnv* env, jstring title,
                                                jstring url) {
  RobloxWebViewOpenRequest request;
  Status status =
      CopyJniString(env, title, 512, "native overlay title", &request.title);
  if (!status.ok()) {
    return status;
  }
  status = CopyJniString(env, url, kMaximumRobloxWebViewUrlBytes,
                         "native overlay URL", &request.url);
  if (!status.ok() || request.url.empty() || HasControlBytes(request.url) ||
      HasControlBytes(request.title)) {
    return status.ok() ? Invalid("native overlay request is invalid") : status;
  }
  std::fprintf(stderr, "  [webview] native overlay request received\n");
  return DispatchOpenRequest(std::move(request), OpenSource::kNativeOverlay);
}

Status RobloxWebViewBridge::HandleOpenWebActivity(JNIEnv* env, jstring url,
                                                  jstring title) {
  RobloxWebViewOpenRequest request;
  Status status = CopyJniString(env, url, kMaximumRobloxWebViewUrlBytes,
                                "web activity URL", &request.url);
  if (status.ok()) {
    status =
        CopyJniString(env, title, 512, "web activity title", &request.title);
  }
  if (!status.ok() || request.url.empty() || HasControlBytes(request.url) ||
      HasControlBytes(request.title)) {
    return status.ok() ? Invalid("web activity request is invalid") : status;
  }
  std::fprintf(stderr, "  [webview] web activity request received\n");
  return DispatchOpenRequest(std::move(request), OpenSource::kWebActivity);
}

Status RobloxWebViewBridge::SignalJavascriptCallback(
    const std::string& payload) {
  if (payload.empty() || payload.size() > kMaximumRobloxWebViewJsonBytes ||
      payload.find('\0') != std::string::npos) {
    return Invalid("WebView hybrid callback payload is invalid");
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    return status;
  }
  jclass protocol_class = env->FindClass(kWebViewProtocolClass);
  jstring message = env->NewStringUTF(payload.c_str());
  status = CheckJniException(env, "prepare WebView hybrid callback");
  if (!status.ok() || protocol_class == nullptr || message == nullptr) {
    if (message != nullptr) {
      env->DeleteLocalRef(message);
    }
    if (protocol_class != nullptr) {
      env->DeleteLocalRef(protocol_class);
    }
    return status.ok()
               ? Unavailable(
                     "WebView hybrid callback JNI objects are unavailable")
               : status;
  }
  symbols_.signal_javascript_callback(env, protocol_class, message);
  env->DeleteLocalRef(message);
  env->DeleteLocalRef(protocol_class);
  return CheckJniException(env, "signal WebView JavaScript callback");
}

bool RobloxWebViewBridge::initialized() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return initialized_;
}

Status RobloxWebViewBridge::CheckJniException(JNIEnv *env,
                                              const char *operation) const {
  if (env == nullptr) {
    return Unavailable("JNIEnv is unavailable");
  }
  if (env->ExceptionCheck() == JNI_FALSE) {
    return Status::Ok();
  }
  env->ExceptionClear();
  return Status::Error(
      StatusCode::kPlatformError,
      std::string(operation != nullptr ? operation : "JNI operation") +
          " raised a JNI exception");
}

Status RobloxWebViewBridge::BroadcastDataModelFocus(JNIEnv *env,
                                                    const char *state) {
  if (env == nullptr || state == nullptr || state[0] == '\0') {
    return Invalid("DataModel focus event arguments are invalid");
  }
  jclass native_gl_class = env->FindClass(kNativeGlInterfaceClass);
  jstring event_namespace = env->NewStringUTF("AppInput");
  jstring event_detail = env->NewStringUTF("");
  jstring event_state = env->NewStringUTF(state);
  if (native_gl_class == nullptr || event_namespace == nullptr ||
      event_detail == nullptr || event_state == nullptr) {
    if (event_state != nullptr) {
      env->DeleteLocalRef(event_state);
    }
    if (event_detail != nullptr) {
      env->DeleteLocalRef(event_detail);
    }
    if (event_namespace != nullptr) {
      env->DeleteLocalRef(event_namespace);
    }
    if (native_gl_class != nullptr) {
      env->DeleteLocalRef(native_gl_class);
    }
    return Unavailable("could not allocate DataModel focus event");
  }
  symbols_.broadcast_data_model_focus(env, native_gl_class, event_namespace,
                                      event_detail, event_state);
  env->DeleteLocalRef(event_state);
  env->DeleteLocalRef(event_detail);
  env->DeleteLocalRef(event_namespace);
  env->DeleteLocalRef(native_gl_class);
  return CheckJniException(env, "broadcast AppInput DataModel focus event");
}

Status RobloxWebViewBridge::BeginDispatch() {
  std::lock_guard<std::mutex> lock(mutex_);
  if ((!initializing_ && !initialized_) || !accepting_messages_) {
    return FailedPrecondition("WebView bridge is not accepting messages");
  }
  ++in_flight_dispatches_;
  return Status::Ok();
}

void RobloxWebViewBridge::EndDispatch() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (in_flight_dispatches_ > 0) {
    --in_flight_dispatches_;
  }
  if (in_flight_dispatches_ == 0) {
    dispatch_drained_.notify_all();
  }
}

void RobloxWebViewBridge::RawMessageCallback(void *context, JNIEnv *env,
                                             jstring message) {
  auto *target = static_cast<RawCallbackTarget *>(context);
  if (target == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(target->mutex);
  if (target->bridge == nullptr) {
    return;
  }
  Status status = Status::Ok();
  switch (target->kind) {
    case SubscriptionKind::kOpenWindow:
      std::fprintf(stderr, "  [webview] openWindow event received\n");
      status = target->bridge->HandleRawMessage(env, message);
      break;
    case SubscriptionKind::kMutateWindow:
      std::fprintf(stderr, "  [webview] mutateWindow event received\n");
      status = target->bridge->HandleMutateRawMessage(env, message);
      break;
    case SubscriptionKind::kCloseWindow:
      std::fprintf(stderr, "  [webview] closeWindow event received\n");
      // The APK callback does not inspect the JSONObject at all.
      status = target->bridge->HandleCloseWindow();
      break;
  }
    if (!status.ok()) {
      std::fprintf(stderr, "  [webview] window event rejected: %s\n",
                   status.message().c_str());
    }
}

std::string RobloxWebViewBridge::AvailabilityCallback(void*, JNIEnv*, jstring) {
  std::fprintf(stderr, "  [webview] availability request answered\n");
  return R"({"available":true})";
}

void RobloxWebViewBridge::DataModelNotificationCallback(void *context,
                                                        JNIEnv *env,
                                                        jstring type,
                                                        jstring data) {
  auto *target = static_cast<RawCallbackTarget *>(context);
  if (target == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(target->mutex);
  if (target->bridge == nullptr) {
    return;
  }
  const Status status =
      target->bridge->HandleDataModelNotification(env, type, data);
  if (!status.ok()) {
    std::fprintf(stderr, "  [webview] DataModel notification rejected: %s\n",
                 status.message().c_str());
  }
}

void RobloxWebViewBridge::AppBridgeNotificationCallback(void* context,
                                                        JNIEnv* env,
                                                        jstring type,
                                                        jstring data) {
  auto* target = static_cast<RawCallbackTarget*>(context);
  if (target == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(target->mutex);
  if (target->bridge == nullptr) {
    return;
  }
  const Status status =
      target->bridge->HandleAppBridgeNotification(env, type, data);
  if (!status.ok()) {
    std::fprintf(stderr, "  [webview] AppBridge notification rejected: %s\n",
                 status.message().c_str());
  }
}

void RobloxWebViewBridge::NativeOverlayCallback(void* context, JNIEnv* env,
                                                jstring title, jstring url) {
  auto* target = static_cast<RawCallbackTarget*>(context);
  if (target == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(target->mutex);
  if (target->bridge == nullptr) {
    return;
  }
  const Status status = target->bridge->HandleNativeOverlay(env, title, url);
  if (!status.ok()) {
    std::fprintf(stderr, "  [webview] native overlay rejected: %s\n",
                 status.message().c_str());
  }
}

void RobloxWebViewBridge::OpenWebActivityCallback(void* context, JNIEnv* env,
                                                  jstring url, jstring title) {
  auto* target = static_cast<RawCallbackTarget*>(context);
  if (target == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(target->mutex);
  if (target->bridge == nullptr) {
    return;
  }
  const Status status = target->bridge->HandleOpenWebActivity(env, url, title);
  if (!status.ok()) {
    std::fprintf(stderr, "  [webview] web activity rejected: %s\n",
                 status.message().c_str());
  }
}

void RobloxWebViewBridge::SyncCookiesCallback(void* context, JNIEnv* env,
                                              jstring cookie) {
  auto* target = static_cast<RawCallbackTarget*>(context);
  if (target == nullptr) {
    return;
  }
  std::string header;
  const Status copied =
      CopyJniString(env, cookie, 16 * 1024 + 16, "Roblox cookie sync", &header);
  if (!copied.ok()) {
    std::fprintf(stderr, "  [webview] cookie sync rejected: %s\n",
                 copied.message().c_str());
    ClearSensitiveString(&header);
    return;
  }
  Status status;
  {
    std::lock_guard<std::mutex> lock(target->mutex);
    status = target->bridge == nullptr
                 ? FailedPrecondition("WebView bridge is unavailable")
                 : target->bridge->sink_.dispatch_cookie(
                       target->bridge->sink_.context, header);
  }
  ClearSensitiveString(&header);
  if (!status.ok()) {
    std::fprintf(stderr, "  [webview] cookie sync rejected: %s\n",
                 status.message().c_str());
  }
}

void RobloxWebViewBridge::SetCookieCallback(void* context, JNIEnv* env,
                                            jstring cookie, jstring url) {
  (void)url;
  SyncCookiesCallback(context, env, cookie);
}

void RobloxWebViewBridge::HostWindowExited(void *context) {
  auto *exit_context = static_cast<HostWindowExitContext *>(context);
  if (exit_context == nullptr || exit_context->target == nullptr ||
      exit_context->generation == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(exit_context->target->mutex);
  RobloxWebViewBridge *bridge = exit_context->target->bridge;
  if (bridge == nullptr ||
      exit_context->target->active_generation != exit_context->generation ||
      exit_context->target->close_queued) {
    return;
  }
  HostWindowEventType close_event = HostWindowEventType::kDataModelFocused;
  bool queue_close_event = true;
  switch (exit_context->target->active_source) {
    case OpenSource::kDataModelNotification:
    case OpenSource::kWebActivity:
    case OpenSource::kCaptcha:
      break;
    case OpenSource::kMessageBus:
      close_event = HostWindowEventType::kMessageBusWindowClose;
      break;
    case OpenSource::kNativeOverlay:
      exit_context->target->active_generation = 0;
      queue_close_event = false;
      break;
  }
  if (!queue_close_event) {
    return;
  }
  exit_context->target->close_queued = true;
  bridge->QueueHostWindowEvent(close_event, exit_context->generation);
}

} // namespace runtime
} // namespace mocktail
