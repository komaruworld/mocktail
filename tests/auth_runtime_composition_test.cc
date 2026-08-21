#include "runtime/auth_runtime_composition.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "jnivm/jnivm.h"
#include "legacy/legacy_runtime.h"
#include "runtime/environment.h"
#include "runtime/runtime_paths.h"
#include "services/auth_service.h"
#include "services/http_client.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr char kCredential[] = "_|test-auth-runtime-credential";

static_assert(!std::is_copy_constructible_v<SecureRobloxCredential>);
static_assert(!std::is_move_constructible_v<ScopedRobloxCredentialBinding>,
              "credential provider context must keep a stable address");
static_assert(!std::is_move_assignable_v<ScopedRobloxCredentialBinding>,
              "credential provider binding must not move implicitly");
static_assert(!std::is_copy_assignable_v<SecureRobloxCredential>);
static_assert(std::is_nothrow_move_constructible_v<SecureRobloxCredential>);

class MapEnvironment final : public Environment {
 public:
  void Set(std::string name, std::string value) {
    values_[std::move(name)] = std::move(value);
  }

  std::optional<std::string> Get(std::string_view name) const override {
    const auto found = values_.find(std::string(name));
    if (found == values_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

 private:
  std::map<std::string, std::string> values_;
};

class FakeHttpClient final : public services::HttpClient {
 public:
  services::HttpResponse Get(const services::HttpRequest& request) override {
    ++request_count;
    last_request = request;
    if (before_response) {
      before_response();
    }
    return response;
  }

  services::HttpResponse response;
  services::HttpRequest last_request;
  std::function<void()> before_response;
  int request_count = 0;
};

class TempDirectory final {
 public:
  TempDirectory() {
    static std::atomic<unsigned int> serial{0};
    path_ = std::filesystem::temp_directory_path() /
            ("mocktail-auth-composition-" + std::to_string(getpid()) + "-" +
             std::to_string(serial.fetch_add(1)));
    std::error_code error;
    std::filesystem::create_directories(path_, error);
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

bool WriteFile(const std::filesystem::path& path, const std::string& contents) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return false;
  }
  if (chmod(path.parent_path().c_str(), S_IRWXU) != 0) {
    return false;
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << contents;
  stream.close();
  return stream.good() && chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

RuntimePaths PathsFor(const MapEnvironment& environment,
                      const TempDirectory& directory) {
  return RuntimePaths::FromEnvironment(environment, directory.path());
}

std::string ReadJavaString(JNIEnv* env, jstring value) {
  if (env == nullptr || value == nullptr) {
    return {};
  }
  const char* chars = env->GetStringUTFChars(value, nullptr);
  if (chars == nullptr) {
    return {};
  }
  const std::string result(chars);
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

struct NativeCookieCallbackProbe {
  int calls = 0;
  std::string cookie;
  std::string url;
};

void IgnoreDataModelNotification(void*, JNIEnv*, jstring, jstring) {}

void CaptureNativeCookie(void* context, JNIEnv* env, jstring cookie,
                         jstring url) {
  auto* probe = static_cast<NativeCookieCallbackProbe*>(context);
  ++probe->calls;
  probe->cookie = ReadJavaString(env, cookie);
  probe->url = ReadJavaString(env, url);
}

TEST(AuthRuntimeCompositionTest,
     ResolvesSavedCookieAndInjectsOnlyTypedIdentity) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_ROBLOX_COOKIES",
                  std::string(".ROBLOSECURITY=") + kCredential);
  FakeHttpClient http;
  http.response = {
      true,
      200,
      R"({"id":321,"name":"RuntimeUser","displayName":"Runtime Display"})",
      {}};
  services::AuthService auth_service(http);

  const AuthRuntimeComposition composition = ComposeAuthRuntime(
      environment, PathsFor(environment, directory), auth_service);

  ASSERT_TRUE(composition);
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kAuthenticated);
  EXPECT_EQ(composition.http_status, 200);
  EXPECT_TRUE(composition.error.empty());
  ASSERT_EQ(http.request_count, 1);
  EXPECT_FALSE(http.last_request.follow_redirects);
  ASSERT_EQ(http.last_request.headers.size(), 2U);
  EXPECT_EQ(http.last_request.headers[1],
            std::string("Cookie: .ROBLOSECURITY=") + kCredential);
  EXPECT_EQ(composition.credential.view(),
            std::string(".ROBLOSECURITY=") + kCredential);

  const jnivm::RobloxAuthIdentity identity =
      composition.jni_vm->GetRobloxAuthIdentitySnapshot();
  EXPECT_EQ(identity.user_id, 321);
  EXPECT_EQ(identity.username, "RuntimeUser");
  EXPECT_EQ(identity.display_name, "Runtime Display");
  EXPECT_EQ(composition.account_identity.user_id, identity.user_id);
  EXPECT_EQ(composition.account_identity.username, identity.username);
  EXPECT_EQ(identity.username.find(kCredential), std::string::npos);
  EXPECT_EQ(identity.display_name.find(kCredential), std::string::npos);
}

TEST(AuthRuntimeCompositionTest, SoberCookieIsIgnoredAndGuestPolicyRequired) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  ASSERT_TRUE(WriteFile(
      directory.path() /
          ".var/app/org.vinegarhq.Sober/data/sober/cookies",
      std::string(".ROBLOSECURITY=") + kCredential + "\n"));
  FakeHttpClient http;
  services::AuthService auth_service(http);

  AuthRuntimeComposition composition = ComposeAuthRuntime(
      environment, PathsFor(environment, directory), auth_service);
  EXPECT_FALSE(composition);
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kInvalidCredentials);
  EXPECT_EQ(composition.error, "missing .ROBLOSECURITY cookie");
  EXPECT_EQ(http.request_count, 0);
  EXPECT_TRUE(composition.credential.empty());
  EXPECT_EQ(composition.account_identity.user_id, -1);

