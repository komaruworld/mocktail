#include "jnivm/jnivm.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <list>
#include <mutex>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace jnivm {

void Class::RegisterMethod(const std::string& method_name,
                           const std::string& signature,
                           MethodCallback callback) {
  const std::string key = method_name + ":" + signature;
  methods_[key] = std::move(callback);
}

const MethodCallback* Class::FindMethod(const std::string& method_name,
                                        const std::string& signature) const {
  const std::string key = method_name + ":" + signature;
  auto it = methods_.find(key);
  if (it == methods_.end()) {
    return nullptr;
  }
  return &it->second;
}

extern void* my_segment[100000];
extern void* my_segment_table[];
extern int g_jni_ref_index;

extern "C" {
void* mocktail_gameactivity_on_start_native = nullptr;
void* mocktail_gameactivity_on_resume_native = nullptr;
void* mocktail_gameactivity_on_surface_created_native = nullptr;
void* mocktail_gameactivity_on_surface_changed_native = nullptr;
void* mocktail_gameactivity_on_surface_redraw_needed_native = nullptr;
}

namespace {
thread_local JNIEnv* g_thread_local_env = nullptr;
thread_local JNIEnv g_thread_env_storage = {};
thread_local VM* g_thread_vm_instance = nullptr;

VM* g_vm_instance = nullptr;
std::recursive_mutex g_jni_state_mutex;

bool JniVmTraceEnabled() {
  static const bool enabled = std::getenv("MOCKTAIL_JNI_VM_TRACE") != nullptr;
  return enabled;
}

VM* CurrentVM() {
  return g_thread_vm_instance == g_vm_instance ? g_thread_vm_instance
                                               : g_vm_instance;
}

PlatformIdentity CurrentPlatformIdentity() {
  VM* vm = CurrentVM();
  return vm != nullptr ? vm->GetPlatformIdentitySnapshot() : PlatformIdentity{};
}

bool IsThreadLocalEnvValid() {
  return g_thread_local_env == &g_thread_env_storage &&
         g_thread_local_env->functions != nullptr;
}

struct PseudoArray {
  std::vector<jbyte> bytes;
  std::vector<jfloat> floats;
  std::vector<jobject> objects;
};

constexpr jchar kEmptyUtf16[] = {0};

struct PseudoJavaObject : Object {
  explicit PseudoJavaObject(std::shared_ptr<Class> cls) : Object(std::move(cls)) {}

  std::unordered_map<std::string, jobject> object_fields;
  std::unordered_map<std::string, jint> int_fields;
  std::unordered_map<std::string, jlong> long_fields;
  std::unordered_map<std::string, jfloat> float_fields;
  std::unordered_map<std::string, jboolean> boolean_fields;
};

void AppendUtf8CodePoint(std::uint32_t code_point, std::string* output) {
  if (code_point <= 0x7f) {
    output->push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7ff) {
    output->push_back(static_cast<char>(0xc0 | (code_point >> 6)));
    output->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else if (code_point <= 0xffff) {
    output->push_back(static_cast<char>(0xe0 | (code_point >> 12)));
    output->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    output->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else {
    output->push_back(static_cast<char>(0xf0 | (code_point >> 18)));
    output->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
    output->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    output->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
}

std::string Utf16ToUtf8(const std::vector<jchar>& utf16) {
  std::string output;
  output.reserve(utf16.size());
  for (std::size_t index = 0; index < utf16.size(); ++index) {
    std::uint32_t code_point = utf16[index];
    if (code_point >= 0xd800 && code_point <= 0xdbff &&
        index + 1 < utf16.size() && utf16[index + 1] >= 0xdc00 &&
        utf16[index + 1] <= 0xdfff) {
      code_point = 0x10000 + ((code_point - 0xd800) << 10) +
                   (utf16[++index] - 0xdc00);
    } else if (code_point >= 0xd800 && code_point <= 0xdfff) {
      code_point = 0xfffd;
    }
    AppendUtf8CodePoint(code_point, &output);
  }
  return output;
}

std::string Utf16ToModifiedUtf8(const std::vector<jchar>& utf16,
                                std::size_t begin = 0,
                                std::size_t count = std::string::npos) {
  std::string output;
  if (begin >= utf16.size()) {
    return output;
  }
  const std::size_t end =
      std::min(utf16.size(), begin + std::min(count, utf16.size() - begin));
  output.reserve((end - begin) * 3);
  for (std::size_t index = begin; index < end; ++index) {
    const std::uint32_t code_unit = utf16[index];
    if (code_unit == 0) {
      output.push_back(static_cast<char>(0xc0));
      output.push_back(static_cast<char>(0x80));
    } else if (code_unit <= 0x7f) {
      output.push_back(static_cast<char>(code_unit));
    } else if (code_unit <= 0x7ff) {
      output.push_back(static_cast<char>(0xc0 | (code_unit >> 6)));
      output.push_back(static_cast<char>(0x80 | (code_unit & 0x3f)));
    } else {
      output.push_back(static_cast<char>(0xe0 | (code_unit >> 12)));
      output.push_back(
          static_cast<char>(0x80 | ((code_unit >> 6) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | (code_unit & 0x3f)));
    }
  }
  return output;
}

std::vector<jchar> ModifiedUtf8ToUtf16(const char* input) {
  std::vector<jchar> output;
  if (input == nullptr) {
    return output;
  }
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(input);
  const std::size_t size = std::strlen(input);
  for (std::size_t index = 0; index < size;) {
    const std::uint8_t first = bytes[index];
    std::uint32_t code_point = 0xfffd;
    std::size_t consumed = 1;
    if (first != 0 && first <= 0x7f) {
      code_point = first;
    } else if ((first & 0xe0) == 0xc0 && index + 1 < size &&
               (bytes[index + 1] & 0xc0) == 0x80) {
      code_point = ((first & 0x1f) << 6) | (bytes[index + 1] & 0x3f);
      if (code_point == 0 || code_point >= 0x80) {
        consumed = 2;
      } else {
        code_point = 0xfffd;
      }
    } else if ((first & 0xf0) == 0xe0 && index + 2 < size &&
               (bytes[index + 1] & 0xc0) == 0x80 &&
               (bytes[index + 2] & 0xc0) == 0x80) {
      code_point = ((first & 0x0f) << 12) |
                   ((bytes[index + 1] & 0x3f) << 6) |
                   (bytes[index + 2] & 0x3f);
      if (code_point >= 0x800) {
        consumed = 3;
      } else {
        code_point = 0xfffd;
      }
    } else if ((first & 0xf8) == 0xf0 && index + 3 < size &&
               (bytes[index + 1] & 0xc0) == 0x80 &&
               (bytes[index + 2] & 0xc0) == 0x80 &&
               (bytes[index + 3] & 0xc0) == 0x80) {
      code_point = ((first & 0x07) << 18) |
                   ((bytes[index + 1] & 0x3f) << 12) |
                   ((bytes[index + 2] & 0x3f) << 6) |
                   (bytes[index + 3] & 0x3f);
      if (code_point >= 0x10000 && code_point <= 0x10ffff) {
        consumed = 4;
      } else {
        code_point = 0xfffd;
      }
    }
    if (code_point <= 0xffff) {
      output.push_back(static_cast<jchar>(code_point));
    } else {
      code_point -= 0x10000;
      output.push_back(static_cast<jchar>(0xd800 + (code_point >> 10)));
      output.push_back(static_cast<jchar>(0xdc00 + (code_point & 0x3ff)));
    }
    index += consumed;
  }
  return output;
}

struct PseudoStringObject : PseudoJavaObject {
  PseudoStringObject(std::shared_ptr<Class> cls, const char* utf)
      : PseudoJavaObject(std::move(cls)),
        chars(ModifiedUtf8ToUtf16(utf)),
        value(Utf16ToUtf8(chars)),
        modified_utf8(Utf16ToModifiedUtf8(chars)) {}

  PseudoStringObject(std::shared_ptr<Class> cls, const jchar* utf16,
                     jsize length)
      : PseudoJavaObject(std::move(cls)) {
    if (utf16 != nullptr && length > 0) {
      chars.assign(utf16, utf16 + length);
    }
    value = Utf16ToUtf8(chars);
    modified_utf8 = Utf16ToModifiedUtf8(chars);
  }

  std::vector<jchar> chars;
  std::string value;
  std::string modified_utf8;
};

using jnivm::my_segment;
using jnivm::g_jni_ref_index;

std::vector<std::unique_ptr<Object>> g_object_storage;
std::shared_ptr<void> g_segment_owners[100000];
std::unordered_set<jobject> g_known_objects;
std::unordered_set<jclass> g_known_classes;
std::unordered_map<std::string, jobject> g_singleton_objects;
std::unordered_map<std::string, std::shared_ptr<Class>> g_fallback_classes;
std::list<std::string> g_string_storage;
std::unordered_set<jstring> g_known_strings;
std::list<std::string> g_method_name_storage;
std::list<std::string> g_method_signature_storage;
std::unordered_map<std::string, jmethodID> g_method_ids;
std::unordered_map<jmethodID, const char*> g_method_names;
std::unordered_map<jmethodID, const char*> g_method_signatures;
std::vector<std::unique_ptr<PseudoArray>> g_array_storage;
std::unordered_map<jarray, PseudoArray*> g_arrays;
std::unordered_map<std::string, jobject> g_static_object_fields;
jobject g_app_bridge_notification_listener = nullptr;
jobject g_engine_java_callback = nullptr;
bool g_android_graph_ready = false;
bool g_fmod_initialized = true;
constexpr jlong kLocalStorageUninitializedUser = -2;
constexpr jlong kLocalStorageNoUser = -1;
std::unordered_map<std::string, std::string> g_local_storage_values;
std::unordered_set<jlong> g_local_storage_users;
jlong g_local_storage_current_user = kLocalStorageUninitializedUser;
bool g_cookie_store_loaded = false;
std::string g_cookie_header;

int AllocateSegmentSlot(void* value, std::shared_ptr<void> owner = nullptr) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  int index = g_jni_ref_index++;
  if (index >= 100000) {
    g_jni_ref_index = 1;
    index = 1;
  }
  my_segment[index] = value;
  g_segment_owners[index] = std::move(owner);
  return index;
}

std::shared_ptr<Class> FallbackClassForName(const std::string& class_name) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto it = g_fallback_classes.find(class_name);
  if (it != g_fallback_classes.end()) {
    return it->second;
  }
  auto cls = std::make_shared<Class>(class_name);
  g_fallback_classes[class_name] = cls;
  return cls;
}

bool TraceEnabled() {
  return std::getenv("MOCKTAIL_JNI_TRACE") != nullptr;
}

bool EnvironmentTraceEnabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

bool StringTraceEnabled() {
  return EnvironmentTraceEnabled("MOCKTAIL_JNI_STRING_TRACE") ||
         EnvironmentTraceEnabled("MOCKTAIL_TRACE_ALL") ||
         EnvironmentTraceEnabled("MOCKTAIL_FULL_TRACE");
}

void Trace(const char* name) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] " << name << '\n';
  }
}

const char* MethodName(jmethodID method_id) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto it = g_method_names.find(method_id);
  return it == g_method_names.end() ? "unknown" : it->second;
}

const char* MethodSignature(jmethodID method_id) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto it = g_method_signatures.find(method_id);
  return it == g_method_signatures.end() ? "" : it->second;
}

jmethodID StoreMethodId(const char* name, const char* sig) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  if (!name) {
    return nullptr;
  }
  std::string key = name;
  key += ':';
  key += sig ? sig : "";
  auto it = g_method_ids.find(key);
  if (it != g_method_ids.end()) {
    return it->second;
  }
  g_method_name_storage.emplace_back(name);
  g_method_signature_storage.emplace_back(sig ? sig : "");
  const char* stored_name = g_method_name_storage.back().c_str();
  const char* stored_signature = g_method_signature_storage.back().c_str();
  jmethodID method_id =
      reinterpret_cast<jmethodID>(const_cast<char*>(stored_name));
  g_method_ids[key] = method_id;
  g_method_names[method_id] = stored_name;
  g_method_signatures[method_id] = stored_signature;
  return method_id;
}

jobject ObjectResultForMethod(jmethodID method_id);
jobject ObjectFieldValue(jobject obj, const char* field_name);
jint IntFieldValue(jobject obj, const char* field_name);
jlong LongFieldValue(jobject obj, const char* field_name);
jboolean BooleanFieldValue(jobject obj, const char* field_name);
std::string StringFromJString(jstring str);
std::string CookieHeaderForJava();
jbyteArray MakeByteArray(jsize len);
PseudoArray* ArrayFromRef(jarray array);

std::string GetterFieldName(const char* method_name) {
  if (!method_name || method_name[0] == '\0') {
    return "";
  }

  std::string field_name = method_name;
  if (field_name.rfind("get", 0) == 0 && field_name.size() > 3) {
    field_name.erase(0, 3);
  } else if (field_name.rfind("is", 0) == 0 && field_name.size() > 2) {
    field_name.erase(0, 2);
  }

  if (!field_name.empty()) {
    field_name[0] = static_cast<char>(
        std::tolower(static_cast<unsigned char>(field_name[0])));
  }
  return field_name;
}

jint IntResultForName(const char* name) {
  if (std::strcmp(name, "getScreenWidth") == 0 ||
      std::strcmp(name, "getWidth") == 0) {
    return 1280;
  }
  if (std::strcmp(name, "getScreenHeight") == 0 ||
      std::strcmp(name, "getHeight") == 0) {
    return 720;
  }
  if (std::strcmp(name, "getDensityDpi") == 0) {
    return 160;
  }
  if (std::strcmp(name, "getApiVersion") == 0 ||
      std::strcmp(name, "getAndroidApiVersion") == 0 ||
      std::strcmp(name, "getTargetSdkVersion") == 0) {
    return 33;
  }
  if (std::strcmp(name, "getRefreshRate") == 0) {
    return 60;
  }
  if (std::strcmp(name, "getSdkVersion") == 0) {
    return 33;
  }
  if (std::strcmp(name, "getOutputSampleRate") == 0) {
    return 48000;
  }
  if (std::strcmp(name, "getOutputBlockSize") == 0) {
    return 512;
  }
  if (std::strcmp(name, "getRotation") == 0 ||
      std::strcmp(name, "getInt") == 0 ||
      std::strcmp(name, "getMode") == 0) {
    return 0;
  }
  return 0;
}

jint IntResultForMethod(jmethodID method_id) {
  return IntResultForName(MethodName(method_id));
}

jint IdentityHashCode(jobject obj) {
  uintptr_t value = reinterpret_cast<uintptr_t>(obj);
  value ^= value >> 32;
  value &= 0x7fffffff;
  return value == 0 ? 1 : static_cast<jint>(value);
}

jint StaticIntResultForMethodV(jmethodID method_id, va_list args) {
  const char* name = MethodName(method_id);
  if (std::strcmp(name, "identityHashCode") == 0) {
    jobject obj = va_arg(args, jobject);
    return IdentityHashCode(obj);
  }
  return IntResultForMethod(method_id);
}

jint StaticIntResultForMethodA(jmethodID method_id, const jvalue* args) {
  const char* name = MethodName(method_id);
  if (std::strcmp(name, "identityHashCode") == 0) {
    return IdentityHashCode(args ? args[0].l : nullptr);
  }
  return IntResultForMethod(method_id);
}

jlong CurrentTimeMillis() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<jlong>(ts.tv_sec) * 1000 +
         static_cast<jlong>(ts.tv_nsec / 1000000);
}

jlong NanoTime() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<jlong>(ts.tv_sec) * 1000000000LL +
         static_cast<jlong>(ts.tv_nsec);
}

RobloxAuthIdentity ResearchRobloxIdentityFromEnvironment() {
  RobloxAuthIdentity identity;
  const char* value = std::getenv("MOCKTAIL_ROBLOX_USER_ID");
  if (!value || value[0] == '\0') {
    return identity;
  }

  errno = 0;
  char* end = nullptr;
  const long long parsed = std::strtoll(value, &end, 10);
  if (errno == ERANGE || end == value || *end != '\0' || parsed <= 0 ||
      static_cast<unsigned long long>(parsed) >
          static_cast<unsigned long long>(
              std::numeric_limits<int64_t>::max())) {
    return identity;
  }

  // Normal startup injects identity through VM::SetRobloxAuthIdentity.
  identity.user_id = static_cast<int64_t>(parsed);
  const char* username = std::getenv("MOCKTAIL_ROBLOX_USERNAME");
  if (username && username[0] != '\0') {
    identity.username = username;
    identity.display_name = username;
  }
  return identity;
}

RobloxAuthIdentity RobloxIdentityForJava() {
  VM* vm = CurrentVM();
  if (vm) {
    RobloxAuthIdentity identity = vm->GetRobloxAuthIdentitySnapshot();
    if (identity.user_id > 0) {
      return identity;
    }
  }
  return ResearchRobloxIdentityFromEnvironment();
}

jlong RobloxUserIdForJava() {
  return static_cast<jlong>(RobloxIdentityForJava().user_id);
}

jlong StaticLongResultForMethod(jmethodID method_id) {
  const char* name = MethodName(method_id);
  if (std::strcmp(name, "currentTimeMillis") == 0) {
    return CurrentTimeMillis();
  }
  if (std::strcmp(name, "nanoTime") == 0) {
    return NanoTime();
  }
  if (std::strcmp(name, "getUserId") == 0) {
    return RobloxUserIdForJava();
  }
  return 0;
}

std::shared_ptr<Class> ClassFromJClass(jclass clazz) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  if (!clazz) {
    return nullptr;
  }
  uintptr_t uclazz = reinterpret_cast<uintptr_t>(clazz);
  uint32_t index = uclazz >> 16;
  Class* cls = nullptr;
  if (index > 0 && index < 100000) {
    cls = reinterpret_cast<Class*>(my_segment[index]);
    if (g_segment_owners[index]) {
      return std::static_pointer_cast<Class>(g_segment_owners[index]);
    }
  } else {
    cls = reinterpret_cast<Class*>(clazz);
  }
  if (!cls) {
    return nullptr;
  }
  return FallbackClassForName(cls->GetName());
}

// g_segment_owners keeps the encoded class handle alive.
static jclass StoreClass(std::shared_ptr<Class> cls) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  Class* raw_ptr = cls.get();
  int index = AllocateSegmentSlot(raw_ptr, cls);
  jclass handle = reinterpret_cast<jclass>(static_cast<uintptr_t>(index << 16));
  g_known_classes.insert(handle);
  return handle;
}

jobject StoreObject(std::unique_ptr<Object> object) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  g_object_storage.push_back(std::move(object));
  Object* raw_ptr = g_object_storage.back().get();
  int index = AllocateSegmentSlot(raw_ptr);
  jobject handle =
      reinterpret_cast<jobject>(static_cast<uintptr_t>(index << 16));
  g_known_objects.insert(handle);
  return handle;
}

PseudoJavaObject* PseudoObjectFromRef(jobject obj) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  if (!obj || g_known_objects.find(obj) == g_known_objects.end()) {
    return nullptr;
  }
  uintptr_t uobj = reinterpret_cast<uintptr_t>(obj);
  uint32_t index = uobj >> 16;
  Object* raw_ptr = nullptr;
  if (index > 0 && index < 100000) {
    raw_ptr = reinterpret_cast<Object*>(my_segment[index]);
  } else {
    raw_ptr = reinterpret_cast<Object*>(obj);
  }
  if (!raw_ptr) return nullptr;
  // Avoid host __dynamic_cast on potentially Bionic RTTI; segment entries are
  // PseudoJavaObjects by construction.
  return static_cast<PseudoJavaObject*>(raw_ptr);
}

jobject MakeObjectForClass(const std::string& class_name) {
  auto cls = FallbackClassForName(class_name);
  return StoreObject(std::make_unique<PseudoJavaObject>(std::move(cls)));
}

jobject MakeObject(jclass clazz) {
  auto cls = ClassFromJClass(clazz);
  if (!cls) {
    cls = FallbackClassForName("java/lang/Object");
  }
  return StoreObject(std::make_unique<PseudoJavaObject>(std::move(cls)));
}

jobject SingletonObject(const std::string& class_name) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto it = g_singleton_objects.find(class_name);
  if (it != g_singleton_objects.end()) {
    return it->second;
  }
  jobject object = MakeObjectForClass(class_name);
  g_singleton_objects[class_name] = object;
  return object;
}

std::string ObjectClassName(jobject obj) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  PseudoJavaObject* pseudo_object = PseudoObjectFromRef(obj);
  if (!pseudo_object || !pseudo_object->GetClass()) {
    return "";
  }
  return pseudo_object->GetClass()->GetName();
}

jobject ExactMessageBusStaticObject(jclass clazz, jmethodID method_id) {
  const std::shared_ptr<Class> object_class = ClassFromJClass(clazz);
  if (object_class == nullptr ||
      object_class->GetName() !=
          "com/roblox/universalapp/messagebus/MessageBus" ||
      std::strcmp(MethodName(method_id), "f") != 0 ||
      std::strcmp(MethodSignature(method_id),
                  "()Lcom/roblox/universalapp/messagebus/MessageBus;") != 0) {
    return nullptr;
  }
  return SingletonObject("com/roblox/universalapp/messagebus/MessageBus");
}

jobject EngineJavaCallbackObject() {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  if (g_engine_java_callback != nullptr) {
    return g_engine_java_callback;
  }
  g_engine_java_callback =
      MakeObjectForClass("com/roblox/engine/jni/EngineJavaCallback2");
  g_static_object_fields["sImplementation"] = g_engine_java_callback;
  return g_engine_java_callback;
}

void SetObjectFieldRaw(jobject obj, const char* field_name, jobject value) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  PseudoJavaObject* pseudo_object = PseudoObjectFromRef(obj);
  if (pseudo_object && field_name) {
    pseudo_object->object_fields[field_name] = value;
  }
}

void SetStringFieldRaw(jobject obj, const char* field_name, const char* value);

void SetIntFieldRaw(jobject obj, const char* field_name, jint value) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  PseudoJavaObject* pseudo_object = PseudoObjectFromRef(obj);
  if (pseudo_object && field_name) {
    pseudo_object->int_fields[field_name] = value;
  }
}

void SetLongFieldRaw(jobject obj, const char* field_name, jlong value) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  PseudoJavaObject* pseudo_object = PseudoObjectFromRef(obj);
  if (pseudo_object && field_name) {
    pseudo_object->long_fields[field_name] = value;
  }
}

void SetFloatFieldRaw(jobject obj, const char* field_name, jfloat value) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  PseudoJavaObject* pseudo_object = PseudoObjectFromRef(obj);
  if (pseudo_object && field_name) {
    pseudo_object->float_fields[field_name] = value;
  }
}

void SetBooleanFieldRaw(jobject obj, const char* field_name, jboolean value) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  PseudoJavaObject* pseudo_object = PseudoObjectFromRef(obj);
  if (pseudo_object && field_name) {
    pseudo_object->boolean_fields[field_name] = value;
  }
}

jstring MakeString(const char* utf) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto cls = FallbackClassForName("java/lang/String");
  auto object = std::make_unique<PseudoStringObject>(std::move(cls), utf);
  jobject handle = StoreObject(std::move(object));
  jstring str = reinterpret_cast<jstring>(handle);
  g_known_strings.insert(str);
  return str;
}

jstring MakeUtf16String(const jchar* utf16, jsize length) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto cls = FallbackClassForName("java/lang/String");
  auto object =
      std::make_unique<PseudoStringObject>(std::move(cls), utf16, length);
  jobject handle = StoreObject(std::move(object));
  jstring str = reinterpret_cast<jstring>(handle);
  g_known_strings.insert(str);
  return str;
}

void SetStringFieldRaw(jobject obj, const char* field_name, const char* value) {
  SetObjectFieldRaw(obj, field_name, MakeString(value));
}

void RecordAppBridgeNotification(jobject obj, jstring type, jstring data) {
  const std::string type_value = StringFromJString(type);
  const std::string data_value = StringFromJString(data);
  SetStringFieldRaw(obj, "lastAppBridgeNotificationType", type_value.c_str());
  SetStringFieldRaw(obj, "lastAppBridgeNotificationData", data_value.c_str());
  if (type_value == "APP_READY") {
    SetBooleanFieldRaw(obj, "appReady", JNI_TRUE);
  }
  if (TraceEnabled()) {
    std::cout << "  [JNI callback] OnAppBridgeNotificationListener.a type="
              << type_value << " data_bytes=" << data_value.size() << '\n';
  }
}

void ForwardDataModelNotificationToAppBridgeListener(jstring type,
                                                     jstring data) {
  jobject listener = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    listener = g_app_bridge_notification_listener;
  }
  if (listener == nullptr) {
    return;
  }
  RecordAppBridgeNotification(listener, type, data);
}

void RecordDataModelNotification(jobject obj, jstring type, jstring data) {
  const std::string type_value = StringFromJString(type);
  const std::string data_value = StringFromJString(data);
  SetStringFieldRaw(obj, "lastDataModelNotificationType", type_value.c_str());
  SetStringFieldRaw(obj, "lastDataModelNotificationData", data_value.c_str());
  if (type_value == "APP_READY") {
    SetBooleanFieldRaw(obj, "appReady", JNI_TRUE);
  }
  if (TraceEnabled()) {
    std::cout << "  [JNI callback] EngineJavaCallback2.f type=" << type_value
              << " data=" << data_value << '\n';
  }
  VM *vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxDataModelNotification(vm->GetJNIEnv(), type, data);
  }
  ForwardDataModelNotificationToAppBridgeListener(type, data);
}

jobject MakeNativeHelperObject(jobject activity) {
  jobject helper = SingletonObject("com/roblox/client/startup/NativeHelper");
  jobject helper_activity =
      activity ? activity
               : SingletonObject("com/roblox/client/startup/MainGameActivity");
  SetObjectFieldRaw(helper, "activity", helper_activity);
  SetObjectFieldRaw(helper, "a", helper_activity);
  SetBooleanFieldRaw(helper, "isVisible", JNI_TRUE);
  SetBooleanFieldRaw(helper, "g", JNI_TRUE);
  SetBooleanFieldRaw(helper, "isForeground", JNI_TRUE);
  SetBooleanFieldRaw(helper, "h", JNI_TRUE);
  SetBooleanFieldRaw(helper, "isEngineInitialized", JNI_FALSE);
  SetBooleanFieldRaw(helper, "i", JNI_FALSE);
  SetBooleanFieldRaw(helper, "isInExperience", JNI_FALSE);
  SetBooleanFieldRaw(helper, "j", JNI_FALSE);
  SetIntFieldRaw(helper, "contentViewId", 0);
  SetIntFieldRaw(helper, "m", 0);
  return helper;
}

void SetNativeHelperBoolean(jobject helper,
                            const char* field_name,
                            const char* obfuscated_field_name,
                            jboolean value) {
  SetBooleanFieldRaw(helper, field_name, value);
  SetBooleanFieldRaw(helper, obfuscated_field_name, value);
}

void RecordNativeHelperCallback(jobject obj, const char* name) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] NativeHelper callback: " << name << '\n';
  }
  SetBooleanFieldRaw(obj, name, JNI_TRUE);
}

std::string StringFromJString(jstring str) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  if (!str) {
    return {};
  }
  if (g_known_strings.find(str) != g_known_strings.end()) {
    auto* string_object = dynamic_cast<PseudoStringObject*>(
        PseudoObjectFromRef(reinterpret_cast<jobject>(str)));
    return string_object ? string_object->value : std::string();
  }
  const char* utf = reinterpret_cast<const char*>(str);
  return utf ? std::string(utf) : std::string();
}

bool IsUtf8CharsetName(jstring charset_name) {
  std::string normalized = StringFromJString(charset_name);
  for (char& character : normalized) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return normalized == "utf-8" || normalized == "utf8" ||
         normalized == "unicode-1-1-utf-8";
}

std::string JavaStringUtf8Bytes(const std::vector<jchar>& utf16) {
  std::string output;
  output.reserve(utf16.size());
  for (std::size_t index = 0; index < utf16.size(); ++index) {
    std::uint32_t code_point = utf16[index];
    if (code_point >= 0xd800 && code_point <= 0xdbff &&
        index + 1 < utf16.size() && utf16[index + 1] >= 0xdc00 &&
        utf16[index + 1] <= 0xdfff) {
      code_point = 0x10000 + ((code_point - 0xd800) << 10) +
                   (utf16[++index] - 0xdc00);
    } else if (code_point >= 0xd800 && code_point <= 0xdfff) {
      // CharsetEncoder replaces each malformed UTF-16 unit with '?'.
      output.push_back('?');
      continue;
    }
    AppendUtf8CodePoint(code_point, &output);
  }
  return output;
}

