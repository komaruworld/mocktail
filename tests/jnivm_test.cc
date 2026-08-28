#include "jnivm/jnivm.h"

#include <gtest/gtest.h>

#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace jnivm {
namespace {

class JniVmTest : public ::testing::Test {
protected:
  void SetUp() override {
    unsetenv("MOCKTAIL_ROBLOX_COOKIES");
    unsetenv("MOCKTAIL_ROBLOX_USER_ID");
    unsetenv("MOCKTAIL_ROBLOX_USERNAME");
    unsetenv("MOCKTAIL_JNI_STRING_TRACE");
    unsetenv("MOCKTAIL_TRACE_ALL");
    unsetenv("MOCKTAIL_FULL_TRACE");
    unsetenv("MOCKTAIL_RESOLVED_THEME_INTERNAL");
    vm_ = std::make_shared<VM>();
  }

  void TearDown() override {
    unsetenv("MOCKTAIL_ROBLOX_COOKIES");
    unsetenv("MOCKTAIL_ROBLOX_USER_ID");
    unsetenv("MOCKTAIL_ROBLOX_USERNAME");
    unsetenv("MOCKTAIL_JNI_STRING_TRACE");
    unsetenv("MOCKTAIL_TRACE_ALL");
    unsetenv("MOCKTAIL_FULL_TRACE");
    unsetenv("MOCKTAIL_RESOLVED_THEME_INTERNAL");
  }

  std::shared_ptr<VM> vm_;
};

std::string ReadJavaString(JNIEnv *env, jstring value) {
  if (!value) {
    return {};
  }
  const char *chars = env->GetStringUTFChars(value, nullptr);
  if (!chars) {
    return {};
  }
  const std::string result(chars);
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

struct MemStorageCallbackProbe {
  std::mutex mutex;
  std::condition_variable condition;
  int calls = 0;
  bool block = false;
  bool entered = false;
  bool release = false;
  std::vector<std::string> values;
};

void ProbeMemStorageItemSet(void *context, JNIEnv *env, jstring value) {
  auto *probe = static_cast<MemStorageCallbackProbe *>(context);
  std::unique_lock<std::mutex> lock(probe->mutex);
  ++probe->calls;
  probe->values.push_back(ReadJavaString(env, value));
  probe->entered = true;
  probe->condition.notify_all();
  probe->condition.wait(lock,
                        [probe]() { return !probe->block || probe->release; });
}

struct DataModelNotificationProbe {
  int calls = 0;
  int app_bridge_calls = 0;
  int native_overlay_calls = 0;
  int open_web_activity_calls = 0;
  int cookie_sync_calls = 0;
  int cookie_set_calls = 0;
  std::string type;
  std::string data;
  std::string title;
  std::string url;
  std::string cookie;
  std::string cookie_set_url;
};

void ProbeDataModelNotification(void *context, JNIEnv *env, jstring type,
                                jstring data) {
  auto *probe = static_cast<DataModelNotificationProbe *>(context);
  ++probe->calls;
  probe->type = ReadJavaString(env, type);
  probe->data = ReadJavaString(env, data);
}

void ProbeAppBridgeNotification(void* context, JNIEnv* env, jstring type,
                                jstring data) {
  auto* probe = static_cast<DataModelNotificationProbe*>(context);
  ++probe->app_bridge_calls;
  probe->type = ReadJavaString(env, type);
  probe->data = ReadJavaString(env, data);
}

void ProbeNativeOverlay(void* context, JNIEnv* env, jstring title,
                        jstring url) {
  auto* probe = static_cast<DataModelNotificationProbe*>(context);
  ++probe->native_overlay_calls;
  probe->title = ReadJavaString(env, title);
  probe->url = ReadJavaString(env, url);
}

void ProbeOpenWebActivity(void* context, JNIEnv* env, jstring url,
                          jstring title) {
  auto* probe = static_cast<DataModelNotificationProbe*>(context);
  ++probe->open_web_activity_calls;
  probe->url = ReadJavaString(env, url);
  probe->title = ReadJavaString(env, title);
}

void ProbeCookieSync(void* context, JNIEnv* env, jstring cookie) {
  auto* probe = static_cast<DataModelNotificationProbe*>(context);
  ++probe->cookie_sync_calls;
  probe->cookie = ReadJavaString(env, cookie);
}

void ProbeCookieSet(void* context, JNIEnv* env, jstring cookie, jstring url) {
  auto* probe = static_cast<DataModelNotificationProbe*>(context);
  ++probe->cookie_set_calls;
  probe->cookie = ReadJavaString(env, cookie);
  probe->cookie_set_url = ReadJavaString(env, url);
}

struct CredentialSinkProbe {
  int calls = 0;
  std::string credential;
};

bool ProbeCredentialStore(void* context, const char* data, std::size_t size) {
  auto* probe = static_cast<CredentialSinkProbe*>(context);
  if (probe == nullptr || data == nullptr || size == 0) {
    return false;
  }
  ++probe->calls;
  probe->credential.assign(data, size);
  return true;
}

MemStorageCallbackCallbacks ProbeMemStorageCallbacks() {
  MemStorageCallbackCallbacks callbacks;
  callbacks.on_item_set = ProbeMemStorageItemSet;
  return callbacks;
}

RobloxCredentialView TestCredentialProvider(const void *context) {
  const auto *credential = static_cast<const std::string *>(context);
  return credential != nullptr
             ? RobloxCredentialView{credential->data(), credential->size()}
             : RobloxCredentialView{};
}

jstring QuickLoginCookieGetter(JNIEnv* env, jclass, jstring) {
  return env->NewStringUTF(
      ".ROBLOSECURITY=_|quick-login-cookie; RBXEventTracker=ignored");
}

struct FmodAudioDeviceProbe {
  int init_calls = 0;
  int write_calls = 0;
  int close_calls = 0;
  int shutdown_calls = 0;
  const void *last_identity = nullptr;
  int channels = 0;
  int sample_rate_hz = 0;
  int block_size_frames = 0;
  int block_count = 0;
  std::vector<std::uint8_t> bytes;
};

struct AndroidWindowProbe {
  int calls = 0;
  int flags = 0;
  int mask = 0;
};

bool ProbeAndroidWindowFlags(void* context, int flags, int mask) {
  auto* probe = static_cast<AndroidWindowProbe*>(context);
  ++probe->calls;
  probe->flags = flags;
  probe->mask = mask;
  return true;
}

bool ProbeFmodInit(void *context, const void *identity, int channels,
                   int sample_rate_hz, int block_size_frames, int block_count) {
  auto *probe = static_cast<FmodAudioDeviceProbe *>(context);
  ++probe->init_calls;
  probe->last_identity = identity;
  probe->channels = channels;
  probe->sample_rate_hz = sample_rate_hz;
  probe->block_size_frames = block_size_frames;
  probe->block_count = block_count;
  return true;
}

bool ProbeFmodWrite(void *context, const void *identity,
                    const std::uint8_t *data, std::size_t size) {
  auto *probe = static_cast<FmodAudioDeviceProbe *>(context);
  ++probe->write_calls;
  probe->last_identity = identity;
  probe->bytes.assign(data, data + size);
  return true;
}

bool ProbeFmodClose(void *context, const void *identity) {
  auto *probe = static_cast<FmodAudioDeviceProbe *>(context);
  ++probe->close_calls;
  probe->last_identity = identity;
  return true;
}

void ProbeFmodShutdown(void *context) {
  ++static_cast<FmodAudioDeviceProbe *>(context)->shutdown_calls;
}

FmodAudioDeviceCallbacks ProbeFmodCallbacks() {
  FmodAudioDeviceCallbacks callbacks;
  callbacks.init = ProbeFmodInit;
  callbacks.write = ProbeFmodWrite;
  callbacks.close = ProbeFmodClose;
  callbacks.shutdown = ProbeFmodShutdown;
  return callbacks;
}

struct RobloxTextInputProbe {
  std::mutex mutex;
  std::condition_variable condition;
  int show_calls = 0;
  int hide_calls = 0;
  int replace_calls = 0;
  int properties_calls = 0;
  int shutdown_calls = 0;
  bool block_show = false;
  bool show_entered = false;
  bool release_show = false;
  RobloxTextInputShowRequest request;
  std::string replacement;
};

void ProbeTextInputShow(void *context,
                        const RobloxTextInputShowRequest &request) {
  auto *probe = static_cast<RobloxTextInputProbe *>(context);
  std::unique_lock<std::mutex> lock(probe->mutex);
  ++probe->show_calls;
  probe->request = request;
  probe->show_entered = true;
  probe->condition.notify_all();
  probe->condition.wait(
      lock, [probe]() { return !probe->block_show || probe->release_show; });
}

void ProbeTextInputHide(void *context) {
  ++static_cast<RobloxTextInputProbe *>(context)->hide_calls;
}

void ProbeTextInputReplace(void *context, const std::string &text) {
  auto *probe = static_cast<RobloxTextInputProbe *>(context);
  ++probe->replace_calls;
  probe->replacement = text;
}

void ProbeTextInputPropertiesChanged(void *context) {
  ++static_cast<RobloxTextInputProbe *>(context)->properties_calls;
}

void ProbeTextInputShutdown(void *context) {
  ++static_cast<RobloxTextInputProbe *>(context)->shutdown_calls;
}

RobloxTextInputCallbacks ProbeTextInputCallbacks() {
  RobloxTextInputCallbacks callbacks;
  callbacks.show = ProbeTextInputShow;
  callbacks.hide = ProbeTextInputHide;
  callbacks.replace_text = ProbeTextInputReplace;
  callbacks.properties_changed = ProbeTextInputPropertiesChanged;
  callbacks.shutdown = ProbeTextInputShutdown;
  return callbacks;
}

jboolean CallBooleanMethodVForTest(JNIEnv *env, jobject obj,
                                   jmethodID method_id, ...) {
  va_list args;
  va_start(args, method_id);
  const jboolean result =
      env->functions->CallBooleanMethodV(env, obj, method_id, args);
  va_end(args);
  return result;
}

jobject CallObjectMethodVForTest(JNIEnv *env, jobject obj, jmethodID method_id,
                                 ...) {
  va_list args;
  va_start(args, method_id);
  jobject result = env->functions->CallObjectMethodV(env, obj, method_id, args);
  va_end(args);
  return result;
}

void CallVoidMethodVForTest(JNIEnv *env, jobject obj, jmethodID method_id,
                            ...) {
  va_list args;
  va_start(args, method_id);
  env->functions->CallVoidMethodV(env, obj, method_id, args);
  va_end(args);
}

jobject NewObjectVForTest(JNIEnv *env, jclass cls, jmethodID method_id, ...) {
  va_list args;
  va_start(args, method_id);
  jobject result = env->functions->NewObjectV(env, cls, method_id, args);
  va_end(args);
  return result;
}

void CallStaticVoidMethodVForTest(JNIEnv *env, jclass cls, jmethodID method_id,
                                  ...) {
  va_list args;
  va_start(args, method_id);
  env->functions->CallStaticVoidMethodV(env, cls, method_id, args);
  va_end(args);
}

jobject MakeTextBoxInfo(JNIEnv *env) {
  jclass cls = env->FindClass("com/roblox/engine/jni/model/NativeTextBoxInfo");
  const jmethodID constructor =
      env->GetMethodID(cls, "<init>", "(FFFFFZIIIIIIZZ)V");
  jobject info =
      env->NewObject(cls, constructor, 1.0, 2.0, 300.0, 40.0, 18.0, JNI_TRUE, 1,
                     2, 0x10203040, 4, 5, 6, JNI_TRUE, JNI_FALSE);
  const auto set_float = [env, cls, info](const char *name, jfloat value) {
    env->SetFloatField(info, env->GetFieldID(cls, name, "F"), value);
  };
  const auto set_int = [env, cls, info](const char *name, jint value) {
    env->SetIntField(info, env->GetFieldID(cls, name, "I"), value);
  };
  const auto set_bool = [env, cls, info](const char *name, jboolean value) {
    env->SetBooleanField(info, env->GetFieldID(cls, name, "Z"), value);
  };
  set_float("x", 1.0f);
  set_float("y", 2.0f);
  set_float("width", 300.0f);
  set_float("height", 40.0f);
  set_float("fontSize", 18.0f);
  set_bool("multiline", JNI_TRUE);
  set_int("xAlignment", 1);
  set_int("yAlignment", 2);
  set_int("textColor", 0x10203040);
  set_int("font", 4);
  set_int("textInputType", 5);
  set_int("returnKeyType", 6);
  set_bool("manualFocusRelease", JNI_TRUE);
  set_bool("textWrapped", JNI_FALSE);
  return info;
}

TEST_F(JniVmTest, JavaVmPointerIsNotNull) {
  EXPECT_NE(vm_->GetJavaVM(), nullptr);
}

TEST_F(JniVmTest, JniEnvPointerIsNotNull) {
  EXPECT_NE(vm_->GetJNIEnv(), nullptr);
}

TEST_F(JniVmTest, TraceAllNeverPrintsJStringContentOrPrefix) {
  constexpr char kCookieShapedSecret[] =
      ".ROBLOSECURITY=_|SECRET_CANARY_PREFIX_never_log_this_value";
  setenv("MOCKTAIL_TRACE_ALL", "1", 1);
  JNIEnv *env = vm_->GetJNIEnv();
  jstring value = env->NewStringUTF(kCookieShapedSecret);
  ASSERT_NE(value, nullptr);

  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  const char *chars = env->GetStringUTFChars(value, nullptr);
  const std::size_t returned_length = chars ? std::strlen(chars) : 0;
  env->ReleaseStringUTFChars(value, chars);
  const std::string stderr_output = testing::internal::GetCapturedStderr();
  const std::string stdout_output = testing::internal::GetCapturedStdout();
  const std::string trace_output = stdout_output + stderr_output;

  ASSERT_NE(chars, nullptr);
  EXPECT_EQ(returned_length, std::strlen(kCookieShapedSecret));
  EXPECT_NE(trace_output.find("GetStringUTFChars"), std::string::npos);
  EXPECT_NE(trace_output.find("len="), std::string::npos);
  EXPECT_EQ(trace_output.find(kCookieShapedSecret), std::string::npos);
  EXPECT_EQ(trace_output.find(".ROBLOSECURITY"), std::string::npos);
  EXPECT_EQ(trace_output.find("SECRET_CANARY_PREFIX"), std::string::npos);
}

TEST_F(JniVmTest, InitialClassCountIsZero) {
  EXPECT_EQ(vm_->GetClassCount(), 0u);
}

TEST_F(JniVmTest, RegisterClassReturnsNonNull) {
  auto cls = vm_->RegisterClass("android/content/Context");
  EXPECT_NE(cls, nullptr);
}

TEST_F(JniVmTest, RegisterClassPreservesName) {
  const std::string kName = "com/roblox/client/RobloxActivity";
  auto cls = vm_->RegisterClass(kName);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->GetName(), kName);
}

TEST_F(JniVmTest, RegisterClassIsDeduplicated) {
  auto cls1 = vm_->RegisterClass("rbx/JNIRobloxSettings");
  auto cls2 = vm_->RegisterClass("rbx/JNIRobloxSettings");
  EXPECT_EQ(cls1.get(), cls2.get());
}

TEST_F(JniVmTest, ClassCountIncrements) {
  vm_->RegisterClass("android/content/Context");
  vm_->RegisterClass("android/app/Activity");
  EXPECT_EQ(vm_->GetClassCount(), 2u);
}

TEST_F(JniVmTest, DuplicateRegistrationDoesNotInflateCount) {
  vm_->RegisterClass("android/content/Context");
  vm_->RegisterClass("android/content/Context");
  EXPECT_EQ(vm_->GetClassCount(), 1u);
}

TEST_F(JniVmTest, FindClassReturnsNullForUnknownClass) {
  EXPECT_EQ(vm_->FindClass("not/Registered"), nullptr);
}

TEST_F(JniVmTest, FindClassReturnsRegisteredInstance) {
  auto registered = vm_->RegisterClass("android/os/Bundle");
  auto found = vm_->FindClass("android/os/Bundle");
  EXPECT_EQ(registered.get(), found.get());
}

TEST_F(JniVmTest, RegisteredMethodIsFound) {
  auto cls = vm_->RegisterClass("rbx/JNIRobloxSettings");
  cls->RegisterMethod("nativeInitClientSettings", "()V",
                      [](JNIEnv *, jobject) {});
  EXPECT_NE(cls->FindMethod("nativeInitClientSettings", "()V"), nullptr);
}

TEST_F(JniVmTest, MethodLookupWithWrongSignatureReturnsNull) {
  auto cls = vm_->RegisterClass("rbx/JNIRobloxSettings");
  cls->RegisterMethod("nativeInitClientSettings", "()V",
                      [](JNIEnv *, jobject) {});
  EXPECT_EQ(cls->FindMethod("nativeInitClientSettings", "(I)V"), nullptr);
}

TEST_F(JniVmTest, LookupOfNonExistentMethodReturnsNull) {
  auto cls = vm_->RegisterClass("android/content/Context");
  EXPECT_EQ(cls->FindMethod("getSystemService",
                            "(Ljava/lang/String;)Ljava/lang/Object;"),
            nullptr);
}

TEST_F(JniVmTest, MethodCallbackIsInvoked) {
  auto cls = vm_->RegisterClass("rbx/JNIRobloxSettings");
  bool invoked = false;
  cls->RegisterMethod("nativeInitClientSettings", "()V",
                      [&invoked](JNIEnv *, jobject) { invoked = true; });

  const MethodCallback *cb = cls->FindMethod("nativeInitClientSettings", "()V");
  ASSERT_NE(cb, nullptr);
  (*cb)(vm_->GetJNIEnv(), nullptr);

  EXPECT_TRUE(invoked);
}

TEST_F(JniVmTest, CallbackReceivesCorrectJniEnv) {
  auto cls = vm_->RegisterClass("android/content/Context");
  JNIEnv *received_env = nullptr;
  cls->RegisterMethod("onCreate", "()V",
                      [&](JNIEnv *env, jobject) { received_env = env; });

  const MethodCallback *cb = cls->FindMethod("onCreate", "()V");
  ASSERT_NE(cb, nullptr);
  (*cb)(vm_->GetJNIEnv(), nullptr);

  EXPECT_EQ(received_env, vm_->GetJNIEnv());
}

TEST_F(JniVmTest, NetworkUtilsReturnsMockIpv4Address) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass cls = env->FindClass("com/roblox/engine/jni/util/NetworkUtils");
  ASSERT_NE(cls, nullptr);

