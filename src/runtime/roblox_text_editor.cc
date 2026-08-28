#include "runtime/roblox_text_editor.h"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <utf8proc.h>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::size_t kMaximumTextBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaximumPendingNativeEchoes = 16;
constexpr uint32_t kShortcutSelectAll = 1U << 0U;
constexpr uint32_t kShortcutCopy = 1U << 1U;
constexpr uint32_t kShortcutPaste = 1U << 2U;
constexpr uint32_t kShortcutCut = 1U << 3U;

Status InvalidArgument(const char* message) {
  return Status::Error(StatusCode::kInvalidArgument, message);
}

Status FailedPrecondition(const char* message) {
  return Status::Error(StatusCode::kFailedPrecondition, message);
}

Status Unsupported(const char* message) {
  return Status::Error(StatusCode::kUnsupported, message);
}

struct Utf8Layout {
  bool valid = false;
  int32_t utf16_units = 0;
  std::vector<std::size_t> grapheme_boundaries;
};

Utf8Layout Layout(const std::string& text) {
  Utf8Layout layout;
  layout.grapheme_boundaries.push_back(0);
  utf8proc_int32_t previous = 0;
  utf8proc_int32_t break_state = 0;
  bool have_previous = false;
  std::size_t offset = 0;
  int64_t utf16_units = 0;
  while (offset < text.size()) {
    utf8proc_int32_t codepoint = 0;
    const utf8proc_ssize_t count = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(text.data() + offset),
        static_cast<utf8proc_ssize_t>(text.size() - offset), &codepoint);
    if (count <= 0) {
      return layout;
    }
    if (have_previous &&
        utf8proc_grapheme_break_stateful(previous, codepoint, &break_state)) {
      layout.grapheme_boundaries.push_back(offset);
    }
    utf16_units += codepoint > 0xffff ? 2 : 1;
    if (utf16_units > std::numeric_limits<int32_t>::max()) {
      return layout;
    }
    previous = codepoint;
    have_previous = true;
    offset += static_cast<std::size_t>(count);
  }
  if (layout.grapheme_boundaries.back() != text.size()) {
    layout.grapheme_boundaries.push_back(text.size());
  }
  layout.valid = true;
  layout.utf16_units = static_cast<int32_t>(utf16_units);
  return layout;
}

int32_t Utf16Cursor(const std::string& text, std::size_t byte_offset) {
  std::size_t offset = 0;
  int32_t units = 0;
  while (offset < byte_offset) {
    utf8proc_int32_t codepoint = 0;
    const utf8proc_ssize_t count = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(text.data() + offset),
        static_cast<utf8proc_ssize_t>(byte_offset - offset), &codepoint);
    if (count <= 0) {
      return 0;
    }
    units += codepoint > 0xffff ? 2 : 1;
    offset += static_cast<std::size_t>(count);
  }
  return units;
}

std::size_t Utf8CharacterByteOffset(const std::string& text,
                                    int character_index) {
  if (character_index < 0) {
    return text.size();
  }
  std::size_t offset = 0;
  for (int index = 0; index < character_index && offset < text.size();
       ++index) {
    utf8proc_int32_t codepoint = 0;
    const utf8proc_ssize_t count = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(text.data() + offset),
        static_cast<utf8proc_ssize_t>(text.size() - offset), &codepoint);
    if (count <= 0) {
      return text.size();
    }
    offset += static_cast<std::size_t>(count);
  }
  return offset;
}

RobloxTextEditResult Result(bool handled, bool dispatched, bool ended,
                            Status status = Status::Ok()) {
  return {handled, dispatched, ended, std::move(status)};
}

void SecureClear(std::string* text) {
  if (text == nullptr) {
    return;
  }
  volatile char* bytes = text->empty() ? nullptr : &(*text)[0];
  for (std::size_t index = 0; index < text->size(); ++index) {
    bytes[index] = 0;
  }
  std::string empty;
  text->swap(empty);
}

uint32_t ShortcutBit(SDL_Scancode scancode) {
  switch (scancode) {
    case SDL_SCANCODE_A:
      return kShortcutSelectAll;
    case SDL_SCANCODE_C:
      return kShortcutCopy;
    case SDL_SCANCODE_V:
      return kShortcutPaste;
    case SDL_SCANCODE_X:
      return kShortcutCut;
    default:
      return 0;
  }
}

