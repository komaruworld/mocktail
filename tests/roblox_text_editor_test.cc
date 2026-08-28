#include "runtime/roblox_text_editor.h"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

struct Call {
  std::string operation;
  int64_t handle = 0;
  std::string text;
  bool finished = false;
  int32_t cursor = 0;
};

struct Probe {
  std::mutex mutex;
  std::vector<Call> calls;
  bool fail_next = false;
};

struct DisplayCall {
  RobloxTextDisplayEvent event = RobloxTextDisplayEvent::kHide;
  uint64_t generation = 0;
  int32_t area_x = 0;
  int32_t area_y = 0;
  int32_t area_width = 0;
  int32_t area_height = 0;
  float font_size = 0.0F;
  bool multiline = false;
  int32_t font = 0;
  int32_t text_input_type = 0;
  bool text_wrapped = false;
  std::string text;
  int32_t cursor_utf16 = 0;
  int32_t selection_begin_utf16 = 0;
  int32_t selection_end_utf16 = 0;
};

struct DisplayProbe {
  std::vector<DisplayCall> calls;
};

void DisplayUpdate(void* context, const RobloxTextDisplayUpdate& update) {
  auto* probe = static_cast<DisplayProbe*>(context);
  DisplayCall call;
  call.event = update.event;
  call.generation = update.generation;
  call.area_x = update.area_x;
  call.area_y = update.area_y;
  call.area_width = update.area_width;
  call.area_height = update.area_height;
  call.font_size = update.font_size;
  call.multiline = update.multiline;
  call.font = update.font;
  call.text_input_type = update.text_input_type;
  call.text_wrapped = update.text_wrapped;
  if (update.utf8 != nullptr) {
    call.text.assign(update.utf8, update.utf8_size);
  }
  call.cursor_utf16 = update.cursor_utf16;
  call.selection_begin_utf16 = update.selection_begin_utf16;
  call.selection_end_utf16 = update.selection_end_utf16;
  probe->calls.push_back(std::move(call));
}

class ClipboardProbe final : public platform::TextClipboard {
 public:
  Status ReadText(std::string* text) override {
    ++read_calls;
    if (fail_read) {
      return Status::Error(StatusCode::kPlatformError,
                           "injected clipboard read failure");
    }
    *text = read_text;
    return Status::Ok();
  }

  Status WriteText(const std::string& text) override {
    ++write_calls;
    if (fail_write) {
      return Status::Error(StatusCode::kPlatformError,
                           "injected clipboard write failure");
    }
    written_text = text;
    return Status::Ok();
  }

  std::string read_text;
  std::string written_text;
  int read_calls = 0;
  int write_calls = 0;
  bool fail_read = false;
  bool fail_write = false;
};

Status Record(Probe* probe, Call call) {
  std::lock_guard<std::mutex> lock(probe->mutex);
  probe->calls.push_back(std::move(call));
  if (probe->fail_next) {
    probe->fail_next = false;
    return Status::Error(StatusCode::kPlatformError, "injected native error");
  }
  return Status::Ok();
}

Status Sync(void* context, const char* utf8, std::size_t size, int32_t cursor) {
  return Record(static_cast<Probe*>(context),
                {"sync", 0, std::string(utf8, size), false, cursor});
}

Status Pass(void* context, int64_t handle, const char* utf8, std::size_t size,
            bool finished, int32_t cursor) {
  return Record(static_cast<Probe*>(context),
                {"pass", handle, std::string(utf8, size), finished, cursor});
}

Status Return(void* context, int64_t handle) {
  return Record(static_cast<Probe*>(context), {"return", handle, {}, false, 0});
}

Status Release(void* context, int64_t handle) {
  return Record(static_cast<Probe*>(context),
                {"release", handle, {}, false, 0});
}

RobloxTextSink Sink(Probe* probe) {
  return {probe, Sync, Pass, Return, Release};
}

RobloxTextDisplaySink DisplaySink(DisplayProbe* probe) {
  return {probe, DisplayUpdate};
}

platform::KeyEvent Key(SDL_Scancode scancode, bool pressed = true,
                       uint32_t modifiers = 0) {
  platform::KeyEvent event;
  event.pressed = pressed;
  event.scancode = static_cast<uint32_t>(scancode);
  event.modifiers = modifiers;
  return event;
}

platform::KeyEvent Shortcut(SDL_Scancode scancode, bool pressed = true) {
  return Key(scancode, pressed, static_cast<uint32_t>(SDL_KMOD_CTRL));
}