  jmethodID method = env->GetStaticMethodID(cls, "getPublicIPv4Addresseses",
                                            "()Ljava/lang/String;");
  ASSERT_NE(method, nullptr);

  auto result = static_cast<jstring>(env->CallStaticObjectMethod(cls, method));
  ASSERT_NE(result, nullptr);

  const char *chars = env->GetStringUTFChars(result, nullptr);
  ASSERT_NE(chars, nullptr);
  EXPECT_STREQ(chars, "127.0.0.1");
  env->ReleaseStringUTFChars(result, chars);
}

TEST_F(JniVmTest, JniCookieManagerReturnsConfiguredCookie) {
  setenv("MOCKTAIL_ROBLOX_COOKIES", ".ROBLOSECURITY=test-cookie-value", 1);
  JNIEnv *env = vm_->GetJNIEnv();
  jclass cls =
      env->FindClass("com/roblox/universalapp/cookie/JNICookieManager");
  ASSERT_NE(cls, nullptr);

  jmethodID method = env->GetStaticMethodID(
      cls, "getCookie", "(Ljava/lang/String;)Ljava/lang/String;");
  ASSERT_NE(method, nullptr);

  jstring domain = env->NewStringUTF("roblox.com");
  auto result =
      static_cast<jstring>(env->CallStaticObjectMethod(cls, method, domain));
  ASSERT_NE(result, nullptr);

  const char *chars = env->GetStringUTFChars(result, nullptr);
  ASSERT_NE(chars, nullptr);
  EXPECT_STREQ(chars, ".ROBLOSECURITY=test-cookie-value");
  env->ReleaseStringUTFChars(result, chars);
}

TEST_F(JniVmTest, NativeSettingsCookieNetscapeFormatUsesConfiguredCookie) {
  setenv("MOCKTAIL_ROBLOX_COOKIES", ".ROBLOSECURITY=test-cookie-value", 1);
  JNIEnv *env = vm_->GetJNIEnv();
  jclass cls = env->FindClass("com/roblox/engine/jni/NativeSettingsInterface");
  ASSERT_NE(cls, nullptr);

  jmethodID method =
      env->GetStaticMethodID(cls, "nativeGetCookiesInNetscapeFormat",
                             "(Ljava/lang/String;)Ljava/lang/String;");
  ASSERT_NE(method, nullptr);

  jstring domain = env->NewStringUTF("https://www.roblox.com/");
  auto result =
      static_cast<jstring>(env->CallStaticObjectMethod(cls, method, domain));
  ASSERT_NE(result, nullptr);

  const char *chars = env->GetStringUTFChars(result, nullptr);
  ASSERT_NE(chars, nullptr);
  EXPECT_STREQ(chars,
               ".www.roblox.com\tTRUE\t/\tTRUE\t2147483647\t"
               ".ROBLOSECURITY\ttest-cookie-value");
  env->ReleaseStringUTFChars(result, chars);
}

TEST_F(JniVmTest, TypedCredentialProviderOverridesFallbackAndJavaSetter) {
  setenv("MOCKTAIL_ROBLOX_COOKIES", ".ROBLOSECURITY=_|untrusted-fallback-b", 1);
  std::string credential = ".ROBLOSECURITY=_|typed-credential-a";
  vm_->SetRobloxCredentialProvider(&credential, &TestCredentialProvider);
  JNIEnv *env = vm_->GetJNIEnv();
  jclass cls =
      env->FindClass("com/roblox/universalapp/cookie/JNICookieManager");
  ASSERT_NE(cls, nullptr);
  jmethodID get_cookie = env->GetStaticMethodID(
      cls, "getCookie", "(Ljava/lang/String;)Ljava/lang/String;");
  ASSERT_NE(get_cookie, nullptr);
  jstring domain = env->NewStringUTF("roblox.com");

  auto first = static_cast<jstring>(
      env->CallStaticObjectMethod(cls, get_cookie, domain));
  EXPECT_EQ(ReadJavaString(env, first), credential);

  setenv("MOCKTAIL_ROBLOX_COOKIES", ".ROBLOSECURITY=_|replacement-fallback-c",
         1);
  jobject manager = env->AllocObject(cls);
  jmethodID set_cookie = env->GetMethodID(
      cls, "setCookie", "(Ljava/lang/String;Ljava/lang/String;)V");
  ASSERT_NE(set_cookie, nullptr);
  env->CallVoidMethod(
      manager, set_cookie, domain,
      env->NewStringUTF(".ROBLOSECURITY=_|java-setter-replacement-d"));
  auto second = static_cast<jstring>(
      env->CallStaticObjectMethod(cls, get_cookie, domain));
  EXPECT_EQ(ReadJavaString(env, second), credential);
  vm_->ClearRobloxCredentialProvider();
}

TEST_F(JniVmTest, EmptyTypedCredentialBlocksFallbackForGuest) {
  setenv("MOCKTAIL_ROBLOX_COOKIES", ".ROBLOSECURITY=_|untrusted-fallback", 1);
  const std::string credential;
  vm_->SetRobloxCredentialProvider(&credential, &TestCredentialProvider);
  JNIEnv *env = vm_->GetJNIEnv();
  jclass cls =
      env->FindClass("com/roblox/universalapp/cookie/JNICookieManager");
  jmethodID get_cookie = env->GetStaticMethodID(
      cls, "getCookie", "(Ljava/lang/String;)Ljava/lang/String;");
  auto result = static_cast<jstring>(env->CallStaticObjectMethod(
      cls, get_cookie, env->NewStringUTF("roblox.com")));
  EXPECT_TRUE(ReadJavaString(env, result).empty());

  jmethodID load_from_disk =
      env->GetStaticMethodID(cls, "setCookiesFromDisk", "()Z");
  EXPECT_EQ(env->CallStaticBooleanMethod(cls, load_from_disk), JNI_FALSE);
  vm_->ClearRobloxCredentialProvider();
}

TEST_F(JniVmTest, PersistedNativeCredentialSupersedesGuestProvider) {
  const std::string guest;
  vm_->SetRobloxCredentialProvider(&guest, &TestCredentialProvider);
  auto credential_probe = std::make_shared<CredentialSinkProbe>();
  vm_->SetRobloxCredentialSink(
      credential_probe, RobloxCredentialSinkCallbacks{&ProbeCredentialStore});
  const std::string native_credential =
      ".ROBLOSECURITY=_|persisted-native-credential";

  ASSERT_TRUE(vm_->DispatchRobloxCredential(native_credential.data(),
                                            native_credential.size()));
  std::string current;
  ASSERT_TRUE(vm_->CopyRobloxCredentialFromProvider(&current));
  EXPECT_EQ(current, native_credential);
  EXPECT_EQ(credential_probe->calls, 1);
  EXPECT_EQ(credential_probe->credential, native_credential);

  vm_->ClearRobloxCredentialProvider();
  EXPECT_FALSE(vm_->CopyRobloxCredentialFromProvider(&current));
  EXPECT_TRUE(current.empty());
}

TEST_F(JniVmTest, NativeUserJavaInterfaceReturnsInjectedIdentityExactly) {
  RobloxAuthIdentity identity;
  identity.user_id = 123456789;
  identity.username = "AccountName";
  identity.display_name = "Display Name";
  vm_->SetRobloxAuthIdentity(identity);

  // Injected identity takes precedence over environment fallbacks.
  setenv("MOCKTAIL_ROBLOX_USER_ID", "77", 1);
  setenv("MOCKTAIL_ROBLOX_USERNAME", "EnvironmentName", 1);
  setenv("MOCKTAIL_RESOLVED_THEME_INTERNAL", "Light", 1);

  JNIEnv *env = vm_->GetJNIEnv();
  jclass cls =
      env->FindClass("com/roblox/engine/jni/user/NativeUserJavaInterface");
  ASSERT_NE(cls, nullptr);

  jmethodID get_user_id = env->GetStaticMethodID(cls, "getUserId", "()J");
  ASSERT_NE(get_user_id, nullptr);
  EXPECT_EQ(env->CallStaticLongMethod(cls, get_user_id), identity.user_id);

  const auto ReadIdentityMethod = [env, cls](const char *method_name) {
    jmethodID method =
        env->GetStaticMethodID(cls, method_name, "()Ljava/lang/String;");
    EXPECT_NE(method, nullptr);
    return ReadJavaString(
        env, static_cast<jstring>(env->CallStaticObjectMethod(cls, method)));
  };
  EXPECT_EQ(ReadIdentityMethod("getUsername"), identity.username);
  EXPECT_EQ(ReadIdentityMethod("getDisplayName"), identity.display_name);
  EXPECT_EQ(ReadIdentityMethod("getAlternateName"), identity.display_name);
  EXPECT_EQ(ReadIdentityMethod("getLastLoggedInUser"), identity.username);
  EXPECT_EQ(ReadIdentityMethod("getLastLoggedInUserId"), "123456789");
  EXPECT_EQ(ReadIdentityMethod("getTheme"), "Light");

  jclass system_theme =
      env->FindClass("com/roblox/universalapp/systemtheme/SystemThemeProtocol");
  ASSERT_NE(system_theme, nullptr);
  jmethodID is_available =
      env->GetStaticMethodID(system_theme, "isSystemThemeAvailable", "()Z");
  jmethodID get_system_theme =
      env->GetStaticMethodID(system_theme, "getSystemTheme", "()I");
  EXPECT_EQ(env->CallStaticBooleanMethod(system_theme, is_available), JNI_TRUE);
  EXPECT_EQ(env->CallStaticIntMethod(system_theme, get_system_theme), 3);
}