bool IsPasswordInputType(int32_t text_input_type) {
  return text_input_type == 5 || text_input_type == 9 ||
         text_input_type == 10;
}

}  // namespace

RobloxTextEditor::RobloxTextEditor(RobloxTextSink sink,
                                   RobloxTextDisplaySink display_sink,
                                   platform::TextClipboard* clipboard)
    : sink_(sink), display_sink_(display_sink), clipboard_(clipboard) {}

Status RobloxTextEditor::BeginFocusSession(RobloxTextFocusSession session) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (session.textbox_handle == 0 || session.generation == 0) {
    return InvalidArgument("TextBox handle and generation must be non-zero");
  }
  if (session.initial_text.size() > kMaximumTextBytes ||
      !Layout(session.initial_text).valid) {
    return InvalidArgument(
        "initial TextBox text is invalid UTF-8 or too large");
  }
  if (sink_.sync == nullptr || sink_.pass_text == nullptr ||
      sink_.return_pressed == nullptr || sink_.release_focus == nullptr) {
    return Unsupported("Roblox TextBox native export table is incomplete");
  }
  if (snapshot_.focused) {
    // A new gameActivity_showKeyboard callback is authoritative evidence that
    // the engine has already switched focus. Replacing it must not re-enter
    // nativeReleaseFocus for the old handle.
    ClearLocked();
  }
  text_ = std::move(session.initial_text);
  session.initial_text.clear();
  session_ = std::move(session);
  cursor_byte_ = text_.size();
  selection_anchor_byte_ = cursor_byte_;
  composition_begin_byte_ = 0;
  composition_end_byte_ = 0;
  composition_original_cursor_byte_ = 0;
  composition_original_anchor_byte_ = 0;
  SecureClear(&composition_replaced_text_);
  composition_active_ = false;
  active_shortcuts_ = 0;
  snapshot_.focused = true;
  snapshot_.textbox_handle = session_.textbox_handle;
  snapshot_.generation = session_.generation;
  snapshot_.text_bytes = text_.size();
  UpdateSelectionSnapshotLocked();
  PublishDisplayLocked(RobloxTextDisplayEvent::kShow);
  return Status::Ok();
}

Status RobloxTextEditor::EndFocusSession(int64_t textbox_handle,
                                         uint64_t generation,
                                         bool notify_native) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.focused) {
    return Status::Ok();
  }
  if (textbox_handle != session_.textbox_handle ||
      generation != session_.generation) {
    return FailedPrecondition("stale Roblox TextBox focus generation");
  }
  Status status = Status::Ok();
  if (notify_native) {
    status = sink_.release_focus(sink_.context, session_.textbox_handle);
    if (!status.ok()) {
      ++snapshot_.native_errors;
    }
  }
  ClearLocked();
  return status;
}

Status RobloxTextEditor::ReplaceFocusedTextFromEngine(
    uint64_t generation, std::string authoritative_utf8) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.focused || generation != session_.generation) {
    SecureClear(&authoritative_utf8);
    return FailedPrecondition("stale Roblox TextBox engine update");
  }
  if (authoritative_utf8.size() > kMaximumTextBytes ||
      !Layout(authoritative_utf8).valid) {
    SecureClear(&authoritative_utf8);
    return InvalidArgument("engine TextBox text is invalid UTF-8 or too large");
  }
  // EngineJavaCallback2.g may asynchronously reflect an earlier outbound
  // editor state. Consume any known snapshot instead of letting it replace a
  // newer host composition. Unknown values remain authoritative Lua-side
  // edits; bounded tombstones are retained for echoes that arrive later.
  if (ConsumePendingNativeEchoLocked(authoritative_utf8)) {
    SecureClear(&authoritative_utf8);
    return Status::Ok();
  }
  if (authoritative_utf8 == text_) {
    SecureClear(&authoritative_utf8);
    return Status::Ok();
  }
  SecureClear(&text_);
  text_ = std::move(authoritative_utf8);
  cursor_byte_ = text_.size();
  selection_anchor_byte_ = cursor_byte_;
  composition_begin_byte_ = 0;
  composition_end_byte_ = 0;
  composition_original_cursor_byte_ = 0;
  composition_original_anchor_byte_ = 0;
  SecureClear(&composition_replaced_text_);
  composition_active_ = false;
  snapshot_.text_bytes = text_.size();
  UpdateSelectionSnapshotLocked();
  PublishDisplayLocked(RobloxTextDisplayEvent::kUpdate);
  return Status::Ok();
}