RobloxTextFocusSession Session(int64_t handle = 42, uint64_t generation = 7,
                               std::string text = {}) {
  return {handle, generation, std::move(text), false, false};
}

TEST(RobloxTextEditorTest, RejectsInvalidSessionAndInvalidUtf8) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  EXPECT_FALSE(editor.BeginFocusSession(Session(0)).ok());
  EXPECT_FALSE(
      editor.BeginFocusSession(Session(42, 7, std::string("\xff", 1))).ok());
  EXPECT_FALSE(editor.Snapshot().focused);
}

TEST(RobloxTextEditorTest, CommitsUtf8WithJavaUtf16CursorAndApkOrder) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, "🙂")).ok());

  const RobloxTextEditResult result =
      editor.HandleTextInput(platform::TextInputEvent{"é"});
  ASSERT_TRUE(result.status.ok());
  EXPECT_TRUE(result.dispatched);
  ASSERT_EQ(probe.calls.size(), 2u);
  EXPECT_EQ(probe.calls[0].operation, "sync");
  EXPECT_EQ(probe.calls[0].cursor, 3);
  EXPECT_EQ(probe.calls[1].operation, "pass");
  EXPECT_FALSE(probe.calls[1].finished);
  EXPECT_EQ(probe.calls[1].cursor, 3);
  EXPECT_EQ(editor.Snapshot().text_bytes, std::string("🙂é").size());
}

TEST(RobloxTextEditorTest, StreamsAndReplacesPreeditUntilCommitArrives) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, "prefix ")).ok());

  const auto first = editor.HandleTextEditing({"п", 1, 0});
  ASSERT_TRUE(first.status.ok());
  EXPECT_TRUE(first.dispatched);
  ASSERT_EQ(probe.calls.size(), 2u);
  EXPECT_EQ(probe.calls[0].text, "prefix п");
  EXPECT_EQ(probe.calls[0].cursor, 8);
  EXPECT_EQ(probe.calls[1].text, "prefix п");
  EXPECT_FALSE(probe.calls[1].finished);

  const auto second = editor.HandleTextEditing({"при", 3, 0});
  ASSERT_TRUE(second.status.ok());
  ASSERT_EQ(probe.calls.size(), 4u);
  EXPECT_EQ(probe.calls[2].text, "prefix при");
  EXPECT_EQ(probe.calls[2].cursor, 10);

  const auto committed = editor.HandleTextInput({"при"});
  ASSERT_TRUE(committed.status.ok());
  ASSERT_EQ(probe.calls.size(), 6u);
  EXPECT_EQ(probe.calls[4].text, "prefix при");
  EXPECT_EQ(probe.calls[5].text, "prefix при");
  EXPECT_EQ(editor.Snapshot().text_bytes, std::string("prefix при").size());
}

TEST(RobloxTextEditorTest, EmptyPreeditCancelsVisibleComposition) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, "base")).ok());
  ASSERT_TRUE(editor.HandleTextEditing({"候補", 2, 0}).status.ok());
  ASSERT_TRUE(editor.HandleTextEditing({"", 0, 0}).status.ok());

  ASSERT_EQ(probe.calls.size(), 4u);
  EXPECT_EQ(probe.calls[2].text, "base");
  EXPECT_EQ(probe.calls[3].text, "base");
  EXPECT_EQ(editor.Snapshot().text_bytes, 4u);
}

TEST(RobloxTextEditorTest, PreeditCursorConvertsUtf8CharactersToUtf16Units) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, "x")).ok());

  ASSERT_TRUE(editor.HandleTextEditing({"😀é", 1, 1}).status.ok());

  ASSERT_EQ(probe.calls.size(), 2u);
  EXPECT_EQ(probe.calls[0].text, "x😀é");
  EXPECT_EQ(probe.calls[0].cursor, 3);
  EXPECT_EQ(probe.calls[1].cursor, 3);
}

TEST(RobloxTextEditorTest, EngineEchoDoesNotBreakActiveComposition) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, "a")).ok());
  ASSERT_TRUE(editor.HandleTextEditing({"б", 1, 0}).status.ok());
  ASSERT_TRUE(editor.ReplaceFocusedTextFromEngine(7, "aб").ok());
  ASSERT_TRUE(editor.HandleTextEditing({"бы", 2, 0}).status.ok());

  ASSERT_EQ(probe.calls.size(), 4u);
  EXPECT_EQ(probe.calls[2].text, "aбы");
  EXPECT_EQ(probe.calls[3].text, "aбы");
}