TEST_F(JniVmTest, RobloxAuthIdentitySnapshotIsIndependentAndClearable) {
  RobloxAuthIdentity source;
  source.user_id = 42;
  source.username = "OriginalUser";
  source.display_name = "Original Display";
  vm_->SetRobloxAuthIdentity(source);

  source.username = "ChangedAfterSet";
  RobloxAuthIdentity snapshot = vm_->GetRobloxAuthIdentitySnapshot();
  EXPECT_EQ(snapshot.user_id, 42);
  EXPECT_EQ(snapshot.username, "OriginalUser");
  EXPECT_EQ(snapshot.display_name, "Original Display");

  snapshot.display_name = "ChangedSnapshot";
  EXPECT_EQ(vm_->GetRobloxAuthIdentitySnapshot().display_name,
            "Original Display");

  vm_->ClearRobloxAuthIdentity();
  const RobloxAuthIdentity cleared = vm_->GetRobloxAuthIdentitySnapshot();
  EXPECT_EQ(cleared.user_id, -1);
  EXPECT_TRUE(cleared.username.empty());
  EXPECT_TRUE(cleared.display_name.empty());
}

TEST_F(JniVmTest, InvalidInjectedIdentityNormalizesToUnresolved) {
  RobloxAuthIdentity identity;
  identity.user_id = 0;
  identity.username = "MustNotLeak";
  identity.display_name = "Must Not Leak";

  vm_->SetRobloxAuthIdentity(identity);
  const RobloxAuthIdentity snapshot = vm_->GetRobloxAuthIdentitySnapshot();

  EXPECT_EQ(snapshot.user_id, -1);
  EXPECT_TRUE(snapshot.username.empty());
  EXPECT_TRUE(snapshot.display_name.empty());
}

TEST_F(JniVmTest, NativeUserJavaInterfaceSupportsExplicitResearchFallback) {
  setenv("MOCKTAIL_ROBLOX_USER_ID", "123456789", 1);
  setenv("MOCKTAIL_ROBLOX_USERNAME", "ResearchUser", 1);
  JNIEnv *env = vm_->GetJNIEnv();
  jclass cls =
      env->FindClass("com/roblox/engine/jni/user/NativeUserJavaInterface");
  ASSERT_NE(cls, nullptr);

  jmethodID method = env->GetStaticMethodID(cls, "getUserId", "()J");
  ASSERT_NE(method, nullptr);

  EXPECT_EQ(env->CallStaticLongMethod(cls, method), 123456789);

  jmethodID get_username =
      env->GetStaticMethodID(cls, "getUsername", "()Ljava/lang/String;");
  ASSERT_NE(get_username, nullptr);
  EXPECT_EQ(
      ReadJavaString(env, static_cast<jstring>(
                              env->CallStaticObjectMethod(cls, get_username))),
      "ResearchUser");
}

TEST_F(JniVmTest, CookieAloneDoesNotPopulateOrAuthenticateIdentity) {
  setenv("MOCKTAIL_ROBLOX_COOKIES", ".ROBLOSECURITY=test-cookie-value", 1);
  JNIEnv *env = vm_->GetJNIEnv();
  jclass cls =
      env->FindClass("com/roblox/engine/jni/user/NativeUserJavaInterface");
  ASSERT_NE(cls, nullptr);

  jmethodID method = env->GetStaticMethodID(cls, "getUserId", "()J");
  ASSERT_NE(method, nullptr);

  EXPECT_EQ(env->CallStaticLongMethod(cls, method), -1);

  for (const char *method_name : {"getUsername", "getDisplayName",
                                  "getAlternateName", "getLastLoggedInUser"}) {
    SCOPED_TRACE(method_name);
    jmethodID string_method =
        env->GetStaticMethodID(cls, method_name, "()Ljava/lang/String;");
    ASSERT_NE(string_method, nullptr);
    EXPECT_TRUE(
        ReadJavaString(env, static_cast<jstring>(env->CallStaticObjectMethod(
                                cls, string_method)))
            .empty());
  }

  jmethodID last_user_id = env->GetStaticMethodID(cls, "getLastLoggedInUserId",
                                                  "()Ljava/lang/String;");
  ASSERT_NE(last_user_id, nullptr);
  EXPECT_EQ(
      ReadJavaString(env, static_cast<jstring>(
                              env->CallStaticObjectMethod(cls, last_user_id))),
      "-1");

  const RobloxAuthIdentity snapshot = vm_->GetRobloxAuthIdentitySnapshot();
  EXPECT_EQ(snapshot.user_id, -1);
  EXPECT_TRUE(snapshot.username.empty());
  EXPECT_TRUE(snapshot.display_name.empty());
}

TEST_F(JniVmTest, VideoCodecsReturnsEmptyObjectArray) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass cls =
      env->FindClass("com/roblox/engine/jni/video/MediaCodecInfoUtils");
  ASSERT_NE(cls, nullptr);

  jmethodID method = env->GetStaticMethodID(
      cls, "getVideoCodecs",
      "()[Lcom/roblox/engine/jni/video/VideoCodecCapability;");
  ASSERT_NE(method, nullptr);

  auto result =
      static_cast<jobjectArray>(env->CallStaticObjectMethod(cls, method));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(env->GetArrayLength(result), 0);
}

TEST_F(JniVmTest, FloatArrayElementsAreMutable) {
  JNIEnv *env = vm_->GetJNIEnv();
  jfloatArray array = env->NewFloatArray(2);
  ASSERT_NE(array, nullptr);

  const jfloat values[] = {60.0f, 120.0f};
  env->SetFloatArrayRegion(array, 0, 2, values);

  EXPECT_EQ(env->GetArrayLength(array), 2);
  jboolean is_copy = JNI_TRUE;
  jfloat *elements = env->GetFloatArrayElements(array, &is_copy);
  ASSERT_NE(elements, nullptr);
  EXPECT_EQ(is_copy, JNI_FALSE);
  EXPECT_FLOAT_EQ(elements[0], 60.0f);
  EXPECT_FLOAT_EQ(elements[1], 120.0f);
  env->ReleaseFloatArrayElements(array, elements, 0);
}

TEST_F(JniVmTest, ObjectMethodCanReturnMockClassLoader) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass cls = env->FindClass("java/lang/Object");
  ASSERT_NE(cls, nullptr);
  jobject obj = env->AllocObject(cls);
  ASSERT_NE(obj, nullptr);

  jmethodID method =
      env->GetMethodID(cls, "getClassLoader", "()Ljava/lang/ClassLoader;");
  ASSERT_NE(method, nullptr);

  jobject loader = env->CallObjectMethod(obj, method);
  EXPECT_NE(loader, nullptr);
}

TEST_F(JniVmTest, AndroidContextReturnsCoreObjectGraph) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass context_class = env->FindClass("android/content/Context");
  ASSERT_NE(context_class, nullptr);
  jobject context = env->AllocObject(context_class);
  ASSERT_NE(context, nullptr);

  jmethodID get_assets = env->GetMethodID(
      context_class, "getAssets", "()Landroid/content/res/AssetManager;");
  jmethodID get_resources = env->GetMethodID(
      context_class, "getResources", "()Landroid/content/res/Resources;");
  jmethodID get_package_manager =
      env->GetMethodID(context_class, "getPackageManager",
                       "()Landroid/content/pm/PackageManager;");
  jmethodID get_application_info =
      env->GetMethodID(context_class, "getApplicationInfo",
                       "()Landroid/content/pm/ApplicationInfo;");

  EXPECT_NE(env->CallObjectMethod(context, get_assets), nullptr);
  EXPECT_NE(env->CallObjectMethod(context, get_resources), nullptr);
  EXPECT_NE(env->CallObjectMethod(context, get_package_manager), nullptr);

  jobject app_info = env->CallObjectMethod(context, get_application_info);
  ASSERT_NE(app_info, nullptr);
  jclass app_info_class = env->FindClass("android/content/pm/ApplicationInfo");
  jfieldID source_dir =
      env->GetFieldID(app_info_class, "sourceDir", "Ljava/lang/String;");
  auto source_dir_value =
      static_cast<jstring>(env->GetObjectField(app_info, source_dir));
  ASSERT_NE(source_dir_value, nullptr);
  EXPECT_STREQ(env->GetStringUTFChars(source_dir_value, nullptr),
               "rbx_bin/sober_apk/base.apk");
}

TEST_F(JniVmTest, AndroidContextReturnsWindowServiceAndSharedPreferences) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass context_class = env->FindClass("android/content/Context");
  ASSERT_NE(context_class, nullptr);
  jobject context = env->AllocObject(context_class);

  jmethodID get_system_service =
      env->GetMethodID(context_class, "getSystemService",
                       "(Ljava/lang/String;)Ljava/lang/Object;");
  jobject window_manager = env->CallObjectMethod(context, get_system_service,
                                                 env->NewStringUTF("window"));
  ASSERT_NE(window_manager, nullptr);

  jclass window_manager_class = env->GetObjectClass(window_manager);
  jmethodID get_default_display = env->GetMethodID(
      window_manager_class, "getDefaultDisplay", "()Landroid/view/Display;");
  EXPECT_NE(env->CallObjectMethod(window_manager, get_default_display),
            nullptr);

  jmethodID get_shared_preferences =
      env->GetMethodID(context_class, "getSharedPreferences",
                       "(Ljava/lang/String;I)"
                       "Landroid/content/SharedPreferences;");
  jobject preferences = env->CallObjectMethod(context, get_shared_preferences,
                                              env->NewStringUTF("mocktail"), 0);
  ASSERT_NE(preferences, nullptr);

  jclass preferences_class = env->GetObjectClass(preferences);
  jmethodID get_string =
      env->GetMethodID(preferences_class, "getString",
                       "(Ljava/lang/String;Ljava/lang/String;)"
                       "Ljava/lang/String;");
  auto value = static_cast<jstring>(env->CallObjectMethod(
      preferences, get_string, env->NewStringUTF("missing"),
      env->NewStringUTF("fallback")));
  ASSERT_NE(value, nullptr);
  EXPECT_STREQ(env->GetStringUTFChars(value, nullptr), "fallback");
}

TEST_F(JniVmTest, NewStringUtfIsObjectFieldCompatible) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass params_class =
      env->FindClass("com/roblox/client/startup/AppBridgeInitParams");
  ASSERT_NE(params_class, nullptr);
  jobject params = env->AllocObject(params_class);
  ASSERT_NE(params, nullptr);

  jstring asset_path = env->NewStringUTF("/tmp/mocktail/assets/content");
  ASSERT_NE(asset_path, nullptr);

  jfieldID field =
      env->GetFieldID(params_class, "assetPath", "Ljava/lang/String;");
  env->SetObjectField(params, field, asset_path);

  auto stored = static_cast<jstring>(env->GetObjectField(params, field));
  ASSERT_EQ(stored, asset_path);
  EXPECT_EQ(env->GetStringUTFLength(stored), 28);
  EXPECT_STREQ(env->GetStringUTFChars(stored, nullptr),
               "/tmp/mocktail/assets/content");

  char region[9] = {};
  env->GetStringUTFRegion(stored, 5, 8, region);
  EXPECT_STREQ(region, "mocktail");

  const jchar *chars = env->GetStringChars(stored, nullptr);
  ASSERT_NE(chars, nullptr);
  EXPECT_EQ(chars[0], static_cast<jchar>('/'));
}

TEST_F(JniVmTest, JniStringsPreserveUtf16SurrogatePairsAndModifiedUtf8) {
  JNIEnv *env = vm_->GetJNIEnv();
  const jchar unicode[] = {0xd83d, 0xde42, 0x00e9};
  jstring value = env->NewString(unicode, 3);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(env->GetStringLength(value), 3);

  jboolean utf16_is_copy = JNI_TRUE;
  const jchar *utf16 = env->GetStringChars(value, &utf16_is_copy);
  ASSERT_NE(utf16, nullptr);
  EXPECT_EQ(utf16_is_copy, JNI_FALSE);
  EXPECT_EQ(utf16[0], 0xd83d);
  EXPECT_EQ(utf16[1], 0xde42);
  EXPECT_EQ(utf16[2], 0x00e9);
  env->ReleaseStringChars(value, utf16);

  const std::string expected_modified_utf8("\xed\xa0\xbd\xed\xb9\x82\xc3\xa9",
                                           8);
  EXPECT_EQ(env->GetStringUTFLength(value), 8);
  jboolean utf8_is_copy = JNI_TRUE;
  const char *modified_utf8 = env->GetStringUTFChars(value, &utf8_is_copy);
  ASSERT_NE(modified_utf8, nullptr);
  EXPECT_EQ(utf8_is_copy, JNI_FALSE);
  EXPECT_EQ(std::string(modified_utf8, 8), expected_modified_utf8);
  env->ReleaseStringUTFChars(value, modified_utf8);

  char region[9] = {};
  env->GetStringUTFRegion(value, 0, 3, region);
  EXPECT_EQ(std::string(region, 8), expected_modified_utf8);
  jchar utf16_region[3] = {};
  env->GetStringRegion(value, 0, 3, utf16_region);
  EXPECT_EQ(std::vector<jchar>(utf16_region, utf16_region + 3),
            std::vector<jchar>(unicode, unicode + 3));
}

TEST_F(JniVmTest, NewStringUtfAcceptsUnicodeAndUsesJniModifiedUtf8) {
  JNIEnv *env = vm_->GetJNIEnv();
  const char standard_utf8[] = "\xf0\x9f\x99\x82\xc3\xa9";
  jstring value = env->NewStringUTF(standard_utf8);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(env->GetStringLength(value), 3);
  EXPECT_EQ(env->GetStringUTFLength(value), 8);
  const char *modified_utf8 = env->GetStringUTFChars(value, nullptr);
  ASSERT_NE(modified_utf8, nullptr);
  EXPECT_EQ(std::string(modified_utf8, 8),
            std::string("\xed\xa0\xbd\xed\xb9\x82\xc3\xa9", 8));
  env->ReleaseStringUTFChars(value, modified_utf8);
}