Status RobloxTextEditor::UpdateFocusProperties(
    uint64_t generation, const RobloxTextFocusProperties& properties) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.focused || generation != session_.generation) {
    return FailedPrecondition("stale Roblox TextBox property update");
  }
  session_.manual_focus_release = properties.manual_focus_release;
  session_.multiline = properties.multiline;
  session_.area_x = properties.area_x;
  session_.area_y = properties.area_y;
  session_.area_width = properties.area_width;
  session_.area_height = properties.area_height;
  session_.font_size = properties.font_size;
  session_.x_alignment = properties.x_alignment;
  session_.y_alignment = properties.y_alignment;
  session_.text_color = properties.text_color;
  session_.font = properties.font;
  session_.text_input_type = properties.text_input_type;
  session_.return_key_type = properties.return_key_type;
  session_.text_wrapped = properties.text_wrapped;
  PublishDisplayLocked(RobloxTextDisplayEvent::kUpdate);
  return Status::Ok();
}

RobloxTextEditResult RobloxTextEditor::HandleTextInput(
    const platform::TextInputEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.focused) {
    return Result(false, false, false);
  }
  if (event.text.empty()) {
    return Result(true, false, false);
  }
  const std::size_t replace_begin =
      composition_active_ ? composition_begin_byte_ : SelectionBeginLocked();
  const std::size_t replace_end =
      composition_active_ ? composition_end_byte_ : SelectionEndLocked();
  if (!Layout(event.text).valid ||
      event.text.size() >
          kMaximumTextBytes - (text_.size() - (replace_end - replace_begin))) {
    return Result(
        true, false, false,
        InvalidArgument("committed text is invalid UTF-8 or too large"));
  }
  if (composition_active_) {
    composition_active_ = false;
    SecureClear(&composition_replaced_text_);
    return ReplaceLocked(replace_begin, replace_end, event.text);
  }
  return ReplaceLocked(replace_begin, replace_end, event.text);
}

RobloxTextEditResult RobloxTextEditor::HandleTextEditing(
    const platform::TextEditingEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.focused) {
    return Result(false, false, false);
  }
  return ReplaceCompositionLocked(event);
}