bool IsJavaStringGetBytesMethod(jobject obj, jmethodID method_id) {
  if (obj == nullptr || method_id == nullptr ||
      ObjectClassName(obj) != "java/lang/String" ||
      std::strcmp(MethodName(method_id), "getBytes") != 0 ||
      std::strcmp(MethodSignature(method_id),
                  "(Ljava/lang/String;)[B") != 0) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  return g_known_strings.find(reinterpret_cast<jstring>(obj)) !=
         g_known_strings.end();
}

jbyteArray JavaStringGetUtf8Bytes(jobject obj, jstring charset_name) {
  if (!IsUtf8CharsetName(charset_name)) {
    if (TraceEnabled()) {
      std::cerr << "  [JNI] java/lang/String.getBytes rejected unsupported "
                   "charset\n";
    }
    return nullptr;
  }

  std::string bytes;
  {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    auto* string_object = dynamic_cast<PseudoStringObject*>(
        PseudoObjectFromRef(obj));
    if (string_object == nullptr) {
      return nullptr;
    }
    bytes = JavaStringUtf8Bytes(string_object->chars);
  }
  if (bytes.size() >
      static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
    return nullptr;
  }

  jbyteArray result =
      MakeByteArray(static_cast<jsize>(bytes.size()));
  PseudoArray* array = ArrayFromRef(result);
  if (array == nullptr || array->bytes.size() != bytes.size()) {
    return nullptr;
  }
  if (!bytes.empty()) {
    std::memcpy(array->bytes.data(), bytes.data(), bytes.size());
  }
  return result;
}

const char* StringChars(jstring str) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  if (!str) {
    return nullptr;
  }
  if (g_known_strings.find(str) != g_known_strings.end()) {
    auto* string_object = dynamic_cast<PseudoStringObject*>(
        PseudoObjectFromRef(reinterpret_cast<jobject>(str)));
    return string_object ? string_object->modified_utf8.c_str() : "";
  }
  return reinterpret_cast<const char*>(str);
}

const jchar* StringUtf16Chars(jstring str) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  if (!str) {
    return nullptr;
  }
  if (g_known_strings.find(str) != g_known_strings.end()) {
    auto* string_object = dynamic_cast<PseudoStringObject*>(
        PseudoObjectFromRef(reinterpret_cast<jobject>(str)));
    return string_object && !string_object->chars.empty()
               ? string_object->chars.data()
               : kEmptyUtf16;
  }
  return reinterpret_cast<const jchar*>(str);
}

jsize StringUtf16Length(jstring str) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  if (g_known_strings.find(str) != g_known_strings.end()) {
    auto* string_object = dynamic_cast<PseudoStringObject*>(
        PseudoObjectFromRef(reinterpret_cast<jobject>(str)));
    return string_object != nullptr
               ? static_cast<jsize>(string_object->chars.size())
               : 0;
  }
  const char* bytes = reinterpret_cast<const char*>(str);
  return bytes != nullptr ? static_cast<jsize>(std::strlen(bytes)) : 0;
}

jsize StringModifiedUtf8Length(jstring str) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  if (g_known_strings.find(str) != g_known_strings.end()) {
    auto* string_object = dynamic_cast<PseudoStringObject*>(
        PseudoObjectFromRef(reinterpret_cast<jobject>(str)));
    return string_object != nullptr
               ? static_cast<jsize>(string_object->modified_utf8.size())
               : 0;
  }
  const char* bytes = reinterpret_cast<const char*>(str);
  return bytes != nullptr ? static_cast<jsize>(std::strlen(bytes)) : 0;
}

void CopyStringRegion(jstring str, jsize start, jsize length, jchar* output) {
  if (str == nullptr || output == nullptr || start < 0 || length <= 0) {
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto* string_object = dynamic_cast<PseudoStringObject*>(
      PseudoObjectFromRef(reinterpret_cast<jobject>(str)));
  if (string_object == nullptr ||
      static_cast<std::size_t>(start) >= string_object->chars.size()) {
    return;
  }
  const std::size_t count = std::min(
      static_cast<std::size_t>(length),
      string_object->chars.size() - static_cast<std::size_t>(start));
  std::copy_n(string_object->chars.data() + start, count, output);
}

void CopyStringModifiedUtf8Region(jstring str, jsize start, jsize length,
                                  char* output) {
  if (str == nullptr || output == nullptr || start < 0 || length <= 0) {
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto* string_object = dynamic_cast<PseudoStringObject*>(
      PseudoObjectFromRef(reinterpret_cast<jobject>(str)));
  if (string_object == nullptr ||
      static_cast<std::size_t>(start) >= string_object->chars.size()) {
    return;
  }
  const std::size_t count = std::min(
      static_cast<std::size_t>(length),
      string_object->chars.size() - static_cast<std::size_t>(start));
  const std::string encoded = Utf16ToModifiedUtf8(
      string_object->chars, static_cast<std::size_t>(start), count);
  std::memcpy(output, encoded.data(), encoded.size());
}

std::string TrimString(const std::string& value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(begin, end - begin);
}

std::string ReadTextFile(const std::string& path) {
  if (path.empty()) {
    return {};
  }
  std::ifstream file(path);
  if (!file) {
    return {};
  }
  std::string content;
  std::string line;
  while (std::getline(file, line)) {
    if (!content.empty()) {
      content.push_back('\n');
    }
    content += line;
  }
  return content;
}

std::string HomePath(const char* suffix) {
  const char* home = std::getenv("HOME");
  if (!home || home[0] == '\0') {
    return {};
  }
  std::string path = home;
  path += suffix ? suffix : "";
  return path;
}

std::string CookieValueAfterEquals(const std::string& text,
                                   std::size_t name_pos) {
  std::size_t equals = text.find('=', name_pos);
  if (equals == std::string::npos) {
    return {};
  }
  std::size_t start = equals + 1;
  while (start < text.size() &&
         std::isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }
  std::size_t end = text.find_first_of(";\r\n\t", start);
  return TrimString(text.substr(start, end == std::string::npos
                                           ? std::string::npos
                                           : end - start));
}

std::string NormalizeCookieHeader(const std::string& content) {
  constexpr const char* kCookieName = ".ROBLOSECURITY";
  constexpr const char* kCookiePrefix = ".ROBLOSECURITY=";
  std::string text = TrimString(content);
  if (text.empty()) {
    return {};
  }
  std::size_t prefixed_pos = text.find(kCookiePrefix);
  if (prefixed_pos != std::string::npos) {
    std::string value = CookieValueAfterEquals(text, prefixed_pos);
    return value.empty() ? std::string() : std::string(kCookiePrefix) + value;
  }
  std::size_t name_pos = text.find(kCookieName);
  if (name_pos != std::string::npos) {
    std::string value = CookieValueAfterEquals(text, name_pos);
    return value.empty() ? std::string() : std::string(kCookiePrefix) + value;
  }
  if (text.find('=') == std::string::npos &&
      text.find('\t') == std::string::npos &&
      text.find(';') == std::string::npos) {
    return std::string(kCookiePrefix) + text;
  }
  return {};
}

constexpr jsize kMaximumCookieSetCount = 128;
constexpr jsize kMaximumCookieSetBytes = 64 * 1024;

std::string CookieValueFromHeader(const std::string& header) {
  constexpr const char* kCookiePrefix = ".ROBLOSECURITY=";
  std::size_t pos = header.find(kCookiePrefix);
  if (pos == std::string::npos) {
    return {};
  }
  return CookieValueAfterEquals(header, pos);
}

void ClearCookieString(std::string* value) {
  if (value == nullptr) {
    return;
  }
  volatile char* byte = value->empty() ? nullptr : value->data();
  for (std::size_t index = 0; index < value->size(); ++index) {
    byte[index] = '\0';
  }
  value->clear();
}

void ClearLegacyCookieStore() {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  ClearCookieString(&g_cookie_header);
  g_cookie_store_loaded = false;
}

std::string MocktailConfigRoot() {
  const char* root = std::getenv("MOCKTAIL_CONFIG_ROOT");
  if (root && root[0] != '\0') {
    return root;
  }
  return HomePath("/.config/mocktail");
}

void EnsureCookieStoreLoadedLocked() {
  if (g_cookie_store_loaded) {
    return;
  }
  g_cookie_store_loaded = true;

  const char* env_cookie = std::getenv("MOCKTAIL_ROBLOX_COOKIES");
  g_cookie_header = NormalizeCookieHeader(env_cookie ? env_cookie : "");
  if (g_cookie_header.empty()) {
    const char* cookie_file = std::getenv("MOCKTAIL_COOKIE_FILE");
    if (cookie_file && cookie_file[0] != '\0') {
      g_cookie_header = NormalizeCookieHeader(ReadTextFile(cookie_file));
    }
  }
  if (g_cookie_header.empty()) {
    g_cookie_header =
        NormalizeCookieHeader(ReadTextFile(MocktailConfigRoot() + "/cookie"));
  }
  if (TraceEnabled()) {
    std::cout << "  [JNI] cookie store loaded bytes="
              << g_cookie_header.size() << '\n';
  }
}

void StoreCookieHeader(const std::string& cookie) {
  VM* vm = CurrentVM();
  std::string normalized = NormalizeCookieHeader(cookie);
  if (normalized.empty()) {
    return;
  }
  if (vm != nullptr) {
    (void)vm->DispatchRobloxCredential(normalized.data(), normalized.size());
    if (vm->CopyRobloxCredentialFromProvider(nullptr)) {
      ClearCookieString(&normalized);
      return;
    }
  }
  {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    g_cookie_header = normalized;
    g_cookie_store_loaded = true;
  }
  ClearCookieString(&normalized);
}

std::string CookieHeaderForJava() {
  std::string credential;
  VM* vm = CurrentVM();
  if (vm != nullptr && vm->CopyRobloxCredentialFromProvider(&credential)) {
    return credential;
  }
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  EnsureCookieStoreLoadedLocked();
  return g_cookie_header;
}

std::string CookieNetscapeForJava(const std::string& domain_or_url) {
  std::string header = CookieHeaderForJava();
  std::string value = CookieValueFromHeader(header);
  ClearCookieString(&header);
  if (value.empty()) {
    return {};
  }
  std::string domain = domain_or_url;
  std::size_t scheme = domain.find("://");
  if (scheme != std::string::npos) {
    domain = domain.substr(scheme + 3);
  }
  std::size_t slash = domain.find('/');
  if (slash != std::string::npos) {
    domain = domain.substr(0, slash);
  }
  if (domain.empty()) {
    domain = "roblox.com";
  }
  if (domain.front() != '.') {
    domain = "." + domain;
  }
  std::string result =
      domain + "\tTRUE\t/\tTRUE\t2147483647\t.ROBLOSECURITY\t" + value;
  ClearCookieString(&value);
  return result;
}

jobject MakeCookieStringForJava(std::string cookie) {
  jobject result = MakeString(cookie.c_str());
  ClearCookieString(&cookie);
  return result;
}

bool CookieAvailableForJava() {
  std::string cookie = CookieHeaderForJava();
  const bool available = !cookie.empty();
  ClearCookieString(&cookie);
  return available;
}

bool CookieObjectResultForMethodV(const char* name, va_list args,
                                  jobject* result) {
  if (!name || !result) {
    return false;
  }
  if (std::strcmp(name, "getCookie") == 0 ||
      std::strcmp(name, "nativeGetCookiesForDomain") == 0) {
    (void)va_arg(args, jstring);
    *result = MakeCookieStringForJava(CookieHeaderForJava());
    return true;
  }
  if (std::strcmp(name, "nativeGetCookiesInNetscapeFormat") == 0) {
    jstring domain = va_arg(args, jstring);
    *result = MakeCookieStringForJava(
        CookieNetscapeForJava(StringFromJString(domain)));
    return true;
  }
  return false;
}

bool CookieObjectResultForMethodA(const char* name, const jvalue* args,
                                  jobject* result) {
  if (!name || !result) {
    return false;
  }
  if (std::strcmp(name, "getCookie") == 0 ||
      std::strcmp(name, "nativeGetCookiesForDomain") == 0) {
    *result = MakeCookieStringForJava(CookieHeaderForJava());
    return true;
  }
  if (std::strcmp(name, "nativeGetCookiesInNetscapeFormat") == 0) {
    std::string domain =
        args ? StringFromJString(reinterpret_cast<jstring>(args[0].l)) : "";
    *result = MakeCookieStringForJava(CookieNetscapeForJava(domain));
    return true;
  }
  return false;
}

bool CookieBooleanResultForMethod(const char* name, jboolean* result) {
  if (!name || !result) {
    return false;
  }
  if (std::strcmp(name, "setCookiesFromDisk") == 0) {
    *result = CookieAvailableForJava() ? JNI_TRUE : JNI_FALSE;
    return true;
  }
  return false;
}

bool FmodBooleanResultForMethod(const char* name, jboolean* result) {
  if (!name || !result) {
    return false;
  }
  if (std::strcmp(name, "checkInit") == 0) {
    *result = g_fmod_initialized ? JNI_TRUE : JNI_FALSE;
    return true;
  }
  if (std::strcmp(name, "supportsAAudio") == 0 ||
      std::strcmp(name, "supportsLowLatency") == 0 ||
      std::strcmp(name, "lowLatencyFlag") == 0 ||
      std::strcmp(name, "proAudioFlag") == 0 ||
      std::strcmp(name, "isBluetoothOn") == 0) {
    *result = JNI_FALSE;
    return true;
  }
  return false;
}

jlong ParseLocalStorageUserIdFromEnv() {
  const char* value = std::getenv("MOCKTAIL_LOCAL_STORAGE_CURRENT_USER_ID");
  if (!value || value[0] == '\0') {
    value = std::getenv("MOCKTAIL_ROBLOX_USER_ID");
  }
  if (!value || value[0] == '\0') {
    return 0;
  }
  char* end = nullptr;
  long long parsed = std::strtoll(value, &end, 10);
  return end == value ? 0 : static_cast<jlong>(parsed);
}

jlong CurrentLocalStorageUserLocked() {
  if (g_local_storage_current_user == kLocalStorageUninitializedUser) {
    g_local_storage_current_user = ParseLocalStorageUserIdFromEnv();
    if (g_local_storage_current_user != kLocalStorageNoUser) {
      g_local_storage_users.insert(g_local_storage_current_user);
    }
  }
  return g_local_storage_current_user;
}

std::string LocalStorageUserKey(const std::string& key, jlong user_id) {
  return std::to_string(user_id) + "_" + key;
}

bool CommaListContains(const std::string& list, const std::string& value) {
  std::size_t start = 0;
  while (start <= list.size()) {
    std::size_t end = list.find(',', start);
    std::string item =
        list.substr(start, end == std::string::npos ? std::string::npos
                                                    : end - start);
    if (item == value) {
      return true;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}

void AppendCommaListValue(std::string* list, const std::string& value) {
  if (!list || value.empty() || CommaListContains(*list, value)) {
    return;
  }
  if (!list->empty()) {
    list->push_back(',');
  }
  *list += value;
}

void RefreshLocalStorageUsersValueLocked() {
  std::string users;
  for (jlong user_id : g_local_storage_users) {
    AppendCommaListValue(&users, std::to_string(user_id));
  }
  g_local_storage_values["Users"] = users;
}

std::string LocalStorageGetValue(const std::string& key) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  CurrentLocalStorageUserLocked();
  auto it = g_local_storage_values.find(key);
  return it == g_local_storage_values.end() ? std::string() : it->second;
}

void LocalStorageSetUserValueLocked(jlong user_id, const std::string& key,
                                    const std::string& value) {
  g_local_storage_users.insert(user_id);
  g_local_storage_values[LocalStorageUserKey(key, user_id)] = value;
  std::string& user_keys =
      g_local_storage_values[LocalStorageUserKey("UserKeys", user_id)];
  AppendCommaListValue(&user_keys, key);
  RefreshLocalStorageUsersValueLocked();
}

bool LocalStorageDeleteUserValues(jlong user_id) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  CurrentLocalStorageUserLocked();
  std::string prefix = std::to_string(user_id) + "_";
  for (auto it = g_local_storage_values.begin();
       it != g_local_storage_values.end();) {
    if (it->first.rfind(prefix, 0) == 0) {
      it = g_local_storage_values.erase(it);
    } else {
      ++it;
    }
  }
  g_local_storage_users.erase(user_id);
  if (g_local_storage_current_user == user_id) {
    g_local_storage_current_user = kLocalStorageNoUser;
  }
  RefreshLocalStorageUsersValueLocked();
  return JNI_TRUE;
}

bool LocalStorageObjectResultForMethodV(const char* name, va_list args,
                                        jobject* result) {
  if (!name || !result) {
    return false;
  }
  if (std::strcmp(name, "getSecureValue") == 0) {
    jstring key = va_arg(args, jstring);
    *result = MakeString(LocalStorageGetValue(StringFromJString(key)).c_str());
    return true;
  }
  if (std::strcmp(name, "getSecureValueForCurrentUser") == 0) {
    jstring key = va_arg(args, jstring);
    jlong user_id = 0;
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      user_id = CurrentLocalStorageUserLocked();
    }
    std::string value;
    if (user_id != kLocalStorageNoUser) {
      value = LocalStorageGetValue(LocalStorageUserKey(StringFromJString(key),
                                                       user_id));
    }
    *result = MakeString(value.c_str());
    return true;
  }
  if (std::strcmp(name, "getSecureValueForUser") == 0) {
    jstring key = va_arg(args, jstring);
    jlong user_id = va_arg(args, jlong);
    *result = MakeString(LocalStorageGetValue(
        LocalStorageUserKey(StringFromJString(key), user_id)).c_str());
    return true;
  }
  if (std::strcmp(name, "getUsers") == 0) {
    jobject users = MakeObjectForClass("java/util/HashSet");
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      CurrentLocalStorageUserLocked();
      SetIntFieldRaw(users, "size",
                     static_cast<jint>(g_local_storage_users.size()));
    }
    *result = users;
    return true;
  }
  if (std::strcmp(name, "iterator") == 0) {
    *result = SingletonObject("java/util/Iterator");
    return true;
  }
  return false;
}

bool LocalStorageObjectResultForMethodA(const char* name, const jvalue* args,
                                        jobject* result) {
  if (!name || !result) {
    return false;
  }
  if (std::strcmp(name, "getSecureValue") == 0) {
    jstring key = args ? static_cast<jstring>(args[0].l) : nullptr;
    *result = MakeString(LocalStorageGetValue(StringFromJString(key)).c_str());
    return true;
  }
  if (std::strcmp(name, "getSecureValueForCurrentUser") == 0) {
    jstring key = args ? static_cast<jstring>(args[0].l) : nullptr;
    jlong user_id = 0;
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      user_id = CurrentLocalStorageUserLocked();
    }
    std::string value;
    if (user_id != kLocalStorageNoUser) {
      value = LocalStorageGetValue(LocalStorageUserKey(StringFromJString(key),
                                                       user_id));
    }
    *result = MakeString(value.c_str());
    return true;
  }
  if (std::strcmp(name, "getSecureValueForUser") == 0) {
    jstring key = args ? static_cast<jstring>(args[0].l) : nullptr;
    jlong user_id = args ? args[1].j : 0;
    *result = MakeString(LocalStorageGetValue(
        LocalStorageUserKey(StringFromJString(key), user_id)).c_str());
    return true;
  }
  if (std::strcmp(name, "getUsers") == 0) {
    jobject users = MakeObjectForClass("java/util/HashSet");
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      CurrentLocalStorageUserLocked();
      SetIntFieldRaw(users, "size",
                     static_cast<jint>(g_local_storage_users.size()));
    }
    *result = users;
    return true;
  }
  if (std::strcmp(name, "iterator") == 0) {
    *result = SingletonObject("java/util/Iterator");
    return true;
  }
  return false;
}

bool LocalStorageBooleanResultForMethodV(const char* name, va_list args,
                                         jboolean* result) {
  if (!name || !result) {
    return false;
  }
  if (std::strcmp(name, "setCurrentUser") == 0) {
    jlong user_id = va_arg(args, jlong);
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    g_local_storage_current_user = user_id;
    if (user_id != kLocalStorageNoUser) {
      g_local_storage_users.insert(user_id);
    }
    RefreshLocalStorageUsersValueLocked();
    g_local_storage_values["CurrentUser"] = std::to_string(user_id);
    *result = JNI_TRUE;
    return true;
  }
  if (std::strcmp(name, "setSecureValue") == 0) {
    jstring key = va_arg(args, jstring);
    jstring value = va_arg(args, jstring);
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    CurrentLocalStorageUserLocked();
    g_local_storage_values[StringFromJString(key)] = StringFromJString(value);
    *result = JNI_TRUE;
    return true;
  }
  if (std::strcmp(name, "setSecureValueForCurrentUser") == 0) {
    jstring key = va_arg(args, jstring);
    jstring value = va_arg(args, jstring);
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    jlong user_id = CurrentLocalStorageUserLocked();
    if (user_id == kLocalStorageNoUser) {
      *result = JNI_FALSE;
      return true;
    }
    LocalStorageSetUserValueLocked(user_id, StringFromJString(key),
                                   StringFromJString(value));
    *result = JNI_TRUE;
    return true;
  }
  if (std::strcmp(name, "setSecureValueForUser") == 0) {
    jstring key = va_arg(args, jstring);
    jstring value = va_arg(args, jstring);
    jlong user_id = va_arg(args, jlong);
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    CurrentLocalStorageUserLocked();
    LocalStorageSetUserValueLocked(user_id, StringFromJString(key),
                                   StringFromJString(value));
    *result = JNI_TRUE;
    return true;
  }
  if (std::strcmp(name, "deleteSecureValue") == 0) {
    jstring key = va_arg(args, jstring);
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    CurrentLocalStorageUserLocked();
    g_local_storage_values.erase(StringFromJString(key));
    *result = JNI_TRUE;
    return true;
  }
  if (std::strcmp(name, "deleteUserValues") == 0) {
    jlong user_id = va_arg(args, jlong);
    *result = LocalStorageDeleteUserValues(user_id);
    return true;
  }
  if (std::strcmp(name, "deleteCurrentUserValues") == 0) {
    jlong user_id = 0;
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      user_id = CurrentLocalStorageUserLocked();
    }
    if (user_id == kLocalStorageNoUser) {
      *result = JNI_FALSE;
    } else {
      *result = LocalStorageDeleteUserValues(user_id);
    }
    return true;
  }
  if (std::strcmp(name, "hasNext") == 0) {
    *result = JNI_FALSE;
    return true;
  }
  return false;
}

bool LocalStorageBooleanResultForMethodA(const char* name, const jvalue* args,
                                         jboolean* result) {
  if (!name || !result) {
    return false;
  }
  if (std::strcmp(name, "setCurrentUser") == 0) {
    jlong user_id = args ? args[0].j : 0;
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    g_local_storage_current_user = user_id;
    if (user_id != kLocalStorageNoUser) {
      g_local_storage_users.insert(user_id);
    }
    RefreshLocalStorageUsersValueLocked();
    g_local_storage_values["CurrentUser"] = std::to_string(user_id);
    *result = JNI_TRUE;
    return true;
  }
  if (std::strcmp(name, "setSecureValue") == 0) {
    jstring key = args ? static_cast<jstring>(args[0].l) : nullptr;
    jstring value = args ? static_cast<jstring>(args[1].l) : nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    CurrentLocalStorageUserLocked();
    g_local_storage_values[StringFromJString(key)] = StringFromJString(value);
    *result = JNI_TRUE;
    return true;
  }
  if (std::strcmp(name, "setSecureValueForCurrentUser") == 0) {
    jstring key = args ? static_cast<jstring>(args[0].l) : nullptr;
    jstring value = args ? static_cast<jstring>(args[1].l) : nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    jlong user_id = CurrentLocalStorageUserLocked();
    if (user_id == kLocalStorageNoUser) {
      *result = JNI_FALSE;
      return true;
    }
    LocalStorageSetUserValueLocked(user_id, StringFromJString(key),
                                   StringFromJString(value));
    *result = JNI_TRUE;
    return true;
  }
  if (std::strcmp(name, "setSecureValueForUser") == 0) {
    jstring key = args ? static_cast<jstring>(args[0].l) : nullptr;
    jstring value = args ? static_cast<jstring>(args[1].l) : nullptr;
    jlong user_id = args ? args[2].j : 0;
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    CurrentLocalStorageUserLocked();
    LocalStorageSetUserValueLocked(user_id, StringFromJString(key),
                                   StringFromJString(value));
    *result = JNI_TRUE;
    return true;
  }
  if (std::strcmp(name, "deleteSecureValue") == 0) {
    jstring key = args ? static_cast<jstring>(args[0].l) : nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    CurrentLocalStorageUserLocked();
    g_local_storage_values.erase(StringFromJString(key));
    *result = JNI_TRUE;
    return true;
  }
  if (std::strcmp(name, "deleteUserValues") == 0) {
    jlong user_id = args ? args[0].j : 0;
    *result = LocalStorageDeleteUserValues(user_id);
    return true;
  }
  if (std::strcmp(name, "deleteCurrentUserValues") == 0) {
    jlong user_id = 0;
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      user_id = CurrentLocalStorageUserLocked();
    }
    if (user_id == kLocalStorageNoUser) {
      *result = JNI_FALSE;
    } else {
      *result = LocalStorageDeleteUserValues(user_id);
    }
    return true;
  }
  if (std::strcmp(name, "hasNext") == 0) {
    *result = JNI_FALSE;
    return true;
  }
  return false;
}

bool LocalStorageLongResultForMethod(const char* name, jlong* result) {
  if (!name || !result) {
    return false;
  }
  if (std::strcmp(name, "getCurrentUser") == 0) {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    *result = CurrentLocalStorageUserLocked();
    return true;
  }
  return false;
}

bool HandleAndroidSetWindowFlagsMethodV(jobject obj, jmethodID method_id,
                                        va_list args);