TEST_F(JniVmTest, JniStringsPreserveEmbeddedNullAsModifiedUtf8) {
  JNIEnv *env = vm_->GetJNIEnv();
  const jchar unicode[] = {'A', 0, 'B'};
  jstring value = env->NewString(unicode, 3);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(env->GetStringLength(value), 3);
  EXPECT_EQ(env->GetStringUTFLength(value), 4);
  const char *modified_utf8 = env->GetStringUTFChars(value, nullptr);
  ASSERT_NE(modified_utf8, nullptr);
  EXPECT_EQ(std::string(modified_utf8, 4), std::string("A\xc0\x80"
                                                       "B",
                                                       4));
  const jchar *utf16 = env->GetStringChars(value, nullptr);
  ASSERT_NE(utf16, nullptr);
  EXPECT_EQ(utf16[0], 'A');
  EXPECT_EQ(utf16[1], 0);
  EXPECT_EQ(utf16[2], 'B');
}

TEST_F(JniVmTest, JniStringsPreserveUnpairedSurrogatesDeterministically) {
  JNIEnv *env = vm_->GetJNIEnv();
  const jchar unicode[] = {0xd83d, 'x', 0xdc00};
  jstring value = env->NewString(unicode, 3);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(env->GetStringLength(value), 3);
  const jchar *utf16 = env->GetStringChars(value, nullptr);
  ASSERT_NE(utf16, nullptr);
  EXPECT_EQ(std::vector<jchar>(utf16, utf16 + 3),
            std::vector<jchar>(unicode, unicode + 3));
  EXPECT_EQ(env->GetStringUTFLength(value), 7);
  const char *modified_utf8 = env->GetStringUTFChars(value, nullptr);
  EXPECT_EQ(std::string(modified_utf8, 7),
            std::string("\xed\xa0\xbdx\xed\xb0\x80", 7));

  jstring from_modified = env->NewStringUTF("\xed\xa0\xbd");
  EXPECT_EQ(env->GetStringLength(from_modified), 1);
  const jchar *decoded = env->GetStringChars(from_modified, nullptr);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded[0], 0xd83d);
}

TEST_F(JniVmTest, JavaStringGetBytesReturnsUtf8AcrossObjectCallForms) {
  JNIEnv *env = vm_->GetJNIEnv();
  const jchar unicode[] = {'A', 0, 0x00e9, 0xd83d, 0xde42, 0xd83d, 'x', 0xdc00};
  jstring value = env->NewString(unicode, 8);
  ASSERT_NE(value, nullptr);
  jclass string_class = env->GetObjectClass(value);
  ASSERT_NE(string_class, nullptr);
  jmethodID get_bytes =
      env->GetMethodID(string_class, "getBytes", "(Ljava/lang/String;)[B");
  ASSERT_NE(get_bytes, nullptr);

  const std::vector<std::uint8_t> expected = {
      0x41, 0x00, 0xc3, 0xa9, 0xf0, 0x9f, 0x99, 0x82, 0x3f, 0x78, 0x3f};
  const auto read_bytes = [env](jbyteArray array) {
    const jsize size = env->GetArrayLength(array);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0) {
      env->GetByteArrayRegion(array, 0, size,
                              reinterpret_cast<jbyte *>(bytes.data()));
    }
    return bytes;
  };

  auto direct = static_cast<jbyteArray>(
      env->CallObjectMethod(value, get_bytes, env->NewStringUTF("UTF-8")));
  ASSERT_NE(direct, nullptr);
  EXPECT_EQ(read_bytes(direct), expected);
  jboolean is_copy = JNI_TRUE;
  jbyte *direct_elements = env->GetByteArrayElements(direct, &is_copy);
  ASSERT_NE(direct_elements, nullptr);
  EXPECT_EQ(is_copy, JNI_FALSE);
  EXPECT_EQ(
      std::vector<std::uint8_t>(
          reinterpret_cast<std::uint8_t *>(direct_elements),
          reinterpret_cast<std::uint8_t *>(direct_elements) + expected.size()),
      expected);
  env->ReleaseByteArrayElements(direct, direct_elements, JNI_ABORT);

  auto v_result = static_cast<jbyteArray>(CallObjectMethodVForTest(
      env, value, get_bytes, env->NewStringUTF("utf8")));
  ASSERT_NE(v_result, nullptr);
  EXPECT_EQ(read_bytes(v_result), expected);

  jvalue args[1] = {};
  args[0].l = env->NewStringUTF("unicode-1-1-utf-8");
  auto a_result =
      static_cast<jbyteArray>(env->CallObjectMethodA(value, get_bytes, args));
  ASSERT_NE(a_result, nullptr);
  EXPECT_EQ(read_bytes(a_result), expected);
}

TEST_F(JniVmTest, JavaStringGetBytesRejectsUnsupportedOrMismatchedCalls) {
  JNIEnv *env = vm_->GetJNIEnv();
  jstring value = env->NewStringUTF("text");
  jclass string_class = env->GetObjectClass(value);
  jmethodID get_bytes =
      env->GetMethodID(string_class, "getBytes", "(Ljava/lang/String;)[B");

  jvalue unsupported[1] = {};
  unsupported[0].l = env->NewStringUTF("ISO-8859-1");
  EXPECT_EQ(env->CallObjectMethodA(value, get_bytes, unsupported), nullptr);
  EXPECT_EQ(env->CallObjectMethodA(value, get_bytes, nullptr), nullptr);

  jmethodID wrong_signature =
      env->GetMethodID(string_class, "getBytes", "()[B");
  EXPECT_EQ(env->CallObjectMethod(value, wrong_signature), nullptr);

  jclass object_class = env->FindClass("java/lang/Object");
  jobject object = env->AllocObject(object_class);
  EXPECT_EQ(
      env->CallObjectMethod(object, get_bytes, env->NewStringUTF("UTF-8")),
      nullptr);
}

TEST_F(JniVmTest, RobloxActivityReturnsStableSurfaceGraph) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass activity_class = env->FindClass("com/roblox/client/RobloxActivity");
  ASSERT_NE(activity_class, nullptr);
  jobject activity = env->AllocObject(activity_class);
  ASSERT_NE(activity, nullptr);

  jmethodID get_window =
      env->GetMethodID(activity_class, "getWindow", "()Landroid/view/Window;");
  jobject window = env->CallObjectMethod(activity, get_window);
  ASSERT_NE(window, nullptr);

  jclass window_class = env->GetObjectClass(window);
  jmethodID get_decor_view =
      env->GetMethodID(window_class, "getDecorView", "()Landroid/view/View;");
  jobject view = env->CallObjectMethod(window, get_decor_view);
  ASSERT_NE(view, nullptr);

  jclass view_class = env->GetObjectClass(view);
  jmethodID get_holder = env->GetMethodID(view_class, "getHolder",
                                          "()Landroid/view/SurfaceHolder;");
  jobject holder = env->CallObjectMethod(view, get_holder);
  ASSERT_NE(holder, nullptr);

  jclass holder_class = env->GetObjectClass(holder);
  jmethodID get_surface =
      env->GetMethodID(holder_class, "getSurface", "()Landroid/view/Surface;");
  jobject surface = env->CallObjectMethod(holder, get_surface);
  ASSERT_NE(surface, nullptr);

  jclass surface_class = env->GetObjectClass(surface);
  jmethodID is_valid = env->GetMethodID(surface_class, "isValid", "()Z");
  EXPECT_EQ(env->CallBooleanMethod(surface, is_valid), JNI_TRUE);
}

TEST_F(JniVmTest, NativeGlJavaInterfaceReturnsDeviceStaticParams) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass gl_class =
      env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface");
  ASSERT_NE(gl_class, nullptr);

  jmethodID get_device_static_params = env->GetStaticMethodID(
      gl_class, "getDeviceStaticParams",
      "()Lcom/roblox/engine/jni/model/DeviceStaticParams;");
  ASSERT_NE(get_device_static_params, nullptr);

  jobject params =
      env->CallStaticObjectMethod(gl_class, get_device_static_params);
  ASSERT_NE(params, nullptr);

  jclass params_class =
      env->FindClass("com/roblox/engine/jni/model/DeviceStaticParams");
  jfieldID manufacturer =
      env->GetFieldID(params_class, "manufacturer", "Ljava/lang/String;");
  auto value = static_cast<jstring>(env->GetObjectField(params, manufacturer));
  ASSERT_NE(value, nullptr);
  EXPECT_STREQ(env->GetStringUTFChars(value, nullptr), "Mocktail");

  jfieldID os_version =
      env->GetFieldID(params_class, "osVersion", "Ljava/lang/String;");
  auto os_version_value =
      static_cast<jstring>(env->GetObjectField(params, os_version));
  ASSERT_NE(os_version_value, nullptr);
  EXPECT_STREQ(env->GetStringUTFChars(os_version_value, nullptr), "Android 13");

  jfieldID cpu64_bit = env->GetFieldID(params_class, "cpu64Bit", "Z");
  EXPECT_EQ(env->GetBooleanField(params, cpu64_bit), JNI_TRUE);
}

TEST_F(JniVmTest, DeviceStaticParamsUseTypedPlatformIdentity) {
  PlatformIdentity pixel;
  pixel.touch_enabled = true;
  pixel.mouse_enabled = false;
  pixel.keyboard_enabled = false;
  pixel.pc_hardware = false;
  pixel.platform_name = "Android";
  pixel.device_name = "Google Pixel 7";
  pixel.manufacturer = "Google";
  pixel.model = "Pixel 7";
  pixel.brand = "google";
  pixel.device_code = "panther";
  pixel.device_sku = "panther";
  pixel.soc_model = "Google Tensor G2";
  vm_->SetPlatformIdentity(pixel);

  JNIEnv* env = vm_->GetJNIEnv();
  jclass gl_class =
      env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface");
  jmethodID get_device_static_params = env->GetStaticMethodID(
      gl_class, "getDeviceStaticParams",
      "()Lcom/roblox/engine/jni/model/DeviceStaticParams;");
  jobject params =
      env->CallStaticObjectMethod(gl_class, get_device_static_params);
  ASSERT_NE(params, nullptr);
  jclass params_class = env->GetObjectClass(params);
  const auto read_string = [env, params, params_class](const char* name) {
    jfieldID field = env->GetFieldID(params_class, name, "Ljava/lang/String;");
    return ReadJavaString(
        env, static_cast<jstring>(env->GetObjectField(params, field)));
  };

  EXPECT_EQ(read_string("deviceName"), "Google Pixel 7");
  EXPECT_EQ(read_string("manufacturer"), "Google");
  EXPECT_EQ(read_string("model"), "Pixel 7");
  EXPECT_EQ(read_string("brand"), "google");
  EXPECT_EQ(read_string("device"), "panther");
  EXPECT_EQ(read_string("deviceSku"), "panther");
  EXPECT_EQ(read_string("socModel"), "Google Tensor G2");
  EXPECT_EQ(read_string("osVersion"), "Android 13");

  jclass build_info = env->FindClass("org/webrtc/voiceengine/BuildInfo");
  jmethodID get_device_model = env->GetStaticMethodID(
      build_info, "getDeviceModel", "()Ljava/lang/String;");
  EXPECT_EQ(
      ReadJavaString(env, static_cast<jstring>(env->CallStaticObjectMethod(
                              build_info, get_device_model))),
      "Pixel 7");
}

TEST_F(JniVmTest, NativeGlJavaInterfaceReturnsDefaultEngineCallback) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass gl_class =
      env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface");
  ASSERT_NE(gl_class, nullptr);

  jmethodID get_implementation =
      env->GetStaticMethodID(gl_class, "getImplementation",
                             "()Lcom/roblox/engine/jni/EngineJavaCallback2;");
  ASSERT_NE(get_implementation, nullptr);

  jobject implementation =
      env->CallStaticObjectMethod(gl_class, get_implementation);
  ASSERT_NE(implementation, nullptr);

  jfieldID implementation_field =
      env->GetStaticFieldID(gl_class, "sImplementation",
                            "Lcom/roblox/engine/jni/EngineJavaCallback2;");
  ASSERT_NE(implementation_field, nullptr);
  EXPECT_EQ(env->GetStaticObjectField(gl_class, implementation_field),
            implementation);

  jclass callback_class =
      env->FindClass("com/roblox/engine/jni/EngineJavaCallback2");
  ASSERT_NE(callback_class, nullptr);
  jmethodID exit_game = env->GetMethodID(callback_class, "a", "(I)V");
  ASSERT_NE(exit_game, nullptr);
  env->CallVoidMethod(implementation, exit_game, 0);
}

TEST_F(JniVmTest, NativeGlJavaInterfaceStoresEngineCallback) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass gl_class =
      env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface");
  ASSERT_NE(gl_class, nullptr);
  jclass callback_class =
      env->FindClass("com/roblox/engine/jni/EngineJavaCallback2");
  ASSERT_NE(callback_class, nullptr);
  jclass listener_class =
      env->FindClass("com/roblox/engine/jni/OnAppBridgeNotificationListener");
  ASSERT_NE(listener_class, nullptr);

  jobject implementation = env->AllocObject(callback_class);
  ASSERT_NE(implementation, nullptr);
  jobject listener = env->AllocObject(listener_class);
  ASSERT_NE(listener, nullptr);

  jmethodID set_implementation =
      env->GetStaticMethodID(gl_class, "setImplementation",
                             "(Lcom/roblox/engine/jni/EngineJavaCallback2;)V");
  ASSERT_NE(set_implementation, nullptr);
  env->CallStaticVoidMethod(gl_class, set_implementation, implementation);

  jmethodID set_listener = env->GetStaticMethodID(
      gl_class, "setAppBridgeNotificationListener",
      "(Lcom/roblox/engine/jni/OnAppBridgeNotificationListener;)V");
  ASSERT_NE(set_listener, nullptr);
  env->CallStaticVoidMethod(gl_class, set_listener, listener);

  jfieldID implementation_field =
      env->GetStaticFieldID(gl_class, "sImplementation",
                            "Lcom/roblox/engine/jni/EngineJavaCallback2;");
  ASSERT_NE(implementation_field, nullptr);
  EXPECT_EQ(env->GetStaticObjectField(gl_class, implementation_field),
            implementation);
}