  environment.Set("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1");
  composition = ComposeAuthRuntime(
      environment, PathsFor(environment, directory), auth_service);
  ASSERT_TRUE(composition);
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kGuest);
  EXPECT_EQ(http.request_count, 0);
  const jnivm::RobloxAuthIdentity identity =
      composition.jni_vm->GetRobloxAuthIdentitySnapshot();
  EXPECT_EQ(identity.user_id, -1);
  EXPECT_TRUE(identity.username.empty());
  EXPECT_TRUE(identity.display_name.empty());
}

TEST(AuthRuntimeCompositionTest,
     GuestJavaCookiePersistsPrivatelyAndBecomesCurrentCredential) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1");
  const RuntimePaths paths = PathsFor(environment, directory);
  FakeHttpClient http;
  services::AuthService auth_service(http);
  AuthRuntimeComposition composition =
      ComposeAuthRuntime(environment, paths, auth_service);
  ASSERT_TRUE(composition);
  ASSERT_EQ(composition.status, AuthRuntimeStatus::kGuest);

  const std::string saved = ".ROBLOSECURITY=_|embedded-login-cookie";
  {
    const ScopedRobloxCredentialBinding binding(composition.jni_vm.get(),
                                                composition.credential);
    ASSERT_TRUE(binding.bound());
    JNIEnv* env = composition.jni_vm->GetJNIEnv();
    jclass clazz =
        env->FindClass("com/roblox/universalapp/cookie/JNICookieManager");
    ASSERT_NE(clazz, nullptr);
    jobject manager = env->AllocObject(clazz);
    jmethodID setter = env->GetMethodID(
        clazz, "setCookie", "(Ljava/lang/String;Ljava/lang/String;)V");
    ASSERT_NE(setter, nullptr);
    env->CallVoidMethod(manager, setter,
                        env->NewStringUTF("https://www.roblox.com/"),
                        env->NewStringUTF(saved.c_str()));

    std::string provider_value = "must be cleared";
    EXPECT_TRUE(composition.jni_vm->CopyRobloxCredentialFromProvider(
        &provider_value));
    EXPECT_EQ(provider_value, saved);
    jmethodID getter = env->GetStaticMethodID(
        clazz, "getCookie", "(Ljava/lang/String;)Ljava/lang/String;");
    auto current = static_cast<jstring>(env->CallStaticObjectMethod(
        clazz, getter, env->NewStringUTF("roblox.com")));
    EXPECT_EQ(ReadJavaString(env, current), saved);
    EXPECT_EQ(
        composition.jni_vm->GetRobloxAuthIdentitySnapshot().user_id, -1);
  }

  std::ifstream input(paths.cookie_file(), std::ios::binary);
  ASSERT_TRUE(input.good());
  const std::string persisted((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
  EXPECT_EQ(persisted, saved + "\n");

  struct stat auth_status = {};
  struct stat cookie_status = {};
  ASSERT_EQ(lstat(paths.auth_root().c_str(), &auth_status), 0);
  ASSERT_EQ(lstat(paths.cookie_file().c_str(), &cookie_status), 0);
  EXPECT_TRUE(S_ISDIR(auth_status.st_mode));
  EXPECT_EQ(auth_status.st_mode & (S_IRWXG | S_IRWXO), 0U);
  EXPECT_TRUE(S_ISREG(cookie_status.st_mode));
  EXPECT_EQ(cookie_status.st_mode & (S_IRWXG | S_IRWXO), 0U);
}

TEST(AuthRuntimeCompositionTest,
     NativeSignInPromotesValidatedIdentityWithoutRestart) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1");
  const RuntimePaths paths = PathsFor(environment, directory);
  auto http = std::make_shared<FakeHttpClient>();
  http->response = {
      true,
      200,
      R"({"id":987,"name":"LiveUser","displayName":"Live User"})",
      {}};
  services::AuthService auth_service(*http);
  AuthRuntimeComposition composition =
      ComposeAuthRuntime(environment, paths, auth_service, http);
  ASSERT_TRUE(composition);
  ASSERT_EQ(composition.status, AuthRuntimeStatus::kGuest);
  ASSERT_EQ(http->request_count, 0);

  const std::string saved = ".ROBLOSECURITY=_|live-native-login-cookie";
  {
    const ScopedRobloxCredentialBinding binding(composition.jni_vm.get(),
                                                composition.credential);
    ASSERT_TRUE(binding.bound());
    ASSERT_TRUE(composition.jni_vm->DispatchRobloxCredential(saved.data(),
                                                             saved.size()));
    std::string current;
    ASSERT_TRUE(
        composition.jni_vm->CopyRobloxCredentialFromProvider(&current));
    EXPECT_EQ(current, saved);
  }

  ASSERT_EQ(http->request_count, 1);
  ASSERT_GE(http->last_request.headers.size(), 2U);
  EXPECT_EQ(http->last_request.headers[1], "Cookie: " + saved);
  const jnivm::RobloxAuthIdentity identity =
      composition.jni_vm->GetRobloxAuthIdentitySnapshot();
  EXPECT_EQ(identity.user_id, 987);
  EXPECT_EQ(identity.username, "LiveUser");
  EXPECT_EQ(identity.display_name, "Live User");
  EXPECT_EQ(ReadFile(paths.cookie_file()), saved + "\n");
}