void HandleVoidMethod(jobject obj, jmethodID method_id, va_list args) {
  const char *name = MethodName(method_id);
  if (!name) {
    return;
  }
  if (HandleAndroidSetWindowFlagsMethodV(obj, method_id, args)) {
    return;
  }
  if (std::strcmp(name, "run") == 0 &&
      ObjectClassName(obj) ==
          "com/roblox/universalapp/messagebus/RawCallback") {
    jstring message = va_arg(args, jstring);
    VM *vm = CurrentVM();
    if (vm != nullptr) {
      vm->DispatchMessageBusRawCallback(obj, vm->GetJNIEnv(), message);
    }
    return;
  }
  if (std::strcmp(name, "a") == 0) {
    if (ObjectClassName(obj) !=
        "com/roblox/engine/jni/OnAppBridgeNotificationListener") {
      return;
    }
    auto type = va_arg(args, jstring);
    auto data = va_arg(args, jstring);
    RecordAppBridgeNotification(obj, type, data);
    return;
  }
  if (std::strcmp(name, "f") == 0 &&
      std::strcmp(MethodSignature(method_id),
                  "(Ljava/lang/String;Ljava/lang/String;)V") == 0 &&
      ObjectClassName(obj) == "com/roblox/engine/jni/EngineJavaCallback2") {
    auto type = va_arg(args, jstring);
    auto data = va_arg(args, jstring);
    RecordDataModelNotification(obj, type, data);
    return;
  }
  if (ObjectClassName(obj) == "com/roblox/engine/jni/EngineJavaCallback2" &&
      std::strlen(name) == 1 && name[0] >= 'b' && name[0] <= 'o') {
    return;
  }
  if (std::strcmp(name, "setBaseUrl") == 0) {
    auto value = va_arg(args, jstring);
    std::string base_url = StringFromJString(value);
    if (base_url.empty()) {
      base_url = "https://www.roblox.com";
    }
    SetStringFieldRaw(obj, "baseUrl", base_url.c_str());
    if (TraceEnabled()) {
      std::cout << "  [JNI] setBaseUrl value=" << base_url << '\n';
    }
    return;
  }
  if (std::strcmp(name, "setCookie") == 0) {
    auto first = va_arg(args, jstring);
    auto second = va_arg(args, jstring);
    SetStringFieldRaw(obj, "cookieName", StringFromJString(first).c_str());
    SetStringFieldRaw(obj, "cookieValue", StringFromJString(second).c_str());
    StoreCookieHeader(StringFromJString(second));
    return;
  }
  if (std::strcmp(name, "bootstrapTheApp") == 0) {
    SetBooleanFieldRaw(obj, "bootstrapStarted", JNI_TRUE);
    if (TraceEnabled()) {
      std::cout << "  [JNI] MainGameActivity.bootstrapTheApp no-op; "
                << "native bootstrap is driven by Mocktail\n";
    }
    return;
  }
  if (std::strcmp(name, "setImeEditorInfoFields") == 0 ||
      std::strcmp(name, "setWindowFlags") == 0 ||
      std::strcmp(name, "runOnUiThread") == 0 ||
      std::strcmp(name, "requestOrientationAsDefault") == 0 ||
      std::strcmp(name, "showLeaveAppPrompt") == 0 ||
      std::strcmp(name, "hideKeyboard") == 0 ||
      std::strcmp(name, "restartInput") == 0 ||
      std::strcmp(name, "setSoftKeyboardActive") == 0 ||
      std::strcmp(name, "setState") == 0) {
    return;
  }
  if (std::strcmp(name, "syncCookiesFromEngine") == 0) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      std::string header = CookieHeaderForJava();
      jstring cookie = static_cast<jstring>(MakeString(header.c_str()));
      (void)vm->DispatchRobloxCookieSync(vm->GetJNIEnv(), cookie);
      ClearCookieString(&header);
    }
    return;
  }
  if (std::strcmp(name, "l0") == 0) {
    jint content_view_id = va_arg(args, jint);
    SetIntFieldRaw(obj, "contentViewId", content_view_id);
    SetIntFieldRaw(obj, "m", content_view_id);
    return;
  }
  if (std::strcmp(name, "k1") == 0) {
    jboolean visible = static_cast<jboolean>(va_arg(args, jint));
    SetNativeHelperBoolean(MakeNativeHelperObject(nullptr), "isVisible", "g",
                           visible);
    return;
  }
  if (std::strcmp(name, "gameActivity_onEngineInitialized") == 0) {
    SetNativeHelperBoolean(obj, "isEngineInitialized", "i", JNI_TRUE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strcmp(name, "gameActivity_onFlagsLoaded") == 0) {
    (void)va_arg(args, jobject);
    SetBooleanFieldRaw(obj, "flagsLoaded", JNI_TRUE);
    SetBooleanFieldRaw(obj, "flagsFailed", JNI_FALSE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strcmp(name, "gameActivity_onFlagsFailed") == 0) {
    SetBooleanFieldRaw(obj, "flagsLoaded", JNI_FALSE);
    SetBooleanFieldRaw(obj, "flagsFailed", JNI_TRUE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strcmp(name, "gameActivity_onGameLoaded") == 0) {
    jlong place_id = va_arg(args, jlong);
    SetLongFieldRaw(obj, "placeId", place_id);
    SetNativeHelperBoolean(obj, "isInExperience", "j",
                           place_id > 0 ? JNI_TRUE : JNI_FALSE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strcmp(name, "gameActivity_onExperienceStart") == 0) {
    SetNativeHelperBoolean(obj, "isInExperience", "j", JNI_TRUE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strcmp(name, "gameActivity_onExperienceStop") == 0) {
    (void)va_arg(args, double);
    SetNativeHelperBoolean(obj, "isInExperience", "j", JNI_FALSE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strcmp(name, "gameActivity_onScreenOrientationChanged") == 0) {
    jint orientation = va_arg(args, jint);
    jboolean in_experience = static_cast<jboolean>(va_arg(args, jint));
    SetIntFieldRaw(obj, "orientation", orientation);
    SetNativeHelperBoolean(obj, "isInExperience", "j", in_experience);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strcmp(name, "gameActivity_onAppReady") == 0 ||
      std::strcmp(name, "gameActivity_onDidLogInReceived") == 0 ||
      std::strcmp(name, "gameActivity_onDidSignUp") == 0 ||
      std::strcmp(name, "gameActivity_onMotionEventListening") == 0 ||
      std::strcmp(name, "gameActivity_onLuaTextBoxChanged") == 0 ||
      std::strcmp(name, "gameActivity_onScreenshotReady") == 0) {
    (void)va_arg(args, jstring);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strcmp(name, "gameActivity_onDidLogOutReceived") == 0 ||
      std::strcmp(name, "gameActivity_onDidSwitchAccountReceived") == 0 ||
      std::strcmp(name, "gameActivity_onLuaAppDidReturn") == 0 ||
      std::strcmp(name, "gameActivity_onLuaTextBoxPropertyChanged") == 0 ||
      std::strcmp(name, "gameActivity_onRestartLuaApp") == 0 ||
      std::strcmp(name, "gameActivity_onScanQrCode") == 0 ||
      std::strcmp(name, "gameActivity_hideKeyboard") == 0) {
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strcmp(name, "gameActivity_showKeyboard") == 0 ||
      std::strcmp(name, "gameActivity_setAppUpgradeStatus") == 0) {
    RecordNativeHelperCallback(obj, name);
  }
}

bool IsMemStorageCallbackMethod(jobject obj, jmethodID method_id) {
  return obj != nullptr && method_id != nullptr &&
         ObjectClassName(obj) ==
             "com/roblox/engine/jni/memstorage/Callback" &&
         std::strcmp(MethodName(method_id), "onItemSet") == 0 &&
         std::strcmp(MethodSignature(method_id),
                     "(Ljava/lang/String;)V") == 0;
}

bool HandleMemStorageCallbackVoidMethodV(jobject obj, jmethodID method_id,
                                         va_list args) {
  if (!IsMemStorageCallbackMethod(obj, method_id)) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchMemStorageCallback(obj, vm->GetJNIEnv(),
                                         va_arg(args, jstring));
  }
  return true;
}

bool HandleMemStorageCallbackVoidMethodA(jobject obj, jmethodID method_id,
                                         const jvalue* args) {
  if (!IsMemStorageCallbackMethod(obj, method_id)) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchMemStorageCallback(
        obj, vm->GetJNIEnv(),
        args != nullptr ? static_cast<jstring>(args[0].l) : nullptr);
  }
  return true;
}

bool IsRobloxCookieSetHandlerMethod(jobject obj, jmethodID method_id) {
  return obj != nullptr && method_id != nullptr &&
         ObjectClassName(obj) ==
             "com/roblox/universalapp/cookie/"
             "JNICookieProtocol$OnSetCookieHandler" &&
         std::strcmp(MethodName(method_id), "onSetCookie") == 0 &&
         std::strcmp(MethodSignature(method_id),
                     "([Ljava/lang/String;Ljava/lang/String;)V") == 0;
}

bool HandleRobloxCookieSetVoidMethodV(jobject obj, jmethodID method_id,
                                      va_list args) {
  if (!IsRobloxCookieSetHandlerMethod(obj, method_id)) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    auto cookies = va_arg(args, jobjectArray);
    auto url = va_arg(args, jstring);
    (void)vm->DispatchRobloxCookieSet(vm->GetJNIEnv(), cookies, url);
  }
  return true;
}

bool HandleRobloxCookieSetVoidMethodA(jobject obj, jmethodID method_id,
                                      const jvalue* args) {
  if (!IsRobloxCookieSetHandlerMethod(obj, method_id)) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxCookieSet(
        vm->GetJNIEnv(),
        args != nullptr ? static_cast<jobjectArray>(args[0].l) : nullptr,
        args != nullptr ? static_cast<jstring>(args[1].l) : nullptr);
  }
  return true;
}

jobject MakeFileObject(const char* path) {
  jobject file = MakeObjectForClass("java/io/File");
  auto* pseudo_object = PseudoObjectFromRef(file);
  if (pseudo_object) {
    pseudo_object->object_fields["path"] = MakeString(path);
  }
  return file;
}

jobject MakeApplicationInfoObject() {
  jobject object = SingletonObject("android/content/pm/ApplicationInfo");
  SetStringFieldRaw(object, "packageName", "com.roblox.client");
  SetStringFieldRaw(object, "sourceDir", "rbx_bin/sober_apk/base.apk");
  SetStringFieldRaw(object, "publicSourceDir", "rbx_bin/sober_apk/base.apk");
  SetStringFieldRaw(object, "nativeLibraryDir", "rbx_bin");
  SetStringFieldRaw(object, "dataDir", "/data/user/0/com.roblox.client");
  SetStringFieldRaw(object, "processName", "com.roblox.client");
  SetStringFieldRaw(object, "className", "com.roblox.client.RobloxApplication");
  SetIntFieldRaw(object, "flags", 0);
  return object;
}

jobject MakePackageInfoObject() {
  jobject object = SingletonObject("android/content/pm/PackageInfo");
  SetStringFieldRaw(object, "packageName", "com.roblox.client");
  SetObjectFieldRaw(object, "applicationInfo", MakeApplicationInfoObject());
  const char* version_name = std::getenv("MOCKTAIL_ROBLOX_VERSION");
  SetStringFieldRaw(object, "versionName",
                    version_name != nullptr ? version_name : "unknown");
  const char* version_code = std::getenv("MOCKTAIL_ROBLOX_VERSION_CODE");
  const int parsed_version_code =
      version_code != nullptr ? std::atoi(version_code) : 0;
  SetIntFieldRaw(object, "versionCode", parsed_version_code);
  return object;
}

jobject MakeDeviceStaticParamsObject() {
  jobject object =
      SingletonObject("com/roblox/engine/jni/model/DeviceStaticParams");
  auto* pseudo_object = PseudoObjectFromRef(object);
  if (pseudo_object) {
    const PlatformIdentity identity = CurrentPlatformIdentity();
    pseudo_object->object_fields["osVersion"] = MakeString("Android 13");
    pseudo_object->object_fields["deviceName"] =
        MakeString(identity.device_name.c_str());
    const char* app_version = std::getenv("MOCKTAIL_ROBLOX_VERSION");
    pseudo_object->object_fields["appVersion"] =
        MakeString(app_version != nullptr ? app_version : "unknown");
    pseudo_object->object_fields["manufacturer"] =
        MakeString(identity.manufacturer.c_str());
    pseudo_object->object_fields["model"] = MakeString(identity.model.c_str());
    pseudo_object->object_fields["brand"] = MakeString(identity.brand.c_str());
    pseudo_object->object_fields["device"] =
        MakeString(identity.device_code.c_str());
    pseudo_object->object_fields["deviceSku"] =
        MakeString(identity.device_sku.c_str());
    pseudo_object->object_fields["appBuildVariant"] = MakeString("headless");
    pseudo_object->object_fields["socModel"] =
        MakeString(identity.soc_model.c_str());
    pseudo_object->object_fields["soc_model"] =
        MakeString(identity.soc_model.c_str());
    pseudo_object->boolean_fields["cpu64Bit"] = JNI_TRUE;
    pseudo_object->int_fields["screenWidth"] = 1280;
    pseudo_object->int_fields["screenHeight"] = 720;
    pseudo_object->int_fields["screenDensityDpi"] = 160;
    pseudo_object->int_fields["apiVersion"] = 33;
    pseudo_object->int_fields["sdkVersion"] = 33;
    pseudo_object->float_fields["density"] = 1.0f;
    pseudo_object->float_fields["scaledDensity"] = 1.0f;
    pseudo_object->float_fields["xdpi"] = 160.0f;
    pseudo_object->float_fields["ydpi"] = 160.0f;
  }
  return object;
}

jobject MakePlatformSystemDialogHandlerObject() {
  jobject object = SingletonObject(
      "com/roblox/protocols/systemdialog/PlatformSystemDialogHandler");
  SetBooleanFieldRaw(object, "available", JNI_TRUE);
  SetIntFieldRaw(object, "nextDialogId", 1);
  return object;
}

jobject MakeNativeTextBoxInfoObject() {
  jobject object =
      SingletonObject("com/roblox/engine/jni/model/NativeTextBoxInfo");
  SetStringFieldRaw(object, "text", "");
  SetStringFieldRaw(object, "currentText", "");
  SetStringFieldRaw(object, "hint", "");
  SetStringFieldRaw(object, "placeholder", "");
  SetStringFieldRaw(object, "keyboardType", "text");
  SetStringFieldRaw(object, "imeOptions", "none");
  SetIntFieldRaw(object, "cursorPosition", 0);
  SetIntFieldRaw(object, "selectionStart", 0);
  SetIntFieldRaw(object, "selectionEnd", 0);
  SetIntFieldRaw(object, "maxLength", 0);
  SetBooleanFieldRaw(object, "focused", JNI_FALSE);
  SetBooleanFieldRaw(object, "isFocused", JNI_FALSE);
  SetBooleanFieldRaw(object, "multiline", JNI_FALSE);
  SetBooleanFieldRaw(object, "secureTextEntry", JNI_FALSE);
  SetBooleanFieldRaw(object, "showKeyboard", JNI_FALSE);
  return object;
}

jobject MakeMessageBusConnectionObject() {
  jobject object =
      SingletonObject("com/roblox/universalapp/messagebus/Connection");
  SetBooleanFieldRaw(object, "connected", JNI_TRUE);
  SetBooleanFieldRaw(object, "isConnected", JNI_TRUE);
  SetIntFieldRaw(object, "id", 1);
  SetLongFieldRaw(object, "nativePtr", 1);
  return object;
}

void EnsureAndroidObjectGraph() {
  if (g_android_graph_ready) {
    return;
  }
  g_android_graph_ready = true;

  jobject activity = SingletonObject("com/roblox/client/RobloxActivity");
  jobject main_activity =
      SingletonObject("com/roblox/client/startup/MainGameActivity");
  jobject context = SingletonObject("android/content/Context");
  jobject app_context = SingletonObject("android/app/Application");
  jobject resources = SingletonObject("android/content/res/Resources");
  jobject asset_manager = SingletonObject("android/content/res/AssetManager");
  jobject package_manager = SingletonObject("android/content/pm/PackageManager");
  jobject class_loader = SingletonObject("java/lang/ClassLoader");
  jobject shared_preferences =
      SingletonObject("android/content/SharedPreferences");
  jobject preferences_editor =
      SingletonObject("android/content/SharedPreferences$Editor");
  jobject window = SingletonObject("android/view/Window");
  jobject window_manager = SingletonObject("android/view/WindowManager");
  jobject display_manager =
      SingletonObject("android/hardware/display/DisplayManager");
  jobject display = SingletonObject("android/view/Display");
  jobject decor_view = SingletonObject("android/view/View");
  jobject root_view = SingletonObject("android/view/ViewRootImpl");
  jobject surface_view = SingletonObject("android/view/SurfaceView");
  jobject surface_holder = SingletonObject("android/view/SurfaceHolder");
  jobject surface = SingletonObject("android/view/Surface");
  jobject configuration =
      SingletonObject("android/content/res/Configuration");
  jobject display_metrics = SingletonObject("android/util/DisplayMetrics");
  jobject app_info = MakeApplicationInfoObject();
  jobject package_info = MakePackageInfoObject();
  jobject system_dialog_handler = MakePlatformSystemDialogHandlerObject();
  jobject text_box_info = MakeNativeTextBoxInfoObject();
  jobject message_bus_connection = MakeMessageBusConnectionObject();
  jobject native_helper = MakeNativeHelperObject(main_activity);

  SetObjectFieldRaw(activity, "context", context);
  SetObjectFieldRaw(activity, "baseContext", context);
  SetObjectFieldRaw(activity, "applicationContext", app_context);
  SetObjectFieldRaw(activity, "resources", resources);
  SetObjectFieldRaw(activity, "assetManager", asset_manager);
  SetObjectFieldRaw(activity, "packageManager", package_manager);
  SetObjectFieldRaw(activity, "classLoader", class_loader);
  SetObjectFieldRaw(activity, "window", window);
  SetObjectFieldRaw(activity, "windowManager", window_manager);
  SetObjectFieldRaw(activity, "display", display);
  SetObjectFieldRaw(activity, "surface", surface);
  SetObjectFieldRaw(activity, "surfaceHolder", surface_holder);
  SetStringFieldRaw(activity, "packageName", "com.roblox.client");

  SetObjectFieldRaw(main_activity, "context", context);
  SetObjectFieldRaw(main_activity, "baseContext", context);
  SetObjectFieldRaw(main_activity, "applicationContext", app_context);
  SetObjectFieldRaw(main_activity, "resources", resources);
  SetObjectFieldRaw(main_activity, "assetManager", asset_manager);
  SetObjectFieldRaw(main_activity, "packageManager", package_manager);
  SetObjectFieldRaw(main_activity, "classLoader", class_loader);
  SetObjectFieldRaw(main_activity, "window", window);
  SetObjectFieldRaw(main_activity, "windowManager", window_manager);
  SetObjectFieldRaw(main_activity, "display", display);
  SetObjectFieldRaw(main_activity, "surface", surface);
  SetObjectFieldRaw(main_activity, "surfaceHolder", surface_holder);
  SetObjectFieldRaw(main_activity, "nativeHelper", native_helper);
  SetObjectFieldRaw(main_activity, "H", native_helper);
  SetStringFieldRaw(main_activity, "packageName", "com.roblox.client");
  SetBooleanFieldRaw(main_activity, "bootstrapStarted", JNI_FALSE);

  SetObjectFieldRaw(context, "applicationContext", app_context);
  SetObjectFieldRaw(context, "resources", resources);
  SetObjectFieldRaw(context, "assetManager", asset_manager);
  SetObjectFieldRaw(context, "assets", asset_manager);
  SetObjectFieldRaw(context, "packageManager", package_manager);
  SetObjectFieldRaw(context, "classLoader", class_loader);
  SetObjectFieldRaw(context, "sharedPreferences", shared_preferences);
  SetObjectFieldRaw(context, "applicationInfo", app_info);
  SetObjectFieldRaw(context, "packageInfo", package_info);
  SetObjectFieldRaw(context, "window", window);
  SetObjectFieldRaw(context, "windowManager", window_manager);
  SetObjectFieldRaw(context, "display", display);
  SetStringFieldRaw(context, "packageName", "com.roblox.client");

  SetObjectFieldRaw(app_context, "applicationContext", app_context);
  SetObjectFieldRaw(app_context, "resources", resources);
  SetObjectFieldRaw(app_context, "assetManager", asset_manager);
  SetObjectFieldRaw(app_context, "packageManager", package_manager);
  SetObjectFieldRaw(app_context, "classLoader", class_loader);
  SetObjectFieldRaw(app_context, "applicationInfo", app_info);
  SetStringFieldRaw(app_context, "packageName", "com.roblox.client");
  SetObjectFieldRaw(app_context, "systemDialogHandler", system_dialog_handler);
  SetObjectFieldRaw(app_context, "nativeTextBoxInfo", text_box_info);
  SetObjectFieldRaw(app_context, "messageBusConnection", message_bus_connection);

  SetObjectFieldRaw(resources, "assets", asset_manager);
  SetObjectFieldRaw(resources, "assetManager", asset_manager);
  SetObjectFieldRaw(resources, "configuration", configuration);
  SetObjectFieldRaw(resources, "displayMetrics", display_metrics);

  SetObjectFieldRaw(package_manager, "applicationInfo", app_info);
  SetObjectFieldRaw(package_manager, "packageInfo", package_info);

  SetObjectFieldRaw(shared_preferences, "editor", preferences_editor);
  SetBooleanFieldRaw(preferences_editor, "committed", JNI_TRUE);

  SetObjectFieldRaw(window, "decorView", decor_view);
  SetObjectFieldRaw(window, "rootView", decor_view);
  SetObjectFieldRaw(window, "surfaceView", surface_view);
  SetObjectFieldRaw(window, "windowManager", window_manager);

  SetObjectFieldRaw(window_manager, "defaultDisplay", display);
  SetObjectFieldRaw(display_manager, "display", display);

  SetObjectFieldRaw(decor_view, "rootView", decor_view);
  SetObjectFieldRaw(decor_view, "holder", surface_holder);
  SetObjectFieldRaw(decor_view, "surface", surface);
  SetObjectFieldRaw(decor_view, "display", display);
  SetIntFieldRaw(decor_view, "width", 1280);
  SetIntFieldRaw(decor_view, "height", 720);

  SetObjectFieldRaw(surface_view, "holder", surface_holder);
  SetObjectFieldRaw(surface_view, "surface", surface);
  SetObjectFieldRaw(surface_view, "rootView", root_view);
  SetIntFieldRaw(surface_view, "width", 1280);
  SetIntFieldRaw(surface_view, "height", 720);

  SetObjectFieldRaw(surface_holder, "surface", surface);
  SetObjectFieldRaw(surface_holder, "surfaceFrame",
                    SingletonObject("android/graphics/Rect"));
  SetBooleanFieldRaw(surface, "valid", JNI_TRUE);
  SetBooleanFieldRaw(surface, "isValid", JNI_TRUE);

  SetIntFieldRaw(display, "width", 1280);
  SetIntFieldRaw(display, "height", 720);
  SetIntFieldRaw(display, "rotation", 0);
  SetIntFieldRaw(display_metrics, "widthPixels", 1280);
  SetIntFieldRaw(display_metrics, "heightPixels", 720);
  SetIntFieldRaw(display_metrics, "densityDpi", 160);
  SetFloatFieldRaw(display_metrics, "density", 1.0f);
  SetFloatFieldRaw(display_metrics, "scaledDensity", 1.0f);
  SetFloatFieldRaw(display_metrics, "xdpi", 160.0f);
  SetFloatFieldRaw(display_metrics, "ydpi", 160.0f);
  const PlatformIdentity identity =
      CurrentVM() != nullptr ? CurrentVM()->GetPlatformIdentitySnapshot()
                             : PlatformIdentity{};
  SetIntFieldRaw(configuration, "orientation", 2);
  SetIntFieldRaw(configuration, "densityDpi", 160);
  SetIntFieldRaw(configuration, "screenWidthDp", 1280);
  SetIntFieldRaw(configuration, "screenHeightDp", 720);
  SetIntFieldRaw(configuration, "smallestScreenWidthDp", 720);
  SetIntFieldRaw(configuration, "touchscreen", identity.touch_enabled ? 3 : 1);
  SetIntFieldRaw(configuration, "keyboard", identity.keyboard_enabled ? 2 : 1);
  SetIntFieldRaw(configuration, "keyboardHidden",
                 identity.keyboard_enabled ? 1 : 2);
  SetIntFieldRaw(configuration, "hardKeyboardHidden",
                 identity.keyboard_enabled ? 1 : 2);
}

jobject SystemServiceObject(const std::string& service_name) {
  EnsureAndroidObjectGraph();
  if (service_name == "window") {
    return SingletonObject("android/view/WindowManager");
  }
  if (service_name == "display") {
    return SingletonObject("android/hardware/display/DisplayManager");
  }
  if (service_name == "audio") {
    return SingletonObject("android/media/AudioManager");
  }
  if (service_name == "input_method") {
    return SingletonObject("android/view/inputmethod/InputMethodManager");
  }
  if (service_name == "sensor") {
    return SingletonObject("android/hardware/SensorManager");
  }
  if (service_name == "connectivity") {
    return SingletonObject("android/net/ConnectivityManager");
  }
  if (service_name == "power") {
    return SingletonObject("android/os/PowerManager");
  }
  return SingletonObject("java/lang/Object");
}

jobject AndroidObjectForMethod(const char* name) {
  EnsureAndroidObjectGraph();
  if (!name) {
    return nullptr;
  }
  if (std::strcmp(name, "currentActivity") == 0 ||
      std::strcmp(name, "getActivity") == 0) {
    return SingletonObject("com/roblox/client/startup/MainGameActivity");
  }
  if (std::strcmp(name, "getApplicationContext") == 0 ||
      std::strcmp(name, "getBaseContext") == 0 ||
      std::strcmp(name, "getContext") == 0) {
    return SingletonObject("android/content/Context");
  }
  if (std::strcmp(name, "getAssets") == 0) {
    return SingletonObject("android/content/res/AssetManager");
  }
  if (std::strcmp(name, "getAssetManager") == 0) {
    return SingletonObject("android/content/res/AssetManager");
  }
  if (std::strcmp(name, "getResources") == 0) {
    return SingletonObject("android/content/res/Resources");
  }
  if (std::strcmp(name, "getClassLoader") == 0) {
    return SingletonObject("java/lang/ClassLoader");
  }
  if (std::strcmp(name, "getSharedPreferences") == 0) {
    return SingletonObject("android/content/SharedPreferences");
  }
  if (std::strcmp(name, "edit") == 0) {
    return SingletonObject("android/content/SharedPreferences$Editor");
  }
  if (std::strcmp(name, "getPackageManager") == 0) {
    return SingletonObject("android/content/pm/PackageManager");
  }
  if (std::strcmp(name, "getApplicationInfo") == 0) {
    return MakeApplicationInfoObject();
  }
  if (std::strcmp(name, "getDeviceStaticParams") == 0) {
    return MakeDeviceStaticParamsObject();
  }
  if (std::strcmp(name, "getPlatformSystemDialogHandler") == 0 ||
      std::strcmp(name, "getSystemDialogHandler") == 0) {
    return MakePlatformSystemDialogHandlerObject();
  }
  if (std::strcmp(name, "getImplementation") == 0) {
    return EngineJavaCallbackObject();
  }
  if (std::strcmp(name, "getTextBoxInfo") == 0 ||
      std::strcmp(name, "getNativeTextBoxInfo") == 0 ||
      std::strcmp(name, "getCurrentTextBoxInfo") == 0 ||
      std::strcmp(name, "nativeGetTextBoxInfo") == 0) {
    return MakeNativeTextBoxInfoObject();
  }
  if (std::strcmp(name, "getMessageBus") == 0 ||
      std::strcmp(name, "getMessageBusConnection") == 0 ||
      std::strcmp(name, "connect") == 0 ||
      std::strcmp(name, "subscribe") == 0 ||
      std::strcmp(name, "setRequestHandler") == 0 ||
      std::strcmp(name, "setRequestHandlerRaw") == 0 ||
      std::strcmp(name, "doSubscribeProtocolMethodResponseRaw") == 0 ||
      std::strcmp(name, "subscribeProtocolMethodResponseRaw") == 0) {
    return MakeMessageBusConnectionObject();
  }
  if (std::strcmp(name, "getPackageInfo") == 0) {
    return MakePackageInfoObject();
  }
  if (std::strcmp(name, "getWindow") == 0) {
    return SingletonObject("android/view/Window");
  }
  if (std::strcmp(name, "getWindowManager") == 0 ||
      std::strcmp(name, "getSystemService") == 0) {
    return SingletonObject("android/view/WindowManager");
  }
  if (std::strcmp(name, "getDefaultDisplay") == 0 ||
      std::strcmp(name, "getDisplay") == 0) {
    return SingletonObject("android/view/Display");
  }
  if (std::strcmp(name, "getDecorView") == 0 ||
      std::strcmp(name, "getRootView") == 0) {
    return SingletonObject("android/view/View");
  }
  if (std::strcmp(name, "getHolder") == 0) {
    return SingletonObject("android/view/SurfaceHolder");
  }
  if (std::strcmp(name, "getSurface") == 0) {
    return SingletonObject("android/view/Surface");
  }
  if (std::strcmp(name, "getFilesDir") == 0) {
    return MakeFileObject("/data/user/0/com.roblox.client/files");
  }
  if (std::strcmp(name, "getCacheDir") == 0) {
    return MakeFileObject("/data/user/0/com.roblox.client/cache");
  }
  if (std::strcmp(name, "getExternalFilesDir") == 0) {
    return MakeFileObject(
        "/sdcard/Android/data/com.roblox.client/files");
  }
  if (std::strcmp(name, "loadClass") == 0 ||
      std::strcmp(name, "findClass") == 0) {
    return SingletonObject("java/lang/Class");
  }
  return nullptr;
}

jobject ObjectResultForReceiverMethod(jobject obj, const char* name) {
  if (!name) {
    return nullptr;
  }
  EnsureAndroidObjectGraph();
  if (std::strcmp(name, "getBaseUrl") == 0 ||
      std::strcmp(name, "getBaseURL") == 0 ||
      std::strcmp(name, "getWwwBaseUrl") == 0 ||
      std::strcmp(name, "getWWWBaseUrl") == 0) {
    return MakeString("https://www.roblox.com");
  }
  if (std::strcmp(name, "getApiBaseUrl") == 0 ||
      std::strcmp(name, "getApiGatewayUrl") == 0) {
    return MakeString("https://apis.roblox.com");
  }
  if (std::strcmp(name, "getSettingsUrl") == 0 ||
      std::strcmp(name, "getClientSettingsUrl") == 0) {
    return MakeString(
        "https://clientsettingscdn.roblox.com/v2/settings-compressed/"
        "application/AndroidApp.zst");
  }
  if (std::strcmp(name, "getApplicationContext") == 0) {
    jobject value = ObjectFieldValue(obj, "applicationContext");
    return value ? value : SingletonObject("android/app/Application");
  }
  if (std::strcmp(name, "getNativeHelper") == 0) {
    jobject value = ObjectFieldValue(obj, "nativeHelper");
    if (!value) {
      value = ObjectFieldValue(obj, "H");
    }
    if (!value) {
      value = MakeNativeHelperObject(obj);
      SetObjectFieldRaw(obj, "nativeHelper", value);
      SetObjectFieldRaw(obj, "H", value);
    } else if (obj) {
      SetObjectFieldRaw(value, "activity", obj);
      SetObjectFieldRaw(value, "a", obj);
    }
    return value;
  }
  if (std::strcmp(name, "getBaseContext") == 0 ||
      std::strcmp(name, "getContext") == 0) {
    jobject value = ObjectFieldValue(obj, "baseContext");
    return value ? value : SingletonObject("android/content/Context");
  }
  if (std::strcmp(name, "getAssets") == 0) {
    jobject value = ObjectFieldValue(obj, "assetManager");
    return value ? value : SingletonObject("android/content/res/AssetManager");
  }
  if (std::strcmp(name, "getResources") == 0) {
    jobject value = ObjectFieldValue(obj, "resources");
    return value ? value : SingletonObject("android/content/res/Resources");
  }
  if (std::strcmp(name, "getClassLoader") == 0) {
    jobject value = ObjectFieldValue(obj, "classLoader");
    return value ? value : SingletonObject("java/lang/ClassLoader");
  }
  if (std::strcmp(name, "getPackageManager") == 0) {
    jobject value = ObjectFieldValue(obj, "packageManager");
    return value ? value : SingletonObject("android/content/pm/PackageManager");
  }
  if (std::strcmp(name, "getApplicationInfo") == 0) {
    jobject value = ObjectFieldValue(obj, "applicationInfo");
    return value ? value : MakeApplicationInfoObject();
  }
  if (std::strcmp(name, "getPackageInfo") == 0) {
    jobject value = ObjectFieldValue(obj, "packageInfo");
    return value ? value : MakePackageInfoObject();
  }
  if (std::strcmp(name, "getWindow") == 0) {
    jobject value = ObjectFieldValue(obj, "window");
    return value ? value : SingletonObject("android/view/Window");
  }
  if (std::strcmp(name, "getWindowManager") == 0) {
    jobject value = ObjectFieldValue(obj, "windowManager");
    return value ? value : SingletonObject("android/view/WindowManager");
  }
  if (std::strcmp(name, "getDefaultDisplay") == 0) {
    jobject value = ObjectFieldValue(obj, "defaultDisplay");
    return value ? value : SingletonObject("android/view/Display");
  }
  if (std::strcmp(name, "getDisplay") == 0) {
    jobject value = ObjectFieldValue(obj, "display");
    return value ? value : SingletonObject("android/view/Display");
  }
  if (std::strcmp(name, "getDecorView") == 0 ||
      std::strcmp(name, "getRootView") == 0) {
    jobject value = ObjectFieldValue(obj, "decorView");
    if (!value) {
      value = ObjectFieldValue(obj, "rootView");
    }
    return value ? value : SingletonObject("android/view/View");
  }
  if (std::strcmp(name, "getHolder") == 0) {
    jobject value = ObjectFieldValue(obj, "holder");
    return value ? value : SingletonObject("android/view/SurfaceHolder");
  }
  if (std::strcmp(name, "getSurface") == 0) {
    jobject value = ObjectFieldValue(obj, "surface");
    return value ? value : SingletonObject("android/view/Surface");
  }
  if (std::strcmp(name, "edit") == 0) {
    jobject value = ObjectFieldValue(obj, "editor");
    return value ? value
                 : SingletonObject("android/content/SharedPreferences$Editor");
  }
  if (jobject value = ObjectFieldValue(obj, name)) {
    return value;
  }
  std::string field_name = GetterFieldName(name);
  if (!field_name.empty()) {
    if (jobject value = ObjectFieldValue(obj, field_name.c_str())) {
      return value;
    }
  }
  return nullptr;
}

jobject ClassObjectForName(const std::string& requested_name) {
  std::string class_name = requested_name;
  std::replace(class_name.begin(), class_name.end(), '.', '/');
  auto cls = FallbackClassForName(class_name.empty() ? "java/lang/Object"
                                                     : class_name);
  return reinterpret_cast<jobject>(StoreClass(std::move(cls)));
}

jobject ObjectResultForMethodV(jobject obj, jmethodID method_id, va_list args) {
  const char* name = MethodName(method_id);
  if (IsJavaStringGetBytesMethod(obj, method_id)) {
    return JavaStringGetUtf8Bytes(obj, va_arg(args, jstring));
  }
  if (std::strcmp(name, "run") == 0 &&
      ObjectClassName(obj) ==
          "com/roblox/universalapp/messagebus/RequestHandlerRaw") {
    VM* vm = CurrentVM();
    return vm != nullptr ? vm->DispatchMessageBusRequestHandler(
                               obj, vm->GetJNIEnv(), va_arg(args, jstring))
                         : nullptr;
  }
  jobject local_storage_result = nullptr;
  if (LocalStorageObjectResultForMethodV(name, args, &local_storage_result)) {
    return local_storage_result;
  }
  jobject cookie_result = nullptr;
  if (CookieObjectResultForMethodV(name, args, &cookie_result)) {
    return cookie_result;
  }
  if (std::strcmp(name, "getSystemService") == 0) {
    jstring service = va_arg(args, jstring);
    return SystemServiceObject(StringFromJString(service));
  }
  if (std::strcmp(name, "loadClass") == 0 ||
      std::strcmp(name, "findClass") == 0 ||
      std::strcmp(name, "forName") == 0) {
    jstring requested_name = va_arg(args, jstring);
    return ClassObjectForName(StringFromJString(requested_name));
  }
  if (std::strcmp(name, "getString") == 0) {
    (void)va_arg(args, jstring);
    jstring default_value = va_arg(args, jstring);
    return default_value ? default_value : MakeString("");
  }
  if (std::strcmp(name, "getProperty") == 0) {
    jstring property_name = va_arg(args, jstring);
    std::string property = StringFromJString(property_name);
    if (property == "android.media.property.OUTPUT_SAMPLE_RATE") {
      return MakeString("48000");
    }
    if (property == "android.media.property.OUTPUT_FRAMES_PER_BUFFER") {
      return MakeString("512");
    }
    return MakeString("");
  }
  if (std::strcmp(name, "get") == 0) {
    (void)va_arg(args, jint);
    return nullptr;
  }
  jobject receiver_result = ObjectResultForReceiverMethod(obj, name);
  if (receiver_result) {
    return receiver_result;
  }
  return ObjectResultForMethod(method_id);
}

jobject ObjectFieldValue(jobject obj, const char* field_name) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto* pseudo_object = PseudoObjectFromRef(obj);
  if (!pseudo_object || !field_name) {
    return nullptr;
  }
  auto it = pseudo_object->object_fields.find(field_name);
  return it == pseudo_object->object_fields.end() ? nullptr : it->second;
}

