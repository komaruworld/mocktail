#ifndef MOCKTAIL_JNIVM_JNIVM_H_
#define MOCKTAIL_JNIVM_JNIVM_H_

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace jnivm {

class VM;
class Class;

class Object {
public:
  explicit Object(std::shared_ptr<Class> klass) : klass_(std::move(klass)) {}
  virtual ~Object() = default;

  std::shared_ptr<Class> GetClass() const { return klass_; }

private:
  std::shared_ptr<Class> klass_;
};

using MethodCallback = std::function<void(JNIEnv *, jobject)>;

// Resolved Roblox account identity supplied by the host runtime. This
// intentionally contains no cookie or other authentication credential.
struct RobloxAuthIdentity {
  int64_t user_id = -1;
  std::string username;
  std::string display_name;
};

struct PlatformIdentity {
  bool touch_enabled = false;
  bool mouse_enabled = true;
  bool keyboard_enabled = true;
  bool pc_hardware = true;
  std::string platform_name = "Linux";
  std::string device_name = "Mocktail Headless";
  std::string manufacturer = "Mocktail";
  std::string model = "Mocktail Headless";
  std::string brand = "Mocktail";
  std::string device_code = "linux-x86_64";
  std::string device_sku = "mocktail-x86_64";
  std::string soc_model = "x86_64";
};

struct RobloxCredentialView {
  const char *data = nullptr;
  std::size_t size = 0;
};

using RobloxCredentialProvider = RobloxCredentialView (*)(const void *context);

jobject CreateAndroidConfiguration(JNIEnv *env);

// A stored credential replaces the bound provider for the rest of this VM's
// lifetime.
struct RobloxCredentialSinkCallbacks {
  bool (*store)(void *context, const char *data, std::size_t size) = nullptr;
};

// The VM keeps the host context opaque. Receiver identities let the backend
// track devices without retaining JNI local references.
struct FmodAudioDeviceCallbacks {
  bool (*init)(void *context, const void *identity, int channels,
               int sample_rate_hz, int block_size_frames,
               int block_count) = nullptr;
  bool (*write)(void *context, const void *identity, const std::uint8_t *data,
                std::size_t size) = nullptr;
  bool (*close)(void *context, const void *identity) = nullptr;
  void (*shutdown)(void *context) = nullptr;
};

// Guest JNI threads only publish Android window flags. The host callback must
// marshal SDL work onto the main thread.
struct AndroidWindowCallbacks {
  bool (*set_flags)(void *context, int flags, int mask) = nullptr;
};

struct RobloxTextBoxInfo {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  float font_size = 0.0f;
  bool multiline = false;
  int x_alignment = 0;
  int y_alignment = 0;
  int text_color = 0;
  int font = 0;
  int text_input_type = 0;
  int return_key_type = 0;
  bool manual_focus_release = false;
  bool text_wrapped = false;
};

struct RobloxTextInputShowRequest {
  std::int64_t text_box = 0;
  bool show_native_input = false;
  std::string text;
  RobloxTextBoxInfo info;
};

// JNI-owned arguments are copied before the host callback runs.
struct RobloxTextInputCallbacks {
  void (*show)(void *context,
               const RobloxTextInputShowRequest &request) = nullptr;
  void (*hide)(void *context) = nullptr;
  void (*replace_text)(void *context, const std::string &text) = nullptr;
  void (*properties_changed)(void *context) = nullptr;
  void (*shutdown)(void *context) = nullptr;
};

// Dispatch accepts run(String) only on a receiver created by this VM. The
// context stays alive while the callback runs.
struct MessageBusRawCallbacks {
  void (*run)(void *context, JNIEnv *env, jstring message) = nullptr;
};

struct MessageBusRequestHandlerCallbacks {
  std::string (*run)(void *context, JNIEnv *env, jstring message) = nullptr;
};

// Dispatch accepts Callback.onItemSet(String) only on a receiver created by
// this VM. The context stays alive while the callback runs.
struct MemStorageCallbackCallbacks {
  void (*on_item_set)(void *context, JNIEnv *env, jstring value) = nullptr;
};

// Web callbacks share a retained context while in flight. Consumers must copy
// JNI strings before returning.
struct RobloxDataModelNotificationCallbacks {
  void (*on_notification)(void *context, JNIEnv *env, jstring type,
                          jstring data) = nullptr;
  void (*on_app_bridge_notification)(void* context, JNIEnv* env, jstring type,
                                     jstring data) = nullptr;
  void (*on_native_overlay)(void* context, JNIEnv* env, jstring title,
                            jstring url) = nullptr;
  void (*on_open_web_activity)(void* context, JNIEnv* env, jstring url,
                               jstring title) = nullptr;
  void (*on_sync_cookies)(void* context, JNIEnv* env, jstring cookie) = nullptr;
  void (*on_set_cookie)(void* context, JNIEnv* env, jstring cookie,
                        jstring url) = nullptr;
};

struct RobloxExperienceLifecycleCallbacks {
  void (*on_lua_app_did_return)(void *context) = nullptr;
};

class Class {
public:
  explicit Class(std::string name) : name_(std::move(name)) {}

  const std::string &GetName() const { return name_; }

  void RegisterMethod(const std::string &method_name,
                      const std::string &signature, MethodCallback callback);

  const MethodCallback *FindMethod(const std::string &method_name,
                                   const std::string &signature) const;

  const std::unordered_map<std::string, MethodCallback> &GetMethods() const {
    return methods_;
  }

private:
  std::string name_;

  std::unordered_map<std::string, MethodCallback> methods_;
};

class VM {
public:
  VM();
  ~VM();

  VM(const VM &) = delete;
  VM &operator=(const VM &) = delete;

  // Repeated registration returns the existing class.
  std::shared_ptr<Class> RegisterClass(const std::string &class_name);

  // Returns nullptr if the class has not been registered.
  std::shared_ptr<Class> FindClass(const std::string &class_name) const;

  JavaVM *GetJavaVM() { return java_vm_; }

  // Resolves the process pseudo-VM that owns an exact JavaVM pointer. Returns
  // null for foreign VMs and after the owner is destroyed.
  static VM *FromJavaVM(JavaVM *java_vm);

  JNIEnv *GetJNIEnv();

  std::size_t GetClassCount() const { return class_registry_.size(); }

  // Replaces the resolved account identity with an independent copy. Invalid
  // (non-positive) identities are normalized to the unresolved state.
  void SetRobloxAuthIdentity(const RobloxAuthIdentity &identity);

  void ClearRobloxAuthIdentity();

  // Returns an immutable-by-value snapshot safe for use by JNI callbacks on
  // another thread.
  RobloxAuthIdentity GetRobloxAuthIdentitySnapshot() const;

  void SetPlatformIdentity(const PlatformIdentity &identity);
  PlatformIdentity GetPlatformIdentitySnapshot() const;

  // Installs a non-owning production credential provider. A configured
  // provider returning an empty view blocks legacy env/disk
  // fallback for guest sessions. The provider owner must outlive all JNI
  // calls and clear the provider only after native workers have stopped.
  void SetRobloxCredentialProvider(const void *context,
                                   RobloxCredentialProvider provider);
  void ClearRobloxCredentialProvider();

  // Returns true when the authoritative provider is configured, including an
  // explicitly empty guest credential. A credential accepted by the sink is
  // retained privately and returned instead until the provider is cleared.
  bool CopyRobloxCredentialFromProvider(std::string *credential) const;

  void SetRobloxCredentialSink(std::shared_ptr<void> context,
                               const RobloxCredentialSinkCallbacks &callbacks);
  void ClearRobloxCredentialSink();
  bool DispatchRobloxCredential(const char *data, std::size_t size);

  // Installs an org/fmod/AudioDevice backend. The VM retains context for
  // every in-flight callback and calls shutdown when the binding is cleared or
  // the VM is destroyed. Replacing or clearing a binding is only safe after
  // guest JNI worker threads have stopped.
  void SetFmodAudioDeviceCallbacks(std::shared_ptr<void> context,
                                   const FmodAudioDeviceCallbacks &callbacks);
  void ClearFmodAudioDeviceCallbacks();

  // Internal dispatch entry points used by the JNI function table. They are
  // public because JNI requires captureless C-compatible callbacks.
  bool DispatchFmodAudioDeviceInit(const void *identity, int channels,
                                   int sample_rate_hz, int block_size_frames,
                                   int block_count);
  bool DispatchFmodAudioDeviceWrite(const void *identity,
                                    const std::uint8_t *data, std::size_t size);
  bool DispatchFmodAudioDeviceClose(const void *identity);

  void SetAndroidWindowCallbacks(std::shared_ptr<void> context,
                                 const AndroidWindowCallbacks &callbacks);
  void ClearAndroidWindowCallbacks();
  bool DispatchAndroidWindowFlags(int flags, int mask);

  void SetRobloxTextInputCallbacks(std::shared_ptr<void> context,
                                   const RobloxTextInputCallbacks &callbacks);
  void ClearRobloxTextInputCallbacks();
  bool DispatchRobloxTextInputShow(const RobloxTextInputShowRequest &request);
  bool DispatchRobloxTextInputHide();
  bool DispatchRobloxTextInputReplaceText(const std::string &text);
  bool DispatchRobloxTextInputPropertiesChanged();

  // Creates a pseudo com.roblox.universalapp.messagebus.RawCallback receiver
  // backed by a host callback. Clear removes future dispatch while an
  // already-running call keeps its shared context alive until return.
  jobject CreateMessageBusRawCallback(std::shared_ptr<void> context,
                                      const MessageBusRawCallbacks &callbacks);
  void ClearMessageBusRawCallback(jobject callback);
  bool DispatchMessageBusRawCallback(jobject callback, JNIEnv *env,
                                     jstring message);
  jobject CreateMessageBusRequestHandler(
      std::shared_ptr<void> context,
      const MessageBusRequestHandlerCallbacks &callbacks);
  void ClearMessageBusRequestHandler(jobject handler);
  jstring DispatchMessageBusRequestHandler(jobject handler, JNIEnv *env,
                                           jstring message);

  // Creates the exact com.roblox.engine.jni.memstorage.Callback receiver used
  // by MemStorage.bind. Clear prevents future dispatch while an in-flight call
  // retains its shared context until the callback returns.
  jobject CreateMemStorageCallback(
      std::shared_ptr<void> context,
      const MemStorageCallbackCallbacks& callbacks);
  void ClearMemStorageCallback(jobject callback);
  bool DispatchMemStorageCallback(jobject callback, JNIEnv *env, jstring value);

  // Installs the platform web callbacks reached through NativeGLJavaInterface,
  // MainGameActivity, and JNICookieProtocol. Clear prevents future dispatch
  // while an in-flight call retains its shared context until return.
  void SetRobloxDataModelNotificationCallbacks(
      std::shared_ptr<void> context,
      const RobloxDataModelNotificationCallbacks &callbacks);
  void ClearRobloxDataModelNotificationCallbacks();
  bool DispatchRobloxDataModelNotification(JNIEnv *env, jstring type,
                                           jstring data);
  bool DispatchRobloxAppBridgeNotification(JNIEnv* env, jstring type,
                                           jstring data);
  bool DispatchRobloxNativeOverlay(JNIEnv* env, jstring title, jstring url);
  bool DispatchRobloxOpenWebActivity(JNIEnv* env, jstring url, jstring title);
  bool DispatchRobloxCookieSync(JNIEnv* env, jstring cookie);
  bool DispatchRobloxCookieSet(JNIEnv* env, jobjectArray cookies, jstring url);

  // Installs the callback used by the exact NativeHelper
  // gameActivity_onLuaAppDidReturn()V and ASMA/V2 NativeGLJavaInterface
  // gameDidLeave()V contracts. Dispatch snapshots the shared binding so Clear
  // cannot destroy context while a call is in flight.
  void SetRobloxExperienceLifecycleCallbacks(
      std::shared_ptr<void> context,
      const RobloxExperienceLifecycleCallbacks &callbacks);
  void ClearRobloxExperienceLifecycleCallbacks();
  bool DispatchRobloxExperienceLuaAppDidReturn();

  // Restores the pseudo-JNI table after guest code replaces env->functions.
  void RestoreFunctions();

private:
  JNIInvokeInterface_ invoke_interface_ = {};
  JavaVM java_vm_storage_ = {};
  JavaVM *java_vm_ = nullptr;

  JNINativeInterface_ native_interface_ = {};
  JNIEnv jni_env_storage_ = {};
  JNIEnv *jni_env_ = nullptr;

  std::unordered_map<std::string, std::shared_ptr<Class>> class_registry_;

  mutable std::mutex roblox_auth_identity_mutex_;
  RobloxAuthIdentity roblox_auth_identity_;

  mutable std::mutex platform_identity_mutex_;
  PlatformIdentity platform_identity_;

  mutable std::mutex roblox_credential_provider_mutex_;
  const void *roblox_credential_provider_context_ = nullptr;
  RobloxCredentialProvider roblox_credential_provider_ = nullptr;
  std::string roblox_credential_override_;

  struct RobloxCredentialSinkBinding {
    std::shared_ptr<void> context;
    RobloxCredentialSinkCallbacks callbacks;
  };
  mutable std::mutex roblox_credential_sink_mutex_;
  RobloxCredentialSinkBinding roblox_credential_sink_binding_;

  struct FmodAudioDeviceBinding {
    std::shared_ptr<void> context;
    FmodAudioDeviceCallbacks callbacks;
  };
  mutable std::mutex fmod_audio_device_mutex_;
  FmodAudioDeviceBinding fmod_audio_device_binding_;

  struct AndroidWindowBinding {
    std::shared_ptr<void> context;
    AndroidWindowCallbacks callbacks;
  };
  mutable std::mutex android_window_mutex_;
  AndroidWindowBinding android_window_binding_;

  struct RobloxTextInputBinding;
  mutable std::mutex roblox_text_input_mutex_;
  std::shared_ptr<RobloxTextInputBinding> roblox_text_input_binding_;

  struct MessageBusRawBinding {
    std::shared_ptr<void> context;
    MessageBusRawCallbacks callbacks;
  };
  mutable std::mutex message_bus_raw_mutex_;
  std::unordered_map<jobject, std::shared_ptr<MessageBusRawBinding>>
      message_bus_raw_bindings_;

  struct MessageBusRequestHandlerBinding {
    std::shared_ptr<void> context;
    MessageBusRequestHandlerCallbacks callbacks;
  };
  mutable std::mutex message_bus_request_handler_mutex_;
  std::unordered_map<jobject, std::shared_ptr<MessageBusRequestHandlerBinding>>
      message_bus_request_handler_bindings_;

  struct MemStorageCallbackBinding {
    std::shared_ptr<void> context;
    MemStorageCallbackCallbacks callbacks;
  };
  mutable std::mutex mem_storage_callback_mutex_;
  std::unordered_map<jobject, std::shared_ptr<MemStorageCallbackBinding>>
      mem_storage_callback_bindings_;

  struct RobloxDataModelNotificationBinding {
    std::shared_ptr<void> context;
    RobloxDataModelNotificationCallbacks callbacks;
  };
  mutable std::mutex roblox_data_model_notification_mutex_;
  std::shared_ptr<RobloxDataModelNotificationBinding>
      roblox_data_model_notification_binding_;

  struct RobloxExperienceLifecycleBinding {
    std::shared_ptr<void> context;
    RobloxExperienceLifecycleCallbacks callbacks;
  };
  mutable std::mutex roblox_experience_lifecycle_mutex_;
  std::shared_ptr<RobloxExperienceLifecycleBinding>
      roblox_experience_lifecycle_binding_;

  void InitJNIFunctionTables();
};

} // namespace jnivm

#endif  // MOCKTAIL_JNIVM_JNIVM_H_
