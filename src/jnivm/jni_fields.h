#ifndef MOCKTAIL_JNIVM_JNI_FIELDS_H_
#define MOCKTAIL_JNIVM_JNI_FIELDS_H_

#include <jni.h>

namespace jnivm::internal {

// Field names are interned for the lifetime of the process. Object getters
// return independently owned local references; setters retain their values.
jfieldID StoreFieldId(const char* name);
void SetStaticObjectFieldRaw(const char* name, jobject value);
jobject StaticObjectFieldValue(const char* name);
void SetObjectFieldRaw(jobject obj, const char* name, jobject value);
void SetIntFieldRaw(jobject obj, const char* name, jint value);
void SetLongFieldRaw(jobject obj, const char* name, jlong value);
void SetFloatFieldRaw(jobject obj, const char* name, jfloat value);
void SetBooleanFieldRaw(jobject obj, const char* name, jboolean value);
jobject ObjectFieldValue(jobject obj, const char* name);
jint IntFieldValue(jobject obj, const char* name);
jlong LongFieldValue(jobject obj, const char* name);
jfloat FloatFieldValue(jobject obj, const char* name);
jboolean BooleanFieldValue(jobject obj, const char* name);

}  // namespace jnivm::internal
#endif
