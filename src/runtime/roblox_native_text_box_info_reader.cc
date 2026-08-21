#include "runtime/roblox_native_text_box_info_reader.h"

#include <string>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

Status InvalidArgument(const char* message) {
  return Status::Error(StatusCode::kInvalidArgument, message);
}

Status CheckJni(JNIEnv* env, const char* operation) {
  if (env == nullptr) {
    return InvalidArgument("NativeTextBoxInfo reader requires JNIEnv");
  }
  if (env->ExceptionCheck() == JNI_FALSE) {
    return Status::Ok();
  }
  env->ExceptionClear();
  std::string message = operation != nullptr ? operation : "JNI operation";
  message += " raised an exception while reading NativeTextBoxInfo";
  return Status::Error(StatusCode::kPlatformError, std::move(message));
}

Status RequireField(JNIEnv* env, jclass info_class, const char* name,
                    const char* signature, jfieldID* field) {
  *field = env->GetFieldID(info_class, name, signature);
  Status status = CheckJni(env, name);
  if (!status.ok()) {
    return status;
  }
  if (*field == nullptr) {
    return Status::Error(
        StatusCode::kUnavailable,
        std::string("NativeTextBoxInfo field is unavailable: ") + name);
  }
  return Status::Ok();
}

Status ReadFloat(JNIEnv* env, jclass info_class, jobject info, const char* name,
                 float* value) {
  jfieldID field = nullptr;
  Status status = RequireField(env, info_class, name, "F", &field);
  if (!status.ok()) {
    return status;
  }
  *value = env->GetFloatField(info, field);
  return CheckJni(env, name);
}

Status ReadInt(JNIEnv* env, jclass info_class, jobject info, const char* name,
               int* value) {
  jfieldID field = nullptr;
  Status status = RequireField(env, info_class, name, "I", &field);
  if (!status.ok()) {
    return status;
  }
  *value = env->GetIntField(info, field);
  return CheckJni(env, name);
}

Status ReadBoolean(JNIEnv* env, jclass info_class, jobject info,
                   const char* name, bool* value) {
  jfieldID field = nullptr;
  Status status = RequireField(env, info_class, name, "Z", &field);
  if (!status.ok()) {
    return status;
  }
  *value = env->GetBooleanField(info, field) == JNI_TRUE;
  return CheckJni(env, name);
}

Status Snapshot(JNIEnv* env, jobject info,
                RobloxNativeTextBoxInfoSnapshot* snapshot) {
  jclass info_class = env->GetObjectClass(info);
  Status status = CheckJni(env, "GetObjectClass");
  if (!status.ok() || info_class == nullptr) {
    if (info_class != nullptr) {
      env->DeleteLocalRef(info_class);
      (void)CheckJni(env, "DeleteLocalRef(class)");
    }
    return status.ok() ? Status::Error(StatusCode::kUnavailable,
                                       "NativeTextBoxInfo class is unavailable")
                       : status;
  }

  RobloxNativeTextBoxInfoSnapshot candidate;
  status = ReadFloat(env, info_class, info, "x", &candidate.x);
  if (status.ok()) {
    status = ReadFloat(env, info_class, info, "y", &candidate.y);
  }
  if (status.ok()) {
    status = ReadFloat(env, info_class, info, "width", &candidate.width);
  }
  if (status.ok()) {
    status = ReadFloat(env, info_class, info, "height", &candidate.height);
  }
  if (status.ok()) {
    status = ReadFloat(env, info_class, info, "fontSize", &candidate.font_size);
  }
  if (status.ok()) {
    status =
        ReadBoolean(env, info_class, info, "multiline", &candidate.multiline);
  }
  if (status.ok()) {
    status =
        ReadInt(env, info_class, info, "xAlignment", &candidate.x_alignment);
  }
  if (status.ok()) {
    status =
        ReadInt(env, info_class, info, "yAlignment", &candidate.y_alignment);
  }
  if (status.ok()) {
    status = ReadInt(env, info_class, info, "textColor", &candidate.text_color);
  }
  if (status.ok()) {
    status = ReadInt(env, info_class, info, "font", &candidate.font);
  }
  if (status.ok()) {
    status = ReadInt(env, info_class, info, "textInputType",
                     &candidate.text_input_type);
  }
  if (status.ok()) {
    status = ReadInt(env, info_class, info, "returnKeyType",
                     &candidate.return_key_type);
  }
  if (status.ok()) {
    status = ReadBoolean(env, info_class, info, "manualFocusRelease",
                         &candidate.manual_focus_release);
  }
  if (status.ok()) {
    status = ReadBoolean(env, info_class, info, "textWrapped",
                         &candidate.text_wrapped);
  }

  env->DeleteLocalRef(info_class);
  const Status delete_status = CheckJni(env, "DeleteLocalRef(class)");
  if (!status.ok()) {
    return status;
  }
  if (!delete_status.ok()) {
    return delete_status;
  }
  *snapshot = candidate;
  return Status::Ok();
}

}  // namespace

Status QueryRobloxNativeTextBoxInfo(
    JNIEnv* env, jclass native_gl_class,
    RobloxNativeGetTextBoxInfoFn native_get_text_box_info,
    RobloxNativeTextBoxInfoQueryResult* result) {
  if (env == nullptr || native_gl_class == nullptr ||
      native_get_text_box_info == nullptr || result == nullptr) {
    return InvalidArgument(
        "NativeTextBoxInfo query requires env, class, export and output");
  }
  *result = {};
  Status status = CheckJni(env, "query precondition");
  if (!status.ok()) {
    return status;
  }

  jobject local_info = native_get_text_box_info(env, native_gl_class);
  status = CheckJni(env, "nativeGetTextBoxInfo");
  if (!status.ok()) {
    if (local_info != nullptr) {
      env->DeleteLocalRef(local_info);
      (void)CheckJni(env, "DeleteLocalRef(info)");
    }
    return status;
  }
  if (local_info == nullptr) {
    return Status::Ok();
  }

  RobloxNativeTextBoxInfoSnapshot snapshot;
  status = Snapshot(env, local_info, &snapshot);
  env->DeleteLocalRef(local_info);
  const Status delete_status = CheckJni(env, "DeleteLocalRef(info)");
  if (!status.ok()) {
    return status;
  }
  if (!delete_status.ok()) {
    return delete_status;
  }
  result->available = true;
  result->info = snapshot;
  return Status::Ok();
}

}  // namespace runtime
}  // namespace mocktail