TEST_F(JniVmTest, NativeGlJavaInterfaceDispatchesDataModelNotification) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass gl_class =
      env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface");
  ASSERT_NE(gl_class, nullptr);
  jclass callback_class =
      env->FindClass("com/roblox/engine/jni/EngineJavaCallback2");
  ASSERT_NE(callback_class, nullptr);
  jclass listener_class =
      env->FindClass("com/roblox/engine/jni/OnAppBridgeNotificationListener");
  ASSERT_NE(listener_class, nullptr);

  jobject implementation = env->AllocObject(callback_class);
  ASSERT_NE(implementation, nullptr);
  jobject listener = env->AllocObject(listener_class);
  ASSERT_NE(listener, nullptr);
  auto notification_probe = std::make_shared<DataModelNotificationProbe>();
  vm_->SetRobloxDataModelNotificationCallbacks(
      notification_probe,
      RobloxDataModelNotificationCallbacks{&ProbeDataModelNotification});

  jmethodID set_implementation =
      env->GetStaticMethodID(gl_class, "setImplementation",
                             "(Lcom/roblox/engine/jni/EngineJavaCallback2;)V");
  ASSERT_NE(set_implementation, nullptr);
  env->CallStaticVoidMethod(gl_class, set_implementation, implementation);

  jmethodID set_listener = env->GetStaticMethodID(
      gl_class, "setAppBridgeNotificationListener",
      "(Lcom/roblox/engine/jni/OnAppBridgeNotificationListener;)V");
  ASSERT_NE(set_listener, nullptr);
  env->CallStaticVoidMethod(gl_class, set_listener, listener);

  jmethodID on_data_model_notification =
      env->GetStaticMethodID(gl_class, "onDataModelNotificationCallback",
                             "(Ljava/lang/String;Ljava/lang/String;)V");
  ASSERT_NE(on_data_model_notification, nullptr);
  env->CallStaticVoidMethod(gl_class, on_data_model_notification,
                            env->NewStringUTF("APP_READY"),
                            env->NewStringUTF("{\"ready\":true}"));

  jfieldID type_field = env->GetFieldID(
      callback_class, "lastDataModelNotificationType", "Ljava/lang/String;");
  ASSERT_NE(type_field, nullptr);
  auto type =
      static_cast<jstring>(env->GetObjectField(implementation, type_field));
  ASSERT_NE(type, nullptr);
  EXPECT_STREQ(env->GetStringUTFChars(type, nullptr), "APP_READY");

  jfieldID data_field = env->GetFieldID(
      callback_class, "lastDataModelNotificationData", "Ljava/lang/String;");
  ASSERT_NE(data_field, nullptr);
  auto data =
      static_cast<jstring>(env->GetObjectField(implementation, data_field));
  ASSERT_NE(data, nullptr);
  EXPECT_STREQ(env->GetStringUTFChars(data, nullptr), "{\"ready\":true}");

  jfieldID app_ready = env->GetFieldID(callback_class, "appReady", "Z");
  ASSERT_NE(app_ready, nullptr);
  EXPECT_EQ(env->GetBooleanField(implementation, app_ready), JNI_TRUE);

  jfieldID listener_type_field = env->GetFieldID(
      listener_class, "lastAppBridgeNotificationType", "Ljava/lang/String;");
  ASSERT_NE(listener_type_field, nullptr);
  auto listener_type =
      static_cast<jstring>(env->GetObjectField(listener, listener_type_field));
  ASSERT_NE(listener_type, nullptr);
  EXPECT_STREQ(env->GetStringUTFChars(listener_type, nullptr), "APP_READY");

  jfieldID listener_data_field = env->GetFieldID(
      listener_class, "lastAppBridgeNotificationData", "Ljava/lang/String;");
  ASSERT_NE(listener_data_field, nullptr);
  auto listener_data =
      static_cast<jstring>(env->GetObjectField(listener, listener_data_field));
  ASSERT_NE(listener_data, nullptr);
  EXPECT_STREQ(env->GetStringUTFChars(listener_data, nullptr),
               "{\"ready\":true}");

  jfieldID listener_app_ready =
      env->GetFieldID(listener_class, "appReady", "Z");
  ASSERT_NE(listener_app_ready, nullptr);
  EXPECT_EQ(env->GetBooleanField(listener, listener_app_ready), JNI_TRUE);
  EXPECT_EQ(notification_probe->calls, 1);
  EXPECT_EQ(notification_probe->type, "APP_READY");
  EXPECT_EQ(notification_probe->data, "{\"ready\":true}");

  jmethodID callback_notification = env->GetMethodID(
      callback_class, "f", "(Ljava/lang/String;Ljava/lang/String;)V");
  ASSERT_NE(callback_notification, nullptr);
  jvalue callback_args[2] = {};
  callback_args[0].l = env->NewStringUTF("OPEN_CUSTOM_WEBVIEW");
  callback_args[1].l = env->NewStringUTF("{\"url\":\"www:games/2\"}");
  env->CallVoidMethodA(implementation, callback_notification, callback_args);
  EXPECT_EQ(notification_probe->calls, 2);
  EXPECT_EQ(notification_probe->type, "OPEN_CUSTOM_WEBVIEW");

  jclass wrong_class = env->FindClass("example/EngineJavaCallback2");
  jobject wrong_receiver = env->AllocObject(wrong_class);
  env->CallVoidMethod(wrong_receiver, callback_notification, callback_args[0].l,
                      callback_args[1].l);
  jmethodID wrong_signature = env->GetMethodID(callback_class, "f", "(I)V");
  env->CallVoidMethod(implementation, wrong_signature, 7);
  env->CallStaticVoidMethod(wrong_class, on_data_model_notification,
                            callback_args[0].l, callback_args[1].l);
  EXPECT_EQ(notification_probe->calls, 2);

  vm_->ClearRobloxDataModelNotificationCallbacks();
  env->CallStaticVoidMethod(gl_class, on_data_model_notification,
                            env->NewStringUTF("OPEN_CUSTOM_WEBVIEW"),
                            env->NewStringUTF("{\"url\":\"www:games/1\"}"));
  EXPECT_EQ(notification_probe->calls, 2);
}

TEST_F(JniVmTest, NativeGlJavaInterfaceDispatchesPlatformWebCallbacks) {
  JNIEnv* env = vm_->GetJNIEnv();
  jclass gl_class =
      env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface");
  ASSERT_NE(gl_class, nullptr);
  auto probe = std::make_shared<DataModelNotificationProbe>();
  vm_->SetRobloxDataModelNotificationCallbacks(
      probe, RobloxDataModelNotificationCallbacks{&ProbeDataModelNotification,
                                                  &ProbeAppBridgeNotification,
                                                  &ProbeNativeOverlay});

  jmethodID app_bridge =
      env->GetStaticMethodID(gl_class, "onAppBridgeNotification",
                             "(Ljava/lang/String;Ljava/lang/String;)V");
  jmethodID native_overlay = env->GetStaticMethodID(
      gl_class, "openNativeOverlay", "(Ljava/lang/String;Ljava/lang/String;)V");
  ASSERT_NE(app_bridge, nullptr);
  ASSERT_NE(native_overlay, nullptr);

  jstring captcha_type = env->NewStringUTF("OPEN_CAPTCHA_VIEW");
  jstring captcha_data = env->NewStringUTF(
      R"({"captchaType":"login","cvalueType":"username","cvalue":"user"})");
  env->CallStaticVoidMethod(gl_class, app_bridge, captcha_type, captcha_data);
  EXPECT_EQ(probe->app_bridge_calls, 1);
  EXPECT_EQ(probe->type, "OPEN_CAPTCHA_VIEW");
  EXPECT_EQ(
      probe->data,
      R"({"captchaType":"login","cvalueType":"username","cvalue":"user"})");

  jvalue overlay_args[2] = {};
  overlay_args[0].l = env->NewStringUTF("Account verification");
  overlay_args[1].l = env->NewStringUTF("https://www.roblox.com/login");
  env->CallStaticVoidMethodA(gl_class, native_overlay, overlay_args);
  EXPECT_EQ(probe->native_overlay_calls, 1);
  EXPECT_EQ(probe->title, "Account verification");
  EXPECT_EQ(probe->url, "https://www.roblox.com/login");

  jclass wrong_class = env->FindClass("example/NativeGLJavaInterface");
  env->CallStaticVoidMethodA(wrong_class, native_overlay, overlay_args);
  EXPECT_EQ(probe->native_overlay_calls, 1);

  vm_->ClearRobloxDataModelNotificationCallbacks();
  env->CallStaticVoidMethod(gl_class, app_bridge, captcha_type, captcha_data);
  env->CallStaticVoidMethodA(gl_class, native_overlay, overlay_args);
  EXPECT_EQ(probe->app_bridge_calls, 1);
  EXPECT_EQ(probe->native_overlay_calls, 1);
}

TEST_F(JniVmTest, MainGameActivityDispatchesOpenWebActivity) {
  JNIEnv* env = vm_->GetJNIEnv();
  jclass activity_class =
      env->FindClass("com/roblox/client/startup/MainGameActivity");
  ASSERT_NE(activity_class, nullptr);
  jobject activity = env->AllocObject(activity_class);
  ASSERT_NE(activity, nullptr);
  auto probe = std::make_shared<DataModelNotificationProbe>();
  vm_->SetRobloxDataModelNotificationCallbacks(
      probe, RobloxDataModelNotificationCallbacks{
                 &ProbeDataModelNotification, &ProbeAppBridgeNotification,
                 &ProbeNativeOverlay, &ProbeOpenWebActivity});

  jmethodID open = env->GetMethodID(activity_class, "openWebActivity",
                                    "(Ljava/lang/String;Ljava/lang/String;)V");
  ASSERT_NE(open, nullptr);
  env->CallVoidMethod(activity, open,
                      env->NewStringUTF("https://www.roblox.com/login"),
                      env->NewStringUTF("Log in"));
  EXPECT_EQ(probe->open_web_activity_calls, 1);
  EXPECT_EQ(probe->url, "https://www.roblox.com/login");
  EXPECT_EQ(probe->title, "Log in");

  jclass wrong_class = env->FindClass("example/MainGameActivity");
  jobject wrong_activity = env->AllocObject(wrong_class);
  env->CallVoidMethod(wrong_activity, open,
                      env->NewStringUTF("https://www.roblox.com/signup"),
                      env->NewStringUTF("Sign up"));
  EXPECT_EQ(probe->open_web_activity_calls, 1);

  vm_->ClearRobloxDataModelNotificationCallbacks();
  env->CallVoidMethod(activity, open,
                      env->NewStringUTF("https://www.roblox.com/signup"),
                      env->NewStringUTF("Sign up"));
  EXPECT_EQ(probe->open_web_activity_calls, 1);
}

TEST_F(JniVmTest, GameActivityDispatchesTypedAndroidWindowFlags) {
  JNIEnv* env = vm_->GetJNIEnv();
  jclass activity_interface_class = env->FindClass("android/app/Activity");
  jclass activity_class =
      env->FindClass("com/roblox/client/startup/MainGameActivity");
  jclass game_activity_class =
      env->FindClass("com/google/androidgamesdk/GameActivity");
  ASSERT_NE(activity_interface_class, nullptr);
  ASSERT_NE(activity_class, nullptr);
  ASSERT_NE(game_activity_class, nullptr);
  jobject activity_interface = env->AllocObject(activity_interface_class);
  jobject activity = env->AllocObject(activity_class);
  jobject game_activity = env->AllocObject(game_activity_class);
  auto probe = std::make_shared<AndroidWindowProbe>();
  AndroidWindowCallbacks callbacks;
  callbacks.set_flags = &ProbeAndroidWindowFlags;
  vm_->SetAndroidWindowCallbacks(probe, callbacks);

  jmethodID set_flags =
      env->GetMethodID(activity_class, "setWindowFlags", "(II)V");
  ASSERT_NE(set_flags, nullptr);
  env->CallVoidMethod(activity, set_flags, 0x400, 0x400);
  EXPECT_EQ(probe->calls, 1);
  EXPECT_EQ(probe->flags, 0x400);
  EXPECT_EQ(probe->mask, 0x400);

  CallVoidMethodVForTest(env, game_activity, set_flags, 0, 0x400);
  EXPECT_EQ(probe->calls, 2);
  EXPECT_EQ(probe->flags, 0);
  EXPECT_EQ(probe->mask, 0x400);

  jvalue args[2] = {};
  args[0].i = 0x800;
  args[1].i = 0x800;
  env->CallVoidMethodA(activity, set_flags, args);
  EXPECT_EQ(probe->calls, 3);
  EXPECT_EQ(probe->flags, 0x800);
  EXPECT_EQ(probe->mask, 0x800);

  // LuaApp passes the concrete object as android.app.Activity. The inherited
  // method ID must still dispatch to the host callback.
  env->CallVoidMethod(activity_interface, set_flags, 0x400, 0x400);
  EXPECT_EQ(probe->calls, 4);
  EXPECT_EQ(probe->flags, 0x400);
  EXPECT_EQ(probe->mask, 0x400);

  jclass wrong_class = env->FindClass("example/MainGameActivity");
  env->CallVoidMethod(env->AllocObject(wrong_class), set_flags, 0x400, 0x400);
  jmethodID wrong_signature =
      env->GetMethodID(activity_class, "setWindowFlags", "(J)V");
  env->CallVoidMethod(activity, wrong_signature, static_cast<jlong>(0x400));
  EXPECT_EQ(probe->calls, 4);

  vm_->ClearAndroidWindowCallbacks();
  env->CallVoidMethod(activity, set_flags, 0, 0x400);
  EXPECT_EQ(probe->calls, 4);
}