RobloxTextEditResult RobloxTextEditor::HandleKey(
    const platform::KeyEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.focused) {
    return Result(false, false, false);
  }

  const auto scancode = static_cast<SDL_Scancode>(event.scancode);
  const uint32_t shortcut_bit = ShortcutBit(scancode);
  const bool ctrl_pressed =
      (event.modifiers & static_cast<uint32_t>(SDL_KMOD_CTRL)) != 0;
  const bool alt_pressed =
      (event.modifiers & static_cast<uint32_t>(SDL_KMOD_ALT)) != 0;
  const bool shift_pressed =
      (event.modifiers & static_cast<uint32_t>(SDL_KMOD_SHIFT)) != 0;
  const bool shortcut = shortcut_bit != 0 && ctrl_pressed && !alt_pressed;
  const bool edit_key =
      scancode == SDL_SCANCODE_BACKSPACE || scancode == SDL_SCANCODE_DELETE ||
      scancode == SDL_SCANCODE_LEFT || scancode == SDL_SCANCODE_RIGHT ||
      scancode == SDL_SCANCODE_HOME || scancode == SDL_SCANCODE_END ||
      scancode == SDL_SCANCODE_RETURN || scancode == SDL_SCANCODE_KP_ENTER ||
      scancode == SDL_SCANCODE_ESCAPE || shortcut;
  if (!event.pressed && shortcut_bit != 0 &&
      (active_shortcuts_ & shortcut_bit) != 0) {
    active_shortcuts_ &= ~shortcut_bit;
    return Result(true, false, false);
  }
  if (!edit_key) {
    return Result(false, false, false);
  }
  // Swallow both halves so editing keys are never duplicated through
  // nativePassKeyEvent while the on-screen editor owns focus.
  if (!event.pressed) {
    return Result(true, false, false);
  }
  if (shortcut) {
    active_shortcuts_ |= shortcut_bit;
  }

  if (composition_active_) {
    // SDL's IME owns editing/navigation keys while composition is active and
    // will describe the resulting visible state with TEXT_EDITING.
    return Result(true, false, false);
  }

  if (shortcut_bit == kShortcutSelectAll) {
    return SelectAllLocked();
  }
  if (shortcut_bit == kShortcutCopy) {
    return CopySelectionLocked(false);
  }
  if (shortcut_bit == kShortcutPaste) {
    return PasteLocked();
  }
  if (shortcut_bit == kShortcutCut) {
    return CopySelectionLocked(true);
  }

  const Utf8Layout layout = Layout(text_);
  const auto current =
      std::lower_bound(layout.grapheme_boundaries.begin(),
                       layout.grapheme_boundaries.end(), cursor_byte_);
  if (scancode == SDL_SCANCODE_BACKSPACE) {
    if (HasSelectionLocked()) {
      return ReplaceLocked(SelectionBeginLocked(), SelectionEndLocked(), "");
    }
    if (current == layout.grapheme_boundaries.begin()) {
      return Result(true, false, false);
    }
    return ReplaceLocked(*(current - 1), cursor_byte_, "");
  }
  if (scancode == SDL_SCANCODE_DELETE) {
    if (HasSelectionLocked()) {
      return ReplaceLocked(SelectionBeginLocked(), SelectionEndLocked(), "");
    }
    if (current == layout.grapheme_boundaries.end() ||
        current + 1 == layout.grapheme_boundaries.end()) {
      return Result(true, false, false);
    }
    return ReplaceLocked(cursor_byte_, *(current + 1), "");
  }
  if (scancode == SDL_SCANCODE_LEFT) {
    if (HasSelectionLocked() && !shift_pressed) {
      return MoveCursorLocked(SelectionBeginLocked(), false);
    }
    return MoveCursorLocked(current == layout.grapheme_boundaries.begin()
                                ? cursor_byte_
                                : *(current - 1),
                            shift_pressed);
  }
  if (scancode == SDL_SCANCODE_RIGHT) {
    if (HasSelectionLocked() && !shift_pressed) {
      return MoveCursorLocked(SelectionEndLocked(), false);
    }
    return MoveCursorLocked(current + 1 == layout.grapheme_boundaries.end()
                                ? cursor_byte_
                                : *(current + 1),
                            shift_pressed);
  }
  if (scancode == SDL_SCANCODE_HOME) {
    return MoveCursorLocked(0, shift_pressed);
  }
  if (scancode == SDL_SCANCODE_END) {
    return MoveCursorLocked(text_.size(), shift_pressed);
  }
  if (scancode == SDL_SCANCODE_ESCAPE) {
    return ReleaseLocked();
  }
  if (session_.multiline) {
    return ReplaceLocked(SelectionBeginLocked(), SelectionEndLocked(), "\n");
  }
  return FinishLocked();
}

RobloxTextEditorSnapshot RobloxTextEditor::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

RobloxTextEditResult RobloxTextEditor::ReplaceLocked(
    std::size_t begin, std::size_t end, const std::string& replacement) {
  std::string updated;
  updated.reserve(text_.size() - (end - begin) + replacement.size());
  updated.append(text_, 0, begin);
  updated.append(replacement);
  updated.append(text_, end, std::string::npos);
  SecureClear(&text_);
  text_ = std::move(updated);
  cursor_byte_ = begin + replacement.size();
  selection_anchor_byte_ = cursor_byte_;
  composition_begin_byte_ = 0;
  composition_end_byte_ = 0;
  composition_original_cursor_byte_ = 0;
  composition_original_anchor_byte_ = 0;
  SecureClear(&composition_replaced_text_);
  composition_active_ = false;
  snapshot_.text_bytes = text_.size();
  UpdateSelectionSnapshotLocked();
  Status status = SyncLocked();
  if (status.ok()) {
    status = PassLocked(false);
  }
  if (status.ok()) {
    RememberPendingNativeEchoLocked();
    ++snapshot_.committed_edits;
    PublishDisplayLocked(RobloxTextDisplayEvent::kUpdate);
  } else {
    ++snapshot_.native_errors;
    sink_.release_focus(sink_.context, session_.textbox_handle);
    ClearLocked();
  }
  return Result(true, status.ok(), !status.ok(), std::move(status));
}

