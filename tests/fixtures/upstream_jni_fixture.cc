#include <jni.h>

namespace {

jint NativeIncrement(JNIEnv*, jclass, jint value) {
  return value + 1;
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  if (vm == nullptr) {
    return JNI_ERR;
  }

  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK ||
      env == nullptr) {
    return JNI_ERR;
  }

  jclass fixture_class = env->FindClass("com/mocktail/UpstreamFixture");
  if (fixture_class == nullptr) {
    return JNI_ERR;
  }

  const JNINativeMethod methods[] = {
      {"nativeIncrement", "(I)I", reinterpret_cast<void*>(&NativeIncrement)},
  };
  if (env->RegisterNatives(fixture_class, methods, 1) != JNI_OK) {
    return JNI_ERR;
  }

  return JNI_VERSION_1_6;
}
