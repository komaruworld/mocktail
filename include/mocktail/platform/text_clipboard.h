#ifndef MOCKTAIL_PLATFORM_TEXT_CLIPBOARD_H_
#define MOCKTAIL_PLATFORM_TEXT_CLIPBOARD_H_

#include <memory>
#include <string>

#include "mocktail/status.h"

namespace mocktail {
namespace platform {

// Main-thread platform boundary for UTF-8 clipboard text. Implementations and
// consumers must not log clipboard contents.
class TextClipboard {
 public:
  virtual ~TextClipboard() = default;

  virtual Status ReadText(std::string* text) = 0;
  virtual Status WriteText(const std::string& text) = 0;
};

// Creates the SDL3 clipboard adapter used by both XWayland and native Wayland.
std::unique_ptr<TextClipboard> CreateSdlTextClipboard();

}  // namespace platform
}  // namespace mocktail

#endif  // MOCKTAIL_PLATFORM_TEXT_CLIPBOARD_H_