jint IntFieldValue(jobject obj, const char* field_name) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto* pseudo_object = PseudoObjectFromRef(obj);
  if (!pseudo_object || !field_name) {
    return 0;
  }
  auto it = pseudo_object->int_fields.find(field_name);
  return it == pseudo_object->int_fields.end() ? 0 : it->second;
}

jboolean BooleanFieldValue(jobject obj, const char* field_name) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto* pseudo_object = PseudoObjectFromRef(obj);
  if (!pseudo_object || !field_name) {
    return JNI_FALSE;
  }
  auto it = pseudo_object->boolean_fields.find(field_name);
  return it == pseudo_object->boolean_fields.end() ? JNI_FALSE : it->second;
}

jlong LongFieldValue(jobject obj, const char* field_name) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto* pseudo_object = PseudoObjectFromRef(obj);
  if (!pseudo_object || !field_name) {
    return 0;
  }
  auto it = pseudo_object->long_fields.find(field_name);
  return it == pseudo_object->long_fields.end() ? 0 : it->second;
}

jfloat FloatFieldValue(jobject obj, const char* field_name) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto* pseudo_object = PseudoObjectFromRef(obj);
  if (!pseudo_object || !field_name) {
    return 0.0f;
  }
  auto it = pseudo_object->float_fields.find(field_name);
  return it == pseudo_object->float_fields.end() ? 0.0f : it->second;
}

jboolean BooleanResultForReceiverMethod(jobject obj, const char* name) {
  if (!name) {
    return JNI_FALSE;
  }
  if (std::strcmp(name, "isValid") == 0) {
    jboolean value = BooleanFieldValue(obj, "isValid");
    return value ? value : BooleanFieldValue(obj, "valid");
  }
  if (std::strcmp(name, "commit") == 0 ||
      std::strcmp(name, "apply") == 0 ||
      std::strcmp(name, "contains") == 0 ||
      std::strcmp(name, "attachObserver") == 0 ||
      std::strcmp(name, "detachObserver") == 0 ||
      std::strcmp(name, "isAvailable") == 0 ||
      std::strcmp(name, "isConnected") == 0 ||
      std::strcmp(name, "isEmpty") == 0) {
    return JNI_TRUE;
  }
  if (std::strcmp(name, "isDestroyed") == 0 ||
      std::strcmp(name, "isFinishing") == 0 ||
      std::strcmp(name, "isChangingConfigurations") == 0 ||
      std::strcmp(name, "isBluetoothA2dpOn") == 0 ||
      std::strcmp(name, "isBluetoothScoOn") == 0) {
    return JNI_FALSE;
  }
  jboolean value = BooleanFieldValue(obj, name);
  if (value) {
    return value;
  }
  std::string field_name = GetterFieldName(name);
  return field_name.empty() ? JNI_FALSE
                            : BooleanFieldValue(obj, field_name.c_str());
}

bool PackageManagerBooleanResultForMethodV(jobject obj, jmethodID method_id,
                                           va_list args, jboolean* result) {
  if (result == nullptr ||
      ObjectClassName(obj) != "android/content/pm/PackageManager" ||
      std::strcmp(MethodName(method_id), "hasSystemFeature") != 0) {
    return false;
  }
  const jstring feature = va_arg(args, jstring);
  const std::string feature_name = StringFromJString(feature);
  const PlatformIdentity identity =
      CurrentVM() != nullptr ? CurrentVM()->GetPlatformIdentitySnapshot()
                             : PlatformIdentity{};
  if (feature_name == "android.hardware.type.pc") {
    *result = identity.pc_hardware ? JNI_TRUE : JNI_FALSE;
  } else if (feature_name == "android.hardware.touchscreen" ||
             feature_name == "android.hardware.touchscreen.multitouch" ||
             feature_name ==
                 "android.hardware.touchscreen.multitouch.distinct" ||
             feature_name ==
                 "android.hardware.touchscreen.multitouch.jazzhand") {
    *result = identity.touch_enabled ? JNI_TRUE : JNI_FALSE;
  } else {
    *result = JNI_FALSE;
  }
  return true;
}

bool PackageManagerBooleanResultForMethodA(jobject obj, jmethodID method_id,
                                           const jvalue* args,
                                           jboolean* result) {
  if (result == nullptr ||
      ObjectClassName(obj) != "android/content/pm/PackageManager" ||
      std::strcmp(MethodName(method_id), "hasSystemFeature") != 0) {
    return false;
  }
  const std::string feature_name =
      args != nullptr ? StringFromJString(reinterpret_cast<jstring>(args[0].l))
                      : std::string();
  const PlatformIdentity identity =
      CurrentVM() != nullptr ? CurrentVM()->GetPlatformIdentitySnapshot()
                             : PlatformIdentity{};
  if (feature_name == "android.hardware.type.pc") {
    *result = identity.pc_hardware ? JNI_TRUE : JNI_FALSE;
  } else if (feature_name.rfind("android.hardware.touchscreen", 0) == 0) {
    *result = identity.touch_enabled ? JNI_TRUE : JNI_FALSE;
  } else {
    *result = JNI_FALSE;
  }
  return true;
}

jint IntResultForReceiverMethod(jobject obj, const char* name) {
  if (!name) {
    return 0;
  }
  if (std::strcmp(name, "size") == 0 ||
      std::strcmp(name, "length") == 0) {
    return IntFieldValue(obj, name);
  }
  jint value = IntFieldValue(obj, name);
  if (value != 0) {
    return value;
  }
  std::string field_name = GetterFieldName(name);
  if (!field_name.empty()) {
    value = IntFieldValue(obj, field_name.c_str());
    if (value != 0) {
      return value;
    }
  }
  return IntResultForName(name);
}

PseudoArray* ArrayFromRef(jarray array) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto it = g_arrays.find(array);
  return it == g_arrays.end() ? nullptr : it->second;
}

bool IsFmodAudioDeviceMethod(jobject obj, jmethodID method_id,
                             const char* name, const char* signature) {
  return obj != nullptr && method_id != nullptr &&
         ObjectClassName(obj) == "org/fmod/AudioDevice" &&
         std::strcmp(MethodName(method_id), name) == 0 &&
         std::strcmp(MethodSignature(method_id), signature) == 0;
}

constexpr char kRobloxNativeGlJavaInterfaceClass[] =
    "com/roblox/engine/jni/NativeGLJavaInterface";
constexpr char kRobloxEngineJavaCallbackClass[] =
    "com/roblox/engine/jni/EngineJavaCallback2";
constexpr char kRobloxNativeHelperClass[] =
    "com/roblox/client/startup/NativeHelper";
constexpr char kRobloxShowKeyboardSignature[] =
    "(JZ[BLcom/roblox/engine/jni/model/NativeTextBoxInfo;)V";

void ClearSensitiveString(std::string* value) {
  if (value == nullptr) {
    return;
  }
  volatile char* bytes = value->empty() ? nullptr : value->data();
  for (std::size_t index = 0; index < value->size(); ++index) {
    bytes[index] = '\0';
  }
  value->clear();
}

bool IsValidUtf8(const std::vector<jbyte>& bytes) {
  std::size_t index = 0;
  while (index < bytes.size()) {
    const auto first = static_cast<std::uint8_t>(bytes[index]);
    std::size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    if (first <= 0x7f) {
      ++index;
      continue;
    }
    if ((first & 0xe0) == 0xc0) {
      continuation_count = 1;
      code_point = first & 0x1f;
      if (code_point < 2) {
        return false;
      }
    } else if ((first & 0xf0) == 0xe0) {
      continuation_count = 2;
      code_point = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
      continuation_count = 3;
      code_point = first & 0x07;
    } else {
      return false;
    }
    if (continuation_count > bytes.size() - index - 1) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto next = static_cast<std::uint8_t>(bytes[index + offset]);
      if ((next & 0xc0) != 0x80) {
        return false;
      }
      code_point = (code_point << 6) | (next & 0x3f);
    }
    if ((continuation_count == 2 && code_point < 0x800) ||
        (continuation_count == 3 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) ||
        code_point > 0x10ffff) {
      return false;
    }
    index += continuation_count + 1;
  }
  return true;
}

bool SnapshotRobloxTextInputShow(jlong text_box, jboolean show_native_input,
                                 jbyteArray text_array, jobject info_object,
                                 RobloxTextInputShowRequest* request) {
  if (text_box <= 0 || text_array == nullptr || info_object == nullptr ||
      request == nullptr) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto array = g_arrays.find(text_array);
  auto* info = PseudoObjectFromRef(info_object);
  if (array == g_arrays.end() || array->second == nullptr || info == nullptr ||
      ObjectClassName(info_object) !=
          "com/roblox/engine/jni/model/NativeTextBoxInfo" ||
      array->second->bytes.size() > 32768 ||
      !IsValidUtf8(array->second->bytes)) {
    return false;
  }

  RobloxTextInputShowRequest snapshot;
  snapshot.text_box = static_cast<std::int64_t>(text_box);
  snapshot.show_native_input = show_native_input == JNI_TRUE;
  if (!array->second->bytes.empty()) {
    snapshot.text.assign(
        reinterpret_cast<const char*>(array->second->bytes.data()),
        array->second->bytes.size());
  }
  snapshot.info.x = FloatFieldValue(info_object, "x");
  snapshot.info.y = FloatFieldValue(info_object, "y");
  snapshot.info.width = FloatFieldValue(info_object, "width");
  snapshot.info.height = FloatFieldValue(info_object, "height");
  snapshot.info.font_size = FloatFieldValue(info_object, "fontSize");
  snapshot.info.multiline = BooleanFieldValue(info_object, "multiline");
  snapshot.info.x_alignment = IntFieldValue(info_object, "xAlignment");
  snapshot.info.y_alignment = IntFieldValue(info_object, "yAlignment");
  snapshot.info.text_color = IntFieldValue(info_object, "textColor");
  snapshot.info.font = IntFieldValue(info_object, "font");
  snapshot.info.text_input_type = IntFieldValue(info_object, "textInputType");
  snapshot.info.return_key_type = IntFieldValue(info_object, "returnKeyType");
  snapshot.info.manual_focus_release =
      BooleanFieldValue(info_object, "manualFocusRelease");
  snapshot.info.text_wrapped = BooleanFieldValue(info_object, "textWrapped");
  *request = std::move(snapshot);
  return true;
}

bool IsRobloxTextInputStaticMethod(jclass clazz, jmethodID method_id,
                                   const char* name,
                                   const char* signature) {
  const auto cls = ClassFromJClass(clazz);
  return cls != nullptr &&
         cls->GetName() == kRobloxNativeGlJavaInterfaceClass &&
         std::strcmp(MethodName(method_id), name) == 0 &&
         std::strcmp(MethodSignature(method_id), signature) == 0;
}

bool IsRobloxTextInputInstanceMethod(jobject obj, jmethodID method_id,
                                     const char* name,
                                     const char* signature) {
  return obj != nullptr && ObjectClassName(obj) == kRobloxNativeHelperClass &&
         std::strcmp(MethodName(method_id), name) == 0 &&
         std::strcmp(MethodSignature(method_id), signature) == 0;
}

bool IsRobloxEngineJavaTextInputMethod(jobject obj, jmethodID method_id,
                                       const char* name,
                                       const char* signature) {
  return obj != nullptr && method_id != nullptr &&
         ObjectClassName(obj) == kRobloxEngineJavaCallbackClass &&
         std::strcmp(MethodName(method_id), name) == 0 &&
         std::strcmp(MethodSignature(method_id), signature) == 0;
}

bool HandleRobloxExperienceLifecycleVoidMethod(jobject obj,
                                                jmethodID method_id) {
  if (obj == nullptr || method_id == nullptr ||
      ObjectClassName(obj) != kRobloxNativeHelperClass ||
      std::strcmp(MethodName(method_id),
                  "gameActivity_onLuaAppDidReturn") != 0 ||
      std::strcmp(MethodSignature(method_id), "()V") != 0) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxExperienceLuaAppDidReturn();
  }
  return true;
}

bool HandleRobloxExperienceLifecycleStaticVoidMethod(jclass clazz,
                                                      jmethodID method_id) {
  const std::shared_ptr<Class> method_class = ClassFromJClass(clazz);
  if (method_class == nullptr || method_id == nullptr ||
      method_class->GetName() != kRobloxNativeGlJavaInterfaceClass ||
      std::strcmp(MethodName(method_id), "gameDidLeave") != 0 ||
      std::strcmp(MethodSignature(method_id), "()V") != 0) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxExperienceLuaAppDidReturn();
  }
  return true;
}

bool DispatchRobloxTextInputShow(jlong text_box, jboolean show_native_input,
                                 jbyteArray text, jobject info) {
  RobloxTextInputShowRequest request;
  if (!SnapshotRobloxTextInputShow(text_box, show_native_input, text, info,
                                   &request)) {
    return true;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxTextInputShow(request);
  }
  ClearSensitiveString(&request.text);
  return true;
}

void DispatchRobloxTextInputReplaceText(jstring text) {
  if (text == nullptr) {
    return;
  }
  std::string snapshot;
  {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    snapshot = StringFromJString(text);
  }
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxTextInputReplaceText(snapshot);
  }
  ClearSensitiveString(&snapshot);
}

bool HandleRobloxTextInputStaticVoidMethodV(jclass clazz,
                                             jmethodID method_id,
                                             va_list args) {
  if (IsRobloxTextInputStaticMethod(clazz, method_id, "showKeyboard",
                                    kRobloxShowKeyboardSignature)) {
    const jlong text_box = va_arg(args, jlong);
    const auto show_native_input =
        static_cast<jboolean>(va_arg(args, jint));
    const auto text = va_arg(args, jbyteArray);
    const auto info = va_arg(args, jobject);
    return DispatchRobloxTextInputShow(text_box, show_native_input, text, info);
  }
  if (IsRobloxTextInputStaticMethod(clazz, method_id, "hideKeyboard", "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputHide();
    }
    return true;
  }
  if (IsRobloxTextInputStaticMethod(
          clazz, method_id, "onLuaTextBoxChangedCallback",
          "(Ljava/lang/String;)V")) {
    DispatchRobloxTextInputReplaceText(va_arg(args, jstring));
    return true;
  }
  if (IsRobloxTextInputStaticMethod(
          clazz, method_id, "onLuaTextBoxPropertyChangedCallback", "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputPropertiesChanged();
    }
    return true;
  }
  return false;
}

bool HandleRobloxTextInputStaticVoidMethodA(jclass clazz,
                                             jmethodID method_id,
                                             const jvalue* args) {
  if (IsRobloxTextInputStaticMethod(clazz, method_id, "showKeyboard",
                                    kRobloxShowKeyboardSignature)) {
    if (args != nullptr) {
      return DispatchRobloxTextInputShow(
          args[0].j, args[1].z, static_cast<jbyteArray>(args[2].l), args[3].l);
    }
    return true;
  }
  if (IsRobloxTextInputStaticMethod(clazz, method_id, "hideKeyboard", "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputHide();
    }
    return true;
  }
  if (IsRobloxTextInputStaticMethod(
          clazz, method_id, "onLuaTextBoxChangedCallback",
          "(Ljava/lang/String;)V")) {
    if (args != nullptr) {
      DispatchRobloxTextInputReplaceText(static_cast<jstring>(args[0].l));
    }
    return true;
  }
  if (IsRobloxTextInputStaticMethod(
          clazz, method_id, "onLuaTextBoxPropertyChangedCallback", "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputPropertiesChanged();
    }
    return true;
  }
  return false;
}

bool HandleRobloxTextInputInstanceVoidMethodV(jobject obj,
                                               jmethodID method_id,
                                               va_list args) {
  if (IsRobloxTextInputInstanceMethod(obj, method_id,
                                      "gameActivity_showKeyboard",
                                      kRobloxShowKeyboardSignature) ||
      IsRobloxEngineJavaTextInputMethod(obj, method_id, "q",
                                        kRobloxShowKeyboardSignature)) {
    const jlong text_box = va_arg(args, jlong);
    const auto show_native_input =
        static_cast<jboolean>(va_arg(args, jint));
    const auto text = va_arg(args, jbyteArray);
    const auto info = va_arg(args, jobject);
    return DispatchRobloxTextInputShow(text_box, show_native_input, text, info);
  }
  if (IsRobloxTextInputInstanceMethod(obj, method_id,
                                      "gameActivity_hideKeyboard", "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputHide();
    }
    return true;
  }
  if (IsRobloxEngineJavaTextInputMethod(obj, method_id, "d", "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputHide();
    }
    return true;
  }
  if (IsRobloxTextInputInstanceMethod(
          obj, method_id, "gameActivity_onLuaTextBoxChanged",
          "(Ljava/lang/String;)V")) {
    DispatchRobloxTextInputReplaceText(va_arg(args, jstring));
    return true;
  }
  if (IsRobloxEngineJavaTextInputMethod(obj, method_id, "g",
                                        "(Ljava/lang/String;)V")) {
    DispatchRobloxTextInputReplaceText(va_arg(args, jstring));
    return true;
  }
  if (IsRobloxTextInputInstanceMethod(
          obj, method_id, "gameActivity_onLuaTextBoxPropertyChanged", "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputPropertiesChanged();
    }
    return true;
  }
  if (IsRobloxEngineJavaTextInputMethod(obj, method_id, "h", "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputPropertiesChanged();
    }
    return true;
  }
  return false;
}

bool HandleRobloxTextInputInstanceVoidMethodA(jobject obj,
                                               jmethodID method_id,
                                               const jvalue* args) {
  if (IsRobloxTextInputInstanceMethod(obj, method_id,
                                      "gameActivity_showKeyboard",
                                      kRobloxShowKeyboardSignature) ||
      IsRobloxEngineJavaTextInputMethod(obj, method_id, "q",
                                        kRobloxShowKeyboardSignature)) {
    if (args != nullptr) {
      return DispatchRobloxTextInputShow(
          args[0].j, args[1].z, static_cast<jbyteArray>(args[2].l), args[3].l);
    }
    return true;
  }
  if (IsRobloxTextInputInstanceMethod(obj, method_id,
                                      "gameActivity_hideKeyboard", "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputHide();
    }
    return true;
  }
  if (IsRobloxEngineJavaTextInputMethod(obj, method_id, "d", "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputHide();
    }
    return true;
  }
  if (IsRobloxTextInputInstanceMethod(
          obj, method_id, "gameActivity_onLuaTextBoxChanged",
          "(Ljava/lang/String;)V")) {
    if (args != nullptr) {
      DispatchRobloxTextInputReplaceText(static_cast<jstring>(args[0].l));
    }
    return true;
  }
  if (IsRobloxEngineJavaTextInputMethod(obj, method_id, "g",
                                        "(Ljava/lang/String;)V")) {
    if (args != nullptr) {
      DispatchRobloxTextInputReplaceText(static_cast<jstring>(args[0].l));
    }
    return true;
  }
  if (IsRobloxTextInputInstanceMethod(
          obj, method_id, "gameActivity_onLuaTextBoxPropertyChanged", "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputPropertiesChanged();
    }
    return true;
  }
  if (IsRobloxEngineJavaTextInputMethod(obj, method_id, "h", "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxTextInputPropertiesChanged();
    }
    return true;
  }
  return false;
}

bool DispatchFmodAudioDeviceInit(jobject obj, jint channels,
                                 jint sample_rate_hz,
                                 jint block_size_frames, jint block_count) {
  VM* vm = CurrentVM();
  if (vm == nullptr) {
    return false;
  }
  return vm->DispatchFmodAudioDeviceInit(
      obj, channels, sample_rate_hz, block_size_frames, block_count);
}

bool HandleFmodAudioDeviceBooleanMethodV(jobject obj, jmethodID method_id,
                                         va_list args, jboolean* result) {
  if (!IsFmodAudioDeviceMethod(obj, method_id, "init", "(IIII)Z")) {
    return false;
  }
  const jint channels = va_arg(args, jint);
  const jint sample_rate_hz = va_arg(args, jint);
  const jint block_size_frames = va_arg(args, jint);
  const jint block_count = va_arg(args, jint);
  const bool initialized = DispatchFmodAudioDeviceInit(
      obj, channels, sample_rate_hz, block_size_frames, block_count);
  if (!initialized) {
    std::cerr << "  [mocktail][audio] FMOD AudioDevice.init rejected by "
                 "host bridge\n";
  }
  if (result != nullptr) {
    *result = initialized ? JNI_TRUE : JNI_FALSE;
  }
  return true;
}

bool HandleFmodAudioDeviceBooleanMethodA(jobject obj, jmethodID method_id,
                                         const jvalue* args,
                                         jboolean* result) {
  if (!IsFmodAudioDeviceMethod(obj, method_id, "init", "(IIII)Z")) {
    return false;
  }
  const bool initialized =
      args != nullptr &&
      DispatchFmodAudioDeviceInit(obj, args[0].i, args[1].i, args[2].i,
                                  args[3].i);
  if (!initialized) {
    std::cerr << "  [mocktail][audio] FMOD AudioDevice.init rejected by "
                 "host bridge\n";
  }
  if (result != nullptr) {
    *result = initialized ? JNI_TRUE : JNI_FALSE;
  }
  return true;
}