TEST(RobloxTextEditorTest, DelayedOlderEngineEchoCannotDuplicateImeCommit) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, "a")).ok());
  ASSERT_TRUE(editor.HandleTextEditing({"б", 1, 0}).status.ok());
  ASSERT_TRUE(editor.HandleTextEditing({"бы", 2, 0}).status.ok());

  ASSERT_TRUE(editor.ReplaceFocusedTextFromEngine(7, "aб").ok());
  ASSERT_TRUE(editor.HandleTextInput({"бы"}).status.ok());

  ASSERT_EQ(probe.calls.size(), 6u);
  EXPECT_EQ(probe.calls[4].text, "aбы");
  EXPECT_EQ(probe.calls[5].text, "aбы");
  EXPECT_EQ(editor.Snapshot().text_bytes, std::string("aбы").size());
}

TEST(RobloxTextEditorTest, DelayedHostEchoCannotRollBackUnknownEngineUpdate) {
  Probe probe;
  DisplayProbe display_probe;
  RobloxTextEditor editor(Sink(&probe), DisplaySink(&display_probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, "a")).ok());
  ASSERT_TRUE(editor.HandleTextInput({"b"}).status.ok());

  ASSERT_TRUE(editor.ReplaceFocusedTextFromEngine(7, "server").ok());
  ASSERT_TRUE(editor.ReplaceFocusedTextFromEngine(7, "ab").ok());

  EXPECT_EQ(editor.Snapshot().text_bytes, 6u);
  ASSERT_GE(display_probe.calls.size(), 3u);
  EXPECT_EQ(display_probe.calls.back().text, "server");
}

TEST(RobloxTextEditorTest, OutOfOrderHostEchoesCannotRewindCurrentText) {
  Probe probe;
  DisplayProbe display_probe;
  RobloxTextEditor editor(Sink(&probe), DisplaySink(&display_probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session()).ok());
  ASSERT_TRUE(editor.HandleTextInput({"a"}).status.ok());
  ASSERT_TRUE(editor.HandleTextInput({"b"}).status.ok());

  ASSERT_TRUE(editor.ReplaceFocusedTextFromEngine(7, "ab").ok());
  ASSERT_TRUE(editor.ReplaceFocusedTextFromEngine(7, "a").ok());

  EXPECT_EQ(editor.Snapshot().text_bytes, 2u);
  EXPECT_EQ(display_probe.calls.back().text, "ab");
}

TEST(RobloxTextEditorTest, UnknownEngineUpdateCancelsActiveComposition) {
  Probe probe;
  DisplayProbe display_probe;
  RobloxTextEditor editor(Sink(&probe), DisplaySink(&display_probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, "a")).ok());
  ASSERT_TRUE(editor.HandleTextEditing({"б", 1, 0}).status.ok());

  ASSERT_TRUE(editor.ReplaceFocusedTextFromEngine(7, "server").ok());
  ASSERT_TRUE(editor.HandleTextInput({"c"}).status.ok());

  EXPECT_EQ(editor.Snapshot().text_bytes, 7u);
  EXPECT_EQ(display_probe.calls.back().text, "serverc");
}

TEST(RobloxTextEditorTest, NewFocusGenerationClearsEchoTombstones) {
  Probe probe;
  DisplayProbe display_probe;
  RobloxTextEditor editor(Sink(&probe), DisplaySink(&display_probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, "a")).ok());
  ASSERT_TRUE(editor.HandleTextInput({"b"}).status.ok());
  ASSERT_TRUE(editor.EndFocusSession(42, 7, false).ok());
  ASSERT_TRUE(editor.BeginFocusSession(Session(43, 8, "server")).ok());

  ASSERT_TRUE(editor.ReplaceFocusedTextFromEngine(8, "ab").ok());

  EXPECT_EQ(editor.Snapshot().text_bytes, 2u);
  EXPECT_EQ(display_probe.calls.back().text, "ab");
}