TEST(AuthRuntimeCompositionTest,
     NativeCookieCallbackAuthenticatesFromManagedFileAfterRestart) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1");
  const RuntimePaths paths = PathsFor(environment, directory);
  FakeHttpClient initial_http;
  services::AuthService initial_auth_service(initial_http);
  AuthRuntimeComposition composition =
      ComposeAuthRuntime(environment, paths, initial_auth_service);
  ASSERT_TRUE(composition);
  ASSERT_EQ(composition.status, AuthRuntimeStatus::kGuest);

  const std::string saved = ".ROBLOSECURITY=_|native-login-cookie";
  auto callback_probe = std::make_shared<NativeCookieCallbackProbe>();
  jnivm::RobloxDataModelNotificationCallbacks callbacks;
  callbacks.on_notification = &IgnoreDataModelNotification;
  callbacks.on_set_cookie = &CaptureNativeCookie;
  composition.jni_vm->SetRobloxDataModelNotificationCallbacks(callback_probe,
                                                              callbacks);
  {
    const ScopedRobloxCredentialBinding binding(composition.jni_vm.get(),
                                                composition.credential);
    ASSERT_TRUE(binding.bound());
    JNIEnv* env = composition.jni_vm->GetJNIEnv();
    jclass handler_class = env->FindClass(
        "com/roblox/universalapp/cookie/"
        "JNICookieProtocol$OnSetCookieHandler");
    jobject handler = env->AllocObject(handler_class);
    jmethodID on_set_cookie = env->GetMethodID(
        handler_class, "onSetCookie",
        "([Ljava/lang/String;Ljava/lang/String;)V");
    jclass string_class = env->FindClass("java/lang/String");
    jobjectArray cookies = env->NewObjectArray(1, string_class, nullptr);
    env->SetObjectArrayElement(
        cookies, 0,
        env->NewStringUTF(
            ".ROBLOSECURITY=_|native-login-cookie; Domain=.roblox.com; "
            "Path=/; Secure; HttpOnly"));
    env->CallVoidMethod(handler, on_set_cookie, cookies,
                        env->NewStringUTF("https://www.roblox.com/"));
  }
  composition.jni_vm->ClearRobloxDataModelNotificationCallbacks();

  EXPECT_EQ(callback_probe->calls, 1);
  EXPECT_EQ(callback_probe->cookie, saved);
  EXPECT_EQ(callback_probe->url, "https://www.roblox.com/");
  EXPECT_EQ(ReadFile(paths.cookie_file()), saved + "\n");

  MapEnvironment restart_environment;
  restart_environment.Set("HOME", directory.path().string());
  FakeHttpClient restart_http;
  restart_http.response = {
      true,
      200,
      R"({"id":777,"name":"RestartUser","displayName":"Restart User"})",
      {}};
  services::AuthService restart_auth_service(restart_http);
  const AuthRuntimeComposition restarted = ComposeAuthRuntime(
      restart_environment, PathsFor(restart_environment, directory),
      restart_auth_service);

  ASSERT_TRUE(restarted) << restarted.error;
  EXPECT_EQ(restarted.status, AuthRuntimeStatus::kAuthenticated);
  ASSERT_EQ(restart_http.request_count, 1);
  EXPECT_EQ(restart_http.last_request.headers[1], "Cookie: " + saved);
}

