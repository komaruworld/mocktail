#include "jnivm/jnivm.h"

#include <gtest/gtest.h>

#include <memory>

namespace jnivm {
namespace {

constexpr char kInfoClass[] =
    "com/roblox/engine/jni/model/NativeTextBoxInfo";
constexpr char kInfoConstructor[] = "(FFFFFZIIIIIIZZ)V";

struct ShowProbe {
  int calls = 0;
  RobloxTextInputShowRequest request;
};

void CaptureShow(void* context, const RobloxTextInputShowRequest& request) {
  auto* probe = static_cast<ShowProbe*>(context);
  ++probe->calls;
  probe->request = request;
}

RobloxTextInputCallbacks ShowCallbacks() {
  RobloxTextInputCallbacks callbacks;
  callbacks.show = &CaptureShow;
  return callbacks;
}

jfieldID Field(JNIEnv* env, jclass cls, const char* name, const char* type) {
  jfieldID field = env->GetFieldID(cls, name, type);
  EXPECT_NE(field, nullptr);
  return field;
}

void ExpectGeometry(JNIEnv* env, jclass cls, jobject info) {
  EXPECT_FLOAT_EQ(env->GetFloatField(info, Field(env, cls, "x", "F")), 11.5F);
  EXPECT_FLOAT_EQ(env->GetFloatField(info, Field(env, cls, "y", "F")), 22.5F);
  EXPECT_FLOAT_EQ(env->GetFloatField(info, Field(env, cls, "width", "F")),
                  333.0F);
  EXPECT_FLOAT_EQ(env->GetFloatField(info, Field(env, cls, "height", "F")),
                  44.0F);
  EXPECT_FLOAT_EQ(env->GetFloatField(info, Field(env, cls, "fontSize", "F")),
                  18.0F);
  EXPECT_EQ(env->GetBooleanField(info, Field(env, cls, "multiline", "Z")),
            JNI_TRUE);
  EXPECT_EQ(env->GetIntField(info, Field(env, cls, "xAlignment", "I")), 1);
  EXPECT_EQ(env->GetIntField(info, Field(env, cls, "yAlignment", "I")), 2);
  EXPECT_EQ(env->GetIntField(info, Field(env, cls, "textColor", "I")),
            0x123456);
  EXPECT_EQ(env->GetIntField(info, Field(env, cls, "font", "I")), 4);
  EXPECT_EQ(env->GetIntField(info, Field(env, cls, "textInputType", "I")), 6);
  EXPECT_EQ(env->GetIntField(info, Field(env, cls, "returnKeyType", "I")), 7);
  EXPECT_EQ(env->GetBooleanField(
                info, Field(env, cls, "manualFocusRelease", "Z")),
            JNI_TRUE);
  EXPECT_EQ(env->GetBooleanField(info, Field(env, cls, "textWrapped", "Z")),
            JNI_FALSE);
}

void FillArguments(jvalue* args) {
  args[0].f = 11.5F;
  args[1].f = 22.5F;
  args[2].f = 333.0F;
  args[3].f = 44.0F;
  args[4].f = 18.0F;
  args[5].z = JNI_TRUE;
  args[6].i = 1;
  args[7].i = 2;
  args[8].i = 0x123456;
  args[9].i = 4;
  args[10].i = 6;
  args[11].i = 7;
  args[12].z = JNI_TRUE;
  args[13].z = JNI_FALSE;
}

TEST(JniVmNativeTextBoxInfoTest, DirectAndArrayConstructorsPreserveGeometry) {
  VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  jclass cls = env->FindClass(kInfoClass);
  ASSERT_NE(cls, nullptr);
  jmethodID constructor = env->GetMethodID(cls, "<init>", kInfoConstructor);
  ASSERT_NE(constructor, nullptr);

  jobject direct = env->NewObject(
      cls, constructor, 11.5, 22.5, 333.0, 44.0, 18.0, JNI_TRUE, 1, 2,
      0x123456, 4, 6, 7, JNI_TRUE, JNI_FALSE);
  ASSERT_NE(direct, nullptr);
  ExpectGeometry(env, cls, direct);

  jvalue args[14]{};
  FillArguments(args);
  jobject array = env->NewObjectA(cls, constructor, args);
  ASSERT_NE(array, nullptr);
  ExpectGeometry(env, cls, array);
}

TEST(JniVmNativeTextBoxInfoTest, CopyConstructorPreservesEveryTypedField) {
  VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  jclass cls = env->FindClass(kInfoClass);
  jmethodID constructor = env->GetMethodID(cls, "<init>", kInfoConstructor);
  jmethodID copy_constructor = env->GetMethodID(
      cls, "<init>",
      "(Lcom/roblox/engine/jni/model/NativeTextBoxInfo;)V");
  jvalue args[14]{};
  FillArguments(args);
  jobject source = env->NewObjectA(cls, constructor, args);
  ASSERT_NE(source, nullptr);

  jobject direct_copy = env->NewObject(cls, copy_constructor, source);
  ASSERT_NE(direct_copy, nullptr);
  ExpectGeometry(env, cls, direct_copy);
  jvalue copy_arg{};
  copy_arg.l = source;
  jobject array_copy = env->NewObjectA(cls, copy_constructor, &copy_arg);
  ASSERT_NE(array_copy, nullptr);
  ExpectGeometry(env, cls, array_copy);
}

TEST(JniVmNativeTextBoxInfoTest, ShowKeyboardSnapshotsNonzeroGeometry) {
  VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  auto probe = std::make_shared<ShowProbe>();
  vm.SetRobloxTextInputCallbacks(probe, ShowCallbacks());

  jclass info_class = env->FindClass(kInfoClass);
  jmethodID constructor =
      env->GetMethodID(info_class, "<init>", kInfoConstructor);
  jvalue args[14]{};
  FillArguments(args);
  jobject info = env->NewObjectA(info_class, constructor, args);
  const jbyte text_bytes[] = {'t', 'e', 's', 't'};
  jbyteArray text = env->NewByteArray(4);
  env->SetByteArrayRegion(text, 0, 4, text_bytes);

  jclass interface_class = env->FindClass(
      "com/roblox/engine/jni/NativeGLJavaInterface");
  jmethodID show = env->GetStaticMethodID(
      interface_class, "showKeyboard",
      "(JZ[BLcom/roblox/engine/jni/model/NativeTextBoxInfo;)V");
  env->CallStaticVoidMethod(interface_class, show, static_cast<jlong>(99),
                            JNI_TRUE, text, info);

  ASSERT_EQ(probe->calls, 1);
  EXPECT_EQ(probe->request.text_box, 99);
  EXPECT_EQ(probe->request.text, "test");
  EXPECT_FLOAT_EQ(probe->request.info.x, 11.5F);
  EXPECT_FLOAT_EQ(probe->request.info.y, 22.5F);
  EXPECT_FLOAT_EQ(probe->request.info.width, 333.0F);
  EXPECT_FLOAT_EQ(probe->request.info.height, 44.0F);
  EXPECT_EQ(probe->request.info.text_input_type, 6);
  EXPECT_TRUE(probe->request.info.manual_focus_release);
}

}  // namespace
}  // namespace jnivm