TEST(RobloxTextEditorTest, ImeOwnsEnterUntilCompositionCommits) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session()).ok());
  ASSERT_TRUE(editor.HandleTextEditing({"候補", 2, 0}).status.ok());
  const auto enter = editor.HandleKey(Key(SDL_SCANCODE_RETURN));

  EXPECT_TRUE(enter.handled);
  EXPECT_FALSE(enter.dispatched);
  EXPECT_FALSE(enter.session_ended);
  EXPECT_TRUE(editor.Snapshot().focused);
  ASSERT_EQ(probe.calls.size(), 2u);

  ASSERT_TRUE(editor.HandleTextInput({"候補"}).status.ok());
  const auto finish = editor.HandleKey(Key(SDL_SCANCODE_RETURN));
  EXPECT_TRUE(finish.session_ended);
  EXPECT_FALSE(editor.Snapshot().focused);
}

TEST(RobloxTextEditorTest, ImeOwnsEscapeUntilCompositionCancels) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session()).ok());
  ASSERT_TRUE(editor.HandleTextEditing({"候補", 2, 0}).status.ok());

  const auto escape = editor.HandleKey(Key(SDL_SCANCODE_ESCAPE));

  EXPECT_TRUE(escape.handled);
  EXPECT_FALSE(escape.dispatched);
  EXPECT_FALSE(escape.session_ended);
  EXPECT_TRUE(editor.Snapshot().focused);
  ASSERT_EQ(probe.calls.size(), 2u);
  ASSERT_TRUE(editor.HandleTextEditing({"", 0, 0}).status.ok());
  EXPECT_TRUE(editor.Snapshot().focused);
  EXPECT_EQ(editor.Snapshot().text_bytes, 0u);
}

TEST(RobloxTextEditorTest, BackspaceAndDeleteRespectGraphemeClusters) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  const std::string family = "👨‍👩‍👧‍👦";
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, family + "é")).ok());

  ASSERT_TRUE(editor.HandleKey(Key(SDL_SCANCODE_BACKSPACE)).status.ok());
  EXPECT_EQ(probe.calls.back().text, family);
  ASSERT_TRUE(editor.HandleKey(Key(SDL_SCANCODE_BACKSPACE)).status.ok());
  EXPECT_TRUE(probe.calls.back().text.empty());

  ASSERT_TRUE(editor.HandleTextInput({family + "x"}).status.ok());
  ASSERT_TRUE(editor.HandleKey(Key(SDL_SCANCODE_HOME)).status.ok());
  ASSERT_TRUE(editor.HandleKey(Key(SDL_SCANCODE_DELETE)).status.ok());
  EXPECT_EQ(probe.calls.back().text, "x");
}

TEST(RobloxTextEditorTest, CursorMovementSyncsWithoutPassText) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, "a🙂b")).ok());
  ASSERT_TRUE(editor.HandleKey(Key(SDL_SCANCODE_LEFT)).status.ok());
  ASSERT_EQ(probe.calls.size(), 1u);
  EXPECT_EQ(probe.calls[0].operation, "sync");
  EXPECT_EQ(probe.calls[0].cursor, 3);

  const auto release = editor.HandleKey(Key(SDL_SCANCODE_LEFT, false));
  EXPECT_TRUE(release.handled);
  EXPECT_FALSE(release.dispatched);
  EXPECT_EQ(probe.calls.size(), 1u);
}

TEST(RobloxTextEditorTest, CtrlVReplacesSelectionWithUtf8ClipboardText) {
  Probe probe;
  DisplayProbe display_probe;
  ClipboardProbe clipboard;
  clipboard.read_text = u8"вставка🙂";
  RobloxTextEditor editor(Sink(&probe), DisplaySink(&display_probe),
                          &clipboard);
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, "replace me")).ok());

  const auto select_all = editor.HandleKey(Shortcut(SDL_SCANCODE_A));
  EXPECT_TRUE(select_all.handled);
  EXPECT_FALSE(select_all.dispatched);
  EXPECT_EQ(editor.Snapshot().selection_begin_utf16, 0);
  EXPECT_EQ(editor.Snapshot().selection_end_utf16, 10);
  ASSERT_EQ(display_probe.calls.size(), 2u);
  EXPECT_EQ(display_probe.calls.back().selection_begin_utf16, 0);
  EXPECT_EQ(display_probe.calls.back().selection_end_utf16, 10);

  const auto paste = editor.HandleKey(Shortcut(SDL_SCANCODE_V));
  ASSERT_TRUE(paste.status.ok());
  EXPECT_TRUE(paste.handled);
  EXPECT_TRUE(paste.dispatched);
  EXPECT_EQ(clipboard.read_calls, 1);
  ASSERT_EQ(probe.calls.size(), 2u);
  EXPECT_EQ(probe.calls[0].text, clipboard.read_text);
  EXPECT_EQ(probe.calls[1].text, clipboard.read_text);
  EXPECT_EQ(editor.Snapshot().selection_begin_utf16,
            editor.Snapshot().cursor_utf16);
  EXPECT_EQ(editor.Snapshot().selection_end_utf16,
            editor.Snapshot().cursor_utf16);

  const auto key_up = editor.HandleKey(Key(SDL_SCANCODE_V, false));
  EXPECT_TRUE(key_up.handled);
  EXPECT_FALSE(key_up.dispatched);
}