RobloxTextEditResult RobloxTextEditor::ReplaceCompositionLocked(
    const platform::TextEditingEvent& event) {
  if (!composition_active_ && event.text.empty()) {
    return Result(true, false, false);
  }
  const std::size_t begin =
      composition_active_ ? composition_begin_byte_ : SelectionBeginLocked();
  const std::size_t end =
      composition_active_ ? composition_end_byte_ : SelectionEndLocked();
  if (!Layout(event.text).valid ||
      event.text.size() > kMaximumTextBytes - (text_.size() - (end - begin))) {
    return Result(
        true, false, false,
        InvalidArgument("preedit text is invalid UTF-8 or too large"));
  }

  if (!composition_active_) {
    composition_original_cursor_byte_ = cursor_byte_;
    composition_original_anchor_byte_ = selection_anchor_byte_;
    composition_replaced_text_.assign(text_, begin, end - begin);
  }
  const bool cancel_composition = event.text.empty();
  const std::string& visible_replacement =
      cancel_composition ? composition_replaced_text_ : event.text;

  std::string updated;
  updated.reserve(text_.size() - (end - begin) + visible_replacement.size());
  updated.append(text_, 0, begin);
  updated.append(visible_replacement);
  updated.append(text_, end, std::string::npos);
  SecureClear(&text_);
  text_ = std::move(updated);
  composition_begin_byte_ = begin;
  composition_end_byte_ = begin + visible_replacement.size();
  composition_active_ = !cancel_composition;
  // SDL start/length describe a selection in UTF-8 characters inside the
  // preedit string. Roblox exposes only one UTF-16 cursor position here, so
  // start becomes the caret and the unrepresentable selection length remains
  // owned by the host IME candidate UI.
  if (cancel_composition) {
    cursor_byte_ = composition_original_cursor_byte_;
    selection_anchor_byte_ = composition_original_anchor_byte_;
    composition_original_cursor_byte_ = 0;
    composition_original_anchor_byte_ = 0;
    SecureClear(&composition_replaced_text_);
  } else {
    cursor_byte_ = begin + Utf8CharacterByteOffset(event.text, event.start);
    selection_anchor_byte_ = cursor_byte_;
  }
  snapshot_.text_bytes = text_.size();
  UpdateSelectionSnapshotLocked();

  Status status = SyncLocked();
  if (status.ok()) {
    status = PassLocked(false);
  }
  if (status.ok()) {
    RememberPendingNativeEchoLocked();
    PublishDisplayLocked(RobloxTextDisplayEvent::kUpdate);
  } else {
    ++snapshot_.native_errors;
    sink_.release_focus(sink_.context, session_.textbox_handle);
    ClearLocked();
  }
  return Result(true, status.ok(), !status.ok(), std::move(status));
}

RobloxTextEditResult RobloxTextEditor::MoveCursorLocked(
    std::size_t byte_offset, bool extend_selection) {
  const std::size_t previous_cursor = cursor_byte_;
  const std::size_t previous_anchor = selection_anchor_byte_;
  cursor_byte_ = byte_offset;
  if (!extend_selection) {
    selection_anchor_byte_ = cursor_byte_;
  }
  if (cursor_byte_ == previous_cursor &&
      selection_anchor_byte_ == previous_anchor) {
    return Result(true, false, false);
  }
  UpdateSelectionSnapshotLocked();
  if (cursor_byte_ == previous_cursor) {
    PublishDisplayLocked(RobloxTextDisplayEvent::kUpdate);
    return Result(true, false, false);
  }
  Status status = SyncLocked();
  if (!status.ok()) {
    ++snapshot_.native_errors;
    sink_.release_focus(sink_.context, session_.textbox_handle);
    ClearLocked();
  } else {
    PublishDisplayLocked(RobloxTextDisplayEvent::kUpdate);
  }
  return Result(true, status.ok(), !status.ok(), std::move(status));
}