bool DispatchFmodAudioDeviceWrite(jobject obj, jbyteArray array,
                                  jint requested_size) {
  if (requested_size <= 0) {
    return false;
  }
  const std::size_t size = static_cast<std::size_t>(requested_size);
  std::unique_ptr<std::uint8_t[]> snapshot;
  {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    const auto found = g_arrays.find(array);
    if (found == g_arrays.end() || found->second == nullptr ||
        size > found->second->bytes.size()) {
      return false;
    }
    snapshot.reset(new (std::nothrow) std::uint8_t[size]);
    if (snapshot == nullptr) {
      return false;
    }
    std::memcpy(snapshot.get(), found->second->bytes.data(), size);
  }
  VM* vm = CurrentVM();
  if (vm == nullptr) {
    return false;
  }
  return vm->DispatchFmodAudioDeviceWrite(obj, snapshot.get(), size);
}

bool HandleFmodAudioDeviceVoidMethodV(jobject obj, jmethodID method_id,
                                      va_list args) {
  if (IsFmodAudioDeviceMethod(obj, method_id, "write", "([BI)V")) {
    const auto array = va_arg(args, jbyteArray);
    const jint size = va_arg(args, jint);
    (void)DispatchFmodAudioDeviceWrite(obj, array, size);
    return true;
  }
  if (IsFmodAudioDeviceMethod(obj, method_id, "close", "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchFmodAudioDeviceClose(obj);
    }
    return true;
  }
  return false;
}

bool HandleFmodAudioDeviceVoidMethodA(jobject obj, jmethodID method_id,
                                      const jvalue* args) {
  if (IsFmodAudioDeviceMethod(obj, method_id, "write", "([BI)V")) {
    const auto array =
        args == nullptr ? nullptr : static_cast<jbyteArray>(args[0].l);
    const jint size = args == nullptr ? -1 : args[1].i;
    (void)DispatchFmodAudioDeviceWrite(obj, array, size);
    return true;
  }
  if (IsFmodAudioDeviceMethod(obj, method_id, "close", "()V")) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchFmodAudioDeviceClose(obj);
    }
    return true;
  }
  return false;
}

jbyteArray MakeByteArray(jsize len) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto array = std::make_unique<PseudoArray>();
  if (len > 0) {
    array->bytes.resize(static_cast<std::size_t>(len));
  }
  jbyteArray ref = reinterpret_cast<jbyteArray>(array.get());
  g_arrays[ref] = array.get();
  g_array_storage.push_back(std::move(array));
  return ref;
}

jfloatArray MakeFloatArray(jsize len) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto array = std::make_unique<PseudoArray>();
  if (len > 0) {
    array->floats.resize(static_cast<std::size_t>(len));
  }
  jfloatArray ref = reinterpret_cast<jfloatArray>(array.get());
  g_arrays[ref] = array.get();
  g_array_storage.push_back(std::move(array));
  return ref;
}

jobjectArray MakeObjectArray(jsize len, jobject init) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto array = std::make_unique<PseudoArray>();
  if (len > 0) {
    array->objects.resize(static_cast<std::size_t>(len), init);
  }
  jobjectArray ref = reinterpret_cast<jobjectArray>(array.get());
  g_arrays[ref] = array.get();
  g_array_storage.push_back(std::move(array));
  return ref;
}

jobject StaticObjectResultForMethod(jmethodID method_id) {
  const char* name = MethodName(method_id);
  const PlatformIdentity identity = CurrentPlatformIdentity();
  jobject android_object = AndroidObjectForMethod(name);
  if (android_object) {
    return android_object;
  }
  if (std::strcmp(name, "getLocale") == 0 ||
      std::strcmp(name, "getRobloxLocale") == 0 ||
      std::strcmp(name, "getGameLocale") == 0) {
    return MakeString("en_us");
  }
  if (std::strcmp(name, "getUsername") == 0 ||
      std::strcmp(name, "getLastLoggedInUser") == 0) {
    return MakeString(RobloxIdentityForJava().username.c_str());
  }
  if (std::strcmp(name, "getDisplayName") == 0 ||
      std::strcmp(name, "getAlternateName") == 0) {
    return MakeString(RobloxIdentityForJava().display_name.c_str());
  }
  if (std::strcmp(name, "getPlatformName") == 0) {
    return MakeString(identity.platform_name.c_str());
  }
  if (std::strcmp(name, "getBaseUrl") == 0 ||
      std::strcmp(name, "getBaseURL") == 0 ||
      std::strcmp(name, "getWwwBaseUrl") == 0 ||
      std::strcmp(name, "getWWWBaseUrl") == 0) {
    return MakeString("https://www.roblox.com");
  }
  if (std::strcmp(name, "getApiBaseUrl") == 0 ||
      std::strcmp(name, "getApiGatewayUrl") == 0) {
    return MakeString("https://apis.roblox.com");
  }
  if (std::strcmp(name, "getSettingsUrl") == 0 ||
      std::strcmp(name, "getClientSettingsUrl") == 0) {
    return MakeString(
        "https://clientsettingscdn.roblox.com/v2/settings-compressed/"
        "application/AndroidApp.zst");
  }
  if (std::strcmp(name, "getDevice") == 0) {
    return MakeString(identity.device_code.c_str());
  }
  if (std::strcmp(name, "getBrand") == 0) {
    return MakeString(identity.brand.c_str());
  }
  if (std::strcmp(name, "getDeviceModel") == 0 ||
      std::strcmp(name, "getModel") == 0) {
    return MakeString(identity.model.c_str());
  }
  if (std::strcmp(name, "getProduct") == 0) {
    return MakeString(identity.device_code.c_str());
  }
  if (std::strcmp(name, "getBuildId") == 0) {
    return MakeString("MOCKTAIL");
  }
  if (std::strcmp(name, "getBuildType") == 0) {
    return MakeString("user");
  }
  if (std::strcmp(name, "getBuildRelease") == 0) {
    return MakeString("13");
  }
  if (std::strcmp(name, "getDeviceManufacturer") == 0 ||
      std::strcmp(name, "getManufacturer") == 0) {
    return MakeString(identity.manufacturer.c_str());
  }
  if (std::strcmp(name, "getPackageName") == 0) {
    return MakeString("com.roblox.client");
  }
  if (std::strcmp(name, "getInstallerPackageName") == 0) {
    return MakeString("com.android.vending");
  }
  if (std::strcmp(name, "getAbsolutePath") == 0 ||
      std::strcmp(name, "getCanonicalPath") == 0 ||
      std::strcmp(name, "getPath") == 0) {
    return MakeString("/data/user/0/com.roblox.client/files");
  }
  if (std::strcmp(name, "getString") == 0 ||
      std::strcmp(name, "optString") == 0) {
    return MakeString("");
  }
  if (std::strcmp(name, "getProperty") == 0) {
    return MakeString("");
  }
  if (std::strcmp(name, "getTheme") == 0) {
    return MakeString("dark");
  }
  if (std::strcmp(name, "getFilesDir") == 0) {
    return MakeString("/data/user/0/com.roblox.client/files");
  }
  if (std::strcmp(name, "getAppVersion") == 0) {
    const char* app_version = std::getenv("MOCKTAIL_ROBLOX_VERSION");
    return MakeString(app_version != nullptr ? app_version : "unknown");
  }
  if (std::strcmp(name, "getLastLoggedInUserId") == 0) {
    return MakeString(std::to_string(RobloxIdentityForJava().user_id).c_str());
  }
  if (std::strcmp(name, "getPublicIPv4Addresseses") == 0) {
    return MakeString("127.0.0.1");
  }
  if (std::strcmp(name, "getVideoCodecs") == 0) {
    return MakeObjectArray(0, nullptr);
  }
  if (std::strcmp(name, "getAssetManager") == 0) {
    return SingletonObject("android/content/res/AssetManager");
  }
  if (std::strcmp(name, "getPlatformSystemDialogHandler") == 0 ||
      std::strcmp(name, "getSystemDialogHandler") == 0) {
    return MakePlatformSystemDialogHandlerObject();
  }
  if (std::strcmp(name, "getTextBoxInfo") == 0 ||
      std::strcmp(name, "getNativeTextBoxInfo") == 0 ||
      std::strcmp(name, "getCurrentTextBoxInfo") == 0 ||
      std::strcmp(name, "nativeGetTextBoxInfo") == 0) {
    return MakeNativeTextBoxInfoObject();
  }
  if (std::strcmp(name, "getText") == 0 ||
      std::strcmp(name, "getCurrentText") == 0 ||
      std::strcmp(name, "getPlaceholder") == 0 ||
      std::strcmp(name, "getHint") == 0 ||
      std::strcmp(name, "getLastRaw") == 0) {
    return MakeString("");
  }
  if (std::strcmp(name, "getMessageBus") == 0 ||
      std::strcmp(name, "getMessageBusConnection") == 0 ||
      std::strcmp(name, "connect") == 0 ||
      std::strcmp(name, "subscribe") == 0 ||
      std::strcmp(name, "setRequestHandler") == 0 ||
      std::strcmp(name, "setRequestHandlerRaw") == 0 ||
      std::strcmp(name, "doSubscribeProtocolMethodResponseRaw") == 0 ||
      std::strcmp(name, "subscribeProtocolMethodResponseRaw") == 0) {
    return MakeMessageBusConnectionObject();
  }
  return nullptr;
}

jobject ObjectResultForMethod(jmethodID method_id) {
  const char* name = MethodName(method_id);
  const PlatformIdentity identity = CurrentPlatformIdentity();
  jobject android_object = AndroidObjectForMethod(name);
  if (android_object) {
    return android_object;
  }
  if (std::strcmp(name, "getPackageName") == 0) {
    return MakeString("com.roblox.client");
  }
  if (std::strcmp(name, "getDevice") == 0) {
    return MakeString(identity.device_code.c_str());
  }
  if (std::strcmp(name, "getBrand") == 0) {
    return MakeString(identity.brand.c_str());
  }
  if (std::strcmp(name, "getDeviceModel") == 0 ||
      std::strcmp(name, "getModel") == 0) {
    return MakeString(identity.model.c_str());
  }
  if (std::strcmp(name, "getProduct") == 0) {
    return MakeString(identity.device_code.c_str());
  }
  if (std::strcmp(name, "getBuildId") == 0) {
    return MakeString("MOCKTAIL");
  }
  if (std::strcmp(name, "getBuildType") == 0) {
    return MakeString("user");
  }
  if (std::strcmp(name, "getBuildRelease") == 0) {
    return MakeString("13");
  }
  if (std::strcmp(name, "getDeviceManufacturer") == 0 ||
      std::strcmp(name, "getManufacturer") == 0) {
    return MakeString(identity.manufacturer.c_str());
  }
  if (std::strcmp(name, "getInstallerPackageName") == 0) {
    return MakeString("com.android.vending");
  }
  if (std::strcmp(name, "getAbsolutePath") == 0 ||
      std::strcmp(name, "getCanonicalPath") == 0 ||
      std::strcmp(name, "getPath") == 0) {
    return MakeString("/data/user/0/com.roblox.client/files");
  }
  if (std::strcmp(name, "getString") == 0 ||
      std::strcmp(name, "optString") == 0) {
    return MakeString("");
  }
  if (std::strcmp(name, "getProperty") == 0) {
    return MakeString("");
  }
  return nullptr;
}

void DispatchAppBridgeNotification(JNIEnv* env, jstring type, jstring data) {
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxAppBridgeNotification(env, type, data);
  }
  jobject listener = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    listener = g_app_bridge_notification_listener;
  }
  if (!listener || !env) {
    return;
  }
  jclass listener_class = env->GetObjectClass(listener);
  jmethodID method = env->GetMethodID(
      listener_class, "a", "(Ljava/lang/String;Ljava/lang/String;)V");
  if (method) {
    env->CallVoidMethod(listener, method, type, data);
  }
}

void DispatchDataModelNotification(JNIEnv* env, jstring type, jstring data) {
  jobject callback = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    callback = g_engine_java_callback;
  }
  if (callback == nullptr) {
    callback = EngineJavaCallbackObject();
  }
  if (callback == nullptr) {
    return;
  }
  if (env == nullptr) {
    RecordDataModelNotification(callback, type, data);
    return;
  }
  jclass callback_class = env->GetObjectClass(callback);
  jmethodID method = env->GetMethodID(
      callback_class, "f", "(Ljava/lang/String;Ljava/lang/String;)V");
  if (method != nullptr) {
    env->CallVoidMethod(callback, method, type, data);
  } else {
    RecordDataModelNotification(callback, type, data);
  }
}

void HandleStaticVoidMethodV(JNIEnv *env, jclass clazz, jmethodID methodID,
                             va_list args) {
  if (HandleRobloxTextInputStaticVoidMethodV(clazz, methodID, args)) {
    return;
  }
  if (HandleRobloxExperienceLifecycleStaticVoidMethod(clazz, methodID)) {
    return;
  }
  const char *name = MethodName(methodID);
  if (!name) {
    return;
  }
  const std::shared_ptr<Class> method_class = ClassFromJClass(clazz);
  if (std::strcmp(name, "openNativeOverlay") == 0 &&
      std::strcmp(MethodSignature(methodID),
                  "(Ljava/lang/String;Ljava/lang/String;)V") == 0 &&
      method_class != nullptr &&
      method_class->GetName() ==
          "com/roblox/engine/jni/NativeGLJavaInterface") {
    jstring title = va_arg(args, jstring);
    jstring url = va_arg(args, jstring);
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxNativeOverlay(env, title, url);
    }
    return;
  }
  if (std::strcmp(name, "init") == 0) {
    (void)va_arg(args, jobject);
    g_fmod_initialized = true;
    if (TraceEnabled()) {
      std::cout << "  [JNI] FMOD.init shimmed initialized=true\n";
    }
    return;
  }
  if (std::strcmp(name, "close") == 0 ||
      std::strcmp(name, "OutputAAudioHeadphonesChanged") == 0) {
    if (TraceEnabled()) {
      std::cout << "  [JNI] FMOD." << name << " shimmed no-op\n";
    }
    return;
  }
  if (std::strcmp(name, "setAppBridgeNotificationListener") == 0) {
    jobject listener = va_arg(args, jobject);
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      g_app_bridge_notification_listener = listener;
      g_static_object_fields["sAppBridgeNotificationListener"] = listener;
    }
    if (TraceEnabled()) {
      std::cout << "  [JNI] NativeGLJavaInterface listener=" << listener
                << '\n';
    }
    return;
  }
  if (std::strcmp(name, "setImplementation") == 0) {
    jobject callback = va_arg(args, jobject);
    if (callback == nullptr) {
      callback = EngineJavaCallbackObject();
    }
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      g_engine_java_callback = callback;
      g_static_object_fields["sImplementation"] = callback;
    }
    if (TraceEnabled()) {
      std::cout << "  [JNI] NativeGLJavaInterface implementation=" << callback
                << '\n';
    }
    return;
  }
  if (std::strcmp(name, "onAppBridgeNotification") == 0) {
    jstring type = va_arg(args, jstring);
    jstring data = va_arg(args, jstring);
    std::cout << "  [JNI callback] onAppBridgeNotification type="
              << StringFromJString(type)
              << " data_bytes=" << StringFromJString(data).size() << '\n';
    DispatchAppBridgeNotification(env, type, data);
    return;
  }
  if (std::strcmp(name, "onDataModelNotificationCallback") == 0 &&
      std::strcmp(MethodSignature(methodID),
                  "(Ljava/lang/String;Ljava/lang/String;)V") == 0 &&
      method_class != nullptr &&
      method_class->GetName() ==
          "com/roblox/engine/jni/NativeGLJavaInterface") {
    jstring type = va_arg(args, jstring);
    jstring data = va_arg(args, jstring);
    DispatchDataModelNotification(env, type, data);
    return;
  }
}

void HandleStaticVoidMethodA(JNIEnv *env, jclass clazz, jmethodID methodID,
                             const jvalue *args) {
  if (HandleRobloxTextInputStaticVoidMethodA(clazz, methodID, args)) {
    return;
  }
  if (HandleRobloxExperienceLifecycleStaticVoidMethod(clazz, methodID)) {
    return;
  }
  const char *name = MethodName(methodID);
  if (!name) {
    return;
  }
  if (std::strcmp(name, "init") == 0) {
    g_fmod_initialized = true;
    if (TraceEnabled()) {
      std::cout << "  [JNI] FMOD.init shimmed initialized=true\n";
    }
    return;
  }
  if (std::strcmp(name, "close") == 0 ||
      std::strcmp(name, "OutputAAudioHeadphonesChanged") == 0) {
    if (TraceEnabled()) {
      std::cout << "  [JNI] FMOD." << name << " shimmed no-op\n";
    }
    return;
  }
  if (!args) {
    return;
  }
  const std::shared_ptr<Class> method_class = ClassFromJClass(clazz);
  if (std::strcmp(name, "openNativeOverlay") == 0 &&
      std::strcmp(MethodSignature(methodID),
                  "(Ljava/lang/String;Ljava/lang/String;)V") == 0 &&
      method_class != nullptr &&
      method_class->GetName() ==
          "com/roblox/engine/jni/NativeGLJavaInterface") {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      (void)vm->DispatchRobloxNativeOverlay(env,
                                            static_cast<jstring>(args[0].l),
                                            static_cast<jstring>(args[1].l));
    }
    return;
  }
  if (std::strcmp(name, "setAppBridgeNotificationListener") == 0) {
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      g_app_bridge_notification_listener = args[0].l;
      g_static_object_fields["sAppBridgeNotificationListener"] = args[0].l;
    }
    if (TraceEnabled()) {
      std::cout << "  [JNI] NativeGLJavaInterface listener=" << args[0].l
                << '\n';
    }
    return;
  }
  if (std::strcmp(name, "setImplementation") == 0) {
    jobject callback = args[0].l;
    if (callback == nullptr) {
      callback = EngineJavaCallbackObject();
    }
    {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      g_engine_java_callback = callback;
      g_static_object_fields["sImplementation"] = callback;
    }
    if (TraceEnabled()) {
      std::cout << "  [JNI] NativeGLJavaInterface implementation=" << callback
                << '\n';
    }
    return;
  }
  if (std::strcmp(name, "onAppBridgeNotification") == 0) {
    std::cout << "  [JNI callback] onAppBridgeNotification type="
              << StringFromJString(static_cast<jstring>(args[0].l))
              << " data_bytes="
              << StringFromJString(static_cast<jstring>(args[1].l)).size()
              << '\n';
    DispatchAppBridgeNotification(env, static_cast<jstring>(args[0].l),
                                  static_cast<jstring>(args[1].l));
    return;
  }
  if (std::strcmp(name, "onDataModelNotificationCallback") == 0 &&
      std::strcmp(MethodSignature(methodID),
                  "(Ljava/lang/String;Ljava/lang/String;)V") == 0 &&
      method_class != nullptr &&
      method_class->GetName() ==
          "com/roblox/engine/jni/NativeGLJavaInterface") {
    DispatchDataModelNotification(env, static_cast<jstring>(args[0].l),
                                  static_cast<jstring>(args[1].l));
    return;
  }
}

bool IsRobloxOpenWebActivityMethod(jobject obj, jmethodID method_id) {
  const char* name = MethodName(method_id);
  const char* signature = MethodSignature(method_id);
  return obj != nullptr && name != nullptr && signature != nullptr &&
         ObjectClassName(obj) == "com/roblox/client/startup/MainGameActivity" &&
         std::strcmp(name, "openWebActivity") == 0 &&
         std::strcmp(signature, "(Ljava/lang/String;Ljava/lang/String;)V") == 0;
}

bool HandleRobloxOpenWebActivityMethodV(jobject obj, jmethodID method_id,
                                        va_list args) {
  if (!IsRobloxOpenWebActivityMethod(obj, method_id)) {
    return false;
  }
  jstring url = va_arg(args, jstring);
  jstring title = va_arg(args, jstring);
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchRobloxOpenWebActivity(vm->GetJNIEnv(), url, title);
  }
  return true;
}

bool HandleRobloxOpenWebActivityMethodA(jobject obj, jmethodID method_id,
                                        const jvalue* args) {
  if (!IsRobloxOpenWebActivityMethod(obj, method_id)) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr && args != nullptr) {
    (void)vm->DispatchRobloxOpenWebActivity(vm->GetJNIEnv(),
                                            static_cast<jstring>(args[0].l),
                                            static_cast<jstring>(args[1].l));
  }
  return true;
}

bool IsAndroidSetWindowFlagsMethod(jobject obj, jmethodID method_id) {
  const char* name = MethodName(method_id);
  const char* signature = MethodSignature(method_id);
  const std::string class_name = ObjectClassName(obj);
  return obj != nullptr && name != nullptr && signature != nullptr &&
         (class_name == "android/app/Activity" ||
          class_name == "com/roblox/client/RobloxActivity" ||
          class_name == "com/roblox/client/startup/MainGameActivity" ||
          class_name == "com/google/androidgamesdk/GameActivity") &&
         std::strcmp(name, "setWindowFlags") == 0 &&
         std::strcmp(signature, "(II)V") == 0;
}

bool HandleAndroidSetWindowFlagsMethodV(jobject obj, jmethodID method_id,
                                        va_list args) {
  if (!IsAndroidSetWindowFlagsMethod(obj, method_id)) {
    return false;
  }
  const jint flags = va_arg(args, jint);
  const jint mask = va_arg(args, jint);
  VM* vm = CurrentVM();
  if (vm != nullptr) {
    (void)vm->DispatchAndroidWindowFlags(flags, mask);
  }
  return true;
}

bool HandleAndroidSetWindowFlagsMethodA(jobject obj, jmethodID method_id,
                                        const jvalue* args) {
  if (!IsAndroidSetWindowFlagsMethod(obj, method_id)) {
    return false;
  }
  VM* vm = CurrentVM();
  if (vm != nullptr && args != nullptr) {
    (void)vm->DispatchAndroidWindowFlags(args[0].i, args[1].i);
  }
  return true;
}

void HandleVoidMethodA(jobject obj, jmethodID method_id, const jvalue *args) {
  const char *name = MethodName(method_id);
  if (!name) {
    return;
  }
  if (HandleAndroidSetWindowFlagsMethodA(obj, method_id, args)) {
    return;
  }
  if (std::strcmp(name, "syncCookiesFromEngine") == 0) {
    VM* vm = CurrentVM();
    if (vm != nullptr) {
      std::string header = CookieHeaderForJava();
      jstring cookie = static_cast<jstring>(MakeString(header.c_str()));
      (void)vm->DispatchRobloxCookieSync(vm->GetJNIEnv(), cookie);
      ClearCookieString(&header);
    }
    return;
  }
  if (!args) {
    return;
  }
  if (std::strcmp(name, "run") == 0 &&
      ObjectClassName(obj) ==
          "com/roblox/universalapp/messagebus/RawCallback") {
    VM *vm = CurrentVM();
    if (vm != nullptr) {
      vm->DispatchMessageBusRawCallback(obj, vm->GetJNIEnv(),
                                        static_cast<jstring>(args[0].l));
    }
    return;
  }
  if (std::strcmp(name, "a") == 0) {
    if (ObjectClassName(obj) !=
        "com/roblox/engine/jni/OnAppBridgeNotificationListener") {
      return;
    }
    RecordAppBridgeNotification(obj, static_cast<jstring>(args[0].l),
                                static_cast<jstring>(args[1].l));
    return;
  }
  if (std::strcmp(name, "f") == 0 &&
      std::strcmp(MethodSignature(method_id),
                  "(Ljava/lang/String;Ljava/lang/String;)V") == 0 &&
      ObjectClassName(obj) == "com/roblox/engine/jni/EngineJavaCallback2") {
    RecordDataModelNotification(obj, static_cast<jstring>(args[0].l),
                                static_cast<jstring>(args[1].l));
    return;
  }
  if (ObjectClassName(obj) == "com/roblox/engine/jni/EngineJavaCallback2" &&
      std::strlen(name) == 1 && name[0] >= 'a' && name[0] <= 'o') {
    return;
  }
  if (std::strcmp(name, "setBaseUrl") == 0) {
    std::string base_url =
        StringFromJString(reinterpret_cast<jstring>(args[0].l));
    if (base_url.empty()) {
      base_url = "https://www.roblox.com";
    }
    SetStringFieldRaw(obj, "baseUrl", base_url.c_str());
    return;
  }
  if (std::strcmp(name, "setCookie") == 0) {
    SetStringFieldRaw(obj, "cookieName",
                      StringFromJString(
                          reinterpret_cast<jstring>(args[0].l)).c_str());
    SetStringFieldRaw(obj, "cookieValue",
                      StringFromJString(
                          reinterpret_cast<jstring>(args[1].l)).c_str());
    StoreCookieHeader(
        StringFromJString(reinterpret_cast<jstring>(args[1].l)));
    return;
  }
  if (std::strcmp(name, "l0") == 0) {
    SetIntFieldRaw(obj, "contentViewId", args[0].i);
    SetIntFieldRaw(obj, "m", args[0].i);
    return;
  }
  if (std::strcmp(name, "k1") == 0) {
    SetNativeHelperBoolean(MakeNativeHelperObject(nullptr), "isVisible", "g",
                           args[0].z);
    return;
  }
  if (std::strcmp(name, "gameActivity_onEngineInitialized") == 0) {
    SetNativeHelperBoolean(obj, "isEngineInitialized", "i", JNI_TRUE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strcmp(name, "gameActivity_onFlagsLoaded") == 0) {
    SetBooleanFieldRaw(obj, "flagsLoaded", JNI_TRUE);
    SetBooleanFieldRaw(obj, "flagsFailed", JNI_FALSE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strcmp(name, "gameActivity_onFlagsFailed") == 0) {
    SetBooleanFieldRaw(obj, "flagsLoaded", JNI_FALSE);
    SetBooleanFieldRaw(obj, "flagsFailed", JNI_TRUE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strcmp(name, "gameActivity_onGameLoaded") == 0) {
    SetLongFieldRaw(obj, "placeId", args[0].j);
    SetNativeHelperBoolean(obj, "isInExperience", "j",
                           args[0].j > 0 ? JNI_TRUE : JNI_FALSE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strcmp(name, "gameActivity_onExperienceStart") == 0) {
    SetNativeHelperBoolean(obj, "isInExperience", "j", JNI_TRUE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strcmp(name, "gameActivity_onExperienceStop") == 0) {
    SetNativeHelperBoolean(obj, "isInExperience", "j", JNI_FALSE);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strcmp(name, "gameActivity_onScreenOrientationChanged") == 0) {
    SetIntFieldRaw(obj, "orientation", args[0].i);
    SetNativeHelperBoolean(obj, "isInExperience", "j", args[1].z);
    RecordNativeHelperCallback(obj, name);
    return;
  }
  if (std::strncmp(name, "gameActivity_", 13) == 0) {
    RecordNativeHelperCallback(obj, name);
  }
}

void JNICALL CallStaticVoidMethod(JNIEnv* env, jclass clazz,
                                  jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallStaticVoidMethod: " << MethodName(methodID) << '\n';
  }
  va_list args;
  va_start(args, methodID);
  HandleStaticVoidMethodV(env, clazz, methodID, args);
  va_end(args);
}

jobject JNICALL CallStaticObjectMethod(JNIEnv * /*env*/, jclass clazz,
                                       jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallStaticObjectMethod: " << MethodName(methodID)
              << '\n';
  }
  va_list args;
  va_start(args, methodID);
  jobject result = ObjectResultForMethodV(nullptr, methodID, args);
  va_end(args);
  if (result != nullptr) {
    return result;
  }
  result = ExactMessageBusStaticObject(clazz, methodID);
  return result != nullptr ? result : StaticObjectResultForMethod(methodID);
}

jboolean JNICALL CallStaticBooleanMethod(JNIEnv* /*env*/, jclass /*clazz*/, jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallStaticBooleanMethod: " << MethodName(methodID) << '\n';
  }
  jboolean result = JNI_FALSE;
  const char* name = MethodName(methodID);
  if (FmodBooleanResultForMethod(name, &result)) {
    return result;
  }
  return CookieBooleanResultForMethod(name, &result) ? result : JNI_FALSE;
}

jlong LongResultForReceiverMethod(jobject obj, const char* name) {
  if (!name) {
    return 0;
  }
  if (std::strcmp(name, "open") == 0) {
    jint id = IntFieldValue(obj, "nextDialogId");
    id = id <= 0 ? 1 : id;
    SetIntFieldRaw(obj, "nextDialogId", id + 1);
    return static_cast<jlong>(id);
  }
  if (std::strcmp(name, "getId") == 0) {
    return static_cast<jlong>(IntFieldValue(obj, "id"));
  }
  jlong value = LongFieldValue(obj, name);
  if (value != 0) {
    return value;
  }
  std::string field_name = GetterFieldName(name);
  if (!field_name.empty()) {
    value = LongFieldValue(obj, field_name.c_str());
    if (value != 0) {
      return value;
    }
  }
  return 0;
}

jint JNICALL CallStaticIntMethod(JNIEnv * /*env*/, jclass /*clazz*/,
                                 jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallStaticIntMethod: " << MethodName(methodID)
              << '\n';
  }
  va_list args;
  va_start(args, methodID);
  jint result = StaticIntResultForMethodV(methodID, args);
  va_end(args);
  return result;
}

jlong JNICALL CallStaticLongMethod(JNIEnv * /*env*/, jclass /*clazz*/,
                                   jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallStaticLongMethod: " << MethodName(methodID)
              << '\n';
  }
  return StaticLongResultForMethod(methodID);
}

