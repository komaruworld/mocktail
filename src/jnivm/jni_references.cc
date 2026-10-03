#include "jni_references.h"

#include <atomic>
#include <cstdlib>

namespace jnivm {
// Encoded handles are also read by the guest ABI boundary.
void* my_segment[100000] = {};
int g_jni_ref_index = 1;
}  // namespace jnivm

namespace jnivm::internal {

enum class SegmentType : uint8_t {
  kEmpty = 0,
  kObject = 1,
  kClass = 2,
};

// Class handles are interned by binary name. Real JNI resolves the same class
// to the same runtime class object, and class handles are never released, so
// every FindClass/GetObjectClass used to consume a fresh permanent segment
// slot and exhaust the table during long sessions.
thread_local std::vector<std::vector<jobject>> g_local_frames;
std::recursive_mutex g_jni_state_mutex;
std::unordered_map<jobject, std::unique_ptr<Object>> g_object_storage;
std::unordered_map<jobject, uint32_t> g_jni_ref_counts;
constexpr uint32_t kJniSegmentCapacity = 100000;
constexpr uint32_t kJniHandleShift = 16;
std::shared_ptr<void> g_segment_owners[kJniSegmentCapacity];
std::unordered_set<jclass> g_known_classes;
std::unordered_map<std::string, jclass> g_class_handle_cache;
std::unordered_map<std::string, jobject> g_singleton_objects;
std::unordered_map<std::string, std::shared_ptr<Class>> g_fallback_classes;
std::unordered_set<jstring> g_known_strings;
std::unordered_map<jarray, std::unique_ptr<PseudoArray>> g_array_storage;
static std::atomic<uint8_t> g_segment_types[kJniSegmentCapacity] = {};
std::vector<int> g_free_slots;

static bool g_shutting_down_jnivm = false;

struct JniVmShutdownHook {
  JniVmShutdownHook() {
    std::atexit([]() { g_shutting_down_jnivm = true; });
  }
};
static JniVmShutdownHook g_shutdown_hook;

bool TraceEnabled() {
  static const bool enabled = std::getenv("MOCKTAIL_JNI_TRACE") != nullptr;
  return enabled;
}

jobject JniHandleFromIndex(uint32_t index) {
  return reinterpret_cast<jobject>(static_cast<uintptr_t>(index)
                                   << kJniHandleShift);
}

uint32_t JniIndexFromHandle(jobject obj) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(obj) >>
                               kJniHandleShift);
}

void EnsureLocalFrame() {
  if (g_local_frames.empty()) {
    g_local_frames.emplace_back();
  }
}

void RegisterLocalRef(jobject obj) {
  if (obj == nullptr) {
    return;
  }
  EnsureLocalFrame();
  g_local_frames.back().push_back(obj);
}

void UnregisterLocalRef(jobject obj) {
  if (obj == nullptr) {
    return;
  }
  for (auto frame = g_local_frames.rbegin(); frame != g_local_frames.rend();
       ++frame) {
    for (auto it = frame->rbegin(); it != frame->rend(); ++it) {
      if (*it == obj) {
        frame->erase(std::next(it).base());
        return;
      }
    }
  }
}

void RetainJniReference(jobject obj) {
  if (obj == nullptr) {
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto it = g_jni_ref_counts.find(obj);
  if (it != g_jni_ref_counts.end()) {
    ++it->second;
  } else {
    g_jni_ref_counts[obj] = 1;
  }
}

int AllocateSegmentSlot(void* value, std::shared_ptr<void> owner = nullptr,
                        SegmentType type = SegmentType::kObject) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  int index = 0;
  if (!g_free_slots.empty()) {
    index = g_free_slots.back();
    g_free_slots.pop_back();
  } else if (g_jni_ref_index > 0 &&
             static_cast<uint32_t>(g_jni_ref_index) < kJniSegmentCapacity) {
    index = g_jni_ref_index++;
  } else {
    return 0;
  }
  my_segment[index] = value;
  g_segment_owners[index] = std::move(owner);
  g_segment_types[index].store(static_cast<uint8_t>(type),
                               std::memory_order_release);
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

std::shared_ptr<Class> ClassFromJClass(jclass clazz) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  if (!clazz) {
    return nullptr;
  }
  const uint32_t index = JniIndexFromHandle(reinterpret_cast<jobject>(clazz));
  Class* cls = nullptr;
  if (index > 0 && index < kJniSegmentCapacity) {
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

jclass StoreClass(std::shared_ptr<Class> cls) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  if (!cls) {
    return nullptr;
  }
  const std::string& name = cls->GetName();
  if (const auto cached = g_class_handle_cache.find(name);
      cached != g_class_handle_cache.end()) {
    return cached->second;
  }
  Class* raw_ptr = cls.get();
  int index = AllocateSegmentSlot(raw_ptr, cls, SegmentType::kClass);
  if (index <= 0) {
    return nullptr;
  }
  jclass handle = reinterpret_cast<jclass>(
      JniHandleFromIndex(static_cast<uint32_t>(index)));
  g_known_classes.insert(handle);
  g_class_handle_cache.emplace(name, handle);
  return handle;
}

jobject StoreObject(std::unique_ptr<Object> object) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  Object* raw_ptr = object.get();
  int index = AllocateSegmentSlot(raw_ptr, nullptr, SegmentType::kObject);
  if (index <= 0) {
    return nullptr;
  }
  jobject handle = JniHandleFromIndex(static_cast<uint32_t>(index));
  g_object_storage[handle] = std::move(object);
  g_jni_ref_counts[handle] = 1;
  RegisterLocalRef(handle);
  return handle;
}