TEST_F(JniVmTest, MainGameActivitySynchronizesTypedCookieAndGuestEmpty) {
  JNIEnv* env = vm_->GetJNIEnv();
  jclass activity_class =
      env->FindClass("com/roblox/client/startup/MainGameActivity");
  jobject activity = env->AllocObject(activity_class);
  auto probe = std::make_shared<DataModelNotificationProbe>();
  vm_->SetRobloxDataModelNotificationCallbacks(
      probe, RobloxDataModelNotificationCallbacks{
                 &ProbeDataModelNotification, &ProbeAppBridgeNotification,
                 &ProbeNativeOverlay, &ProbeOpenWebActivity, &ProbeCookieSync});
  jmethodID sync =
      env->GetMethodID(activity_class, "syncCookiesFromEngine", "()V");
  ASSERT_NE(sync, nullptr);

  const std::string authenticated = ".ROBLOSECURITY=_|typed-cookie";
  vm_->SetRobloxCredentialProvider(&authenticated, &TestCredentialProvider);
  env->CallVoidMethod(activity, sync);
  EXPECT_EQ(probe->cookie_sync_calls, 1);
  EXPECT_EQ(probe->cookie, authenticated);

  const std::string guest;
  vm_->SetRobloxCredentialProvider(&guest, &TestCredentialProvider);
  env->CallVoidMethodA(activity, sync, nullptr);
  EXPECT_EQ(probe->cookie_sync_calls, 2);
  EXPECT_TRUE(probe->cookie.empty());
}

TEST_F(JniVmTest, DidLoginRefreshesCredentialFromNativeCookieJar) {
  const std::string guest;
  vm_->SetRobloxCredentialProvider(&guest, &TestCredentialProvider);
  auto credential_probe = std::make_shared<CredentialSinkProbe>();
  vm_->SetRobloxCredentialSink(
      credential_probe, RobloxCredentialSinkCallbacks{&ProbeCredentialStore});
  vm_->SetRobloxCookieGetter(&QuickLoginCookieGetter);

  JNIEnv* env = vm_->GetJNIEnv();
  jclass helper_class =
      env->FindClass("com/roblox/client/startup/NativeHelper");
  jobject helper = env->AllocObject(helper_class);
  jmethodID did_login = env->GetMethodID(
      helper_class, "gameActivity_onDidLogInReceived", "(Ljava/lang/String;)V");
  ASSERT_NE(did_login, nullptr);

  env->CallVoidMethod(helper, did_login,
                      env->NewStringUTF(R"({"userId":123})"));

  EXPECT_EQ(credential_probe->calls, 1);
  EXPECT_EQ(credential_probe->credential,
            ".ROBLOSECURITY=_|quick-login-cookie");
  std::string credential;
  ASSERT_TRUE(vm_->CopyRobloxCredentialFromProvider(&credential));
  EXPECT_EQ(credential, ".ROBLOSECURITY=_|quick-login-cookie");
}

TEST_F(JniVmTest, CookieProtocolPersistsAndDispatchesExactSetHandler) {
  JNIEnv* env = vm_->GetJNIEnv();
  auto platform_probe = std::make_shared<DataModelNotificationProbe>();
  RobloxDataModelNotificationCallbacks platform_callbacks;
  platform_callbacks.on_notification = &ProbeDataModelNotification;
  platform_callbacks.on_set_cookie = &ProbeCookieSet;
  vm_->SetRobloxDataModelNotificationCallbacks(platform_probe,
                                               platform_callbacks);
  auto credential_probe = std::make_shared<CredentialSinkProbe>();
  vm_->SetRobloxCredentialSink(
      credential_probe, RobloxCredentialSinkCallbacks{&ProbeCredentialStore});

  jclass handler_class = env->FindClass(
      "com/roblox/universalapp/cookie/"
      "JNICookieProtocol$OnSetCookieHandler");
  jobject handler = env->AllocObject(handler_class);
  jmethodID on_set_cookie = env->GetMethodID(
      handler_class, "onSetCookie",
      "([Ljava/lang/String;Ljava/lang/String;)V");
  jclass string_class = env->FindClass("java/lang/String");
  jobjectArray cookies = env->NewObjectArray(2, string_class, nullptr);
  env->SetObjectArrayElement(
      cookies, 0, env->NewStringUTF("RBXEventTracker=ignored; Path=/"));
  env->SetObjectArrayElement(
      cookies, 1,
      env->NewStringUTF(
          ".ROBLOSECURITY=_|native-cookie; Domain=.roblox.com; Path=/; "
          "Secure; HttpOnly"));
  jstring url = env->NewStringUTF("https://www.roblox.com/");

  env->CallVoidMethod(handler, on_set_cookie, cookies, url);
  CallVoidMethodVForTest(env, handler, on_set_cookie, cookies, url);
  jvalue args[2] = {};
  args[0].l = cookies;
  args[1].l = url;
  env->CallVoidMethodA(handler, on_set_cookie, args);
  EXPECT_EQ(platform_probe->cookie_set_calls, 3);
  EXPECT_EQ(platform_probe->cookie, ".ROBLOSECURITY=_|native-cookie");
  EXPECT_EQ(platform_probe->cookie_set_url, "https://www.roblox.com/");
  EXPECT_EQ(credential_probe->calls, 3);
  EXPECT_EQ(credential_probe->credential,
            ".ROBLOSECURITY=_|native-cookie");

  jmethodID wrong_signature =
      env->GetMethodID(handler_class, "onSetCookie", "(Ljava/lang/String;)V");
  env->CallVoidMethod(handler, wrong_signature,
                      env->NewStringUTF(".ROBLOSECURITY=_|ignored"));
  jclass wrong_class = env->FindClass("example/OnSetCookieHandler");
  env->CallVoidMethod(env->AllocObject(wrong_class), on_set_cookie, cookies,
                      url);
  EXPECT_EQ(platform_probe->cookie_set_calls, 3);
  EXPECT_EQ(credential_probe->calls, 3);

  vm_->ClearRobloxDataModelNotificationCallbacks();
  env->CallVoidMethod(handler, on_set_cookie, cookies, url);
  EXPECT_EQ(platform_probe->cookie_set_calls, 3);
  EXPECT_EQ(credential_probe->calls, 3);
  EXPECT_FALSE(vm_->DispatchRobloxCookieSet(env, cookies, url));
}

TEST_F(JniVmTest, NativeGlJavaInterfaceStoresAppBridgeNotificationListener) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass gl_class =
      env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface");
  ASSERT_NE(gl_class, nullptr);
  jclass listener_class =
      env->FindClass("com/roblox/engine/jni/OnAppBridgeNotificationListener");
  ASSERT_NE(listener_class, nullptr);

  jobject listener = env->AllocObject(listener_class);
  ASSERT_NE(listener, nullptr);

  jmethodID set_listener = env->GetStaticMethodID(
      gl_class, "setAppBridgeNotificationListener",
      "(Lcom/roblox/engine/jni/OnAppBridgeNotificationListener;)V");
  ASSERT_NE(set_listener, nullptr);
  env->CallStaticVoidMethod(gl_class, set_listener, listener);

  jfieldID listener_field = env->GetStaticFieldID(
      gl_class, "sAppBridgeNotificationListener",
      "Lcom/roblox/engine/jni/OnAppBridgeNotificationListener;");
  ASSERT_NE(listener_field, nullptr);
  EXPECT_EQ(env->GetStaticObjectField(gl_class, listener_field), listener);
}

TEST_F(JniVmTest, WebRtcBuildInfoReturnsHeadlessDeviceData) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass build_info = env->FindClass("org/webrtc/voiceengine/BuildInfo");
  ASSERT_NE(build_info, nullptr);

  jmethodID get_device_model = env->GetStaticMethodID(
      build_info, "getDeviceModel", "()Ljava/lang/String;");
  auto model = static_cast<jstring>(
      env->CallStaticObjectMethod(build_info, get_device_model));
  ASSERT_NE(model, nullptr);
  EXPECT_STREQ(env->GetStringUTFChars(model, nullptr), "Mocktail Headless");

  jmethodID get_sdk_version =
      env->GetStaticMethodID(build_info, "getSdkVersion", "()I");
  EXPECT_EQ(env->CallStaticIntMethod(build_info, get_sdk_version), 33);
}

TEST_F(JniVmTest, FmodJavaShimReportsInitializedNoSoundDevice) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass fmod_class = env->FindClass("org/fmod/FMOD");
  ASSERT_NE(fmod_class, nullptr);

  jmethodID init = env->GetStaticMethodID(fmod_class, "init",
                                          "(Landroid/content/Context;)V");
  ASSERT_NE(init, nullptr);
  env->CallStaticVoidMethod(fmod_class, init, nullptr);

  jmethodID check_init = env->GetStaticMethodID(fmod_class, "checkInit", "()Z");
  ASSERT_NE(check_init, nullptr);
  EXPECT_EQ(env->CallStaticBooleanMethod(fmod_class, check_init), JNI_TRUE);

  jmethodID sample_rate =
      env->GetStaticMethodID(fmod_class, "getOutputSampleRate", "()I");
  ASSERT_NE(sample_rate, nullptr);
  EXPECT_EQ(env->CallStaticIntMethod(fmod_class, sample_rate), 48000);

  jmethodID block_size =
      env->GetStaticMethodID(fmod_class, "getOutputBlockSize", "()I");
  ASSERT_NE(block_size, nullptr);
  EXPECT_EQ(env->CallStaticIntMethod(fmod_class, block_size), 512);

  jmethodID supports_aaudio =
      env->GetStaticMethodID(fmod_class, "supportsAAudio", "()Z");
  ASSERT_NE(supports_aaudio, nullptr);
  EXPECT_EQ(env->CallStaticBooleanMethod(fmod_class, supports_aaudio),
            JNI_FALSE);

  jmethodID asset_manager = env->GetStaticMethodID(
      fmod_class, "getAssetManager", "()Landroid/content/res/AssetManager;");
  ASSERT_NE(asset_manager, nullptr);
  EXPECT_NE(env->CallStaticObjectMethod(fmod_class, asset_manager), nullptr);
}

TEST_F(JniVmTest, FmodAudioDeviceDispatchesExactDirectVAndAMethods) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass device_class = env->FindClass("org/fmod/AudioDevice");
  ASSERT_NE(device_class, nullptr);
  const jmethodID constructor = env->GetMethodID(device_class, "<init>", "()V");
  const jmethodID init = env->GetMethodID(device_class, "init", "(IIII)Z");
  const jmethodID write = env->GetMethodID(device_class, "write", "([BI)V");
  const jmethodID close = env->GetMethodID(device_class, "close", "()V");
  ASSERT_NE(constructor, nullptr);
  ASSERT_NE(init, nullptr);
  ASSERT_NE(write, nullptr);
  ASSERT_NE(close, nullptr);

  auto probe = std::make_shared<FmodAudioDeviceProbe>();
  vm_->SetFmodAudioDeviceCallbacks(probe, ProbeFmodCallbacks());

  jobject direct_device = env->NewObject(device_class, constructor);
  ASSERT_NE(direct_device, nullptr);
  EXPECT_EQ(env->CallBooleanMethod(direct_device, init, 2, 48000, 512, 4),
            JNI_TRUE);

  const jbyte first_bytes[] = {1, 2, 3, 4, 5, 6};
  jbyteArray first_array = env->NewByteArray(6);
  env->SetByteArrayRegion(first_array, 0, 6, first_bytes);
  env->CallVoidMethod(direct_device, write, first_array, 4);
  EXPECT_EQ(probe->bytes, (std::vector<std::uint8_t>{1, 2, 3, 4}));
  env->CallVoidMethod(direct_device, write, first_array, 0);
  env->CallVoidMethod(direct_device, write, first_array, -1);
  env->CallVoidMethod(direct_device, write, first_array, 7);
  env->CallVoidMethod(direct_device, write, nullptr, 1);
  EXPECT_EQ(probe->write_calls, 1);
  env->CallVoidMethod(direct_device, close);

  jobject v_device = env->NewObject(device_class, constructor);
  ASSERT_NE(v_device, nullptr);
  EXPECT_EQ(CallBooleanMethodVForTest(env, v_device, init, 1, 44100, 256, 3),
            JNI_TRUE);
  CallVoidMethodVForTest(env, v_device, write, first_array, 6);
  CallVoidMethodVForTest(env, v_device, close);

  jobject a_device = env->NewObject(device_class, constructor);
  ASSERT_NE(a_device, nullptr);
  jvalue init_args[4] = {};
  init_args[0].i = 6;
  init_args[1].i = 96000;
  init_args[2].i = 128;
  init_args[3].i = 2;
  EXPECT_EQ(env->CallBooleanMethodA(a_device, init, init_args), JNI_TRUE);
  jvalue write_args[2] = {};
  write_args[0].l = first_array;
  write_args[1].i = 2;
  env->CallVoidMethodA(a_device, write, write_args);
  env->CallVoidMethodA(a_device, close, nullptr);

  EXPECT_EQ(probe->init_calls, 3);
  EXPECT_EQ(probe->write_calls, 3);
  EXPECT_EQ(probe->close_calls, 3);
  EXPECT_EQ(probe->last_identity, a_device);
  EXPECT_EQ(probe->channels, 6);
  EXPECT_EQ(probe->sample_rate_hz, 96000);
  EXPECT_EQ(probe->block_size_frames, 128);
  EXPECT_EQ(probe->block_count, 2);

  jclass other_class = env->FindClass("example/UnrelatedAudioDevice");
  jobject other_device = env->NewObject(other_class, constructor);
  EXPECT_EQ(env->CallBooleanMethod(other_device, init, 2, 48000, 512, 4),
            JNI_FALSE);
  EXPECT_EQ(probe->init_calls, 3);

  vm_->ClearFmodAudioDeviceCallbacks();
  EXPECT_EQ(probe->shutdown_calls, 1);
}

TEST_F(JniVmTest, FmodAudioDeviceBindingRetainsContextUntilClear) {
  auto probe = std::make_shared<FmodAudioDeviceProbe>();
  std::weak_ptr<FmodAudioDeviceProbe> weak_probe = probe;
  vm_->SetFmodAudioDeviceCallbacks(probe, ProbeFmodCallbacks());
  probe.reset();
  ASSERT_FALSE(weak_probe.expired());

  JNIEnv *env = vm_->GetJNIEnv();
  jclass device_class = env->FindClass("org/fmod/AudioDevice");
  const jmethodID constructor = env->GetMethodID(device_class, "<init>", "()V");
  const jmethodID init = env->GetMethodID(device_class, "init", "(IIII)Z");
  jobject device = env->NewObject(device_class, constructor);
  EXPECT_EQ(CallBooleanMethodVForTest(env, device, init, 2, 48000, 512, 4),
            JNI_TRUE);

  vm_->ClearFmodAudioDeviceCallbacks();
  EXPECT_TRUE(weak_probe.expired());
}

