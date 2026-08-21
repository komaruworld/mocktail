#include "runtime/roblox_native_text_box_info_reader.h"

#include <gtest/gtest.h>

#include "jnivm/jnivm.h"

namespace mocktail {
namespace runtime {
namespace {

jobject g_query_result = nullptr;
int g_query_calls = 0;

jobject QueryFixture(JNIEnv*, jclass) {
  ++g_query_calls;
  return g_query_result;
}

jobject MakeInfo(JNIEnv* env) {
  jclass info_class =
      env->FindClass("com/roblox/engine/jni/model/NativeTextBoxInfo");
  jmethodID constructor =
      env->GetMethodID(info_class, "<init>", "(FFFFFZIIIIIIZZ)V");
  jvalue arguments[14]{};
  arguments[0].f = 10.5F;
  arguments[1].f = 20.25F;
  arguments[2].f = 360.0F;
  arguments[3].f = 42.0F;
  arguments[4].f = 18.0F;
  arguments[5].z = JNI_TRUE;
  arguments[6].i = 1;
  arguments[7].i = 2;
  arguments[8].i = 0x123456;
  arguments[9].i = 4;
  arguments[10].i = 6;
  arguments[11].i = 7;
  arguments[12].z = JNI_TRUE;
  arguments[13].z = JNI_FALSE;
  return env->NewObjectA(info_class, constructor, arguments);
}

TEST(RobloxNativeTextBoxInfoReaderTest, CopiesEveryTypedField) {
  jnivm::VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  jclass native_gl_class =
      env->FindClass("com/roblox/engine/jni/NativeGLInterface");
  g_query_result = MakeInfo(env);
  g_query_calls = 0;
  RobloxNativeTextBoxInfoQueryResult result;

  const Status status = QueryRobloxNativeTextBoxInfo(env, native_gl_class,
                                                     &QueryFixture, &result);

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(g_query_calls, 1);
  ASSERT_TRUE(result.available);
  EXPECT_FLOAT_EQ(result.info.x, 10.5F);
  EXPECT_FLOAT_EQ(result.info.y, 20.25F);
  EXPECT_FLOAT_EQ(result.info.width, 360.0F);
  EXPECT_FLOAT_EQ(result.info.height, 42.0F);
  EXPECT_FLOAT_EQ(result.info.font_size, 18.0F);
  EXPECT_TRUE(result.info.multiline);
  EXPECT_EQ(result.info.x_alignment, 1);
  EXPECT_EQ(result.info.y_alignment, 2);
  EXPECT_EQ(result.info.text_color, 0x123456);
  EXPECT_EQ(result.info.font, 4);
  EXPECT_EQ(result.info.text_input_type, 6);
  EXPECT_EQ(result.info.return_key_type, 7);
  EXPECT_TRUE(result.info.manual_focus_release);
  EXPECT_FALSE(result.info.text_wrapped);
  g_query_result = nullptr;
}

TEST(RobloxNativeTextBoxInfoReaderTest, NullNativeResultIsNotAnError) {
  jnivm::VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  jclass native_gl_class =
      env->FindClass("com/roblox/engine/jni/NativeGLInterface");
  g_query_result = nullptr;
  g_query_calls = 0;
  RobloxNativeTextBoxInfoQueryResult result;
  result.available = true;
  result.info.width = 999.0F;

  const Status status = QueryRobloxNativeTextBoxInfo(env, native_gl_class,
                                                     &QueryFixture, &result);

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(g_query_calls, 1);
  EXPECT_FALSE(result.available);
  EXPECT_FLOAT_EQ(result.info.width, 0.0F);
}

TEST(RobloxNativeTextBoxInfoReaderTest, RejectsIncompleteQueryBoundary) {
  jnivm::VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  jclass native_gl_class =
      env->FindClass("com/roblox/engine/jni/NativeGLInterface");
  RobloxNativeTextBoxInfoQueryResult result;

  EXPECT_FALSE(QueryRobloxNativeTextBoxInfo(nullptr, native_gl_class,
                                            &QueryFixture, &result)
                   .ok());
  EXPECT_FALSE(
      QueryRobloxNativeTextBoxInfo(env, nullptr, &QueryFixture, &result).ok());
  EXPECT_FALSE(
      QueryRobloxNativeTextBoxInfo(env, native_gl_class, nullptr, &result)
          .ok());
  EXPECT_FALSE(
      QueryRobloxNativeTextBoxInfo(env, native_gl_class, &QueryFixture, nullptr)
          .ok());
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
