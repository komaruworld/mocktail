#ifndef MOCKTAIL_RUNTIME_ROBLOX_WINDOW_INPUT_RUNTIME_H_
#define MOCKTAIL_RUNTIME_ROBLOX_WINDOW_INPUT_RUNTIME_H_

#include <memory>

#include "mocktail/platform/text_clipboard.h"
#include "mocktail/status.h"
#include "runtime/roblox_input_native_adapter.h"
#include "runtime/roblox_text_surface_overlay.h"

namespace mocktail {
namespace runtime {

// Composition owner for the single SDL event observer. It queries logical and
// pixel extents from the window, initializes JNI input first, then registers
// the observer. Shutdown clears the observer before releasing pressed input
// and JNI references, so no event can reach a torn-down Roblox runtime.
class RobloxWindowInputRuntime final {
 public:
  RobloxWindowInputRuntime(JniEnvironmentProvider environment,
                           RobloxInputSymbols symbols);
  ~RobloxWindowInputRuntime();

  RobloxWindowInputRuntime(const RobloxWindowInputRuntime&) = delete;
  RobloxWindowInputRuntime& operator=(const RobloxWindowInputRuntime&) = delete;

  Status Initialize();
  Status Shutdown();
  Status BeginTextFocusSession(RobloxTextFocusSession session);
  Status EndTextFocusSession(int64_t textbox_handle, uint64_t generation,
                             bool notify_native);
  Status ReplaceFocusedTextFromEngine(uint64_t generation,
                                      std::string authoritative_utf8);
  Status QueryCurrentTextBoxInfo(RobloxNativeTextBoxInfoQueryResult* result);
  Status UpdateTextFocusProperties(uint64_t generation,
                                   const RobloxTextFocusProperties& properties);
  RobloxInputSnapshot Snapshot() const;

 private:
  static bool MouseLockQueryCallback(void* context, bool* locked_center);

  RobloxTextSurfaceOverlay text_surface_overlay_;
  std::unique_ptr<platform::TextClipboard> text_clipboard_;
  RobloxInputRuntime runtime_;
  bool observer_registered_ = false;
  bool mouse_lock_query_registered_ = false;
  bool initialized_ = false;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_WINDOW_INPUT_RUNTIME_H_