TEST(AuthRuntimeCompositionTest,
     RejectedEnvironmentCredentialNeverCreatesProductionVm) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_ROBLOX_COOKIES", kCredential);
  environment.Set("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1");
  const RuntimePaths paths = PathsFor(environment, directory);
  const std::string managed_cookie =
      "RBXEventTrackerV2=browserid=123456&browserid_insert_timestamp=1\n";
  ASSERT_TRUE(WriteFile(paths.cookie_file(), managed_cookie));
  FakeHttpClient http;
  http.response = {true, 401, {}, {}};
  services::AuthService auth_service(http);

  const AuthRuntimeComposition composition =
      ComposeAuthRuntime(environment, paths, auth_service);

  EXPECT_FALSE(composition);
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kInvalidCredentials);
  EXPECT_EQ(composition.http_status, 401);
  EXPECT_EQ(composition.error, "authentication rejected");
  EXPECT_EQ(composition.error.find(kCredential), std::string::npos);
  EXPECT_EQ(ReadFile(paths.cookie_file()), managed_cookie);
}

TEST(AuthRuntimeCompositionTest,
     Http401ClearsManagedCredentialAndStartsNativeSignInGuest) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1");
  const RuntimePaths paths = PathsFor(environment, directory);
  const std::string remaining_cookies =
      "RBXEventTrackerV2=browserid=123456&browserid_insert_timestamp=1\n"
      "OtherCookie=keep\n";
  const std::string rejected_cookie =
      std::string(".ROBLOSECURITY=") + kCredential;
  ASSERT_TRUE(WriteFile(paths.cookie_file(),
                        rejected_cookie + "\n" + remaining_cookies));
  FakeHttpClient http;
  http.response = {true, 401, {}, {}};
  services::AuthService auth_service(http);

  const AuthRuntimeComposition composition =
      ComposeAuthRuntime(environment, paths, auth_service);

  ASSERT_TRUE(composition) << composition.error;
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kGuest);
  EXPECT_EQ(composition.http_status, 401);
  EXPECT_TRUE(composition.error.empty());
  EXPECT_TRUE(composition.credential.empty());
  EXPECT_EQ(composition.account_identity.user_id, -1);
  EXPECT_EQ(http.request_count, 1);
  EXPECT_EQ(
      ReadFile(paths.cookie_file()),
      std::string(rejected_cookie.size(), ' ') + "\n" + remaining_cookies);
  struct stat metadata = {};
  ASSERT_EQ(lstat(paths.cookie_file().c_str(), &metadata), 0);
  EXPECT_TRUE(S_ISREG(metadata.st_mode));
  EXPECT_EQ(metadata.st_mode & (S_IRWXG | S_IRWXO), 0U);

  FakeHttpClient restart_http;
  services::AuthService restart_auth_service(restart_http);
  const AuthRuntimeComposition restarted =
      ComposeAuthRuntime(environment, paths, restart_auth_service);
  ASSERT_TRUE(restarted) << restarted.error;
  EXPECT_EQ(restarted.status, AuthRuntimeStatus::kGuest);
  EXPECT_EQ(restart_http.request_count, 0);
}