void ReleaseJniReference(jobject obj) {
  if (obj == nullptr || g_shutting_down_jnivm) {
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  if (g_shutting_down_jnivm) {
    return;
  }
  auto ref_it = g_jni_ref_counts.find(obj);
  if (ref_it != g_jni_ref_counts.end()) {
    if (ref_it->second > 1) {
      --ref_it->second;
      return;
    }
    g_jni_ref_counts.erase(ref_it);
  }

  auto arr_it = g_array_storage.find(reinterpret_cast<jarray>(obj));
  if (arr_it != g_array_storage.end()) {
    std::unique_ptr<PseudoArray> dying_array = std::move(arr_it->second);
    g_array_storage.erase(arr_it);
    return;
  }
  if (g_known_classes.find(reinterpret_cast<jclass>(obj)) !=
      g_known_classes.end()) {
    return;
  }
  for (const auto& pair : g_singleton_objects) {
    if (pair.second == obj) {
      return;
    }
  }
  const uint32_t index = JniIndexFromHandle(obj);
  if (index > 0 && index < kJniSegmentCapacity) {
    g_segment_types[index].store(static_cast<uint8_t>(SegmentType::kEmpty),
                                 std::memory_order_release);
    my_segment[index] = nullptr;
    g_segment_owners[index].reset();
    g_free_slots.push_back(static_cast<int>(index));
  }
  g_known_strings.erase(reinterpret_cast<jstring>(obj));
  std::unique_ptr<Object> dying_object;
  auto obj_it = g_object_storage.find(obj);
  if (obj_it != g_object_storage.end()) {
    dying_object = std::move(obj_it->second);
    g_object_storage.erase(obj_it);
  }
}

PseudoJavaObject* PseudoObjectFromRef(jobject obj) {
  if (__builtin_expect(obj == nullptr, 0)) {
    return nullptr;
  }
  const uint32_t index = JniIndexFromHandle(obj);
  if (__builtin_expect(index > 0 && index < kJniSegmentCapacity, 1)) {
    const uint8_t type = g_segment_types[index].load(std::memory_order_acquire);
    if (__builtin_expect(type == static_cast<uint8_t>(SegmentType::kObject),
                         1)) {
      void* raw_ptr = my_segment[index];
      if (__builtin_expect(raw_ptr != nullptr, 1)) {
        return static_cast<PseudoJavaObject*>(
            reinterpret_cast<Object*>(raw_ptr));
      }
    }
  }
  return nullptr;
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

std::string_view ObjectClassName(jobject obj) {
  PseudoJavaObject* pseudo_object = PseudoObjectFromRef(obj);
  if (!pseudo_object || !pseudo_object->GetClass()) {
    return {};
  }
  return pseudo_object->GetClass()->GetName();
}

PseudoArray* ArrayFromRef(jarray array) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto it = g_array_storage.find(array);
  return it == g_array_storage.end() ? nullptr : it->second.get();
}

jbyteArray MakeByteArray(jsize len) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto array = std::make_unique<PseudoArray>();
  if (len > 0) {
    array->bytes.resize(static_cast<std::size_t>(len));
  }
  jbyteArray ref = reinterpret_cast<jbyteArray>(array.get());
  g_array_storage[ref] = std::move(array);
  g_jni_ref_counts[reinterpret_cast<jobject>(ref)] = 1;
  RegisterLocalRef(reinterpret_cast<jobject>(ref));
  return ref;
}

jfloatArray MakeFloatArray(jsize len) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto array = std::make_unique<PseudoArray>();
  if (len > 0) {
    array->floats.resize(static_cast<std::size_t>(len));
  }
  jfloatArray ref = reinterpret_cast<jfloatArray>(array.get());
  g_array_storage[ref] = std::move(array);
  g_jni_ref_counts[reinterpret_cast<jobject>(ref)] = 1;
  RegisterLocalRef(reinterpret_cast<jobject>(ref));
  return ref;
}

jobjectArray MakeObjectArray(jsize len, jobject init) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto array = std::make_unique<PseudoArray>();
  if (len > 0) {
    array->objects.resize(static_cast<std::size_t>(len), init);
    for (jsize index = 0; index < len; ++index) RetainJniReference(init);
  }
  jobjectArray ref = reinterpret_cast<jobjectArray>(array.get());
  g_array_storage[ref] = std::move(array);
  g_jni_ref_counts[reinterpret_cast<jobject>(ref)] = 1;
  RegisterLocalRef(reinterpret_cast<jobject>(ref));
  return ref;
}

PseudoJavaObject::~PseudoJavaObject() {
  for (const auto& field : object_fields) ReleaseJniReference(field.second);
}

PseudoArray::~PseudoArray() {
  for (jobject value : objects) ReleaseJniReference(value);
}

jobject NewLocalJniReference(jobject obj) {
  if (obj != nullptr) {
    RetainJniReference(obj);
    RegisterLocalRef(obj);
  }
  return obj;
}

void DeleteLocalJniReference(jobject obj) {
  UnregisterLocalRef(obj);
  ReleaseJniReference(obj);
}

void PushLocalJniFrame() {
  EnsureLocalFrame();
  g_local_frames.emplace_back();
}

jobject PopLocalJniFrame(jobject result) {
  EnsureLocalFrame();
  RetainJniReference(result);
  std::vector<jobject> frame = std::move(g_local_frames.back());
  if (g_local_frames.size() > 1)
    g_local_frames.pop_back();
  else
    g_local_frames.back().clear();
  for (jobject obj : frame) ReleaseJniReference(obj);
  RegisterLocalRef(result);
  return result;
}

}  // namespace jnivm::internal
