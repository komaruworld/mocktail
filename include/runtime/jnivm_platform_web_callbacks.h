#ifndef MOCKTAIL_RUNTIME_JNIVM_PLATFORM_WEB_CALLBACKS_H_
#define MOCKTAIL_RUNTIME_JNIVM_PLATFORM_WEB_CALLBACKS_H_

#include <jni.h>

#include <memory>

namespace mocktail {
namespace runtime {

// The opaque context must be the jnivm::VM owned by startup composition.
bool SetJnivmPlatformWebCallbacks(
    void* context, std::shared_ptr<void> callback_context,
    void (*on_data_model_notification)(void*, JNIEnv*, jstring, jstring),
    void (*on_app_bridge_notification)(void*, JNIEnv*, jstring, jstring),
    void (*on_native_overlay)(void*, JNIEnv*, jstring, jstring),
    void (*on_open_web_activity)(void*, JNIEnv*, jstring, jstring),
    void (*on_sync_cookies)(void*, JNIEnv*, jstring),
    void (*on_set_cookie)(void*, JNIEnv*, jstring, jstring));
void ClearJnivmPlatformWebCallbacks(void* context);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_JNIVM_PLATFORM_WEB_CALLBACKS_H_