TEST(RobloxTextEditorTest,
     ShiftSelectionRespectsGraphemesAndCommittedTextReplacesIt) {
  Probe probe;
  DisplayProbe display_probe;
  RobloxTextEditor editor(Sink(&probe), DisplaySink(&display_probe));
  const std::string family = u8"👨‍👩‍👧‍👦";
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, family + "x")).ok());

  const uint32_t shift = static_cast<uint32_t>(SDL_KMOD_SHIFT);
  ASSERT_TRUE(
      editor.HandleKey(Key(SDL_SCANCODE_LEFT, true, shift)).status.ok());
  ASSERT_TRUE(
      editor.HandleKey(Key(SDL_SCANCODE_LEFT, true, shift)).status.ok());
  EXPECT_EQ(editor.Snapshot().selection_begin_utf16, 0);
  EXPECT_EQ(editor.Snapshot().selection_end_utf16, 12);
  EXPECT_EQ(editor.Snapshot().cursor_utf16, 0);
  ASSERT_FALSE(display_probe.calls.empty());
  EXPECT_EQ(display_probe.calls.back().selection_begin_utf16, 0);

  ASSERT_TRUE(editor.HandleTextInput({"ok"}).status.ok());
  ASSERT_GE(probe.calls.size(), 4u);
  EXPECT_EQ(probe.calls[probe.calls.size() - 2].text, "ok");
  EXPECT_EQ(probe.calls.back().text, "ok");
  EXPECT_EQ(editor.Snapshot().text_bytes, 2u);
  EXPECT_EQ(editor.Snapshot().selection_begin_utf16, 2);
  EXPECT_EQ(editor.Snapshot().selection_end_utf16, 2);
}

TEST(RobloxTextEditorTest, CancelledImeRestoresTheSelectedTextAndRange) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, u8"old🙂")).ok());
  ASSERT_TRUE(editor.HandleKey(Shortcut(SDL_SCANCODE_A)).status.ok());

  ASSERT_TRUE(editor.HandleTextEditing({u8"нов", 3, 0}).status.ok());
  ASSERT_TRUE(editor.HandleTextEditing({"", 0, 0}).status.ok());

  ASSERT_EQ(probe.calls.size(), 4u);
  EXPECT_EQ(probe.calls[0].text, u8"нов");
  EXPECT_EQ(probe.calls[2].text, u8"old🙂");
  EXPECT_EQ(probe.calls[3].text, u8"old🙂");
  EXPECT_EQ(editor.Snapshot().selection_begin_utf16, 0);
  EXPECT_EQ(editor.Snapshot().selection_end_utf16, 5);
  EXPECT_EQ(editor.Snapshot().cursor_utf16, 5);
}

TEST(RobloxTextEditorTest, CtrlCopyAndCutUseTheSelectedUtf8Range) {
  Probe probe;
  ClipboardProbe clipboard;
  RobloxTextEditor editor(Sink(&probe), {}, &clipboard);
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, u8"copy🙂")).ok());
  ASSERT_TRUE(editor.HandleKey(Shortcut(SDL_SCANCODE_A)).status.ok());

  const auto copy = editor.HandleKey(Shortcut(SDL_SCANCODE_C));
  EXPECT_TRUE(copy.handled);
  EXPECT_FALSE(copy.dispatched);
  EXPECT_EQ(clipboard.write_calls, 1);
  EXPECT_EQ(clipboard.written_text, u8"copy🙂");
  EXPECT_TRUE(probe.calls.empty());

  const auto cut = editor.HandleKey(Shortcut(SDL_SCANCODE_X));
  ASSERT_TRUE(cut.status.ok());
  EXPECT_TRUE(cut.dispatched);
  EXPECT_EQ(clipboard.write_calls, 2);
  EXPECT_EQ(clipboard.written_text, u8"copy🙂");
  ASSERT_EQ(probe.calls.size(), 2u);
  EXPECT_TRUE(probe.calls.back().text.empty());
  EXPECT_EQ(editor.Snapshot().text_bytes, 0u);
}