void JNICALL CallVoidMethod(JNIEnv * /*env*/, jobject obj, jmethodID methodID,
                            ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallVoidMethod: " << MethodName(methodID) << '\n';
  }
  va_list args;
  va_start(args, methodID);
  if (!HandleRobloxExperienceLifecycleVoidMethod(obj, methodID) &&
      !HandleRobloxOpenWebActivityMethodV(obj, methodID, args) &&
      !HandleRobloxTextInputInstanceVoidMethodV(obj, methodID, args) &&
      !HandleFmodAudioDeviceVoidMethodV(obj, methodID, args) &&
      !HandleRobloxCookieSetVoidMethodV(obj, methodID, args) &&
      !HandleMemStorageCallbackVoidMethodV(obj, methodID, args)) {
    HandleVoidMethod(obj, methodID, args);
  }
  va_end(args);
}

jobject JNICALL CallObjectMethod(JNIEnv* /*env*/, jobject obj,
                                 jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallObjectMethod: " << MethodName(methodID) << '\n';
  }
  va_list args;
  va_start(args, methodID);
  jobject result = ObjectResultForMethodV(obj, methodID, args);
  va_end(args);
  return result;
}

jboolean JNICALL CallBooleanMethod(JNIEnv* /*env*/, jobject obj,
                                   jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallBooleanMethod: " << MethodName(methodID) << '\n';
  }
  va_list args;
  va_start(args, methodID);
  jboolean result = JNI_FALSE;
  bool handled =
      HandleFmodAudioDeviceBooleanMethodV(obj, methodID, args, &result);
  if (!handled) {
    handled =
        PackageManagerBooleanResultForMethodV(obj, methodID, args, &result);
  }
  if (!handled) {
    handled =
        LocalStorageBooleanResultForMethodV(MethodName(methodID), args, &result);
  }
  if (!handled) {
    handled = CookieBooleanResultForMethod(MethodName(methodID), &result);
  }
  va_end(args);
  return handled ? result
                 : BooleanResultForReceiverMethod(obj, MethodName(methodID));
}

jint JNICALL CallIntMethod(JNIEnv* /*env*/, jobject obj, jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallIntMethod: " << MethodName(methodID) << '\n';
  }
  return IntResultForReceiverMethod(obj, MethodName(methodID));
}

jlong JNICALL CallLongMethod(JNIEnv* /*env*/, jobject obj,
                             jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallLongMethod: " << MethodName(methodID) << '\n';
  }
  jlong result = 0;
  if (LocalStorageLongResultForMethod(MethodName(methodID), &result)) {
    return result;
  }
  return LongResultForReceiverMethod(obj, MethodName(methodID));
}

jobject ConstructObjectV(jclass clazz, jmethodID methodID, va_list args) {
  jobject object = MakeObject(clazz);
  const std::shared_ptr<Class> object_class = ClassFromJClass(clazz);
  if (object_class == nullptr) {
    return object;
  }
  if (object_class->GetName() ==
          "com/roblox/universalapp/messagebus/Connection" &&
      std::strcmp(MethodName(methodID), "<init>") == 0 &&
      std::strcmp(MethodSignature(methodID), "(J)V") == 0) {
    const jlong handle = va_arg(args, jlong);
    SetLongFieldRaw(object, "a", handle);
    SetLongFieldRaw(object, "f10205a", handle);
    SetLongFieldRaw(object, "nativePtr", handle);
  } else if (object_class->GetName() ==
                 "com/roblox/engine/jni/memstorage/Connection" &&
             std::strcmp(MethodName(methodID), "<init>") == 0 &&
             std::strcmp(MethodSignature(methodID), "(J)V") == 0) {
    SetLongFieldRaw(object, "ref", va_arg(args, jlong));
  } else if (object_class->GetName() ==
                 "com/roblox/engine/jni/model/NativeTextBoxInfo" &&
             std::strcmp(MethodName(methodID), "<init>") == 0 &&
             std::strcmp(MethodSignature(methodID), "(FFFFFZIIIIIIZZ)V") == 0) {
    SetFloatFieldRaw(object, "x", static_cast<jfloat>(va_arg(args, double)));
    SetFloatFieldRaw(object, "y", static_cast<jfloat>(va_arg(args, double)));
    SetFloatFieldRaw(object, "width",
                     static_cast<jfloat>(va_arg(args, double)));
    SetFloatFieldRaw(object, "height",
                     static_cast<jfloat>(va_arg(args, double)));
    SetFloatFieldRaw(object, "fontSize",
                     static_cast<jfloat>(va_arg(args, double)));
    SetBooleanFieldRaw(object, "multiline",
                       static_cast<jboolean>(va_arg(args, jint)));
    constexpr const char *kIntFields[] = {"xAlignment",    "yAlignment",
                                          "textColor",     "font",
                                          "textInputType", "returnKeyType"};
    for (const char *field : kIntFields) {
      SetIntFieldRaw(object, field, va_arg(args, jint));
    }
    SetBooleanFieldRaw(object, "manualFocusRelease",
                       static_cast<jboolean>(va_arg(args, jint)));
    SetBooleanFieldRaw(object, "textWrapped",
                       static_cast<jboolean>(va_arg(args, jint)));
  } else if (object_class->GetName() ==
                 "com/roblox/engine/jni/model/NativeTextBoxInfo" &&
             std::strcmp(MethodName(methodID), "<init>") == 0 &&
             std::strcmp(
                 MethodSignature(methodID),
                 "(Lcom/roblox/engine/jni/model/NativeTextBoxInfo;)V") == 0) {
    const jobject source = va_arg(args, jobject);
    constexpr const char *kFloatFields[] = {"x", "y", "width", "height",
                                            "fontSize"};
    for (const char *field : kFloatFields) {
      SetFloatFieldRaw(object, field, FloatFieldValue(source, field));
    }
    constexpr const char *kIntFields[] = {"xAlignment",    "yAlignment",
                                          "textColor",     "font",
                                          "textInputType", "returnKeyType"};
    for (const char *field : kIntFields) {
      SetIntFieldRaw(object, field, IntFieldValue(source, field));
    }
    constexpr const char *kBooleanFields[] = {"multiline", "manualFocusRelease",
                                              "textWrapped"};
    for (const char *field : kBooleanFields) {
      SetBooleanFieldRaw(object, field, BooleanFieldValue(source, field));
    }
  }
  return object;
}

jobject ConstructObjectA(jclass clazz, jmethodID methodID, const jvalue *args) {
  jobject object = MakeObject(clazz);
  const std::shared_ptr<Class> object_class = ClassFromJClass(clazz);
  if (object_class == nullptr || args == nullptr) {
    return object;
  }
  if (object_class->GetName() ==
          "com/roblox/universalapp/messagebus/Connection" &&
      std::strcmp(MethodName(methodID), "<init>") == 0 &&
      std::strcmp(MethodSignature(methodID), "(J)V") == 0) {
    SetLongFieldRaw(object, "a", args[0].j);
    SetLongFieldRaw(object, "f10205a", args[0].j);
    SetLongFieldRaw(object, "nativePtr", args[0].j);
  } else if (object_class->GetName() ==
                 "com/roblox/engine/jni/memstorage/Connection" &&
             std::strcmp(MethodName(methodID), "<init>") == 0 &&
             std::strcmp(MethodSignature(methodID), "(J)V") == 0) {
    SetLongFieldRaw(object, "ref", args[0].j);
  } else if (object_class->GetName() ==
                 "com/roblox/engine/jni/model/NativeTextBoxInfo" &&
             std::strcmp(MethodName(methodID), "<init>") == 0 &&
             std::strcmp(MethodSignature(methodID), "(FFFFFZIIIIIIZZ)V") == 0) {
    constexpr const char *kFloatFields[] = {"x", "y", "width", "height",
                                            "fontSize"};
    for (std::size_t index = 0; index < 5; ++index) {
      SetFloatFieldRaw(object, kFloatFields[index], args[index].f);
    }
    SetBooleanFieldRaw(object, "multiline", args[5].z);
    constexpr const char *kIntFields[] = {"xAlignment",    "yAlignment",
                                          "textColor",     "font",
                                          "textInputType", "returnKeyType"};
    for (std::size_t index = 0; index < 6; ++index) {
      SetIntFieldRaw(object, kIntFields[index], args[index + 6].i);
    }
    SetBooleanFieldRaw(object, "manualFocusRelease", args[12].z);
    SetBooleanFieldRaw(object, "textWrapped", args[13].z);
  } else if (object_class->GetName() ==
                 "com/roblox/engine/jni/model/NativeTextBoxInfo" &&
             std::strcmp(MethodName(methodID), "<init>") == 0 &&
             std::strcmp(
                 MethodSignature(methodID),
                 "(Lcom/roblox/engine/jni/model/NativeTextBoxInfo;)V") == 0) {
    const jobject source = args[0].l;
    constexpr const char *kFloatFields[] = {"x", "y", "width", "height",
                                            "fontSize"};
    for (const char *field : kFloatFields) {
      SetFloatFieldRaw(object, field, FloatFieldValue(source, field));
    }
    constexpr const char *kIntFields[] = {"xAlignment",    "yAlignment",
                                          "textColor",     "font",
                                          "textInputType", "returnKeyType"};
    for (const char *field : kIntFields) {
      SetIntFieldRaw(object, field, IntFieldValue(source, field));
    }
    constexpr const char *kBooleanFields[] = {"multiline", "manualFocusRelease",
                                              "textWrapped"};
    for (const char *field : kBooleanFields) {
      SetBooleanFieldRaw(object, field, BooleanFieldValue(source, field));
    }
  }
  return object;
}

jobject JNICALL NewObject(JNIEnv * /*env*/, jclass clazz, jmethodID methodID,
                          ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] NewObject: " << MethodName(methodID) << '\n';
  }
  va_list args;
  va_start(args, methodID);
  jobject object = ConstructObjectV(clazz, methodID, args);
  va_end(args);
  return object;
}

jbyte JNICALL CallStaticByteMethod(JNIEnv* /*env*/, jclass /*clazz*/,
                                   jmethodID /*methodID*/, ...) {
  return 0;
}

jchar JNICALL CallStaticCharMethod(JNIEnv* /*env*/, jclass /*clazz*/,
                                   jmethodID /*methodID*/, ...) {
  return 0;
}

jshort JNICALL CallStaticShortMethod(JNIEnv* /*env*/, jclass /*clazz*/,
                                     jmethodID /*methodID*/, ...) {
  return 0;
}

jfloat JNICALL CallStaticFloatMethod(JNIEnv* /*env*/, jclass /*clazz*/,
                                     jmethodID /*methodID*/, ...) {
  return 0.0f;
}

jdouble JNICALL CallStaticDoubleMethod(JNIEnv* /*env*/, jclass /*clazz*/,
                                       jmethodID /*methodID*/, ...) {
  return 0.0;
}

jbyte JNICALL CallByteMethod(JNIEnv* /*env*/, jobject /*obj*/,
                             jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallByteMethod: " << MethodName(methodID) << '\n';
  }
  return 0;
}

jchar JNICALL CallCharMethod(JNIEnv* /*env*/, jobject /*obj*/,
                             jmethodID /*methodID*/, ...) {
  return 0;
}

jshort JNICALL CallShortMethod(JNIEnv* /*env*/, jobject /*obj*/,
                               jmethodID /*methodID*/, ...) {
  return 0;
}

jfloat FloatResultForReceiverMethod(jobject obj, const char* name) {
  if (!name) {
    return 0.0f;
  }
  jfloat value = FloatFieldValue(obj, name);
  if (value != 0.0f) {
    return value;
  }
  std::string field_name = GetterFieldName(name);
  return field_name.empty() ? 0.0f
                            : FloatFieldValue(obj, field_name.c_str());
}

jfloat JNICALL CallFloatMethod(JNIEnv* /*env*/, jobject obj,
                               jmethodID methodID, ...) {
  if (TraceEnabled()) {
    std::cout << "  [JNI] CallFloatMethod: " << MethodName(methodID) << '\n';
  }
  return FloatResultForReceiverMethod(obj, MethodName(methodID));
}

jdouble JNICALL CallDoubleMethod(JNIEnv* /*env*/, jobject /*obj*/,
                                 jmethodID /*methodID*/, ...) {
  return 0.0;
}
}  // namespace

void* my_segment[100000] = { nullptr };
void* my_segment_table[16] = {
    my_segment,
    my_segment + 8192,
    my_segment + 16384,
    my_segment + 24576,
    my_segment + 32768,
    my_segment + 40960,
    my_segment + 49152,
    my_segment + 57344,
    my_segment + 65536,
    my_segment + 73728,
    my_segment + 81920,
    my_segment + 90112,
    my_segment + 98304,
    nullptr,
    nullptr,
    nullptr,
};
int g_jni_ref_index = 1;

jobject CreateAndroidConfiguration(JNIEnv* env) {
  if (env == nullptr) {
    return nullptr;
  }
  jclass clazz = env->FindClass("android/content/res/Configuration");
  if (clazz == nullptr) {
    return nullptr;
  }
  jobject configuration = env->AllocObject(clazz);
  env->DeleteLocalRef(clazz);
  if (configuration == nullptr) {
    return nullptr;
  }
  const PlatformIdentity identity =
      CurrentVM() != nullptr ? CurrentVM()->GetPlatformIdentitySnapshot()
                             : PlatformIdentity{};
  SetIntFieldRaw(configuration, "colorMode", 0);
  SetIntFieldRaw(configuration, "densityDpi", 160);
  SetIntFieldRaw(configuration, "fontWeightAdjustment", 0);
  SetIntFieldRaw(configuration, "hardKeyboardHidden",
                 identity.keyboard_enabled ? 1 : 2);
  SetIntFieldRaw(configuration, "keyboard", identity.keyboard_enabled ? 2 : 1);
  SetIntFieldRaw(configuration, "keyboardHidden",
                 identity.keyboard_enabled ? 1 : 2);
  SetIntFieldRaw(configuration, "mcc", 0);
  SetIntFieldRaw(configuration, "mnc", 0);
  SetIntFieldRaw(configuration, "navigation", 1);
  SetIntFieldRaw(configuration, "navigationHidden", 1);
  SetIntFieldRaw(configuration, "orientation", 2);
  SetIntFieldRaw(configuration, "screenHeightDp", 720);
  SetIntFieldRaw(configuration, "screenLayout", 0);
  SetIntFieldRaw(configuration, "screenWidthDp", 1280);
  SetIntFieldRaw(configuration, "smallestScreenWidthDp", 720);
  SetIntFieldRaw(configuration, "touchscreen", identity.touch_enabled ? 3 : 1);
  SetIntFieldRaw(configuration, "uiMode", 0);
  return configuration;
}

VM::VM() {
  g_vm_instance = this;
  g_thread_vm_instance = this;
  InitJNIFunctionTables();
}

VM::~VM() {
  {
    std::lock_guard<std::mutex> lock(message_bus_raw_mutex_);
    message_bus_raw_bindings_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(message_bus_request_handler_mutex_);
    message_bus_request_handler_bindings_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(mem_storage_callback_mutex_);
    mem_storage_callback_bindings_.clear();
  }
  ClearRobloxDataModelNotificationCallbacks();
  ClearRobloxExperienceLifecycleCallbacks();
  ClearRobloxCredentialSink();
  ClearRobloxCredentialProvider();
  ClearRobloxTextInputCallbacks();
  ClearAndroidWindowCallbacks();
  ClearFmodAudioDeviceCallbacks();
  if (g_thread_vm_instance == this) {
    g_thread_vm_instance = nullptr;
    g_thread_local_env = nullptr;
  }
  if (g_vm_instance == this) {
    g_vm_instance = nullptr;
  }
}

VM* VM::FromJavaVM(JavaVM* java_vm) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  return java_vm != nullptr && g_vm_instance != nullptr &&
                 g_vm_instance->java_vm_ == java_vm
             ? g_vm_instance
             : nullptr;
}

struct VM::RobloxTextInputBinding {
  ~RobloxTextInputBinding() {
    ClearSensitiveString(&last_request.text);
    if (context != nullptr && callbacks.shutdown != nullptr) {
      callbacks.shutdown(context.get());
    }
  }

  std::shared_ptr<void> context;
  RobloxTextInputCallbacks callbacks;
  std::recursive_mutex callback_mutex;
  bool active = false;
  RobloxTextInputShowRequest last_request;
};

namespace {

bool SameRobloxTextBoxInfo(const RobloxTextBoxInfo& left,
                           const RobloxTextBoxInfo& right) {
  return left.x == right.x && left.y == right.y &&
         left.width == right.width && left.height == right.height &&
         left.font_size == right.font_size &&
         left.multiline == right.multiline &&
         left.x_alignment == right.x_alignment &&
         left.y_alignment == right.y_alignment &&
         left.text_color == right.text_color && left.font == right.font &&
         left.text_input_type == right.text_input_type &&
         left.return_key_type == right.return_key_type &&
         left.manual_focus_release == right.manual_focus_release &&
         left.text_wrapped == right.text_wrapped;
}

bool SameRobloxTextInputShowRequest(
    const RobloxTextInputShowRequest& left,
    const RobloxTextInputShowRequest& right) {
  return left.text_box == right.text_box &&
         left.show_native_input == right.show_native_input &&
         left.text == right.text && SameRobloxTextBoxInfo(left.info, right.info);
}

}  // namespace

void VM::SetRobloxTextInputCallbacks(
    std::shared_ptr<void> context,
    const RobloxTextInputCallbacks& callbacks) {
  auto binding = std::make_shared<RobloxTextInputBinding>();
  binding->context = std::move(context);
  binding->callbacks = callbacks;
  std::shared_ptr<RobloxTextInputBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(roblox_text_input_mutex_);
    old_binding = std::move(roblox_text_input_binding_);
    roblox_text_input_binding_ = std::move(binding);
  }
}

void VM::ClearRobloxTextInputCallbacks() {
  std::shared_ptr<RobloxTextInputBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(roblox_text_input_mutex_);
    old_binding = std::move(roblox_text_input_binding_);
  }
}

bool VM::DispatchRobloxTextInputShow(
    const RobloxTextInputShowRequest& request) {
  std::shared_ptr<RobloxTextInputBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_text_input_mutex_);
    binding = roblox_text_input_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.show == nullptr || request.text_box <= 0) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(binding->callback_mutex);
  if (binding->active &&
      SameRobloxTextInputShowRequest(binding->last_request, request)) {
    return true;
  }
  ClearSensitiveString(&binding->last_request.text);
  binding->last_request = request;
  binding->active = true;
  binding->callbacks.show(binding->context.get(), request);
  return true;
}

bool VM::DispatchRobloxTextInputHide() {
  std::shared_ptr<RobloxTextInputBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_text_input_mutex_);
    binding = roblox_text_input_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.hide == nullptr) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(binding->callback_mutex);
  if (!binding->active) {
    return true;
  }
  binding->active = false;
  ClearSensitiveString(&binding->last_request.text);
  binding->last_request = {};
  binding->callbacks.hide(binding->context.get());
  return true;
}

bool VM::DispatchRobloxTextInputReplaceText(const std::string& text) {
  std::shared_ptr<RobloxTextInputBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_text_input_mutex_);
    binding = roblox_text_input_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.replace_text == nullptr) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(binding->callback_mutex);
  if (!binding->active || binding->last_request.text == text) {
    return true;
  }
  ClearSensitiveString(&binding->last_request.text);
  binding->last_request.text = text;
  binding->callbacks.replace_text(binding->context.get(), text);
  return true;
}

bool VM::DispatchRobloxTextInputPropertiesChanged() {
  std::shared_ptr<RobloxTextInputBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_text_input_mutex_);
    binding = roblox_text_input_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.properties_changed == nullptr) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(binding->callback_mutex);
  if (!binding->active) {
    return true;
  }
  binding->callbacks.properties_changed(binding->context.get());
  return true;
}

jobject
VM::CreateMessageBusRawCallback(std::shared_ptr<void> context,
                                const MessageBusRawCallbacks &callbacks) {
  if (context == nullptr || callbacks.run == nullptr) {
    return nullptr;
  }
  JNIEnv *env = GetJNIEnv();
  if (env == nullptr) {
    return nullptr;
  }
  jclass callback_class =
      env->FindClass("com/roblox/universalapp/messagebus/RawCallback");
  if (callback_class == nullptr) {
    return nullptr;
  }
  jobject callback = env->AllocObject(callback_class);
  env->DeleteLocalRef(callback_class);
  if (callback == nullptr) {
    return nullptr;
  }
  auto binding = std::make_shared<MessageBusRawBinding>();
  binding->context = std::move(context);
  binding->callbacks = callbacks;
  {
    std::lock_guard<std::mutex> lock(message_bus_raw_mutex_);
    message_bus_raw_bindings_[callback] = std::move(binding);
  }
  return callback;
}

void VM::ClearMessageBusRawCallback(jobject callback) {
  std::shared_ptr<MessageBusRawBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(message_bus_raw_mutex_);
    const auto found = message_bus_raw_bindings_.find(callback);
    if (found == message_bus_raw_bindings_.end()) {
      return;
    }
    old_binding = std::move(found->second);
    message_bus_raw_bindings_.erase(found);
  }
}

bool VM::DispatchMessageBusRawCallback(jobject callback, JNIEnv *env,
                                       jstring message) {
  std::shared_ptr<MessageBusRawBinding> binding;
  {
    std::lock_guard<std::mutex> lock(message_bus_raw_mutex_);
    const auto found = message_bus_raw_bindings_.find(callback);
    if (found != message_bus_raw_bindings_.end()) {
      binding = found->second;
    }
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.run == nullptr || env == nullptr ||
      message == nullptr) {
    return false;
  }
  binding->callbacks.run(binding->context.get(), env, message);
  return true;
}

jobject VM::CreateMessageBusRequestHandler(
    std::shared_ptr<void> context,
    const MessageBusRequestHandlerCallbacks& callbacks) {
  if (context == nullptr || callbacks.run == nullptr) {
    return nullptr;
  }
  JNIEnv* env = GetJNIEnv();
  jclass handler_class = env != nullptr
                             ? env->FindClass(
                                   "com/roblox/universalapp/messagebus/"
                                   "RequestHandlerRaw")
                             : nullptr;
  if (handler_class == nullptr) {
    return nullptr;
  }
  jobject handler = env->AllocObject(handler_class);
  env->DeleteLocalRef(handler_class);
  if (handler == nullptr) {
    return nullptr;
  }
  auto binding = std::make_shared<MessageBusRequestHandlerBinding>();
  binding->context = std::move(context);
  binding->callbacks = callbacks;
  std::lock_guard<std::mutex> lock(message_bus_request_handler_mutex_);
  message_bus_request_handler_bindings_[handler] = std::move(binding);
  return handler;
}

void VM::ClearMessageBusRequestHandler(jobject handler) {
  std::lock_guard<std::mutex> lock(message_bus_request_handler_mutex_);
  message_bus_request_handler_bindings_.erase(handler);
}

jstring VM::DispatchMessageBusRequestHandler(jobject handler, JNIEnv* env,
                                             jstring message) {
  std::shared_ptr<MessageBusRequestHandlerBinding> binding;
  {
    std::lock_guard<std::mutex> lock(message_bus_request_handler_mutex_);
    const auto found = message_bus_request_handler_bindings_.find(handler);
    if (found != message_bus_request_handler_bindings_.end()) {
      binding = found->second;
    }
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.run == nullptr || env == nullptr) {
    return nullptr;
  }
  const std::string response =
      binding->callbacks.run(binding->context.get(), env, message);
  return env->NewStringUTF(response.c_str());
}

jobject VM::CreateMemStorageCallback(
    std::shared_ptr<void> context,
    const MemStorageCallbackCallbacks& callbacks) {
  if (context == nullptr || callbacks.on_item_set == nullptr) {
    return nullptr;
  }
  JNIEnv* env = GetJNIEnv();
  if (env == nullptr) {
    return nullptr;
  }
  jclass callback_class =
      env->FindClass("com/roblox/engine/jni/memstorage/Callback");
  if (callback_class == nullptr) {
    return nullptr;
  }
  jobject callback = env->AllocObject(callback_class);
  env->DeleteLocalRef(callback_class);
  if (callback == nullptr) {
    return nullptr;
  }

  auto binding = std::make_shared<MemStorageCallbackBinding>();
  binding->context = std::move(context);
  binding->callbacks = callbacks;
  {
    std::lock_guard<std::mutex> lock(mem_storage_callback_mutex_);
    mem_storage_callback_bindings_[callback] = std::move(binding);
  }
  return callback;
}

void VM::ClearMemStorageCallback(jobject callback) {
  std::shared_ptr<MemStorageCallbackBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(mem_storage_callback_mutex_);
    const auto found = mem_storage_callback_bindings_.find(callback);
    if (found == mem_storage_callback_bindings_.end()) {
      return;
    }
    old_binding = std::move(found->second);
    mem_storage_callback_bindings_.erase(found);
  }
}

bool VM::DispatchMemStorageCallback(jobject callback, JNIEnv *env,
                                    jstring value) {
  std::shared_ptr<MemStorageCallbackBinding> binding;
  {
    std::lock_guard<std::mutex> lock(mem_storage_callback_mutex_);
    const auto found = mem_storage_callback_bindings_.find(callback);
    if (found != mem_storage_callback_bindings_.end()) {
      binding = found->second;
    }
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_item_set == nullptr || env == nullptr ||
      value == nullptr) {
    return false;
  }
  binding->callbacks.on_item_set(binding->context.get(), env, value);
  return true;
}

void VM::SetRobloxDataModelNotificationCallbacks(
    std::shared_ptr<void> context,
    const RobloxDataModelNotificationCallbacks &callbacks) {
  std::shared_ptr<RobloxDataModelNotificationBinding> binding;
  if (context != nullptr && callbacks.on_notification != nullptr) {
    binding = std::make_shared<RobloxDataModelNotificationBinding>();
    binding->context = std::move(context);
    binding->callbacks = callbacks;
  }
  std::shared_ptr<RobloxDataModelNotificationBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    old_binding = std::move(roblox_data_model_notification_binding_);
    roblox_data_model_notification_binding_ = std::move(binding);
  }
}

void VM::ClearRobloxDataModelNotificationCallbacks() {
  std::shared_ptr<RobloxDataModelNotificationBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    old_binding = std::move(roblox_data_model_notification_binding_);
  }
}

bool VM::DispatchRobloxDataModelNotification(JNIEnv *env, jstring type,
                                             jstring data) {
  std::shared_ptr<RobloxDataModelNotificationBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    binding = roblox_data_model_notification_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_notification == nullptr || env == nullptr ||
      type == nullptr || data == nullptr) {
    return false;
  }
  binding->callbacks.on_notification(binding->context.get(), env, type, data);
  return true;
}

bool VM::DispatchRobloxAppBridgeNotification(JNIEnv* env, jstring type,
                                             jstring data) {
  std::shared_ptr<RobloxDataModelNotificationBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    binding = roblox_data_model_notification_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_app_bridge_notification == nullptr ||
      env == nullptr || type == nullptr || data == nullptr) {
    return false;
  }
  binding->callbacks.on_app_bridge_notification(binding->context.get(), env,
                                                type, data);
  return true;
}

bool VM::DispatchRobloxNativeOverlay(JNIEnv* env, jstring title, jstring url) {
  std::shared_ptr<RobloxDataModelNotificationBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    binding = roblox_data_model_notification_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_native_overlay == nullptr || env == nullptr ||
      title == nullptr || url == nullptr) {
    return false;
  }
  binding->callbacks.on_native_overlay(binding->context.get(), env, title, url);
  return true;
}

bool VM::DispatchRobloxOpenWebActivity(JNIEnv* env, jstring url,
                                       jstring title) {
  std::shared_ptr<RobloxDataModelNotificationBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    binding = roblox_data_model_notification_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_open_web_activity == nullptr || env == nullptr ||
      url == nullptr || title == nullptr) {
    return false;
  }
  binding->callbacks.on_open_web_activity(binding->context.get(), env, url,
                                          title);
  return true;
}

