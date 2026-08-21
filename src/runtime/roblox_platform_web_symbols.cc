#include "runtime/roblox_platform_web_symbols.h"

#include <string>

#include "linker/linker.h"

namespace mocktail {
namespace runtime {
namespace {

template <typename Function>
Function Resolve(void* library, const char* name) {
  return reinterpret_cast<Function>(
      linker::ResolveSymbol(library, std::string(name)));
}

}  // namespace

RobloxPlatformWebSymbols ResolveRobloxPlatformWebSymbols(
    void* roblox_library, SubscribeWebViewRawFn subscribe_raw,
    DeleteWebViewConnectionFn delete_connection) {
  RobloxPlatformWebSymbols symbols;
  if (roblox_library == nullptr) {
    return symbols;
  }

  symbols.web_view.get_open_window_id = Resolve<GetWebViewOpenWindowIdFn>(
      roblox_library,
      "Java_com_roblox_protocols_webview_WebViewProtocol_getOpenWindowId");
  symbols.web_view.get_handle_window_close_id =
      Resolve<GetWebViewHandleWindowCloseIdFn>(
          roblox_library,
          "Java_com_roblox_protocols_webview_WebViewProtocol_"
          "getHandleWindowCloseId");
  symbols.web_view.get_protocol_name = Resolve<GetWebViewStringFn>(
      roblox_library,
      "Java_com_roblox_protocols_webview_WebViewProtocol_getProtocolName");
  symbols.web_view.get_is_available_id = Resolve<GetWebViewStringFn>(
      roblox_library,
      "Java_com_roblox_protocols_webview_WebViewProtocol_getIsAvailableId");
  symbols.web_view.get_message_id = Resolve<GetWebViewMessageIdFn>(
      roblox_library,
      "Java_com_roblox_universalapp_messagebus_MessageBus_getMessageId");
  symbols.web_view.initialize_android_web_view_protocol =
      Resolve<InitializeAndroidWebViewProtocolFn>(
          roblox_library,
          "Java_com_roblox_protocols_webview_WebViewProtocol_"
          "initializeAndroidWebViewProtocol");
  symbols.web_view.subscribe_raw = subscribe_raw;
  symbols.web_view.delete_connection = delete_connection;
  symbols.web_view.set_request_handler_raw =
      Resolve<SetWebViewRequestHandlerRawFn>(
          roblox_library,
          "Java_com_roblox_universalapp_messagebus_MessageBus_"
          "setRequestHandlerRaw");
  symbols.web_view.clear_request_handler =
      Resolve<ClearWebViewRequestHandlerFn>(
          roblox_library,
          "Java_com_roblox_universalapp_messagebus_MessageBus_"
          "clearRequestHandler");
  symbols.web_view.publish_raw = Resolve<PublishWebViewRawFn>(
      roblox_library,
      "Java_com_roblox_universalapp_messagebus_MessageBus_publishRaw");
  symbols.web_view.broadcast_data_model_focus =
      Resolve<BroadcastWebViewDataModelFocusFn>(
          roblox_library,
          "Java_com_roblox_engine_jni_NativeGLInterface_"
          "nativeBroadcastEventWithNamespace");
  symbols.web_view.get_mutate_window_id = Resolve<GetWebViewMutateWindowIdFn>(
      roblox_library,
      "Java_com_roblox_protocols_webview_WebViewProtocol_"
      "getMutateWindowId");
  symbols.web_view.get_close_window_id = Resolve<GetWebViewCloseWindowIdFn>(
      roblox_library,
      "Java_com_roblox_protocols_webview_WebViewProtocol_getCloseWindowId");
  symbols.web_view.signal_javascript_callback =
      Resolve<SignalWebViewJavascriptCallbackFn>(
          roblox_library,
          "Java_com_roblox_protocols_webview_WebViewProtocol_"
          "signalJavascriptCallback");
  symbols.web_view.update_cookie_set_handler =
      Resolve<UpdateRobloxCookieSetHandlerFn>(
          roblox_library,
          "Java_com_roblox_universalapp_cookie_JNICookieProtocol_"
          "updateOnSetCookieHandler");

  symbols.browser_service.bind = Resolve<BindRobloxMemStorageFn>(
      roblox_library, "Java_com_roblox_engine_jni_memstorage_MemStorage_bind");
  symbols.browser_service.disconnect = Resolve<DisconnectRobloxMemStorageFn>(
      roblox_library,
      "Java_com_roblox_engine_jni_memstorage_Connection_disconnect");
  symbols.browser_service.release_connection =
      Resolve<ReleaseRobloxMemStorageConnectionFn>(
          roblox_library,
          "Java_com_roblox_engine_jni_memstorage_Connection_"
          "releaseConnection");
  symbols.browser_service.fire = Resolve<FireRobloxMemStorageFn>(
      roblox_library, "Java_com_roblox_engine_jni_memstorage_MemStorage_fire");
  return symbols;
}

}  // namespace runtime
}  // namespace mocktail