TEST(RobloxTextEditorTest, PasswordCopyAndClipboardFailuresStayFailClosed) {
  Probe probe;
  ClipboardProbe clipboard;
  clipboard.fail_read = true;
  RobloxTextEditor editor(Sink(&probe), {}, &clipboard);
  RobloxTextFocusSession session = Session(42, 7, "secret");
  session.text_input_type = 5;
  ASSERT_TRUE(editor.BeginFocusSession(std::move(session)).ok());
  ASSERT_TRUE(editor.HandleKey(Shortcut(SDL_SCANCODE_A)).status.ok());

  EXPECT_TRUE(editor.HandleKey(Shortcut(SDL_SCANCODE_C)).status.ok());
  EXPECT_EQ(clipboard.write_calls, 0);
  EXPECT_TRUE(editor.HandleKey(Shortcut(SDL_SCANCODE_V)).status.ok());
  EXPECT_EQ(clipboard.read_calls, 1);
  EXPECT_TRUE(editor.Snapshot().focused);
  EXPECT_EQ(editor.Snapshot().native_errors, 0u);
  EXPECT_TRUE(probe.calls.empty());

  const uint32_t alt_gr = static_cast<uint32_t>(SDL_KMOD_CTRL) |
                          static_cast<uint32_t>(SDL_KMOD_ALT);
  EXPECT_FALSE(editor.HandleKey(Key(SDL_SCANCODE_V, true, alt_gr)).handled);
}

TEST(RobloxTextEditorTest, EnterMatchesSingleLineApkOrdering) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(99, 2, "hello")).ok());
  const auto result = editor.HandleKey(Key(SDL_SCANCODE_RETURN));
  ASSERT_TRUE(result.status.ok());
  EXPECT_TRUE(result.session_ended);
  ASSERT_EQ(probe.calls.size(), 3u);
  EXPECT_EQ(probe.calls[0].operation, "sync");
  EXPECT_EQ(probe.calls[1].operation, "return");
  EXPECT_EQ(probe.calls[2].operation, "pass");
  EXPECT_TRUE(probe.calls[2].finished);
  EXPECT_FALSE(editor.Snapshot().focused);
}

TEST(RobloxTextEditorTest, ManualReleaseEnterKeepsSessionAfterReturn) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  RobloxTextFocusSession session = Session();
  session.manual_focus_release = true;
  ASSERT_TRUE(editor.BeginFocusSession(std::move(session)).ok());
  const auto result = editor.HandleKey(Key(SDL_SCANCODE_RETURN));
  ASSERT_TRUE(result.status.ok());
  EXPECT_FALSE(result.session_ended);
  ASSERT_EQ(probe.calls.size(), 2u);
  EXPECT_EQ(probe.calls[0].operation, "sync");
  EXPECT_EQ(probe.calls[1].operation, "return");
  EXPECT_TRUE(editor.Snapshot().focused);
}

TEST(RobloxTextEditorTest, MultilineEnterCommitsNewline) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  RobloxTextFocusSession session = Session(42, 7, "line");
  session.multiline = true;
  ASSERT_TRUE(editor.BeginFocusSession(std::move(session)).ok());
  const auto result = editor.HandleKey(Key(SDL_SCANCODE_KP_ENTER));
  ASSERT_TRUE(result.status.ok());
  EXPECT_FALSE(result.session_ended);
  ASSERT_EQ(probe.calls.size(), 2u);
  EXPECT_EQ(probe.calls[1].text, "line\n");
  EXPECT_FALSE(probe.calls[1].finished);
}

TEST(RobloxTextEditorTest,
     EscapeReleasesFocusAndStaleHideCannotEndReplacement) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7)).ok());
  ASSERT_TRUE(editor.HandleKey(Key(SDL_SCANCODE_ESCAPE)).status.ok());
  ASSERT_EQ(probe.calls.size(), 1u);
  EXPECT_EQ(probe.calls[0].operation, "release");

  ASSERT_TRUE(editor.BeginFocusSession(Session(43, 8)).ok());
  EXPECT_FALSE(editor.EndFocusSession(42, 7, false).ok());
  EXPECT_TRUE(editor.Snapshot().focused);
  EXPECT_EQ(editor.Snapshot().textbox_handle, 43);
}

