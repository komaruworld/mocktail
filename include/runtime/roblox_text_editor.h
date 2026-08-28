#ifndef MOCKTAIL_RUNTIME_ROBLOX_TEXT_EDITOR_H_
#define MOCKTAIL_RUNTIME_ROBLOX_TEXT_EDITOR_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "mocktail/platform/platform_runtime.h"
#include "mocktail/platform/text_clipboard.h"
#include "mocktail/status.h"

namespace mocktail {
namespace runtime {

struct RobloxTextFocusSession {
  int64_t textbox_handle = 0;
  uint64_t generation = 0;
  std::string initial_text;
  bool manual_focus_release = false;
  bool multiline = false;
  int32_t area_x = 0;
  int32_t area_y = 0;
  int32_t area_width = 0;
  int32_t area_height = 0;
  float font_size = 0.0F;
  int32_t x_alignment = 0;
  int32_t y_alignment = 1;
  int32_t text_color = 0;
  int32_t font = 0;
  int32_t text_input_type = 0;
  int32_t return_key_type = 0;
  bool text_wrapped = false;
};

// Non-text properties returned by NativeGLInterface.nativeGetTextBoxInfo.
// Keeping them separate from the focus session lets the Android property
// callback refresh layout without replacing text or creating a new editor
// generation.
struct RobloxTextFocusProperties {
  bool manual_focus_release = false;
  bool multiline = false;
  int32_t area_x = 0;
  int32_t area_y = 0;
  int32_t area_width = 0;
  int32_t area_height = 0;
  float font_size = 0.0F;
  int32_t x_alignment = 0;
  int32_t y_alignment = 1;
  int32_t text_color = 0;
  int32_t font = 0;
  int32_t text_input_type = 0;
  int32_t return_key_type = 0;
  bool text_wrapped = false;
};

enum class RobloxTextDisplayEvent { kShow, kUpdate, kHide };

// Sensitive, synchronous display snapshot. Observers must copy what they need
// during the call, never log `utf8`, and securely clear any retained copy.
struct RobloxTextDisplayUpdate {
  RobloxTextDisplayEvent event = RobloxTextDisplayEvent::kHide;
  uint64_t generation = 0;
  int32_t area_x = 0;
  int32_t area_y = 0;
  int32_t area_width = 0;
  int32_t area_height = 0;
  float font_size = 0.0F;
  bool multiline = false;
  int32_t x_alignment = 0;
  int32_t y_alignment = 1;
  int32_t text_color = 0;
  int32_t font = 0;
  int32_t text_input_type = 0;
  bool text_wrapped = false;
  const char* utf8 = nullptr;
  std::size_t utf8_size = 0;
  int32_t cursor_utf16 = 0;
  int32_t selection_begin_utf16 = 0;
  int32_t selection_end_utf16 = 0;
};

struct RobloxTextDisplaySink {
  using UpdateFn = void (*)(void* context,
                            const RobloxTextDisplayUpdate& update);

  void* context = nullptr;
  UpdateFn update = nullptr;
};

struct RobloxTextSink {
  using SyncFn = Status (*)(void* context, const char* utf8, std::size_t size,
                            int32_t cursor_utf16);
  using PassTextFn = Status (*)(void* context, int64_t textbox_handle,
                                const char* utf8, std::size_t size,
                                bool finished, int32_t cursor_utf16);
  using HandleFn = Status (*)(void* context, int64_t textbox_handle);

  void* context = nullptr;
  SyncFn sync = nullptr;
  PassTextFn pass_text = nullptr;
  HandleFn return_pressed = nullptr;
  HandleFn release_focus = nullptr;
};

struct RobloxTextEditResult {
  bool handled = false;
  bool dispatched = false;
  bool session_ended = false;
  Status status = Status::Ok();
};

struct RobloxTextEditorSnapshot {
  bool focused = false;
  int64_t textbox_handle = 0;
  uint64_t generation = 0;
  std::size_t text_bytes = 0;
  int32_t cursor_utf16 = 0;
  int32_t selection_begin_utf16 = 0;
  int32_t selection_end_utf16 = 0;
  uint64_t committed_edits = 0;
  uint64_t native_errors = 0;
};

// Thread-safe UTF-8 shadow editor for one Roblox TextBox focus generation.
// Text content is intentionally exposed only as a byte count in snapshots.
class RobloxTextEditor final {
 public:
  explicit RobloxTextEditor(RobloxTextSink sink,
                            RobloxTextDisplaySink display_sink = {},
                            platform::TextClipboard* clipboard = nullptr);

  RobloxTextEditor(const RobloxTextEditor&) = delete;
  RobloxTextEditor& operator=(const RobloxTextEditor&) = delete;

  Status BeginFocusSession(RobloxTextFocusSession session);
  Status EndFocusSession(int64_t textbox_handle, uint64_t generation,
                         bool notify_native);
  Status ReplaceFocusedTextFromEngine(uint64_t generation,
                                      std::string authoritative_utf8);
  Status UpdateFocusProperties(uint64_t generation,
                               const RobloxTextFocusProperties& properties);
  RobloxTextEditResult HandleTextInput(const platform::TextInputEvent& event);
  RobloxTextEditResult HandleTextEditing(
      const platform::TextEditingEvent& event);
  RobloxTextEditResult HandleKey(const platform::KeyEvent& event);
  RobloxTextEditorSnapshot Snapshot() const;

 private:
  RobloxTextEditResult ReplaceLocked(std::size_t begin, std::size_t end,
                                     const std::string& replacement);
  RobloxTextEditResult ReplaceCompositionLocked(
      const platform::TextEditingEvent& event);
  RobloxTextEditResult MoveCursorLocked(std::size_t byte_offset,
                                        bool extend_selection);
  RobloxTextEditResult SelectAllLocked();
  RobloxTextEditResult CopySelectionLocked(bool cut);
  RobloxTextEditResult PasteLocked();
  RobloxTextEditResult FinishLocked();
  RobloxTextEditResult ReleaseLocked();
  Status SyncLocked();
  Status PassLocked(bool finished);
  void RememberPendingNativeEchoLocked();
  bool ConsumePendingNativeEchoLocked(const std::string& text);
  void ClearPendingNativeEchoesLocked();
  void PublishDisplayLocked(RobloxTextDisplayEvent event);
  void UpdateSelectionSnapshotLocked();
  std::size_t SelectionBeginLocked() const;
  std::size_t SelectionEndLocked() const;
  bool HasSelectionLocked() const;
  void ClearLocked();

  const RobloxTextSink sink_;
  const RobloxTextDisplaySink display_sink_;
  platform::TextClipboard* const clipboard_;
  mutable std::mutex mutex_;
  RobloxTextFocusSession session_;
  std::string text_;
  std::size_t cursor_byte_ = 0;
  std::size_t selection_anchor_byte_ = 0;
  std::size_t composition_begin_byte_ = 0;
  std::size_t composition_end_byte_ = 0;
  std::size_t composition_original_cursor_byte_ = 0;
  std::size_t composition_original_anchor_byte_ = 0;
  std::string composition_replaced_text_;
  bool composition_active_ = false;
  uint32_t active_shortcuts_ = 0;
  std::deque<std::string> pending_native_echoes_;
  std::size_t pending_native_echo_bytes_ = 0;
  RobloxTextEditorSnapshot snapshot_;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_TEXT_EDITOR_H_
