#include "mocktail/platform/text_clipboard.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_stdinc.h>

#include <memory>
#include <string>
#include <utility>

namespace mocktail {
namespace platform {
namespace {

Status SdlClipboardError(const char* operation) {
  std::string message = operation != nullptr ? operation : "SDL clipboard";
  message += " failed: ";
  const char* error = SDL_GetError();
  message += error != nullptr && error[0] != '\0' ? error : "unknown SDL error";
  return Status::Error(StatusCode::kPlatformError, std::move(message));
}

class SdlTextClipboard final : public TextClipboard {
 public:
  Status ReadText(std::string* text) override {
    if (text == nullptr) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "clipboard text output is required");
    }
    text->clear();
    SDL_ClearError();
    char* clipboard_text = SDL_GetClipboardText();
    if (clipboard_text == nullptr) {
      return SdlClipboardError("SDL_GetClipboardText");
    }
    text->assign(clipboard_text);
    SDL_free(clipboard_text);
    return Status::Ok();
  }

  Status WriteText(const std::string& text) override {
    if (!SDL_SetClipboardText(text.c_str())) {
      return SdlClipboardError("SDL_SetClipboardText");
    }
    return Status::Ok();
  }
};

}  // namespace

std::unique_ptr<TextClipboard> CreateSdlTextClipboard() {
  return std::make_unique<SdlTextClipboard>();
}

}  // namespace platform
}  // namespace mocktail