TEST(AuthRuntimeCompositionTest,
     Http401ClearsPrivateReadOnlyManagedCredential) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1");
  const RuntimePaths paths = PathsFor(environment, directory);
  const std::string remaining_cookies = "OtherCookie=keep\n";
  const std::string rejected_cookie =
      std::string(".ROBLOSECURITY=") + kCredential;
  ASSERT_TRUE(WriteFile(paths.cookie_file(),
                        rejected_cookie + "\n" + remaining_cookies));
  ASSERT_EQ(chmod(paths.cookie_file().c_str(), S_IRUSR), 0);
  FakeHttpClient http;
  http.response = {true, 401, {}, {}};
  services::AuthService auth_service(http);

  const AuthRuntimeComposition composition =
      ComposeAuthRuntime(environment, paths, auth_service);

  ASSERT_TRUE(composition) << composition.error;
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kGuest);
  EXPECT_EQ(
      ReadFile(paths.cookie_file()),
      std::string(rejected_cookie.size(), ' ') + "\n" + remaining_cookies);
  struct stat metadata = {};
  ASSERT_EQ(lstat(paths.cookie_file().c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & (S_IRWXG | S_IRWXO), 0U);
  EXPECT_NE(metadata.st_mode & S_IWUSR, 0U);
}

TEST(AuthRuntimeCompositionTest,
     Http401RedactsRawAndDuplicateSegmentsWithMixedDelimiters) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1");
  const RuntimePaths paths = PathsFor(environment, directory);
  const std::string raw_cookie = kCredential;
  const std::string duplicate = ".ROBLOSECURITY=duplicate-value";
  const std::string cookies =
      raw_cookie + "\r\nOtherCookie=keep; " + duplicate + "\tTailCookie=keep\n";
  ASSERT_TRUE(WriteFile(paths.cookie_file(), cookies));
  FakeHttpClient http;
  http.response = {true, 401, {}, {}};
  services::AuthService auth_service(http);

  const AuthRuntimeComposition composition =
      ComposeAuthRuntime(environment, paths, auth_service);

  ASSERT_TRUE(composition) << composition.error;
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kGuest);
  const std::string expected =
      std::string(raw_cookie.size(), ' ') + "\r\nOtherCookie=keep;" +
      std::string(duplicate.size() + 1, ' ') + "\tTailCookie=keep\n";
  EXPECT_EQ(ReadFile(paths.cookie_file()), expected);
  EXPECT_EQ(expected.size(), cookies.size());

  FakeHttpClient restart_http;
  services::AuthService restart_auth_service(restart_http);
  const AuthRuntimeComposition restarted =
      ComposeAuthRuntime(environment, paths, restart_auth_service);
  ASSERT_TRUE(restarted) << restarted.error;
  EXPECT_EQ(restarted.status, AuthRuntimeStatus::kGuest);
  EXPECT_EQ(restart_http.request_count, 0);
}

TEST(AuthRuntimeCompositionTest, Http403ClearsManagedCookieAndStartsGuest) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1");
  const RuntimePaths paths = PathsFor(environment, directory);
  const std::string cookies = std::string(".ROBLOSECURITY=") + kCredential +
                              "\nRBXEventTrackerV2=browserid=123456\n";
  ASSERT_TRUE(WriteFile(paths.cookie_file(), cookies));
  FakeHttpClient http;
  http.response = {true, 403, {}, {}};
  services::AuthService auth_service(http);

  const AuthRuntimeComposition composition =
      ComposeAuthRuntime(environment, paths, auth_service);

  ASSERT_TRUE(composition) << composition.error;
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kGuest);
  EXPECT_EQ(composition.http_status, 403);
  EXPECT_TRUE(composition.rejected_credential_retired);
  const std::string rejected_cookie =
      std::string(".ROBLOSECURITY=") + kCredential;
  EXPECT_EQ(ReadFile(paths.cookie_file()),
            std::string(rejected_cookie.size(), ' ') +
                "\nRBXEventTrackerV2=browserid=123456\n");
}