TEST_F(JniVmTest, RobloxTextInputCapturesExactStaticDirectVAndACalls) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass cls = env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface");
  ASSERT_NE(cls, nullptr);
  const jmethodID show = env->GetStaticMethodID(
      cls, "showKeyboard",
      "(JZ[BLcom/roblox/engine/jni/model/NativeTextBoxInfo;)V");
  const jmethodID hide = env->GetStaticMethodID(cls, "hideKeyboard", "()V");
  const jmethodID replace = env->GetStaticMethodID(
      cls, "onLuaTextBoxChangedCallback", "(Ljava/lang/String;)V");
  const jmethodID properties =
      env->GetStaticMethodID(cls, "onLuaTextBoxPropertyChangedCallback", "()V");
  ASSERT_NE(show, nullptr);

  auto probe = std::make_shared<RobloxTextInputProbe>();
  vm_->SetRobloxTextInputCallbacks(probe, ProbeTextInputCallbacks());
  jobject info = MakeTextBoxInfo(env);
  const jbyte utf8[] = {'h', 'e', 'l', 'l', 'o'};
  jbyteArray text = env->NewByteArray(5);
  env->SetByteArrayRegion(text, 0, 5, utf8);

  env->CallStaticVoidMethod(cls, show, static_cast<jlong>(42), JNI_TRUE, text,
                            info);
  EXPECT_EQ(probe->show_calls, 1);
  EXPECT_EQ(probe->request.text_box, 42);
  EXPECT_TRUE(probe->request.show_native_input);
  EXPECT_EQ(probe->request.text, "hello");
  EXPECT_FLOAT_EQ(probe->request.info.x, 1.0f);
  EXPECT_FLOAT_EQ(probe->request.info.font_size, 18.0f);
  EXPECT_TRUE(probe->request.info.multiline);
  EXPECT_EQ(probe->request.info.text_color, 0x10203040);
  EXPECT_EQ(probe->request.info.text_input_type, 5);
  EXPECT_TRUE(probe->request.info.manual_focus_release);

  CallStaticVoidMethodVForTest(env, cls, show, static_cast<jlong>(42), JNI_TRUE,
                               text, info);
  EXPECT_EQ(probe->show_calls, 1);

  jvalue show_args[4] = {};
  show_args[0].j = 43;
  show_args[1].z = JNI_FALSE;
  show_args[2].l = text;
  show_args[3].l = info;
  env->CallStaticVoidMethodA(cls, show, show_args);
  EXPECT_EQ(probe->show_calls, 2);
  EXPECT_EQ(probe->request.text_box, 43);

  jmethodID wrong_signature =
      env->GetStaticMethodID(cls, "showKeyboard", "(JLjava/lang/String;)V");
  env->CallStaticVoidMethod(cls, wrong_signature, static_cast<jlong>(44),
                            env->NewStringUTF("ignored"));
  jclass wrong_cls = env->FindClass("example/NativeGLJavaInterface");
  env->CallStaticVoidMethod(wrong_cls, show, static_cast<jlong>(44), JNI_FALSE,
                            text, info);
  EXPECT_EQ(probe->show_calls, 2);

  jstring replacement = env->NewStringUTF("engine replacement");
  env->CallStaticVoidMethod(cls, replace, replacement);
  env->CallStaticVoidMethod(cls, replace, replacement);
  env->CallStaticVoidMethod(cls, properties);
  EXPECT_EQ(probe->replace_calls, 1);
  EXPECT_EQ(probe->replacement, "engine replacement");
  EXPECT_EQ(probe->properties_calls, 1);

  const jchar unicode_replacement[] = {0xd83d, 0xde42, 0x00e9};
  env->CallStaticVoidMethod(cls, replace,
                            env->NewString(unicode_replacement, 3));
  EXPECT_EQ(probe->replace_calls, 2);
  EXPECT_EQ(probe->replacement, std::string("\xf0\x9f\x99\x82\xc3\xa9", 6));

  env->CallStaticVoidMethod(cls, hide);
  env->CallStaticVoidMethodA(cls, hide, nullptr);
  EXPECT_EQ(probe->hide_calls, 1);
  vm_->ClearRobloxTextInputCallbacks();
  EXPECT_EQ(probe->shutdown_calls, 1);
}

TEST_F(JniVmTest, RobloxTextInputCapturesNativeHelperAndRejectsMalformedCalls) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass helper_cls = env->FindClass("com/roblox/client/startup/NativeHelper");
  jmethodID constructor = env->GetMethodID(helper_cls, "<init>", "()V");
  jobject helper = env->NewObject(helper_cls, constructor);
  const jmethodID show = env->GetMethodID(
      helper_cls, "gameActivity_showKeyboard",
      "(JZ[BLcom/roblox/engine/jni/model/NativeTextBoxInfo;)V");
  const jmethodID hide =
      env->GetMethodID(helper_cls, "gameActivity_hideKeyboard", "()V");
  const jmethodID replace = env->GetMethodID(
      helper_cls, "gameActivity_onLuaTextBoxChanged", "(Ljava/lang/String;)V");
  const jmethodID properties = env->GetMethodID(
      helper_cls, "gameActivity_onLuaTextBoxPropertyChanged", "()V");
  auto probe = std::make_shared<RobloxTextInputProbe>();
  vm_->SetRobloxTextInputCallbacks(probe, ProbeTextInputCallbacks());
  jobject info = MakeTextBoxInfo(env);
  jbyteArray text = env->NewByteArray(2);
  const jbyte valid[] = {'o', 'k'};
  env->SetByteArrayRegion(text, 0, 2, valid);

  env->CallVoidMethod(helper, show, static_cast<jlong>(98), JNI_FALSE, text,
                      info);
  EXPECT_EQ(probe->show_calls, 1);
  CallVoidMethodVForTest(env, helper, show, static_cast<jlong>(99), JNI_FALSE,
                         text, info);
  EXPECT_EQ(probe->show_calls, 2);
  jvalue args[4] = {};
  args[0].j = 100;
  args[2].l = text;
  args[3].l = info;
  env->CallVoidMethodA(helper, show, args);
  EXPECT_EQ(probe->show_calls, 3);

  env->CallVoidMethod(helper, show, static_cast<jlong>(0), JNI_FALSE, text,
                      info);
  env->CallVoidMethod(helper, show, static_cast<jlong>(101), JNI_FALSE, nullptr,
                      info);
  env->CallVoidMethod(helper, show, static_cast<jlong>(101), JNI_FALSE, text,
                      nullptr);
  jbyteArray invalid_utf8 = env->NewByteArray(2);
  const jbyte invalid[] = {static_cast<jbyte>(0xc0), static_cast<jbyte>(0x80)};
  env->SetByteArrayRegion(invalid_utf8, 0, 2, invalid);
  env->CallVoidMethod(helper, show, static_cast<jlong>(101), JNI_FALSE,
                      invalid_utf8, info);
  EXPECT_EQ(probe->show_calls, 3);

  jmethodID wrong_signature = env->GetMethodID(
      helper_cls, "gameActivity_showKeyboard", "(JLjava/lang/String;)V");
  env->CallVoidMethod(helper, wrong_signature, static_cast<jlong>(102),
                      env->NewStringUTF("ignored"));
  jclass wrong_cls = env->FindClass("example/NativeHelper");
  jobject wrong_obj = env->NewObject(wrong_cls, constructor);
  env->CallVoidMethod(wrong_obj, show, static_cast<jlong>(102), JNI_FALSE, text,
                      info);
  EXPECT_EQ(probe->show_calls, 3);

  env->CallVoidMethod(helper, replace, env->NewStringUTF("updated"));
  env->CallVoidMethodA(helper, properties, nullptr);
  EXPECT_EQ(probe->replace_calls, 1);
  EXPECT_EQ(probe->properties_calls, 1);
  CallVoidMethodVForTest(env, helper, hide);
  EXPECT_EQ(probe->hide_calls, 1);
}

TEST_F(JniVmTest, RobloxTextInputCapturesInstalledEngineJavaCallback) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass callback_class =
      env->FindClass("com/roblox/engine/jni/EngineJavaCallback2");
  ASSERT_NE(callback_class, nullptr);
  const jmethodID constructor =
      env->GetMethodID(callback_class, "<init>", "()V");
  jobject callback = env->NewObject(callback_class, constructor);
  ASSERT_NE(callback, nullptr);
  const jmethodID show = env->GetMethodID(
      callback_class, "q",
      "(JZ[BLcom/roblox/engine/jni/model/NativeTextBoxInfo;)V");
  const jmethodID hide = env->GetMethodID(callback_class, "d", "()V");
  const jmethodID replace =
      env->GetMethodID(callback_class, "g", "(Ljava/lang/String;)V");
  const jmethodID properties = env->GetMethodID(callback_class, "h", "()V");
  ASSERT_NE(show, nullptr);
  ASSERT_NE(hide, nullptr);
  ASSERT_NE(replace, nullptr);
  ASSERT_NE(properties, nullptr);

  auto probe = std::make_shared<RobloxTextInputProbe>();
  vm_->SetRobloxTextInputCallbacks(probe, ProbeTextInputCallbacks());
  jobject info = MakeTextBoxInfo(env);
  jbyteArray text = env->NewByteArray(5);
  const jbyte value[] = {'l', 'o', 'g', 'i', 'n'};
  env->SetByteArrayRegion(text, 0, 5, value);

  env->CallVoidMethod(callback, show, static_cast<jlong>(301), JNI_TRUE, text,
                      info);
  EXPECT_EQ(probe->show_calls, 1);
  EXPECT_EQ(probe->request.text_box, 301);
  EXPECT_TRUE(probe->request.show_native_input);
  EXPECT_EQ(probe->request.text, "login");

  CallVoidMethodVForTest(env, callback, show, static_cast<jlong>(302),
                         JNI_FALSE, text, info);
  EXPECT_EQ(probe->show_calls, 2);
  EXPECT_EQ(probe->request.text_box, 302);

  jvalue args[4] = {};
  args[0].j = 303;
  args[1].z = JNI_TRUE;
  args[2].l = text;
  args[3].l = info;
  env->CallVoidMethodA(callback, show, args);
  EXPECT_EQ(probe->show_calls, 3);
  EXPECT_EQ(probe->request.text_box, 303);

  env->CallVoidMethod(callback, replace, env->NewStringUTF("changed"));
  EXPECT_EQ(probe->replace_calls, 1);
  EXPECT_EQ(probe->replacement, "changed");
  env->CallVoidMethodA(callback, properties, nullptr);
  EXPECT_EQ(probe->properties_calls, 1);
  env->CallVoidMethod(callback, hide);
  EXPECT_EQ(probe->hide_calls, 1);

  const jmethodID wrong_signature =
      env->GetMethodID(callback_class, "q", "(JLjava/lang/String;)V");
  env->CallVoidMethod(callback, wrong_signature, static_cast<jlong>(304),
                      env->NewStringUTF("ignored"));
  jclass wrong_class = env->FindClass("example/EngineJavaCallback2");
  jobject wrong_callback = env->NewObject(wrong_class, constructor);
  env->CallVoidMethod(wrong_callback, show, static_cast<jlong>(304), JNI_FALSE,
                      text, info);
  EXPECT_EQ(probe->show_calls, 3);
}

TEST_F(JniVmTest, RobloxTextInputBindingRetainsContextDuringInflightShow) {
  auto probe = std::make_shared<RobloxTextInputProbe>();
  std::weak_ptr<RobloxTextInputProbe> weak_probe = probe;
  probe->block_show = true;
  vm_->SetRobloxTextInputCallbacks(probe, ProbeTextInputCallbacks());

  RobloxTextInputShowRequest request;
  request.text_box = 7;
  request.text = "sensitive transient text";
  std::thread worker([this, request]() {
    EXPECT_TRUE(vm_->DispatchRobloxTextInputShow(request));
  });
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    probe->condition.wait(lock, [probe]() { return probe->show_entered; });
  }
  vm_->ClearRobloxTextInputCallbacks();
  probe.reset();
  EXPECT_FALSE(weak_probe.expired());
  auto retained = weak_probe.lock();
  ASSERT_NE(retained, nullptr);
  EXPECT_EQ(retained->shutdown_calls, 0);
  {
    std::lock_guard<std::mutex> lock(retained->mutex);
    retained->release_show = true;
  }
  retained->condition.notify_all();
  worker.join();
  EXPECT_EQ(retained->shutdown_calls, 1);
  retained.reset();
  EXPECT_TRUE(weak_probe.expired());
}

TEST_F(JniVmTest, PlatformSystemDialogHandlerReturnsNoOpInstance) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass handler_class = env->FindClass(
      "com/roblox/protocols/systemdialog/PlatformSystemDialogHandler");
  ASSERT_NE(handler_class, nullptr);

  jfieldID instance_field = env->GetStaticFieldID(
      handler_class, "INSTANCE",
      "Lcom/roblox/protocols/systemdialog/PlatformSystemDialogHandler;");
  ASSERT_NE(instance_field, nullptr);

  jobject handler = env->GetStaticObjectField(handler_class, instance_field);
  ASSERT_NE(handler, nullptr);

  jmethodID is_available =
      env->GetMethodID(handler_class, "isAvailable", "()Z");
  EXPECT_EQ(env->CallBooleanMethod(handler, is_available), JNI_TRUE);

  jmethodID open = env->GetMethodID(
      handler_class, "open",
      "(Lcom/roblox/protocols/systemdialogplatforminterface/generated/"
      "SystemDialogRequest;"
      "Lcom/roblox/protocols/systemdialogplatforminterface/generated/"
      "ISystemDialogCallback;)J");
  EXPECT_GT(env->CallLongMethod(handler, open, nullptr, nullptr), 0);
}

