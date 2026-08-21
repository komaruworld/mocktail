#ifndef MOCKTAIL_WINDOW_WINDOW_TEXT_INPUT_OWNER_H_
#define MOCKTAIL_WINDOW_WINDOW_TEXT_INPUT_OWNER_H_

#include <atomic>
#include <cstdint>
#include <mutex>

namespace mocktail {
namespace window {

struct TextInputArea {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int cursor = 0;

  bool valid() const { return width > 0 && height > 0; }
};

struct TextInputOptions {
  // Roblox NativeTextBoxInfo.textInputType semantic enum (currently 1..10).
  int roblox_text_input_type = 0;
  bool multiline = false;
};

// Main-thread-only host boundary. Tests inject a deterministic implementation;
// production delegates each call to the corresponding SDL3 API.
class TextInputBackend {
 public:
  virtual ~TextInputBackend() = default;

  virtual bool SetArea(const TextInputArea& area) = 0;
  virtual bool ClearArea() = 0;
  virtual bool Start(const TextInputOptions& options) = 0;
  virtual bool Stop() = 0;
  virtual void FlushTextEvents() = 0;
};

// Owns the cross-thread handoff from guest callbacks to SDL's main thread.
// Only Request* may be called off the main thread. All other methods must be
// called by the window event owner.
class WindowTextInputOwner final {
 public:
  explicit WindowTextInputOwner(TextInputBackend* backend);
  ~WindowTextInputOwner();

  WindowTextInputOwner(const WindowTextInputOwner&) = delete;
  WindowTextInputOwner& operator=(const WindowTextInputOwner&) = delete;

  void SetEnabled(bool enabled);
  bool RequestShowTextInput(uint64_t generation, const TextInputArea& area,
                            const TextInputOptions& options);
  bool RequestHideTextInput(uint64_t generation);

  // Drains the latest guest command. Replaced commands are intentionally
  // coalesced: SDL only needs the most recent desired text-input state.
  bool Pump();
  bool OnViewportChanged();
  void OnFocusGained();
  void OnFocusLost();
  void Shutdown();

  bool active() const { return active_; }
  uint64_t active_generation() const { return active_generation_; }

 private:
  enum class CommandType { kShow, kHide };

  struct Command {
    CommandType type = CommandType::kHide;
    uint64_t generation = 0;
    TextInputArea area;
    TextInputOptions options;
  };

  bool StopAndClear();

  TextInputBackend* backend_ = nullptr;
  std::atomic<bool> enabled_{false};
  std::mutex mutex_;
  bool has_pending_command_ = false;
  Command pending_command_;
  uint64_t accepted_generation_ = 0;

  // Main-thread-owned state below this line.
  bool active_ = false;
  uint64_t active_generation_ = 0;
  TextInputArea active_area_;
  bool focused_ = true;
};

// SDL3 implementation. The opaque window is an SDL_Window* in production.
class SdlTextInputBackend final : public TextInputBackend {
 public:
  explicit SdlTextInputBackend(void* window) : window_(window) {}

  bool SetArea(const TextInputArea& area) override;
  bool ClearArea() override;
  bool Start(const TextInputOptions& options) override;
  bool Stop() override;
  void FlushTextEvents() override;

 private:
  void* window_ = nullptr;
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_WINDOW_TEXT_INPUT_OWNER_H_
