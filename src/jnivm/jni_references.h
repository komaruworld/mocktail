#ifndef MOCKTAIL_JNIVM_JNI_REFERENCES_H_
#define MOCKTAIL_JNIVM_JNI_REFERENCES_H_

#include <string_view>
#include <unordered_set>

#include "jnivm/jnivm.h"

namespace jnivm::internal {

// Shared by reference ownership and object/field access. Callers retaining a
// raw PseudoJavaObject pointer must keep its JNI reference alive.
extern std::recursive_mutex g_jni_state_mutex;
extern std::unordered_set<jstring> g_known_strings;

struct PseudoArray {
  ~PseudoArray();
  std::vector<jbyte> bytes;
  std::vector<jfloat> floats;
  std::vector<jobject> objects;
};

struct PseudoJavaObject : Object {
  explicit PseudoJavaObject(std::shared_ptr<Class> cls)
      : Object(std::move(cls)) {}

  ~PseudoJavaObject() override;

  std::unordered_map<std::string, jobject> object_fields;
  std::unordered_map<std::string, jint> int_fields;
  std::unordered_map<std::string, jlong> long_fields;
  std::unordered_map<std::string, jfloat> float_fields;
  std::unordered_map<std::string, jboolean> boolean_fields;
};

bool TraceEnabled();
void RegisterLocalRef(jobject obj);
void UnregisterLocalRef(jobject obj);
void RetainJniReference(jobject obj);
void ReleaseJniReference(jobject obj);
jobject NewLocalJniReference(jobject obj);
void DeleteLocalJniReference(jobject obj);
void PushLocalJniFrame();
jobject PopLocalJniFrame(jobject result);
std::shared_ptr<Class> FallbackClassForName(const std::string& name);
std::shared_ptr<Class> ClassFromJClass(jclass clazz);
jclass StoreClass(std::shared_ptr<Class> cls);
jobject StoreObject(std::unique_ptr<Object> object);
PseudoJavaObject* PseudoObjectFromRef(jobject obj);
jobject MakeObjectForClass(const std::string& name);
jobject MakeObject(jclass clazz);
jobject SingletonObject(const std::string& name);
std::string_view ObjectClassName(jobject obj);
PseudoArray* ArrayFromRef(jarray array);
jbyteArray MakeByteArray(jsize len);
jfloatArray MakeFloatArray(jsize len);
jobjectArray MakeObjectArray(jsize len, jobject init);

}  // namespace jnivm::internal
#endif