RobloxTextEditResult RobloxTextEditor::SelectAllLocked() {
  const std::size_t previous_cursor = cursor_byte_;
  const std::size_t previous_anchor = selection_anchor_byte_;
  selection_anchor_byte_ = 0;
  cursor_byte_ = text_.size();
  if (cursor_byte_ == previous_cursor &&
      selection_anchor_byte_ == previous_anchor) {
    return Result(true, false, false);
  }
  UpdateSelectionSnapshotLocked();
  Status status = Status::Ok();
  const bool cursor_changed = cursor_byte_ != previous_cursor;
  if (cursor_changed) {
    status = SyncLocked();
  }
  if (!status.ok()) {
    ++snapshot_.native_errors;
    sink_.release_focus(sink_.context, session_.textbox_handle);
    ClearLocked();
  } else {
    PublishDisplayLocked(RobloxTextDisplayEvent::kUpdate);
  }
  return Result(true, cursor_changed && status.ok(), !status.ok(),
                std::move(status));
}

RobloxTextEditResult RobloxTextEditor::CopySelectionLocked(bool cut) {
  if (!HasSelectionLocked() || clipboard_ == nullptr ||
      IsPasswordInputType(session_.text_input_type)) {
    return Result(true, false, false);
  }
  const std::size_t begin = SelectionBeginLocked();
  const std::size_t end = SelectionEndLocked();
  std::string selected(text_, begin, end - begin);
  const Status clipboard_status = clipboard_->WriteText(selected);
  SecureClear(&selected);
  if (!clipboard_status.ok() || !cut) {
    // Clipboard ownership can disappear independently under X11/Wayland. A
    // copy failure is non-fatal and must not close the focused Roblox TextBox.
    return Result(true, false, false);
  }
  return ReplaceLocked(begin, end, "");
}

RobloxTextEditResult RobloxTextEditor::PasteLocked() {
  if (clipboard_ == nullptr) {
    return Result(true, false, false);
  }
  std::string clipboard_text;
  const Status clipboard_status = clipboard_->ReadText(&clipboard_text);
  const std::size_t begin = SelectionBeginLocked();
  const std::size_t end = SelectionEndLocked();
  const bool valid = clipboard_status.ok() && !clipboard_text.empty() &&
                     clipboard_text.size() <=
                         kMaximumTextBytes - (text_.size() - (end - begin)) &&
                     Layout(clipboard_text).valid;
  if (!valid) {
    SecureClear(&clipboard_text);
    return Result(true, false, false);
  }
  RobloxTextEditResult result = ReplaceLocked(begin, end, clipboard_text);
  SecureClear(&clipboard_text);
  return result;
}

RobloxTextEditResult RobloxTextEditor::FinishLocked() {
  // Matches RbxKeyboard.OnEditorActionListener: sync, return, then (unless
  // manual release is requested) pass the final text and hide the editor.
  Status status = SyncLocked();
  if (status.ok()) {
    status = sink_.return_pressed(sink_.context, session_.textbox_handle);
  }
  bool ended = false;
  if (status.ok() && !session_.manual_focus_release) {
    status = PassLocked(true);
    if (status.ok()) {
      ClearLocked();
      ended = true;
    }
  }
  if (!status.ok()) {
    ++snapshot_.native_errors;
    sink_.release_focus(sink_.context, session_.textbox_handle);
    ClearLocked();
    ended = true;
  }
  return Result(true, status.ok(), ended, std::move(status));
}

RobloxTextEditResult RobloxTextEditor::ReleaseLocked() {
  Status status = sink_.release_focus(sink_.context, session_.textbox_handle);
  if (!status.ok()) {
    ++snapshot_.native_errors;
    return Result(true, false, false, std::move(status));
  }
  ClearLocked();
  return Result(true, true, true);
}

Status RobloxTextEditor::SyncLocked() {
  return sink_.sync(sink_.context, text_.data(), text_.size(),
                    snapshot_.cursor_utf16);
}

Status RobloxTextEditor::PassLocked(bool finished) {
  return sink_.pass_text(sink_.context, session_.textbox_handle, text_.data(),
                         text_.size(), finished, snapshot_.cursor_utf16);
}

