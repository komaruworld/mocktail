#include "window/window_text_input_owner.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <utility>

namespace mocktail {
namespace window {

namespace {

constexpr TextInputArea kSafeDefaultArea{0, 0, 1, 1, 0};

SDL_TextInputType MapRobloxTextInputType(int input_type) {
  // Mirrors the APK RbxKeyboard mapping without passing Android bitmasks into
  // SDL. Unknown/new values remain ordinary text and therefore fail safely.
  switch (input_type) {
    case 2:
    case 4:  // SDL has no phone type; numeric is the closest host contract.
    case 8:
      return SDL_TEXTINPUT_TYPE_NUMBER;
    case 3:
      return SDL_TEXTINPUT_TYPE_TEXT_EMAIL;
    case 5:
    case 9:
    case 10:
      return SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN;
    case 6:
      return SDL_TEXTINPUT_TYPE_TEXT_USERNAME;
    default:
      return SDL_TEXTINPUT_TYPE_TEXT;
  }
}

}  // namespace

WindowTextInputOwner::WindowTextInputOwner(TextInputBackend* backend)
    : backend_(backend) {}

WindowTextInputOwner::~WindowTextInputOwner() { Shutdown(); }

void WindowTextInputOwner::SetEnabled(bool enabled) {
  const bool was_enabled =
      enabled_.exchange(enabled, std::memory_order_acq_rel);
  if (enabled != was_enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    has_pending_command_ = false;
    accepted_generation_ = 0;
  }
  if (!enabled && was_enabled) {
    if (active_) {
      StopAndClear();
    } else if (backend_ != nullptr) {
      backend_->FlushTextEvents();
    }
  }
}

bool WindowTextInputOwner::RequestShowTextInput(
    uint64_t generation, const TextInputArea& area,
    const TextInputOptions& options) {
  if (!enabled_.load(std::memory_order_acquire) || generation == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_.load(std::memory_order_relaxed)) {
    return false;
  }
  if (generation < accepted_generation_) {
    return false;
  }
  accepted_generation_ = generation;
  pending_command_ = {CommandType::kShow, generation,
                      area.valid() ? area : kSafeDefaultArea, options};
  has_pending_command_ = true;
  return true;
}

bool WindowTextInputOwner::RequestHideTextInput(uint64_t generation) {
  if (!enabled_.load(std::memory_order_acquire) || generation == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_.load(std::memory_order_relaxed)) {
    return false;
  }
  if (generation != accepted_generation_) {
    return false;
  }
  pending_command_ = {CommandType::kHide, generation, {}, {}};
  has_pending_command_ = true;
  return true;
}

bool WindowTextInputOwner::Pump() {
  if (!enabled_.load(std::memory_order_acquire) || backend_ == nullptr) {
    return true;
  }

  Command command;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_pending_command_) {
      return true;
    }
    command = pending_command_;
    has_pending_command_ = false;
  }

  if (!focused_) {
    return true;
  }

  if (command.type == CommandType::kHide) {
    if (!active_ || active_generation_ != command.generation) {
      return true;
    }
    return StopAndClear();
  }

  if (active_ && active_generation_ != command.generation &&
      !StopAndClear()) {
    return false;
  }
  if (active_) {
    active_area_ = command.area;
    return backend_->SetArea(command.area);
  }
  backend_->FlushTextEvents();
  if (!backend_->SetArea(command.area)) {
    return false;
  }
  if (!backend_->Start(command.options)) {
    backend_->ClearArea();
    return false;
  }
  active_ = true;
  active_generation_ = command.generation;
  active_area_ = command.area;
  return true;
}

bool WindowTextInputOwner::OnViewportChanged() {
  if (!active_ || backend_ == nullptr) {
    return true;
  }
  return backend_->SetArea(active_area_);
}

void WindowTextInputOwner::OnFocusGained() { focused_ = true; }

void WindowTextInputOwner::OnFocusLost() {
  focused_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    has_pending_command_ = false;
  }
  if (active_) {
    StopAndClear();
  } else if (backend_ != nullptr) {
    backend_->FlushTextEvents();
  }
}

void WindowTextInputOwner::Shutdown() {
  enabled_.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    has_pending_command_ = false;
  }
  if (active_) {
    StopAndClear();
  } else if (backend_ != nullptr) {
    backend_->FlushTextEvents();
  }
}

bool WindowTextInputOwner::StopAndClear() {
  bool success = true;
  if (backend_ != nullptr) {
    success = backend_->Stop();
    success = backend_->ClearArea() && success;
    backend_->FlushTextEvents();
  }
  active_ = false;
  active_generation_ = 0;
  active_area_ = {};
  return success;
}

bool SdlTextInputBackend::SetArea(const TextInputArea& area) {
  SDL_Rect rect = {area.x, area.y, area.width, area.height};
  const int cursor = std::max(0, std::min(area.cursor, area.width));
  return window_ != nullptr && SDL_SetTextInputArea(
                                   static_cast<SDL_Window*>(window_), &rect,
                                   cursor);
}

bool SdlTextInputBackend::ClearArea() {
  return window_ != nullptr &&
         SDL_SetTextInputArea(static_cast<SDL_Window*>(window_), nullptr, 0);
}

bool SdlTextInputBackend::Start(const TextInputOptions& options) {
  if (window_ == nullptr) {
    return false;
  }
  const SDL_PropertiesID properties = SDL_CreateProperties();
  if (properties == 0) {
    return false;
  }
  bool success = SDL_SetNumberProperty(
      properties, SDL_PROP_TEXTINPUT_TYPE_NUMBER,
      static_cast<Sint64>(
          MapRobloxTextInputType(options.roblox_text_input_type)));
  success = SDL_SetBooleanProperty(properties,
                                   SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN,
                                   options.multiline) &&
            success;
  if (success) {
    success = SDL_StartTextInputWithProperties(
        static_cast<SDL_Window*>(window_), properties);
  }
  SDL_DestroyProperties(properties);
  return success;
}

bool SdlTextInputBackend::Stop() {
  return window_ != nullptr &&
         SDL_StopTextInput(static_cast<SDL_Window*>(window_));
}

void SdlTextInputBackend::FlushTextEvents() {
  SDL_FlushEvents(SDL_EVENT_TEXT_EDITING, SDL_EVENT_TEXT_INPUT);
  SDL_FlushEvent(SDL_EVENT_TEXT_EDITING_CANDIDATES);
}

}  // namespace window
}  // namespace mocktail