TEST(AuthRuntimeCompositionTest,
     Http401ClearsManagedCredentialButDoesNotStartGuestWhenDisabled) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  const RuntimePaths paths = PathsFor(environment, directory);
  const std::string remaining_cookies = "RBXEventTrackerV2=browserid=123456\n";
  const std::string rejected_cookie =
      std::string(".ROBLOSECURITY=") + kCredential;
  const std::string cookies = rejected_cookie + "\n" + remaining_cookies;
  ASSERT_TRUE(WriteFile(paths.cookie_file(), cookies));
  FakeHttpClient http;
  http.response = {true, 401, {}, {}};
  services::AuthService auth_service(http);

  const AuthRuntimeComposition composition =
      ComposeAuthRuntime(environment, paths, auth_service);

  EXPECT_FALSE(composition);
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kInvalidCredentials);
  EXPECT_EQ(composition.http_status, 401);
  EXPECT_EQ(
      ReadFile(paths.cookie_file()),
      std::string(rejected_cookie.size(), ' ') + "\n" + remaining_cookies);
}

TEST(AuthRuntimeCompositionTest,
     Http401DoesNotClearExplicitCredentialOverride) {
  TempDirectory directory;
  const std::filesystem::path explicit_cookie =
      directory.path() / "explicit-cookie";
  const std::string cookies =
      std::string(".ROBLOSECURITY=") + kCredential + "\n";
  ASSERT_TRUE(WriteFile(explicit_cookie, cookies));
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_COOKIE_FILE", explicit_cookie.string());
  environment.Set("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1");
  FakeHttpClient http;
  http.response = {true, 401, {}, {}};
  services::AuthService auth_service(http);

  const AuthRuntimeComposition composition = ComposeAuthRuntime(
      environment, PathsFor(environment, directory), auth_service);

  EXPECT_FALSE(composition);
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kInvalidCredentials);
  EXPECT_EQ(composition.http_status, 401);
  EXPECT_EQ(ReadFile(explicit_cookie), cookies);
}

TEST(AuthRuntimeCompositionTest,
     Http401FailsClosedWhileCookieWriterLockIsHeld) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1");
  const RuntimePaths paths = PathsFor(environment, directory);
  const std::string cookies =
      std::string(".ROBLOSECURITY=") + kCredential + "\n";
  ASSERT_TRUE(WriteFile(paths.cookie_file(), cookies));
  const std::filesystem::path lock_path =
      paths.cookie_file().parent_path() /
      ("." + paths.cookie_file().filename().string() + ".mocktail-writer.lock");
  ASSERT_TRUE(WriteFile(lock_path, ""));
  const int lock_descriptor =
      open(lock_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  ASSERT_GE(lock_descriptor, 0);
  ASSERT_EQ(flock(lock_descriptor, LOCK_EX | LOCK_NB), 0);
  FakeHttpClient http;
  http.response = {true, 401, {}, {}};
  services::AuthService auth_service(http);

  const AuthRuntimeComposition composition =
      ComposeAuthRuntime(environment, paths, auth_service);

  EXPECT_FALSE(composition);
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kUnavailable);
  EXPECT_EQ(composition.http_status, 401);
  EXPECT_EQ(ReadFile(paths.cookie_file()), cookies);
  EXPECT_EQ(flock(lock_descriptor, LOCK_UN), 0);
  EXPECT_EQ(close(lock_descriptor), 0);
}

TEST(AuthRuntimeCompositionTest,
     CredentialRefreshDuringHttp401RecoveryIsPreserved) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1");
  const RuntimePaths paths = PathsFor(environment, directory);
  ASSERT_TRUE(WriteFile(paths.cookie_file(),
                        std::string(".ROBLOSECURITY=") + kCredential + "\n"));
  struct stat original_metadata = {};
  ASSERT_EQ(lstat(paths.cookie_file().c_str(), &original_metadata), 0);
  const std::string refreshed_contents =
      ".ROBLOSECURITY=_|newly-refreshed-credential\n"
      "RBXEventTrackerV2=browserid=987654\n";
  FakeHttpClient http;
  http.response = {true, 401, {}, {}};
  http.before_response = [&]() {
    EXPECT_TRUE(WriteFile(paths.cookie_file(), refreshed_contents));
  };
  services::AuthService auth_service(http);

  const AuthRuntimeComposition composition =
      ComposeAuthRuntime(environment, paths, auth_service);

  EXPECT_FALSE(composition);
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kUnavailable);
  EXPECT_EQ(composition.http_status, 401);
  EXPECT_EQ(composition.error,
            "Roblox cookie changed while authentication was checked");
  EXPECT_EQ(ReadFile(paths.cookie_file()), refreshed_contents);
  struct stat refreshed_metadata = {};
  ASSERT_EQ(lstat(paths.cookie_file().c_str(), &refreshed_metadata), 0);
  EXPECT_EQ(refreshed_metadata.st_dev, original_metadata.st_dev);
  EXPECT_EQ(refreshed_metadata.st_ino, original_metadata.st_ino);
}