bool VM::DispatchRobloxCookieSync(JNIEnv* env, jstring cookie) {
  std::shared_ptr<RobloxDataModelNotificationBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    binding = roblox_data_model_notification_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_sync_cookies == nullptr || env == nullptr ||
      cookie == nullptr) {
    return false;
  }
  binding->callbacks.on_sync_cookies(binding->context.get(), env, cookie);
  return true;
}

bool VM::DispatchRobloxCookieSet(JNIEnv* env, jobjectArray cookies,
                                jstring url) {
  std::shared_ptr<RobloxDataModelNotificationBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_data_model_notification_mutex_);
    binding = roblox_data_model_notification_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_set_cookie == nullptr || env == nullptr ||
      cookies == nullptr || url == nullptr) {
    return false;
  }
  const jsize cookie_count = env->GetArrayLength(cookies);
  if (cookie_count < 0 || cookie_count > kMaximumCookieSetCount) {
    return false;
  }
  for (jsize index = 0; index < cookie_count; ++index) {
    auto cookie =
        static_cast<jstring>(env->GetObjectArrayElement(cookies, index));
    if (cookie == nullptr) {
      continue;
    }
    const jsize cookie_size = env->GetStringUTFLength(cookie);
    if (cookie_size <= 0 || cookie_size > kMaximumCookieSetBytes) {
      env->DeleteLocalRef(cookie);
      continue;
    }
    const char* cookie_chars = env->GetStringUTFChars(cookie, nullptr);
    if (cookie_chars == nullptr) {
      env->DeleteLocalRef(cookie);
      continue;
    }
    std::string raw_cookie(cookie_chars,
                           static_cast<std::size_t>(cookie_size));
    env->ReleaseStringUTFChars(cookie, cookie_chars);
    env->DeleteLocalRef(cookie);
    std::string canonical_cookie = NormalizeCookieHeader(raw_cookie);
    ClearCookieString(&raw_cookie);
    if (canonical_cookie.empty()) {
      continue;
    }
    StoreCookieHeader(canonical_cookie);
    jstring callback_cookie = env->NewStringUTF(canonical_cookie.c_str());
    ClearCookieString(&canonical_cookie);
    if (callback_cookie == nullptr) {
      continue;
    }
    binding->callbacks.on_set_cookie(binding->context.get(), env,
                                     callback_cookie, url);
    env->DeleteLocalRef(callback_cookie);
  }
  return true;
}

void VM::SetRobloxExperienceLifecycleCallbacks(
    std::shared_ptr<void> context,
    const RobloxExperienceLifecycleCallbacks &callbacks) {
  std::shared_ptr<RobloxExperienceLifecycleBinding> binding;
  if (context != nullptr && callbacks.on_lua_app_did_return != nullptr) {
    binding = std::make_shared<RobloxExperienceLifecycleBinding>();
    binding->context = std::move(context);
    binding->callbacks = callbacks;
  }
  std::shared_ptr<RobloxExperienceLifecycleBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(roblox_experience_lifecycle_mutex_);
    old_binding = std::move(roblox_experience_lifecycle_binding_);
    roblox_experience_lifecycle_binding_ = std::move(binding);
  }
}

void VM::ClearRobloxExperienceLifecycleCallbacks() {
  std::shared_ptr<RobloxExperienceLifecycleBinding> old_binding;
  {
    std::lock_guard<std::mutex> lock(roblox_experience_lifecycle_mutex_);
    old_binding = std::move(roblox_experience_lifecycle_binding_);
  }
}

bool VM::DispatchRobloxExperienceLuaAppDidReturn() {
  std::shared_ptr<RobloxExperienceLifecycleBinding> binding;
  {
    std::lock_guard<std::mutex> lock(roblox_experience_lifecycle_mutex_);
    binding = roblox_experience_lifecycle_binding_;
  }
  if (binding == nullptr || binding->context == nullptr ||
      binding->callbacks.on_lua_app_did_return == nullptr) {
    return false;
  }
  binding->callbacks.on_lua_app_did_return(binding->context.get());
  return true;
}

void VM::SetFmodAudioDeviceCallbacks(
    std::shared_ptr<void> context,
    const FmodAudioDeviceCallbacks& callbacks) {
  FmodAudioDeviceBinding old_binding;
  {
    std::lock_guard<std::mutex> lock(fmod_audio_device_mutex_);
    old_binding = std::move(fmod_audio_device_binding_);
    fmod_audio_device_binding_.context = std::move(context);
    fmod_audio_device_binding_.callbacks = callbacks;
  }
  if (old_binding.context != nullptr &&
      old_binding.callbacks.shutdown != nullptr) {
    old_binding.callbacks.shutdown(old_binding.context.get());
  }
}

void VM::ClearFmodAudioDeviceCallbacks() {
  FmodAudioDeviceBinding old_binding;
  {
    std::lock_guard<std::mutex> lock(fmod_audio_device_mutex_);
    old_binding = std::move(fmod_audio_device_binding_);
    fmod_audio_device_binding_ = {};
  }
  if (old_binding.context != nullptr &&
      old_binding.callbacks.shutdown != nullptr) {
    old_binding.callbacks.shutdown(old_binding.context.get());
  }
}

bool VM::DispatchFmodAudioDeviceInit(const void* identity, int channels,
                                     int sample_rate_hz,
                                     int block_size_frames,
                                     int block_count) {
  FmodAudioDeviceBinding binding;
  {
    std::lock_guard<std::mutex> lock(fmod_audio_device_mutex_);
    binding = fmod_audio_device_binding_;
  }
  return binding.context != nullptr && binding.callbacks.init != nullptr &&
         binding.callbacks.init(binding.context.get(), identity, channels,
                                sample_rate_hz, block_size_frames,
                                block_count);
}

bool VM::DispatchFmodAudioDeviceWrite(const void* identity,
                                      const std::uint8_t* data,
                                      std::size_t size) {
  FmodAudioDeviceBinding binding;
  {
    std::lock_guard<std::mutex> lock(fmod_audio_device_mutex_);
    binding = fmod_audio_device_binding_;
  }
  return binding.context != nullptr && binding.callbacks.write != nullptr &&
         binding.callbacks.write(binding.context.get(), identity, data, size);
}

bool VM::DispatchFmodAudioDeviceClose(const void* identity) {
  FmodAudioDeviceBinding binding;
  {
    std::lock_guard<std::mutex> lock(fmod_audio_device_mutex_);
    binding = fmod_audio_device_binding_;
  }
  return binding.context != nullptr && binding.callbacks.close != nullptr &&
         binding.callbacks.close(binding.context.get(), identity);
}

void VM::SetAndroidWindowCallbacks(
    std::shared_ptr<void> context,
    const AndroidWindowCallbacks& callbacks) {
  std::lock_guard<std::mutex> lock(android_window_mutex_);
  android_window_binding_.context = std::move(context);
  android_window_binding_.callbacks = callbacks;
}

void VM::ClearAndroidWindowCallbacks() {
  std::lock_guard<std::mutex> lock(android_window_mutex_);
  android_window_binding_ = {};
}

bool VM::DispatchAndroidWindowFlags(int flags, int mask) {
  AndroidWindowBinding binding;
  {
    std::lock_guard<std::mutex> lock(android_window_mutex_);
    binding = android_window_binding_;
  }
  return binding.context != nullptr && binding.callbacks.set_flags != nullptr &&
         binding.callbacks.set_flags(binding.context.get(), flags, mask);
}

void VM::SetRobloxAuthIdentity(const RobloxAuthIdentity& identity) {
  std::lock_guard<std::mutex> lock(roblox_auth_identity_mutex_);
  roblox_auth_identity_ =
      identity.user_id > 0 ? identity : RobloxAuthIdentity{};
}

void VM::ClearRobloxAuthIdentity() {
  std::lock_guard<std::mutex> lock(roblox_auth_identity_mutex_);
  roblox_auth_identity_ = {};
}

RobloxAuthIdentity VM::GetRobloxAuthIdentitySnapshot() const {
  std::lock_guard<std::mutex> lock(roblox_auth_identity_mutex_);
  return roblox_auth_identity_;
}

void VM::SetPlatformIdentity(const PlatformIdentity& identity) {
  std::lock_guard<std::mutex> lock(platform_identity_mutex_);
  platform_identity_ = identity;
}

PlatformIdentity VM::GetPlatformIdentitySnapshot() const {
  std::lock_guard<std::mutex> lock(platform_identity_mutex_);
  return platform_identity_;
}

void VM::SetRobloxCredentialProvider(const void* context,
                                     RobloxCredentialProvider provider) {
  {
    std::lock_guard<std::mutex> lock(roblox_credential_provider_mutex_);
    ClearCookieString(&roblox_credential_override_);
    roblox_credential_provider_context_ = context;
    roblox_credential_provider_ = provider;
  }
  if (provider != nullptr) {
    ClearLegacyCookieStore();
  }
}

void VM::ClearRobloxCredentialProvider() {
  std::lock_guard<std::mutex> lock(roblox_credential_provider_mutex_);
  ClearCookieString(&roblox_credential_override_);
  roblox_credential_provider_ = nullptr;
  roblox_credential_provider_context_ = nullptr;
}

bool VM::CopyRobloxCredentialFromProvider(std::string* credential) const {
  std::lock_guard<std::mutex> lock(roblox_credential_provider_mutex_);
  if (roblox_credential_provider_ == nullptr) {
    if (credential != nullptr) {
      credential->clear();
    }
    return false;
  }
  if (credential == nullptr) {
    return true;
  }
  if (!roblox_credential_override_.empty()) {
    credential->assign(roblox_credential_override_);
    return true;
  }
  const RobloxCredentialView view =
      roblox_credential_provider_(roblox_credential_provider_context_);
  if (view.data == nullptr || view.size == 0) {
    credential->clear();
  } else {
    credential->assign(view.data, view.size);
  }
  return true;
}

void VM::SetRobloxCredentialSink(
    std::shared_ptr<void> context,
    const RobloxCredentialSinkCallbacks& callbacks) {
  std::lock_guard<std::mutex> lock(roblox_credential_sink_mutex_);
  roblox_credential_sink_binding_.context = std::move(context);
  roblox_credential_sink_binding_.callbacks = callbacks;
}

void VM::ClearRobloxCredentialSink() {
  RobloxCredentialSinkBinding old_binding;
  {
    std::lock_guard<std::mutex> lock(roblox_credential_sink_mutex_);
    old_binding = std::move(roblox_credential_sink_binding_);
    roblox_credential_sink_binding_ = {};
  }
}

bool VM::DispatchRobloxCredential(const char* data, std::size_t size) {
  RobloxCredentialSinkBinding binding;
  {
    std::lock_guard<std::mutex> lock(roblox_credential_sink_mutex_);
    binding = roblox_credential_sink_binding_;
  }
  const bool stored =
      binding.context != nullptr && binding.callbacks.store != nullptr &&
      data != nullptr && size != 0 &&
      binding.callbacks.store(binding.context.get(), data, size);
  if (!stored) {
    return false;
  }

  std::string accepted(data, size);
  {
    std::lock_guard<std::mutex> lock(roblox_credential_provider_mutex_);
    if (roblox_credential_provider_ != nullptr) {
      ClearCookieString(&roblox_credential_override_);
      roblox_credential_override_ = std::move(accepted);
    }
  }
  ClearCookieString(&accepted);
  return true;
}

JNIEnv* VM::GetJNIEnv() {
  g_thread_vm_instance = this;
  if (JniVmTraceEnabled()) {
    std::cout << "  [JNI] GetJNIEnv enter\n";
  }
  if (!jni_env_) {
    jni_env_ = new JNIEnv();
  }
  jni_env_->functions = &native_interface_;
  if (IsThreadLocalEnvValid()) {
    if (JniVmTraceEnabled()) {
      std::cout << "  [JNI] GetJNIEnv thread-local hit\n";
    }
    return g_thread_local_env;
  }
  g_thread_env_storage.functions = &native_interface_;
  g_thread_local_env = &g_thread_env_storage;
  if (JniVmTraceEnabled()) {
    std::cout << "  [JNI] GetJNIEnv attached thread-local env\n";
  }
  return g_thread_local_env;
}

void VM::RestoreFunctions() {
  // JNI_OnLoad replaces env->functions; restore all known environments.
  java_vm_storage_.functions = &invoke_interface_;
  java_vm_ = &java_vm_storage_;
  if (jni_env_) {
    jni_env_->functions = &native_interface_;
  }
  if (IsThreadLocalEnvValid()) {
    g_thread_local_env->functions = &native_interface_;
  }
}

std::shared_ptr<Class> VM::RegisterClass(const std::string& class_name) {
  if (JniVmTraceEnabled()) {
    fprintf(stderr, "  [JNI-VM] RegisterClass this=%p name_ref=%p name=\"%s\"\n",
            static_cast<void*>(this), static_cast<const void*>(&class_name),
            class_name.c_str());
  }
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto it = class_registry_.find(class_name);
  if (it != class_registry_.end()) {
    return it->second;
  }
  auto cls = std::make_shared<Class>(class_name);
  class_registry_[class_name] = cls;
  return cls;
}

std::shared_ptr<Class> VM::FindClass(const std::string& class_name) const {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto it = class_registry_.find(class_name);
  if (it == class_registry_.end()) {
    return nullptr;
  }
  return it->second;
}

