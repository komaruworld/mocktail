#include "jni_fields.h"

#include "jni_references.h"

namespace jnivm::internal {
namespace {
std::unordered_set<std::string> g_field_names;
std::unordered_map<std::string, jobject> g_static_object_fields;
}  // namespace

jfieldID StoreFieldId(const char* name) {
  if (name == nullptr) return nullptr;
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  const auto stored = g_field_names.emplace(name).first;
  // unordered_set preserves references to its elements across rehashes.
  return reinterpret_cast<jfieldID>(const_cast<char*>(stored->c_str()));
}

void SetStaticObjectFieldRaw(const char* name, jobject value) {
  if (name == nullptr) return;
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  RetainJniReference(value);
  auto& field = g_static_object_fields[name];
  const jobject previous = field;
  field = value;
  ReleaseJniReference(previous);
}

jobject StaticObjectFieldValue(const char* name) {
  if (name == nullptr) return nullptr;
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  const auto found = g_static_object_fields.find(name);
  return found == g_static_object_fields.end()
             ? nullptr
             : NewLocalJniReference(found->second);
}

void SetObjectFieldRaw(jobject obj, const char* field_name, jobject value) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  PseudoJavaObject* pseudo_object = PseudoObjectFromRef(obj);
  if (pseudo_object && field_name) {
    RetainJniReference(value);
    auto it = pseudo_object->object_fields.find(field_name);
    jobject prev =
        (it != pseudo_object->object_fields.end()) ? it->second : nullptr;
    pseudo_object->object_fields[field_name] = value;
    if (prev != nullptr) {
      ReleaseJniReference(prev);
    }
  }
}

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

jobject ObjectFieldValue(jobject obj, const char* field_name) {
  std::lock_guard<std::recursive_mutex> lock(g_jni_state_mutex);
  auto* pseudo_object = PseudoObjectFromRef(obj);
  if (!pseudo_object || !field_name) {
    return nullptr;
  }
  auto it = pseudo_object->object_fields.find(field_name);
  return it == pseudo_object->object_fields.end()
             ? nullptr
             : NewLocalJniReference(it->second);
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

}  // namespace jnivm::internal