TEST(AuthRuntimeCompositionTest,
     CookieReplacementDuringHttp401RecoveryFailsClosed) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1");
  const RuntimePaths paths = PathsFor(environment, directory);
  ASSERT_TRUE(WriteFile(paths.cookie_file(),
                        std::string(".ROBLOSECURITY=") + kCredential + "\n"));
  const std::filesystem::path protected_target =
      directory.path() / "protected-cookie";
  const std::string protected_contents =
      ".ROBLOSECURITY=_|newly-refreshed-credential\n";
  ASSERT_TRUE(WriteFile(protected_target, protected_contents));
  FakeHttpClient http;
  http.response = {true, 401, {}, {}};
  http.before_response = [&]() {
    std::error_code error;
    std::filesystem::remove(paths.cookie_file(), error);
    EXPECT_FALSE(error);
    std::filesystem::create_symlink(protected_target, paths.cookie_file(),
                                    error);
    EXPECT_FALSE(error);
  };
  services::AuthService auth_service(http);

  const AuthRuntimeComposition composition =
      ComposeAuthRuntime(environment, paths, auth_service);

  EXPECT_FALSE(composition);
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kUnavailable);
  EXPECT_EQ(composition.http_status, 401);
  EXPECT_EQ(composition.error,
            "rejected Roblox cookie could not be cleared safely");
  EXPECT_TRUE(std::filesystem::is_symlink(paths.cookie_file()));
  EXPECT_EQ(ReadFile(protected_target), protected_contents);
}

TEST(AuthRuntimeCompositionTest, UnavailableAuthFailsClosedAndRedactsErrors) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_ROBLOX_COOKIES", kCredential);
  FakeHttpClient http;
  http.response = {false, 0, {}, std::string("request exposed ") + kCredential};
  services::AuthService auth_service(http);

  const AuthRuntimeComposition composition = ComposeAuthRuntime(
      environment, PathsFor(environment, directory), auth_service);

  EXPECT_FALSE(composition);
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kUnavailable);
  EXPECT_EQ(composition.error, "authentication service unavailable");
  EXPECT_EQ(composition.error.find(kCredential), std::string::npos);
}

TEST(AuthRuntimeCompositionTest, ExplicitCookieFileIsAuthoritative) {
  TempDirectory directory;
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_CONFIG_ROOT",
                  (directory.path() / "config").string());
  environment.Set("MOCKTAIL_COOKIE_FILE",
                  (directory.path() / "missing-cookie").string());
  environment.Set("MOCKTAIL_ALLOW_NO_COOKIE_LUA_APP", "1");
  const RuntimePaths paths = PathsFor(environment, directory);
  ASSERT_TRUE(WriteFile(paths.cookie_file(),
                        std::string(".ROBLOSECURITY=") + kCredential));
  FakeHttpClient http;
  services::AuthService auth_service(http);

  const AuthRuntimeComposition composition =
      ComposeAuthRuntime(environment, paths, auth_service);

  EXPECT_FALSE(composition);
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kUnavailable);
  EXPECT_EQ(composition.error, "configured Roblox cookie file is unavailable");
  EXPECT_EQ(http.request_count, 0);
}

TEST(AuthRuntimeCompositionTest, RejectsCookieFileWithSharedPermissions) {
  TempDirectory directory;
  const std::filesystem::path cookie_file = directory.path() / "cookie";
  ASSERT_TRUE(
      WriteFile(cookie_file, std::string(".ROBLOSECURITY=") + kCredential));
  ASSERT_EQ(chmod(cookie_file.c_str(), S_IRUSR | S_IWUSR | S_IRGRP), 0);
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_COOKIE_FILE", cookie_file.string());
  FakeHttpClient http;
  services::AuthService auth_service(http);

  const AuthRuntimeComposition composition = ComposeAuthRuntime(
      environment, PathsFor(environment, directory), auth_service);

  EXPECT_FALSE(composition);
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kUnavailable);
  EXPECT_EQ(composition.error,
            "Roblox cookie file permissions are not private");
  EXPECT_EQ(http.request_count, 0);
}