void VM::InitJNIFunctionTables() {
  invoke_interface_.AttachCurrentThread =
      [](JavaVM* vm, void** env, void* /*args*/) -> jint {
    if (JniVmTraceEnabled()) {
      std::cout << "  [JNI] AttachCurrentThread enter vm=" << vm
                << " env_out=" << env << '\n';
    }
    if (!env || !g_vm_instance) {
      if (JniVmTraceEnabled()) {
        std::cout << "  [JNI] AttachCurrentThread invalid args\n";
      }
      return JNI_EDETACHED;
    }
    if (!g_vm_instance) {
      if (JniVmTraceEnabled()) {
        std::cout << "  [JNI] AttachCurrentThread no env\n";
      }
      return JNI_ERR;
    }
    g_thread_vm_instance = g_vm_instance;
    if (!g_vm_instance->jni_env_) {
      g_vm_instance->jni_env_ = &g_vm_instance->jni_env_storage_;
      g_vm_instance->jni_env_->functions = &g_vm_instance->native_interface_;
    }
    g_thread_env_storage.functions =
        g_vm_instance->jni_env_->functions
            ? g_vm_instance->jni_env_->functions
            : &g_vm_instance->native_interface_;
    g_thread_local_env = &g_thread_env_storage;
    *env = g_thread_local_env;
    if (JniVmTraceEnabled()) {
      std::cout << "  [JNI] AttachCurrentThread return env="
                << g_thread_local_env
                << '\n';
    }
    return JNI_OK;
  };

  invoke_interface_.AttachCurrentThreadAsDaemon =
      invoke_interface_.AttachCurrentThread;

  invoke_interface_.DetachCurrentThread = [](JavaVM* /*vm*/) -> jint {
    if (IsThreadLocalEnvValid()) {
      g_thread_local_env = nullptr;
      g_thread_vm_instance = nullptr;
    }
    return JNI_OK;
  };

  invoke_interface_.GetEnv =
      [](JavaVM* vm, void** env, jint version) -> jint {
    if (JniVmTraceEnabled()) {
      std::cout << "  [JNI] GetEnv enter vm=" << vm << " env_out=" << env
                << " version=" << version << '\n';
    }
    if (!env || !g_vm_instance) {
      if (JniVmTraceEnabled()) {
        std::cout << "  [JNI] GetEnv invalid args\n";
      }
      return JNI_EINVAL;
    }
    if (!IsThreadLocalEnvValid()) {
      *env = nullptr;
      if (JniVmTraceEnabled()) {
        std::cout << "  [JNI] GetEnv return detached\n";
      }
      return JNI_EDETACHED;
    }
    *env = g_thread_local_env;
    g_thread_vm_instance = CurrentVM();
    if (JniVmTraceEnabled()) {
      std::cout << "  [JNI] GetEnv return env=" << *env << '\n';
    }
    return JNI_OK;
  };

  invoke_interface_.DestroyJavaVM = [](JavaVM* /*vm*/) -> jint {
    return JNI_OK;
  };

  java_vm_storage_.functions = &invoke_interface_;
  java_vm_ = &java_vm_storage_;

  native_interface_.FindClass =
      [](JNIEnv* /*env*/, const char* name) -> jclass {
    if (JniVmTraceEnabled()) {
      fprintf(stderr, "  [JNI-VM] FindClass name_ptr=%p name=\"%s\" vm=%p\n",
              static_cast<const void*>(name), name ? name : "",
              static_cast<void*>(CurrentVM()));
    }
    if (TraceEnabled()) {
      std::cout << "  [JNI] FindClass: " << (name ? name : "null") << '\n';
    }
    auto cls = FallbackClassForName(name ? name : "java/lang/Object");
    return StoreClass(std::move(cls));
  };

  native_interface_.GetVersion = [](JNIEnv* /*env*/) -> jint {
    return JNI_VERSION_1_6;
  };

  native_interface_.NewStringUTF =
      [](JNIEnv* /*env*/, const char* utf) -> jstring {
    return MakeString(utf);
  };

  native_interface_.GetStringUTFLength =
      [](JNIEnv* /*env*/, jstring str) -> jsize {
    return StringModifiedUtf8Length(str);
  };

  native_interface_.GetStringUTFChars =
      [](JNIEnv* /*env*/, jstring str, jboolean* isCopy) -> const char* {
    if (isCopy) *isCopy = JNI_FALSE;
    const char* result = StringChars(str);
    if (StringTraceEnabled()) {
      const std::size_t length =
          static_cast<std::size_t>(StringModifiedUtf8Length(str));
      fprintf(stderr,
              "  [JNI] GetStringUTFChars str=%p chars=%p len=%zu\n",
              static_cast<void*>(str), static_cast<const void*>(result),
              length);
    }
    return result;
  };

  native_interface_.ReleaseStringUTFChars =
      [](JNIEnv* /*env*/, jstring /*str*/, const char* /*chars*/) {};

  native_interface_.ExceptionOccurred = [](JNIEnv* /*env*/) -> jthrowable {
    return nullptr;
  };

  native_interface_.ExceptionDescribe = [](JNIEnv* /*env*/) {};

  native_interface_.ExceptionClear = [](JNIEnv* /*env*/) {};

  native_interface_.ExceptionCheck = [](JNIEnv* /*env*/) -> jboolean {
    return JNI_FALSE;
  };

  native_interface_.Throw = [](JNIEnv* /*env*/, jthrowable /*obj*/) -> jint {
    return JNI_ERR;
  };

  native_interface_.ThrowNew =
      [](JNIEnv* /*env*/, jclass /*clazz*/, const char* msg) -> jint {
    if (TraceEnabled()) {
      std::cout << "  [JNI] ThrowNew: " << (msg ? msg : "") << '\n';
    }
    return JNI_ERR;
  };

  native_interface_.FatalError = [](JNIEnv* /*env*/, const char* msg) {
    std::cerr << "[JNI] FatalError: " << (msg ? msg : "") << '\n';
    std::abort();
  };

  native_interface_.PushLocalFrame =
      [](JNIEnv* /*env*/, jint /*capacity*/) -> jint {
    return JNI_OK;
  };

  native_interface_.PopLocalFrame =
      [](JNIEnv* /*env*/, jobject result) -> jobject {
    return result;
  };

  native_interface_.EnsureLocalCapacity =
      [](JNIEnv* /*env*/, jint /*capacity*/) -> jint {
    return JNI_OK;
  };

	  native_interface_.RegisterNatives =
	      [](JNIEnv* /*env*/, jclass clazz, const JNINativeMethod* methods, jint nMethods) -> jint {
	    auto cls = ClassFromJClass(clazz);
	    if (methods == nullptr || nMethods <= 0) {
	      return JNI_OK;
	    }
	    for (int i = 0; i < nMethods; ++i) {
	      const char* name = methods[i].name;
	      if (name == nullptr) {
	        continue;
	      }
	      if (std::strcmp(name, "onStartNative") == 0) {
	        mocktail_gameactivity_on_start_native = methods[i].fnPtr;
	      } else if (std::strcmp(name, "onResumeNative") == 0) {
	        mocktail_gameactivity_on_resume_native = methods[i].fnPtr;
	      } else if (std::strcmp(name, "onSurfaceCreatedNative") == 0) {
	        mocktail_gameactivity_on_surface_created_native = methods[i].fnPtr;
	      } else if (std::strcmp(name, "onSurfaceChangedNative") == 0) {
	        mocktail_gameactivity_on_surface_changed_native = methods[i].fnPtr;
	      } else if (std::strcmp(name, "onSurfaceRedrawNeededNative") == 0) {
	        mocktail_gameactivity_on_surface_redraw_needed_native = methods[i].fnPtr;
	      }
	    }
	    if (TraceEnabled()) {
	      std::cout << "  [JNI] RegisterNatives for class "
	                << (cls ? cls->GetName() : "unknown") << " ("
                << nMethods << " methods):\n";
      for (int i = 0; i < nMethods; ++i) {
        std::cout << "    " << methods[i].name << " "
                  << methods[i].signature << " -> " << methods[i].fnPtr
                  << '\n';
      }
    }
    return JNI_OK;
  };

  native_interface_.UnregisterNatives =
      [](JNIEnv* /*env*/, jclass /*clazz*/) -> jint {
    return JNI_OK;
  };

  native_interface_.GetStaticMethodID =
      [](JNIEnv* /*env*/, jclass clazz, const char* name, const char* sig) -> jmethodID {
    auto cls = ClassFromJClass(clazz);
    if (TraceEnabled()) {
      std::cout << "  [JNI] GetStaticMethodID for class "
                << (cls ? cls->GetName() : "unknown") << ": "
                << (name ? name : "null") << " " << (sig ? sig : "null")
                << '\n';
    }
    return StoreMethodId(name, sig);
  };

  native_interface_.GetMethodID =
      [](JNIEnv* /*env*/, jclass clazz, const char* name, const char* sig) -> jmethodID {
    auto cls = ClassFromJClass(clazz);
    if (TraceEnabled()) {
      std::cout << "  [JNI] GetMethodID for class "
                << (cls ? cls->GetName() : "unknown") << ": "
                << (name ? name : "null") << " " << (sig ? sig : "null")
                << '\n';
    }
    return StoreMethodId(name, sig);
  };

  native_interface_.GetStaticFieldID =
      [](JNIEnv* /*env*/, jclass clazz, const char* name, const char* sig) -> jfieldID {
    auto cls = ClassFromJClass(clazz);
    if (TraceEnabled()) {
      std::cout << "  [JNI] GetStaticFieldID for class "
                << (cls ? cls->GetName() : "unknown") << ": "
                << (name ? name : "null") << " " << (sig ? sig : "null")
                << '\n';
    }
    return reinterpret_cast<jfieldID>(const_cast<char*>(name));
  };

  native_interface_.GetFieldID =
      [](JNIEnv* /*env*/, jclass clazz, const char* name, const char* sig) -> jfieldID {
    auto cls = ClassFromJClass(clazz);
    if (TraceEnabled()) {
      std::cout << "  [JNI] GetFieldID for class "
                << (cls ? cls->GetName() : "unknown") << ": "
                << (name ? name : "null") << " " << (sig ? sig : "null")
                << '\n';
    }
    return reinterpret_cast<jfieldID>(const_cast<char*>(name));
  };

  native_interface_.AllocObject =
      [](JNIEnv* /*env*/, jclass clazz) -> jobject {
    Trace("AllocObject");
    return MakeObject(clazz);
  };

  native_interface_.NewObject = NewObject;

  native_interface_.NewObjectV = [](JNIEnv * /*env*/, jclass clazz,
                                    jmethodID methodID,
                                    va_list args) -> jobject {
    if (TraceEnabled()) {
      std::cout << "  [JNI] NewObjectV: " << MethodName(methodID) << '\n';
    }
    va_list copy;
    va_copy(copy, args);
    jobject object = ConstructObjectV(clazz, methodID, copy);
    va_end(copy);
    return object;
  };

  native_interface_.NewObjectA = [](JNIEnv * /*env*/, jclass clazz,
                                    jmethodID methodID,
                                    const jvalue *args) -> jobject {
    if (TraceEnabled()) {
      std::cout << "  [JNI] NewObjectA: " << MethodName(methodID) << '\n';
    }
    return ConstructObjectA(clazz, methodID, args);
  };

  native_interface_.GetObjectClass =
      [](JNIEnv* /*env*/, jobject obj) -> jclass {
    Trace("GetObjectClass");
    auto* pseudo_object = PseudoObjectFromRef(obj);
    if (pseudo_object) {
      return StoreClass(pseudo_object->GetClass());
    }
    return StoreClass(FallbackClassForName("java/lang/Object"));
  };

  native_interface_.GetSuperclass =
      [](JNIEnv* /*env*/, jclass /*sub*/) -> jclass {
    return StoreClass(FallbackClassForName("java/lang/Object"));
  };

  native_interface_.IsAssignableFrom =
      [](JNIEnv* /*env*/, jclass sub, jclass sup) -> jboolean {
    return (sub == sup || sup == nullptr) ? JNI_TRUE : JNI_TRUE;
  };

  native_interface_.IsInstanceOf =
      [](JNIEnv* /*env*/, jobject obj, jclass /*clazz*/) -> jboolean {
    return obj ? JNI_TRUE : JNI_FALSE;
  };

  native_interface_.IsSameObject =
      [](JNIEnv* /*env*/, jobject obj1, jobject obj2) -> jboolean {
    return obj1 == obj2 ? JNI_TRUE : JNI_FALSE;
  };

  native_interface_.FromReflectedMethod =
      [](JNIEnv* /*env*/, jobject method) -> jmethodID {
    return reinterpret_cast<jmethodID>(method);
  };

  native_interface_.FromReflectedField =
      [](JNIEnv* /*env*/, jobject field) -> jfieldID {
    return reinterpret_cast<jfieldID>(field);
  };

  native_interface_.ToReflectedMethod =
      [](JNIEnv* /*env*/, jclass /*cls*/, jmethodID methodID,
         jboolean /*isStatic*/) -> jobject {
    return reinterpret_cast<jobject>(methodID);
  };

  native_interface_.ToReflectedField =
      [](JNIEnv* /*env*/, jclass /*cls*/, jfieldID fieldID,
         jboolean /*isStatic*/) -> jobject {
    return reinterpret_cast<jobject>(fieldID);
  };

  native_interface_.CallStaticVoidMethod = CallStaticVoidMethod;

  native_interface_.CallStaticVoidMethodV =
      [](JNIEnv* env, jclass clazz, jmethodID methodID, va_list args) {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticVoidMethodV: " << MethodName(methodID) << '\n';
    }
    HandleStaticVoidMethodV(env, clazz, methodID, args);
  };

  native_interface_.CallStaticVoidMethodA =
      [](JNIEnv* env, jclass clazz, jmethodID methodID,
         const jvalue* args) {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticVoidMethodA: " << MethodName(methodID) << '\n';
    }
    HandleStaticVoidMethodA(env, clazz, methodID, args);
  };

  native_interface_.CallStaticObjectMethod = CallStaticObjectMethod;

  native_interface_.CallStaticObjectMethodV = [](JNIEnv * /*env*/, jclass clazz,
                                                 jmethodID methodID,
                                                 va_list args) -> jobject {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticObjectMethodV: " << MethodName(methodID)
                << '\n';
    }
    jobject result = ObjectResultForMethodV(nullptr, methodID, args);
    if (result != nullptr) {
      return result;
    }
    result = ExactMessageBusStaticObject(clazz, methodID);
    return result != nullptr ? result : StaticObjectResultForMethod(methodID);
  };

  native_interface_.CallStaticObjectMethodA =
      [](JNIEnv * /*env*/, jclass clazz, jmethodID methodID,
         const jvalue *args) -> jobject {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticObjectMethodA: " << MethodName(methodID)
                << '\n';
    }
    const char *name = MethodName(methodID);
    if (args && std::strcmp(name, "forName") == 0) {
      return ClassObjectForName(
          StringFromJString(reinterpret_cast<jstring>(args[0].l)));
    }
    jobject cookie_result = nullptr;
    if (CookieObjectResultForMethodA(name, args, &cookie_result)) {
      return cookie_result;
    }
    jobject message_bus = ExactMessageBusStaticObject(clazz, methodID);
    if (message_bus != nullptr) {
      return message_bus;
    }
    return StaticObjectResultForMethod(methodID);
  };

  native_interface_.CallStaticBooleanMethod = CallStaticBooleanMethod;

  native_interface_.CallStaticBooleanMethodV =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jmethodID methodID, va_list /*args*/) -> jboolean {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticBooleanMethodV: " << MethodName(methodID) << '\n';
    }
    jboolean result = JNI_FALSE;
    const char* name = MethodName(methodID);
    if (FmodBooleanResultForMethod(name, &result)) {
      return result;
    }
    return CookieBooleanResultForMethod(name, &result) ? result : JNI_FALSE;
  };

  native_interface_.CallStaticBooleanMethodA =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jmethodID methodID,
         const jvalue* /*args*/) -> jboolean {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticBooleanMethodA: " << MethodName(methodID) << '\n';
    }
    jboolean result = JNI_FALSE;
    const char* name = MethodName(methodID);
    if (FmodBooleanResultForMethod(name, &result)) {
      return result;
    }
    return CookieBooleanResultForMethod(name, &result) ? result : JNI_FALSE;
  };

  native_interface_.CallStaticByteMethod = CallStaticByteMethod;
  native_interface_.CallStaticByteMethodV =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jmethodID /*methodID*/,
         va_list /*args*/) -> jbyte { return 0; };
  native_interface_.CallStaticByteMethodA =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jmethodID /*methodID*/,
         const jvalue* /*args*/) -> jbyte { return 0; };

  native_interface_.CallStaticCharMethod = CallStaticCharMethod;
  native_interface_.CallStaticCharMethodV =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jmethodID /*methodID*/,
         va_list /*args*/) -> jchar { return 0; };
  native_interface_.CallStaticCharMethodA =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jmethodID /*methodID*/,
         const jvalue* /*args*/) -> jchar { return 0; };

  native_interface_.CallStaticShortMethod = CallStaticShortMethod;
  native_interface_.CallStaticShortMethodV =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jmethodID /*methodID*/,
         va_list /*args*/) -> jshort { return 0; };
  native_interface_.CallStaticShortMethodA =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jmethodID /*methodID*/,
         const jvalue* /*args*/) -> jshort { return 0; };

  native_interface_.CallStaticIntMethod = CallStaticIntMethod;

  native_interface_.CallStaticIntMethodV =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jmethodID methodID, va_list args) -> jint {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticIntMethodV: " << MethodName(methodID) << '\n';
    }
    return StaticIntResultForMethodV(methodID, args);
  };

  native_interface_.CallStaticIntMethodA =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jmethodID methodID,
         const jvalue* args) -> jint {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticIntMethodA: " << MethodName(methodID) << '\n';
    }
    return StaticIntResultForMethodA(methodID, args);
  };

  native_interface_.CallStaticLongMethod = CallStaticLongMethod;

  native_interface_.CallStaticLongMethodV =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jmethodID methodID, va_list /*args*/) -> jlong {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticLongMethodV: " << MethodName(methodID) << '\n';
    }
    return StaticLongResultForMethod(methodID);
  };

  native_interface_.CallStaticLongMethodA =
      [](JNIEnv * /*env*/, jclass /*clazz*/, jmethodID methodID,
         const jvalue * /*args*/) -> jlong {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallStaticLongMethodA: " << MethodName(methodID) << '\n';
    }
    return StaticLongResultForMethod(methodID);
  };

  native_interface_.CallStaticFloatMethod = CallStaticFloatMethod;
  native_interface_.CallStaticFloatMethodV =
      [](JNIEnv * /*env*/, jclass /*clazz*/, jmethodID /*methodID*/,
         va_list /*args*/) -> jfloat { return 0.0f; };
  native_interface_.CallStaticFloatMethodA =
      [](JNIEnv * /*env*/, jclass /*clazz*/, jmethodID /*methodID*/,
         const jvalue * /*args*/) -> jfloat { return 0.0f; };

  native_interface_.CallStaticDoubleMethod = CallStaticDoubleMethod;
  native_interface_.CallStaticDoubleMethodV =
      [](JNIEnv * /*env*/, jclass /*clazz*/, jmethodID /*methodID*/,
         va_list /*args*/) -> jdouble { return 0.0; };
  native_interface_.CallStaticDoubleMethodA =
      [](JNIEnv * /*env*/, jclass /*clazz*/, jmethodID /*methodID*/,
         const jvalue * /*args*/) -> jdouble { return 0.0; };

  native_interface_.CallVoidMethod = CallVoidMethod;

  native_interface_.CallVoidMethodV = [](JNIEnv * /*env*/, jobject obj,
                                         jmethodID methodID, va_list args) {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallVoidMethodV: " << MethodName(methodID) << '\n';
    }
    if (!HandleRobloxExperienceLifecycleVoidMethod(obj, methodID) &&
        !HandleRobloxOpenWebActivityMethodV(obj, methodID, args) &&
        !HandleRobloxTextInputInstanceVoidMethodV(obj, methodID, args) &&
        !HandleFmodAudioDeviceVoidMethodV(obj, methodID, args) &&
        !HandleRobloxCookieSetVoidMethodV(obj, methodID, args) &&
        !HandleMemStorageCallbackVoidMethodV(obj, methodID, args)) {
      HandleVoidMethod(obj, methodID, args);
    }
  };

  native_interface_.CallVoidMethodA = [](JNIEnv * /*env*/, jobject obj,
                                         jmethodID methodID,
                                         const jvalue *args) {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallVoidMethodA: " << MethodName(methodID) << '\n';
    }
    if (!HandleRobloxExperienceLifecycleVoidMethod(obj, methodID) &&
        !HandleRobloxOpenWebActivityMethodA(obj, methodID, args) &&
        !HandleRobloxTextInputInstanceVoidMethodA(obj, methodID, args) &&
        !HandleFmodAudioDeviceVoidMethodA(obj, methodID, args) &&
        !HandleRobloxCookieSetVoidMethodA(obj, methodID, args) &&
        !HandleMemStorageCallbackVoidMethodA(obj, methodID, args)) {
      HandleVoidMethodA(obj, methodID, args);
    }
  };

  native_interface_.CallObjectMethod = CallObjectMethod;

  native_interface_.CallObjectMethodV =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         va_list args) -> jobject {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallObjectMethodV: " << MethodName(methodID) << '\n';
    }
    return ObjectResultForMethodV(obj, methodID, args);
  };

  native_interface_.CallObjectMethodA =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         const jvalue* args) -> jobject {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallObjectMethodA: " << MethodName(methodID) << '\n';
    }
    const char* name = MethodName(methodID);
    if (IsJavaStringGetBytesMethod(obj, methodID)) {
      return args != nullptr
                 ? JavaStringGetUtf8Bytes(
                       obj, static_cast<jstring>(args[0].l))
                 : nullptr;
    }
    if (args && std::strcmp(name, "run") == 0 &&
        ObjectClassName(obj) ==
            "com/roblox/universalapp/messagebus/RequestHandlerRaw") {
      VM* vm = CurrentVM();
      return vm != nullptr
                 ? vm->DispatchMessageBusRequestHandler(
                       obj, vm->GetJNIEnv(), static_cast<jstring>(args[0].l))
                 : nullptr;
    }
    jobject local_storage_result = nullptr;
    if (LocalStorageObjectResultForMethodA(name, args, &local_storage_result)) {
      return local_storage_result;
    }
    jobject cookie_result = nullptr;
    if (CookieObjectResultForMethodA(name, args, &cookie_result)) {
      return cookie_result;
    }
    if (args && std::strcmp(name, "getSystemService") == 0) {
      return SystemServiceObject(StringFromJString(
          reinterpret_cast<jstring>(args[0].l)));
    }
    if (args && (std::strcmp(name, "loadClass") == 0 ||
                 std::strcmp(name, "findClass") == 0)) {
      return ClassObjectForName(StringFromJString(
          reinterpret_cast<jstring>(args[0].l)));
    }
    if (args && std::strcmp(name, "getString") == 0) {
      return args[1].l ? args[1].l : MakeString("");
    }
    jobject receiver_result = ObjectResultForReceiverMethod(obj, name);
    return receiver_result ? receiver_result : ObjectResultForMethod(methodID);
  };

  native_interface_.CallBooleanMethod = CallBooleanMethod;

  native_interface_.CallBooleanMethodV =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         va_list args) -> jboolean {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallBooleanMethodV: " << MethodName(methodID) << '\n';
    }
    jboolean result = JNI_FALSE;
    if (HandleFmodAudioDeviceBooleanMethodV(obj, methodID, args, &result)) {
      return result;
    }
    if (PackageManagerBooleanResultForMethodV(obj, methodID, args, &result)) {
      return result;
    }
    if (LocalStorageBooleanResultForMethodV(MethodName(methodID), args,
                                            &result)) {
      return result;
    }
    if (CookieBooleanResultForMethod(MethodName(methodID), &result)) {
      return result;
    }
    return BooleanResultForReceiverMethod(obj, MethodName(methodID));
  };

  native_interface_.CallBooleanMethodA =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         const jvalue* args) -> jboolean {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallBooleanMethodA: " << MethodName(methodID) << '\n';
    }
    jboolean result = JNI_FALSE;
    if (HandleFmodAudioDeviceBooleanMethodA(obj, methodID, args, &result)) {
      return result;
    }
    if (PackageManagerBooleanResultForMethodA(obj, methodID, args, &result)) {
      return result;
    }
    if (LocalStorageBooleanResultForMethodA(MethodName(methodID), args,
                                            &result)) {
      return result;
    }
    if (CookieBooleanResultForMethod(MethodName(methodID), &result)) {
      return result;
    }
    return BooleanResultForReceiverMethod(obj, MethodName(methodID));
  };

  native_interface_.CallByteMethod = CallByteMethod;
  native_interface_.CallByteMethodV =
      [](JNIEnv* /*env*/, jobject /*obj*/, jmethodID /*methodID*/,
         va_list /*args*/) -> jbyte { return 0; };
  native_interface_.CallByteMethodA =
      [](JNIEnv* /*env*/, jobject /*obj*/, jmethodID /*methodID*/,
         const jvalue* /*args*/) -> jbyte { return 0; };

  native_interface_.CallCharMethod = CallCharMethod;
  native_interface_.CallCharMethodV =
      [](JNIEnv* /*env*/, jobject /*obj*/, jmethodID /*methodID*/,
         va_list /*args*/) -> jchar { return 0; };
  native_interface_.CallCharMethodA =
      [](JNIEnv* /*env*/, jobject /*obj*/, jmethodID /*methodID*/,
         const jvalue* /*args*/) -> jchar { return 0; };

  native_interface_.CallShortMethod = CallShortMethod;
  native_interface_.CallShortMethodV =
      [](JNIEnv* /*env*/, jobject /*obj*/, jmethodID /*methodID*/,
         va_list /*args*/) -> jshort { return 0; };
  native_interface_.CallShortMethodA =
      [](JNIEnv* /*env*/, jobject /*obj*/, jmethodID /*methodID*/,
         const jvalue* /*args*/) -> jshort { return 0; };

  native_interface_.CallIntMethod = CallIntMethod;

  native_interface_.CallIntMethodV =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID, va_list /*args*/) -> jint {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallIntMethodV: " << MethodName(methodID) << '\n';
    }
    return IntResultForReceiverMethod(obj, MethodName(methodID));
  };

  native_interface_.CallIntMethodA =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         const jvalue* /*args*/) -> jint {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallIntMethodA: " << MethodName(methodID) << '\n';
    }
    return IntResultForReceiverMethod(obj, MethodName(methodID));
  };

  native_interface_.CallLongMethod = CallLongMethod;

  native_interface_.CallLongMethodV =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID, va_list /*args*/) -> jlong {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallLongMethodV: " << MethodName(methodID) << '\n';
    }
    jlong result = 0;
    if (LocalStorageLongResultForMethod(MethodName(methodID), &result)) {
      return result;
    }
    return LongResultForReceiverMethod(obj, MethodName(methodID));
  };

  native_interface_.CallLongMethodA =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         const jvalue* /*args*/) -> jlong {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallLongMethodA: " << MethodName(methodID) << '\n';
    }
    jlong result = 0;
    if (LocalStorageLongResultForMethod(MethodName(methodID), &result)) {
      return result;
    }
    return LongResultForReceiverMethod(obj, MethodName(methodID));
  };

  native_interface_.CallFloatMethod = CallFloatMethod;
  native_interface_.CallFloatMethodV =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         va_list /*args*/) -> jfloat {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallFloatMethodV: " << MethodName(methodID)
                << '\n';
    }
    return FloatResultForReceiverMethod(obj, MethodName(methodID));
  };
  native_interface_.CallFloatMethodA =
      [](JNIEnv* /*env*/, jobject obj, jmethodID methodID,
         const jvalue* /*args*/) -> jfloat {
    if (TraceEnabled()) {
      std::cout << "  [JNI] CallFloatMethodA: " << MethodName(methodID)
                << '\n';
    }
    return FloatResultForReceiverMethod(obj, MethodName(methodID));
  };

  native_interface_.CallDoubleMethod = CallDoubleMethod;
  native_interface_.CallDoubleMethodV =
      [](JNIEnv* /*env*/, jobject /*obj*/, jmethodID /*methodID*/,
         va_list /*args*/) -> jdouble { return 0.0; };
  native_interface_.CallDoubleMethodA =
      [](JNIEnv* /*env*/, jobject /*obj*/, jmethodID /*methodID*/,
         const jvalue* /*args*/) -> jdouble { return 0.0; };

  native_interface_.GetStaticObjectField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID fieldID) -> jobject {
    if (TraceEnabled()) {
      auto* name = reinterpret_cast<const char*>(fieldID);
      std::cout << "  [JNI] GetStaticObjectField: "
                << (name ? name : "unknown") << '\n';
    }
    auto* name = reinterpret_cast<const char*>(fieldID);
    if (name && std::strcmp(name, "ANDROID_ID") == 0) {
      return MakeString("mocktail-android-id");
    }
    if (name && std::strcmp(name, "INSTANCE") == 0) {
      return MakePlatformSystemDialogHandlerObject();
    }
    if (name && std::strcmp(name, "sImplementation") == 0) {
      return EngineJavaCallbackObject();
    }
    if (name) {
      std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
      auto it = g_static_object_fields.find(name);
      if (it != g_static_object_fields.end()) {
        return it->second;
      }
    }
    return nullptr;
  };

  native_interface_.GetStaticBooleanField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jboolean {
    return JNI_FALSE;
  };
  native_interface_.GetStaticByteField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jbyte {
    return 0;
  };
  native_interface_.GetStaticCharField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jchar {
    return 0;
  };
  native_interface_.GetStaticShortField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jshort {
    return 0;
  };
  native_interface_.GetStaticIntField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jint {
    return 0;
  };
  native_interface_.GetStaticLongField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jlong {
    return 0;
  };
  native_interface_.GetStaticFloatField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jfloat {
    return 0.0f;
  };
  native_interface_.GetStaticDoubleField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/) -> jdouble {
    return 0.0;
  };

  native_interface_.SetStaticObjectField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID fieldID,
         jobject value) {
    auto* name = reinterpret_cast<const char*>(fieldID);
    if (!name) {
      return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
    g_static_object_fields[name] = value;
    if (std::strcmp(name, "sAppBridgeNotificationListener") == 0) {
      g_app_bridge_notification_listener = value;
    } else if (std::strcmp(name, "sImplementation") == 0) {
      g_engine_java_callback =
          value != nullptr ? value : EngineJavaCallbackObject();
      g_static_object_fields[name] = g_engine_java_callback;
    }
  };
  native_interface_.SetStaticBooleanField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jboolean /*value*/) {};
  native_interface_.SetStaticByteField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jbyte /*value*/) {};
  native_interface_.SetStaticCharField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jchar /*value*/) {};
  native_interface_.SetStaticShortField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jshort /*value*/) {};
  native_interface_.SetStaticIntField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jint /*value*/) {};
  native_interface_.SetStaticLongField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jlong /*value*/) {};
  native_interface_.SetStaticFloatField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jfloat /*value*/) {};
  native_interface_.SetStaticDoubleField =
      [](JNIEnv* /*env*/, jclass /*clazz*/, jfieldID /*fieldID*/,
         jdouble /*value*/) {};

  native_interface_.GetObjectField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID) -> jobject {
    auto* name = reinterpret_cast<const char*>(fieldID);
    jobject value = ObjectFieldValue(obj, name);
    if (TraceEnabled()) {
      std::cout << "  [JNI] GetObjectField: "
                << (name ? name : "unknown") << " -> " << value << '\n';
    }
    return value;
  };
  native_interface_.GetBooleanField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID) -> jboolean {
    return BooleanFieldValue(obj, reinterpret_cast<const char*>(fieldID));
  };
  native_interface_.GetByteField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/) -> jbyte {
    return 0;
  };
  native_interface_.GetCharField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/) -> jchar {
    return 0;
  };
  native_interface_.GetShortField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/) -> jshort {
    return 0;
  };
  native_interface_.GetIntField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID) -> jint {
    return IntFieldValue(obj, reinterpret_cast<const char*>(fieldID));
  };
  native_interface_.GetLongField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID) -> jlong {
    return LongFieldValue(obj, reinterpret_cast<const char*>(fieldID));
  };
  native_interface_.GetFloatField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID) -> jfloat {
    return FloatFieldValue(obj, reinterpret_cast<const char*>(fieldID));
  };
  native_interface_.GetDoubleField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/) -> jdouble {
    return 0.0;
  };

  native_interface_.SetObjectField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID,
         jobject val) {
    auto* name = reinterpret_cast<const char*>(fieldID);
    auto* pseudo_object = PseudoObjectFromRef(obj);
    if (pseudo_object && name) {
      pseudo_object->object_fields[name] = val;
    }
  };
  native_interface_.SetBooleanField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID,
         jboolean val) {
    auto* name = reinterpret_cast<const char*>(fieldID);
    auto* pseudo_object = PseudoObjectFromRef(obj);
    if (pseudo_object && name) {
      pseudo_object->boolean_fields[name] = val;
    }
  };
  native_interface_.SetByteField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/, jbyte /*val*/) {};
  native_interface_.SetCharField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/, jchar /*val*/) {};
  native_interface_.SetShortField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/, jshort /*val*/) {};
  native_interface_.SetIntField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID, jint val) {
    auto* name = reinterpret_cast<const char*>(fieldID);
    auto* pseudo_object = PseudoObjectFromRef(obj);
    if (pseudo_object && name) {
      pseudo_object->int_fields[name] = val;
    }
  };
  native_interface_.SetLongField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID, jlong val) {
    SetLongFieldRaw(obj, reinterpret_cast<const char*>(fieldID), val);
  };
  native_interface_.SetFloatField =
      [](JNIEnv* /*env*/, jobject obj, jfieldID fieldID, jfloat val) {
    auto* name = reinterpret_cast<const char*>(fieldID);
    auto* pseudo_object = PseudoObjectFromRef(obj);
    if (pseudo_object && name) {
      pseudo_object->float_fields[name] = val;
    }
  };
  native_interface_.SetDoubleField =
      [](JNIEnv* /*env*/, jobject /*obj*/, jfieldID /*fieldID*/, jdouble /*val*/) {};

  native_interface_.NewGlobalRef =
      [](JNIEnv* /*env*/, jobject obj) -> jobject {
    return obj;
  };

  native_interface_.DeleteGlobalRef =
      [](JNIEnv* /*env*/, jobject /*obj*/) {};

  native_interface_.NewLocalRef =
      [](JNIEnv* /*env*/, jobject obj) -> jobject {
    return obj;
  };

  native_interface_.DeleteLocalRef =
      [](JNIEnv* /*env*/, jobject /*obj*/) {};

  native_interface_.NewWeakGlobalRef =
      [](JNIEnv* /*env*/, jobject obj) -> jweak {
    Trace("NewWeakGlobalRef");
    return reinterpret_cast<jweak>(obj);
  };

  native_interface_.DeleteWeakGlobalRef =
      [](JNIEnv* /*env*/, jweak /*ref*/) {};

  native_interface_.GetObjectRefType =
      [](JNIEnv* /*env*/, jobject obj) -> jobjectRefType {
    return obj ? JNILocalRefType : JNIInvalidRefType;
  };

  native_interface_.NewString =
      [](JNIEnv* /*env*/, const jchar* unicode, jsize len) -> jstring {
    return MakeUtf16String(unicode, len);
  };

  native_interface_.GetStringLength =
      [](JNIEnv* /*env*/, jstring str) -> jsize {
    return StringUtf16Length(str);
  };

  native_interface_.GetStringChars =
      [](JNIEnv* /*env*/, jstring str, jboolean* isCopy) -> const jchar* {
    if (isCopy) *isCopy = JNI_FALSE;
    return StringUtf16Chars(str);
  };

  native_interface_.ReleaseStringChars =
      [](JNIEnv* /*env*/, jstring /*str*/, const jchar* /*chars*/) {};

  native_interface_.GetStringUTFRegion =
      [](JNIEnv* /*env*/, jstring str, jsize start, jsize len, char* buf) {
    CopyStringModifiedUtf8Region(str, start, len, buf);
  };

  native_interface_.GetStringRegion =
      [](JNIEnv* /*env*/, jstring str, jsize start, jsize len, jchar* buf) {
    CopyStringRegion(str, start, len, buf);
  };

  native_interface_.GetArrayLength =
      [](JNIEnv* /*env*/, jarray array) -> jsize {
    PseudoArray* pseudo_array = ArrayFromRef(array);
    if (!pseudo_array) {
      return 0;
    }
    if (!pseudo_array->bytes.empty()) {
      return static_cast<jsize>(pseudo_array->bytes.size());
    }
    if (!pseudo_array->floats.empty()) {
      return static_cast<jsize>(pseudo_array->floats.size());
    }
    return static_cast<jsize>(pseudo_array->objects.size());
  };

  native_interface_.NewObjectArray =
      [](JNIEnv* /*env*/, jsize len, jclass /*clazz*/,
         jobject init) -> jobjectArray {
    Trace("NewObjectArray");
    return MakeObjectArray(len, init);
  };

  native_interface_.GetObjectArrayElement =
      [](JNIEnv* /*env*/, jobjectArray array, jsize index) -> jobject {
    PseudoArray* pseudo_array = ArrayFromRef(array);
    if (!pseudo_array || index < 0 ||
        static_cast<std::size_t>(index) >= pseudo_array->objects.size()) {
      return nullptr;
    }
    return pseudo_array->objects[static_cast<std::size_t>(index)];
  };

  native_interface_.SetObjectArrayElement =
      [](JNIEnv* /*env*/, jobjectArray array, jsize index, jobject val) {
    PseudoArray* pseudo_array = ArrayFromRef(array);
    if (!pseudo_array || index < 0 ||
        static_cast<std::size_t>(index) >= pseudo_array->objects.size()) {
      return;
    }
    pseudo_array->objects[static_cast<std::size_t>(index)] = val;
  };

  native_interface_.NewByteArray =
      [](JNIEnv* /*env*/, jsize len) -> jbyteArray {
    Trace("NewByteArray");
    return MakeByteArray(len);
  };

  native_interface_.GetByteArrayElements =
      [](JNIEnv* /*env*/, jbyteArray array, jboolean* isCopy) -> jbyte* {
    if (isCopy) *isCopy = JNI_FALSE;
    PseudoArray* pseudo_array = ArrayFromRef(array);
    return pseudo_array && !pseudo_array->bytes.empty()
               ? pseudo_array->bytes.data()
               : nullptr;
  };

  native_interface_.ReleaseByteArrayElements =
      [](JNIEnv* /*env*/, jbyteArray /*array*/, jbyte* /*elems*/,
         jint /*mode*/) {};

  native_interface_.NewFloatArray =
      [](JNIEnv* /*env*/, jsize len) -> jfloatArray {
    Trace("NewFloatArray");
    return MakeFloatArray(len);
  };

  native_interface_.GetFloatArrayElements =
      [](JNIEnv* /*env*/, jfloatArray array, jboolean* isCopy) -> jfloat* {
    if (isCopy) *isCopy = JNI_FALSE;
    PseudoArray* pseudo_array = ArrayFromRef(array);
    return pseudo_array && !pseudo_array->floats.empty()
               ? pseudo_array->floats.data()
               : nullptr;
  };

  native_interface_.ReleaseFloatArrayElements =
      [](JNIEnv* /*env*/, jfloatArray /*array*/, jfloat* /*elems*/,
         jint /*mode*/) {};

  native_interface_.GetByteArrayRegion =
      [](JNIEnv* /*env*/, jbyteArray array, jsize start, jsize len, jbyte* buf) {
    PseudoArray* pseudo_array = ArrayFromRef(array);
    if (!pseudo_array || !buf || start < 0 || len <= 0) {
      return;
    }
    std::size_t offset = static_cast<std::size_t>(start);
    if (offset >= pseudo_array->bytes.size()) {
      return;
    }
    std::size_t count =
        std::min(static_cast<std::size_t>(len),
                 pseudo_array->bytes.size() - offset);
    std::memcpy(buf, pseudo_array->bytes.data() + offset, count);
  };

  native_interface_.SetByteArrayRegion =
      [](JNIEnv* /*env*/, jbyteArray array, jsize start, jsize len,
         const jbyte* buf) {
    PseudoArray* pseudo_array = ArrayFromRef(array);
    if (!pseudo_array || !buf || start < 0 || len <= 0) {
      return;
    }
    std::size_t offset = static_cast<std::size_t>(start);
    if (offset >= pseudo_array->bytes.size()) {
      return;
    }
    std::size_t count =
        std::min(static_cast<std::size_t>(len),
                 pseudo_array->bytes.size() - offset);
    std::memcpy(pseudo_array->bytes.data() + offset, buf, count);
  };

  native_interface_.SetFloatArrayRegion =
      [](JNIEnv* /*env*/, jfloatArray array, jsize start, jsize len,
         const jfloat* buf) {
    PseudoArray* pseudo_array = ArrayFromRef(array);
    if (!pseudo_array || !buf || start < 0 || len <= 0) {
      return;
    }
    std::size_t offset = static_cast<std::size_t>(start);
    if (offset >= pseudo_array->floats.size()) {
      return;
    }
    std::size_t count =
        std::min(static_cast<std::size_t>(len),
                 pseudo_array->floats.size() - offset);
    std::memcpy(pseudo_array->floats.data() + offset, buf,
                count * sizeof(jfloat));
  };

  native_interface_.GetPrimitiveArrayCritical =
      [](JNIEnv* env, jarray array, jboolean* isCopy) -> void* {
    if (isCopy) *isCopy = JNI_FALSE;
    PseudoArray* pseudo_array = ArrayFromRef(array);
    if (!pseudo_array) {
      return nullptr;
    }
    if (!pseudo_array->bytes.empty()) {
      return pseudo_array->bytes.data();
    }
    return !pseudo_array->floats.empty() ? pseudo_array->floats.data()
                                         : nullptr;
  };

  native_interface_.ReleasePrimitiveArrayCritical =
      [](JNIEnv* /*env*/, jarray /*array*/, void* /*carray*/, jint /*mode*/) {};

  native_interface_.NewDirectByteBuffer =
      [](JNIEnv* /*env*/, void* address, jlong /*capacity*/) -> jobject {
    return reinterpret_cast<jobject>(address);
  };

  native_interface_.GetDirectBufferAddress =
      [](JNIEnv* /*env*/, jobject buf) -> void* {
    return reinterpret_cast<void*>(buf);
  };

  native_interface_.GetDirectBufferCapacity =
      [](JNIEnv* /*env*/, jobject /*buf*/) -> jlong {
    return 0;
  };

  native_interface_.MonitorEnter =
      [](JNIEnv* /*env*/, jobject /*obj*/) -> jint {
    return JNI_OK;
  };

  native_interface_.MonitorExit =
      [](JNIEnv* /*env*/, jobject /*obj*/) -> jint {
    return JNI_OK;
  };

  native_interface_.GetJavaVM =
      [](JNIEnv* env, JavaVM** vm) -> jint {
    VM* current_vm = CurrentVM();
    if (!vm || !current_vm) {
      return JNI_ERR;
    }
    *vm = current_vm->GetJavaVM();
    if (TraceEnabled() || JniVmTraceEnabled()) {
      std::cout << "  [JNI] GetJavaVM env=" << env << " out=" << vm
                << " vm=" << *vm << '\n';
    }
    return JNI_OK;
  };

  jni_env_ = &jni_env_storage_;
  jni_env_->functions = &native_interface_;
}

}  // namespace jnivm