TEST(RobloxTextEditorTest, NewEngineFocusTransactionallyReplacesOldSession) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, "secret-one")).ok());
  ASSERT_TRUE(editor.BeginFocusSession(Session(43, 8, "secret-two")).ok());
  EXPECT_TRUE(probe.calls.empty());
  EXPECT_EQ(editor.Snapshot().textbox_handle, 43);
  EXPECT_EQ(editor.Snapshot().text_bytes, 10u);
}

TEST(RobloxTextEditorTest, EngineReplacementIsAuthoritativeAndNoReentrantCall) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, "old-secret")).ok());
  ASSERT_TRUE(editor.ReplaceFocusedTextFromEngine(7, "🙂new").ok());
  EXPECT_TRUE(probe.calls.empty());
  EXPECT_EQ(editor.Snapshot().text_bytes, std::string("🙂new").size());
  EXPECT_EQ(editor.Snapshot().cursor_utf16, 5);
  EXPECT_FALSE(editor.ReplaceFocusedTextFromEngine(6, "stale").ok());
  EXPECT_EQ(editor.Snapshot().text_bytes, std::string("🙂new").size());
}

TEST(RobloxTextEditorTest,
     PropertyRefreshKeepsGenerationAndTextWhileUpdatingLayout) {
  Probe native_probe;
  DisplayProbe display_probe;
  RobloxTextEditor editor(Sink(&native_probe), DisplaySink(&display_probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 7, "unchanged")).ok());

  RobloxTextFocusProperties properties;
  properties.manual_focus_release = true;
  properties.multiline = true;
  properties.area_x = 69;
  properties.area_y = 36;
  properties.area_width = 633;
  properties.area_height = 36;
  properties.font_size = 16.0F;
  properties.x_alignment = 2;
  properties.y_alignment = 1;
  properties.text_color = static_cast<int32_t>(0xffd4d8e1U);
  properties.font = 4;
  properties.text_input_type = 3;
  properties.return_key_type = 2;
  properties.text_wrapped = true;

  ASSERT_TRUE(editor.UpdateFocusProperties(7, properties).ok());
  ASSERT_EQ(display_probe.calls.size(), 2u);
  const DisplayCall& refresh = display_probe.calls.back();
  EXPECT_EQ(refresh.event, RobloxTextDisplayEvent::kUpdate);
  EXPECT_EQ(refresh.generation, 7u);
  EXPECT_EQ(refresh.area_x, 69);
  EXPECT_EQ(refresh.area_y, 36);
  EXPECT_EQ(refresh.area_width, 633);
  EXPECT_EQ(refresh.area_height, 36);
  EXPECT_FLOAT_EQ(refresh.font_size, 16.0F);
  EXPECT_TRUE(refresh.multiline);
  EXPECT_EQ(refresh.font, 4);
  EXPECT_EQ(refresh.text_input_type, 3);
  EXPECT_TRUE(refresh.text_wrapped);
  EXPECT_EQ(refresh.text, "unchanged");
  EXPECT_EQ(refresh.cursor_utf16, 9);
  EXPECT_EQ(editor.Snapshot().generation, 7u);
  EXPECT_EQ(editor.Snapshot().text_bytes, 9u);
  EXPECT_FALSE(editor.UpdateFocusProperties(6, properties).ok());
}