TEST(AuthRuntimeCompositionTest, RejectsSymlinkCookieSource) {
  TempDirectory directory;
  const std::filesystem::path target = directory.path() / "cookie-target";
  const std::filesystem::path symlink = directory.path() / "cookie-link";
  ASSERT_TRUE(WriteFile(target, std::string(".ROBLOSECURITY=") + kCredential));
  std::error_code error;
  std::filesystem::create_symlink(target, symlink, error);
  ASSERT_FALSE(error);
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_COOKIE_FILE", symlink.string());
  FakeHttpClient http;
  services::AuthService auth_service(http);

  const AuthRuntimeComposition composition = ComposeAuthRuntime(
      environment, PathsFor(environment, directory), auth_service);

  EXPECT_FALSE(composition);
  EXPECT_EQ(composition.status, AuthRuntimeStatus::kUnavailable);
  EXPECT_EQ(composition.error, "configured Roblox cookie file is unavailable");
  EXPECT_EQ(http.request_count, 0);
}

TEST(AuthRuntimeCompositionTest,
     FileReplacementCannotChangeCredentialOrIdentityDependency) {
  constexpr char kCredentialA[] = "_|validated-credential-a";
  constexpr char kCredentialB[] = "_|replacement-credential-b";
  TempDirectory directory;
  const std::filesystem::path cookie_file = directory.path() / "cookie";
  ASSERT_TRUE(
      WriteFile(cookie_file, std::string(".ROBLOSECURITY=") + kCredentialA));
  MapEnvironment environment;
  environment.Set("HOME", directory.path().string());
  environment.Set("MOCKTAIL_COOKIE_FILE", cookie_file.string());
  FakeHttpClient http;
  http.response = {
      true,
      200,
      R"({"id":777,"name":"ConsistentUser","displayName":"Consistent"})",
      {}};
  services::AuthService auth_service(http);
  AuthRuntimeComposition composition = ComposeAuthRuntime(
      environment, PathsFor(environment, directory), auth_service);
  ASSERT_TRUE(composition);

  ASSERT_TRUE(
      WriteFile(cookie_file, std::string(".ROBLOSECURITY=") + kCredentialB));
  mocktail::legacy::RuntimeDependencies dependencies(std::move(composition));

  const std::string expected_header =
      std::string(".ROBLOSECURITY=") + kCredentialA;
  ASSERT_EQ(http.request_count, 1);
  EXPECT_EQ(http.last_request.headers[1], "Cookie: " + expected_header);
  EXPECT_EQ(dependencies.roblox_credential().view(), expected_header);
  EXPECT_EQ(dependencies.account_identity().user_id, 777);
  EXPECT_EQ(dependencies.account_identity().username, "ConsistentUser");
  ASSERT_NE(dependencies.jni_vm(), nullptr);
  const jnivm::RobloxAuthIdentity vm_identity =
      dependencies.jni_vm()->GetRobloxAuthIdentitySnapshot();
  EXPECT_EQ(vm_identity.user_id, dependencies.account_identity().user_id);
  EXPECT_EQ(vm_identity.username, dependencies.account_identity().username);

  {
    const ScopedRobloxCredentialBinding credential_binding(
        dependencies.jni_vm().get(), dependencies.roblox_credential());
    ASSERT_TRUE(credential_binding.bound());
    JNIEnv* env = dependencies.jni_vm()->GetJNIEnv();
    jclass cookie_manager =
        env->FindClass("com/roblox/universalapp/cookie/JNICookieManager");
    jmethodID get_cookie = env->GetStaticMethodID(
        cookie_manager, "getCookie", "(Ljava/lang/String;)Ljava/lang/String;");
    auto java_cookie = static_cast<jstring>(env->CallStaticObjectMethod(
        cookie_manager, get_cookie, env->NewStringUTF("roblox.com")));
    EXPECT_EQ(ReadJavaString(env, java_cookie), expected_header);
  }
  EXPECT_FALSE(
      dependencies.jni_vm()->CopyRobloxCredentialFromProvider(nullptr));
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