TEST_F(JniVmTest, NativeGlJavaInterfaceReturnsTextBoxBackingObject) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass gl_class =
      env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface");
  ASSERT_NE(gl_class, nullptr);

  jmethodID get_text_box = env->GetStaticMethodID(
      gl_class, "getTextBoxInfo",
      "()Lcom/roblox/engine/jni/model/NativeTextBoxInfo;");
  ASSERT_NE(get_text_box, nullptr);

  jobject text_box = env->CallStaticObjectMethod(gl_class, get_text_box);
  ASSERT_NE(text_box, nullptr);

  jclass text_box_class =
      env->FindClass("com/roblox/engine/jni/model/NativeTextBoxInfo");
  ASSERT_NE(text_box_class, nullptr);

  jfieldID text_field =
      env->GetFieldID(text_box_class, "text", "Ljava/lang/String;");
  ASSERT_NE(text_field, nullptr);
  auto text = static_cast<jstring>(env->GetObjectField(text_box, text_field));
  ASSERT_NE(text, nullptr);
  EXPECT_STREQ(env->GetStringUTFChars(text, nullptr), "");

  jfieldID cursor_position =
      env->GetFieldID(text_box_class, "cursorPosition", "I");
  ASSERT_NE(cursor_position, nullptr);
  EXPECT_EQ(env->GetIntField(text_box, cursor_position), 0);
}

TEST_F(JniVmTest, MessageBusConnectionIsNoOpAndConnected) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass message_bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  ASSERT_NE(message_bus_class, nullptr);

  jmethodID connect = env->GetStaticMethodID(
      message_bus_class, "connect",
      "()Lcom/roblox/universalapp/messagebus/Connection;");
  ASSERT_NE(connect, nullptr);

  jobject connection = env->CallStaticObjectMethod(message_bus_class, connect);
  ASSERT_NE(connection, nullptr);

  jclass connection_class =
      env->FindClass("com/roblox/universalapp/messagebus/Connection");
  ASSERT_NE(connection_class, nullptr);

  jmethodID is_connected =
      env->GetMethodID(connection_class, "isConnected", "()Z");
  ASSERT_NE(is_connected, nullptr);
  EXPECT_EQ(env->CallBooleanMethod(connection, is_connected), JNI_TRUE);

  jmethodID disconnect =
      env->GetMethodID(connection_class, "disconnect", "()V");
  ASSERT_NE(disconnect, nullptr);
  env->CallVoidMethod(connection, disconnect);
  EXPECT_EQ(env->CallBooleanMethod(connection, is_connected), JNI_TRUE);
}

TEST_F(JniVmTest, MemStorageCallbackDispatchesExactMethodAcrossCallForms) {
  JNIEnv *env = vm_->GetJNIEnv();
  auto probe = std::make_shared<MemStorageCallbackProbe>();
  jobject callback =
      vm_->CreateMemStorageCallback(probe, ProbeMemStorageCallbacks());
  ASSERT_NE(callback, nullptr);

  jclass callback_class =
      env->FindClass("com/roblox/engine/jni/memstorage/Callback");
  ASSERT_NE(callback_class, nullptr);
  jmethodID on_item_set =
      env->GetMethodID(callback_class, "onItemSet", "(Ljava/lang/String;)V");
  ASSERT_NE(on_item_set, nullptr);

  env->CallVoidMethod(callback, on_item_set, env->NewStringUTF("open"));
  CallVoidMethodVForTest(env, callback, on_item_set,
                         env->NewStringUTF("config"));
  jvalue args[1] = {};
  args[0].l = env->NewStringUTF("close");
  env->CallVoidMethodA(callback, on_item_set, args);
  ASSERT_EQ(probe->values.size(), 3u);
  EXPECT_EQ(probe->values[0], "open");
  EXPECT_EQ(probe->values[1], "config");
  EXPECT_EQ(probe->values[2], "close");

  env->CallVoidMethod(callback, on_item_set, nullptr);
  jmethodID wrong_signature =
      env->GetMethodID(callback_class, "onItemSet", "()V");
  env->CallVoidMethod(callback, wrong_signature);
  jmethodID wrong_name =
      env->GetMethodID(callback_class, "run", "(Ljava/lang/String;)V");
  env->CallVoidMethod(callback, wrong_name, env->NewStringUTF("ignored"));
  jclass wrong_class = env->FindClass("example/memstorage/Callback");
  jobject wrong_callback = env->AllocObject(wrong_class);
  env->CallVoidMethod(wrong_callback, on_item_set,
                      env->NewStringUTF("ignored"));
  EXPECT_EQ(probe->calls, 3);

  vm_->ClearMemStorageCallback(callback);
  env->CallVoidMethod(callback, on_item_set, env->NewStringUTF("ignored"));
  EXPECT_EQ(probe->calls, 3);
  EXPECT_FALSE(vm_->DispatchMemStorageCallback(callback, env,
                                               env->NewStringUTF("ignored")));

  EXPECT_EQ(vm_->CreateMemStorageCallback(nullptr, ProbeMemStorageCallbacks()),
            nullptr);
  EXPECT_EQ(vm_->CreateMemStorageCallback(probe, MemStorageCallbackCallbacks{}),
            nullptr);
}

TEST_F(JniVmTest, MemStorageCallbackRetainsContextDuringInflightDispatch) {
  JNIEnv *env = vm_->GetJNIEnv();
  auto probe = std::make_shared<MemStorageCallbackProbe>();
  std::weak_ptr<MemStorageCallbackProbe> weak_probe = probe;
  probe->block = true;
  jobject callback =
      vm_->CreateMemStorageCallback(probe, ProbeMemStorageCallbacks());
  ASSERT_NE(callback, nullptr);
  jstring value = env->NewStringUTF("BrowserService.OpenBrowserWindow");

  bool dispatched = false;
  std::thread worker([this, callback, env, value, &dispatched]() {
    dispatched = vm_->DispatchMemStorageCallback(callback, env, value);
  });
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    probe->condition.wait(lock, [probe]() { return probe->entered; });
  }

  vm_->ClearMemStorageCallback(callback);
  probe.reset();
  EXPECT_FALSE(weak_probe.expired());
  auto retained = weak_probe.lock();
  ASSERT_NE(retained, nullptr);
  {
    std::lock_guard<std::mutex> lock(retained->mutex);
    retained->release = true;
  }
  retained->condition.notify_all();
  worker.join();
  EXPECT_TRUE(dispatched);
  EXPECT_EQ(retained->calls, 1);
  retained.reset();
  EXPECT_TRUE(weak_probe.expired());
}

TEST_F(JniVmTest, MemStorageConnectionConstructorPreservesNativeRef) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass connection_class =
      env->FindClass("com/roblox/engine/jni/memstorage/Connection");
  ASSERT_NE(connection_class, nullptr);
  jmethodID constructor = env->GetMethodID(connection_class, "<init>", "(J)V");
  ASSERT_NE(constructor, nullptr);
  jfieldID ref = env->GetFieldID(connection_class, "ref", "J");
  ASSERT_NE(ref, nullptr);

  jobject direct = env->NewObject(connection_class, constructor,
                                  static_cast<jlong>(0x1122334455667788));
  ASSERT_NE(direct, nullptr);
  EXPECT_EQ(env->GetLongField(direct, ref),
            static_cast<jlong>(0x1122334455667788));

  jobject v_form = NewObjectVForTest(env, connection_class, constructor,
                                     static_cast<jlong>(0x2233445566778899));
  ASSERT_NE(v_form, nullptr);
  EXPECT_EQ(env->GetLongField(v_form, ref),
            static_cast<jlong>(0x2233445566778899));

  jvalue args[1] = {};
  args[0].j = static_cast<jlong>(0x33445566778899aa);
  jobject a_form = env->NewObjectA(connection_class, constructor, args);
  ASSERT_NE(a_form, nullptr);
  EXPECT_EQ(env->GetLongField(a_form, ref),
            static_cast<jlong>(0x33445566778899aa));

  jmethodID wrong_constructor =
      env->GetMethodID(connection_class, "<init>", "()V");
  jobject wrong = env->NewObject(connection_class, wrong_constructor);
  ASSERT_NE(wrong, nullptr);
  EXPECT_EQ(env->GetLongField(wrong, ref), 0);
}

TEST_F(JniVmTest, DesktopPlatformIdentityAdvertisesPcWithoutTouchscreen) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass package_manager_class =
      env->FindClass("android/content/pm/PackageManager");
  ASSERT_NE(package_manager_class, nullptr);
  jobject package_manager = env->AllocObject(package_manager_class);
  ASSERT_NE(package_manager, nullptr);
  jmethodID has_feature = env->GetMethodID(
      package_manager_class, "hasSystemFeature", "(Ljava/lang/String;)Z");
  ASSERT_NE(has_feature, nullptr);

  jstring pc = env->NewStringUTF("android.hardware.type.pc");
  jstring touchscreen = env->NewStringUTF("android.hardware.touchscreen");
  EXPECT_EQ(env->CallBooleanMethod(package_manager, has_feature, pc), JNI_TRUE);
  EXPECT_EQ(env->CallBooleanMethod(package_manager, has_feature, touchscreen),
            JNI_FALSE);

  jobject configuration = CreateAndroidConfiguration(env);
  ASSERT_NE(configuration, nullptr);
  jclass configuration_class = env->GetObjectClass(configuration);
  ASSERT_NE(configuration_class, nullptr);
  const auto read_int = [env, configuration,
                         configuration_class](const char *name) {
    return env->GetIntField(configuration,
                            env->GetFieldID(configuration_class, name, "I"));
  };
  EXPECT_EQ(read_int("touchscreen"), 1);
  EXPECT_EQ(read_int("keyboard"), 2);
  EXPECT_EQ(read_int("keyboardHidden"), 1);
  EXPECT_EQ(read_int("hardKeyboardHidden"), 1);

  PlatformIdentity touch_profile;
  touch_profile.touch_enabled = true;
  touch_profile.pc_hardware = false;
  vm_->SetPlatformIdentity(touch_profile);
  EXPECT_EQ(env->CallBooleanMethod(package_manager, has_feature, pc),
            JNI_FALSE);
  EXPECT_EQ(env->CallBooleanMethod(package_manager, has_feature, touchscreen),
            JNI_TRUE);
}

TEST_F(JniVmTest, LocalFramesReleaseInnerRefsAndPromoteResult) {
  JNIEnv *env = vm_->GetJNIEnv();
  jstring keep = env->NewStringUTF("keep");
  ASSERT_NE(keep, nullptr);
  ASSERT_EQ(env->PushLocalFrame(16), JNI_OK);
  jstring inner = env->NewStringUTF("inner");
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(ReadJavaString(env, inner), "inner");
  EXPECT_EQ(env->PopLocalFrame(nullptr), nullptr);
  EXPECT_EQ(ReadJavaString(env, inner), "");
  EXPECT_EQ(ReadJavaString(env, keep), "keep");

  ASSERT_EQ(env->PushLocalFrame(16), JNI_OK);
  jstring promoted = env->NewStringUTF("promoted");
  jobject result = env->PopLocalFrame(promoted);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(ReadJavaString(env, static_cast<jstring>(result)), "promoted");
}

TEST_F(JniVmTest, GlobalRefSurvivesLocalFramePop) {
  JNIEnv *env = vm_->GetJNIEnv();
  ASSERT_EQ(env->PushLocalFrame(16), JNI_OK);
  jstring inner = env->NewStringUTF("global");
  jobject global = env->NewGlobalRef(inner);
  ASSERT_NE(global, nullptr);
  env->PopLocalFrame(nullptr);
  EXPECT_EQ(ReadJavaString(env, static_cast<jstring>(global)), "global");
  env->DeleteGlobalRef(global);
}

TEST_F(JniVmTest, EncodedHandlesStayValidPastSignedShiftBoundary) {
  JNIEnv *env = vm_->GetJNIEnv();
  ASSERT_EQ(env->PushLocalFrame(16), JNI_OK);
  jstring high = nullptr;
  for (int i = 0; i < 32768; ++i) {
    high = env->NewStringUTF("slot");
  }
  ASSERT_NE(high, nullptr);
  EXPECT_EQ(ReadJavaString(env, high), "slot");
  env->PopLocalFrame(nullptr);
}

TEST_F(JniVmTest, MainGameActivityReturnsNativeHelperAndTracksCallbacks) {
  JNIEnv *env = vm_->GetJNIEnv();
  jclass activity_class =
      env->FindClass("com/roblox/client/startup/MainGameActivity");
  ASSERT_NE(activity_class, nullptr);
  jobject activity = env->AllocObject(activity_class);
  ASSERT_NE(activity, nullptr);

  jmethodID get_native_helper =
      env->GetMethodID(activity_class, "getNativeHelper",
                       "()Lcom/roblox/client/startup/NativeHelper;");
  ASSERT_NE(get_native_helper, nullptr);
  jobject helper = env->CallObjectMethod(activity, get_native_helper);
  ASSERT_NE(helper, nullptr);

  jclass helper_class =
      env->FindClass("com/roblox/client/startup/NativeHelper");
  ASSERT_NE(helper_class, nullptr);
  jfieldID activity_field = env->GetFieldID(
      helper_class, "activity", "Lcom/roblox/client/startup/MainGameActivity;");
  ASSERT_NE(activity_field, nullptr);
  EXPECT_EQ(env->GetObjectField(helper, activity_field), activity);

  jfieldID is_engine_initialized =
      env->GetFieldID(helper_class, "isEngineInitialized", "Z");
  ASSERT_NE(is_engine_initialized, nullptr);
  EXPECT_EQ(env->GetBooleanField(helper, is_engine_initialized), JNI_FALSE);

  jmethodID bootstrap =
      env->GetMethodID(activity_class, "bootstrapTheApp", "()V");
  ASSERT_NE(bootstrap, nullptr);
  env->CallVoidMethod(activity, bootstrap);

  jfieldID bootstrap_started =
      env->GetFieldID(activity_class, "bootstrapStarted", "Z");
  ASSERT_NE(bootstrap_started, nullptr);
  EXPECT_EQ(env->GetBooleanField(activity, bootstrap_started), JNI_TRUE);

  jmethodID on_engine_initialized =
      env->GetMethodID(helper_class, "gameActivity_onEngineInitialized", "()V");
  ASSERT_NE(on_engine_initialized, nullptr);
  env->CallVoidMethod(helper, on_engine_initialized);
  EXPECT_EQ(env->GetBooleanField(helper, is_engine_initialized), JNI_TRUE);
}

} // namespace
} // namespace jnivm