TEST(RobloxTextEditorTest,
     PublishesCompleteDisplayLifecycleWithoutLoggingSeam) {
  Probe native_probe;
  DisplayProbe display_probe;
  RobloxTextEditor editor(Sink(&native_probe), DisplaySink(&display_probe));
  RobloxTextFocusSession session = Session(42, 17, "initial");
  session.area_x = 12;
  session.area_y = 34;
  session.area_width = 280;
  session.area_height = 44;
  session.font_size = 22.0F;
  session.font = 13;
  session.multiline = true;
  session.text_wrapped = true;
  session.text_input_type = 5;

  ASSERT_TRUE(editor.BeginFocusSession(std::move(session)).ok());
  ASSERT_EQ(display_probe.calls.size(), 1u);
  EXPECT_EQ(display_probe.calls[0].event, RobloxTextDisplayEvent::kShow);
  EXPECT_EQ(display_probe.calls[0].generation, 17u);
  EXPECT_EQ(display_probe.calls[0].area_x, 12);
  EXPECT_EQ(display_probe.calls[0].area_y, 34);
  EXPECT_EQ(display_probe.calls[0].area_width, 280);
  EXPECT_EQ(display_probe.calls[0].area_height, 44);
  EXPECT_FLOAT_EQ(display_probe.calls[0].font_size, 22.0F);
  EXPECT_TRUE(display_probe.calls[0].multiline);
  EXPECT_EQ(display_probe.calls[0].font, 13);
  EXPECT_EQ(display_probe.calls[0].text_input_type, 5);
  EXPECT_TRUE(display_probe.calls[0].text_wrapped);
  EXPECT_EQ(display_probe.calls[0].text, "initial");
  EXPECT_EQ(display_probe.calls[0].cursor_utf16, 7);

  ASSERT_TRUE(editor.HandleTextInput({"🙂"}).status.ok());
  ASSERT_EQ(display_probe.calls.size(), 2u);
  EXPECT_EQ(display_probe.calls[1].event, RobloxTextDisplayEvent::kUpdate);
  EXPECT_EQ(display_probe.calls[1].text, "initial🙂");
  EXPECT_EQ(display_probe.calls[1].cursor_utf16, 9);

  ASSERT_TRUE(editor.ReplaceFocusedTextFromEngine(17, "engine").ok());
  ASSERT_EQ(display_probe.calls.size(), 3u);
  EXPECT_EQ(display_probe.calls[2].event, RobloxTextDisplayEvent::kUpdate);
  EXPECT_EQ(display_probe.calls[2].text, "engine");
  EXPECT_EQ(display_probe.calls[2].cursor_utf16, 6);

  ASSERT_TRUE(editor.EndFocusSession(42, 17, false).ok());
  ASSERT_EQ(display_probe.calls.size(), 4u);
  EXPECT_EQ(display_probe.calls[3].event, RobloxTextDisplayEvent::kHide);
  EXPECT_EQ(display_probe.calls[3].generation, 17u);
  EXPECT_TRUE(display_probe.calls[3].text.empty());
}

TEST(RobloxTextEditorTest, DisplayObserverSeesCursorMovesAndNativeFailureHide) {
  Probe native_probe;
  DisplayProbe display_probe;
  RobloxTextEditor editor(Sink(&native_probe), DisplaySink(&display_probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session(42, 9, "a🙂b")).ok());

  ASSERT_TRUE(editor.HandleKey(Key(SDL_SCANCODE_LEFT)).status.ok());
  ASSERT_EQ(display_probe.calls.size(), 2u);
  EXPECT_EQ(display_probe.calls.back().event, RobloxTextDisplayEvent::kUpdate);
  EXPECT_EQ(display_probe.calls.back().cursor_utf16, 3);

  native_probe.fail_next = true;
  EXPECT_FALSE(editor.HandleTextInput({"x"}).status.ok());
  ASSERT_EQ(display_probe.calls.size(), 3u);
  EXPECT_EQ(display_probe.calls.back().event, RobloxTextDisplayEvent::kHide);
  EXPECT_FALSE(editor.Snapshot().focused);
}

TEST(RobloxTextEditorTest, NativeFailureClosesSessionFailClosed) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session()).ok());
  probe.fail_next = true;
  const auto result = editor.HandleTextInput({"x"});
  EXPECT_FALSE(result.status.ok());
  EXPECT_TRUE(result.session_ended);
  EXPECT_FALSE(editor.Snapshot().focused);
  EXPECT_EQ(editor.Snapshot().native_errors, 1u);
  ASSERT_EQ(probe.calls.size(), 2u);
  EXPECT_EQ(probe.calls[1].operation, "release");
}

TEST(RobloxTextEditorTest, ConcurrentSnapshotAndPreeditAreSafe) {
  Probe probe;
  RobloxTextEditor editor(Sink(&probe));
  ASSERT_TRUE(editor.BeginFocusSession(Session()).ok());
  std::vector<std::thread> threads;
  for (int index = 0; index < 8; ++index) {
    threads.emplace_back([&editor]() {
      for (int iteration = 0; iteration < 100; ++iteration) {
        editor.HandleTextEditing({"preedit", 0, 1});
        EXPECT_TRUE(editor.Snapshot().focused);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(probe.calls.size(), 1600u);
  EXPECT_EQ(editor.Snapshot().text_bytes, 7u);
  EXPECT_TRUE(editor.Snapshot().focused);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