void RobloxTextEditor::RememberPendingNativeEchoLocked() {
  if (!pending_native_echoes_.empty() &&
      pending_native_echoes_.back() == text_) {
    return;
  }
  while (!pending_native_echoes_.empty() &&
         (pending_native_echoes_.size() >= kMaximumPendingNativeEchoes ||
          pending_native_echo_bytes_ > kMaximumTextBytes - text_.size())) {
    pending_native_echo_bytes_ -= pending_native_echoes_.front().size();
    SecureClear(&pending_native_echoes_.front());
    pending_native_echoes_.pop_front();
  }
  pending_native_echoes_.push_back(text_);
  pending_native_echo_bytes_ += text_.size();
}

bool RobloxTextEditor::ConsumePendingNativeEchoLocked(const std::string& text) {
  auto match = std::find(pending_native_echoes_.begin(),
                         pending_native_echoes_.end(), text);
  if (match == pending_native_echoes_.end()) {
    return false;
  }
  pending_native_echo_bytes_ -= match->size();
  SecureClear(&*match);
  pending_native_echoes_.erase(match);
  return true;
}

void RobloxTextEditor::ClearPendingNativeEchoesLocked() {
  for (std::string& text : pending_native_echoes_) {
    SecureClear(&text);
  }
  pending_native_echoes_.clear();
  pending_native_echo_bytes_ = 0;
}

void RobloxTextEditor::PublishDisplayLocked(RobloxTextDisplayEvent event) {
  if (display_sink_.update == nullptr) {
    return;
  }
  RobloxTextDisplayUpdate update;
  update.event = event;
  update.generation = session_.generation;
  update.area_x = session_.area_x;
  update.area_y = session_.area_y;
  update.area_width = session_.area_width;
  update.area_height = session_.area_height;
  update.font_size = session_.font_size;
  update.multiline = session_.multiline;
  update.x_alignment = session_.x_alignment;
  update.y_alignment = session_.y_alignment;
  update.text_color = session_.text_color;
  update.font = session_.font;
  update.text_input_type = session_.text_input_type;
  update.text_wrapped = session_.text_wrapped;
  if (event != RobloxTextDisplayEvent::kHide) {
    update.utf8 = text_.data();
    update.utf8_size = text_.size();
    update.cursor_utf16 = snapshot_.cursor_utf16;
    update.selection_begin_utf16 = snapshot_.selection_begin_utf16;
    update.selection_end_utf16 = snapshot_.selection_end_utf16;
  }
  display_sink_.update(display_sink_.context, update);
}

void RobloxTextEditor::UpdateSelectionSnapshotLocked() {
  snapshot_.cursor_utf16 = Utf16Cursor(text_, cursor_byte_);
  snapshot_.selection_begin_utf16 =
      Utf16Cursor(text_, SelectionBeginLocked());
  snapshot_.selection_end_utf16 = Utf16Cursor(text_, SelectionEndLocked());
}

std::size_t RobloxTextEditor::SelectionBeginLocked() const {
  return std::min(cursor_byte_, selection_anchor_byte_);
}

std::size_t RobloxTextEditor::SelectionEndLocked() const {
  return std::max(cursor_byte_, selection_anchor_byte_);
}

bool RobloxTextEditor::HasSelectionLocked() const {
  return cursor_byte_ != selection_anchor_byte_;
}

void RobloxTextEditor::ClearLocked() {
  if (snapshot_.focused) {
    PublishDisplayLocked(RobloxTextDisplayEvent::kHide);
  }
  SecureClear(&session_.initial_text);
  session_ = {};
  SecureClear(&text_);
  ClearPendingNativeEchoesLocked();
  cursor_byte_ = 0;
  selection_anchor_byte_ = 0;
  composition_begin_byte_ = 0;
  composition_end_byte_ = 0;
  composition_original_cursor_byte_ = 0;
  composition_original_anchor_byte_ = 0;
  SecureClear(&composition_replaced_text_);
  composition_active_ = false;
  active_shortcuts_ = 0;
  snapshot_.focused = false;
  snapshot_.textbox_handle = 0;
  snapshot_.generation = 0;
  snapshot_.text_bytes = 0;
  snapshot_.cursor_utf16 = 0;
  snapshot_.selection_begin_utf16 = 0;
  snapshot_.selection_end_utf16 = 0;
}

}  // namespace runtime
}  // namespace mocktail
