// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/mus/ws/display_manager.h"

#include "base/memory/ptr_util.h"
#include "components/mus/ws/display.h"
#include "components/mus/ws/display_manager_delegate.h"
#include "components/mus/ws/server_window.h"
#include "components/mus/ws/user_display_manager.h"

namespace mus {
namespace ws {

DisplayManager::DisplayManager(DisplayManagerDelegate* delegate)
    // |next_root_id_| is used as the lower bits, so that starting at 0 is
    // fine. |next_display_id_| is used by itself, so we start at 1 to reserve
    // 0 as invalid.
    : delegate_(delegate),
      next_root_id_(0),
      next_display_id_(1) {}

DisplayManager::~DisplayManager() {
  DestroyAllDisplays();
}

UserDisplayManager* DisplayManager::GetUserDisplayManager(
    const UserId& user_id) {
  if (!user_display_managers_.count(user_id)) {
    user_display_managers_[user_id] =
        base::WrapUnique(new UserDisplayManager(this, user_id));
  }
  return user_display_managers_[user_id].get();
}

void DisplayManager::AddDisplay(Display* display) {
  DCHECK_EQ(0u, pending_displays_.count(display));
  pending_displays_.insert(display);
}

void DisplayManager::DestroyDisplay(Display* display) {
  if (pending_displays_.count(display)) {
    pending_displays_.erase(display);
  } else {
    for (const auto& pair : user_display_managers_)
      pair.second->OnWillDestroyDisplay(display);

    DCHECK(displays_.count(display));
    displays_.erase(display);
  }
  delete display;

  // If we have no more roots left, let the app know so it can terminate.
  // TODO(sky): move to delegate/observer.
  if (!displays_.size() && !pending_displays_.size())
    delegate_->OnNoMoreDisplays();
}

void DisplayManager::DestroyAllDisplays() {
  while (!pending_displays_.empty())
    DestroyDisplay(*pending_displays_.begin());
  DCHECK(pending_displays_.empty());

  while (!displays_.empty())
    DestroyDisplay(*displays_.begin());
  DCHECK(displays_.empty());
}

std::set<const Display*> DisplayManager::displays() const {
  std::set<const Display*> ret_value(displays_.begin(), displays_.end());
  return ret_value;
}

Display* DisplayManager::GetDisplayContaining(ServerWindow* window) {
  return const_cast<Display*>(
      static_cast<const DisplayManager*>(this)->GetDisplayContaining(window));
}

const Display* DisplayManager::GetDisplayContaining(
    const ServerWindow* window) const {
  while (window && window->parent())
    window = window->parent();
  for (Display* display : displays_) {
    if (window == display->root_window())
      return display;
  }
  return nullptr;
}

WindowManagerAndDisplayConst DisplayManager::GetWindowManagerAndDisplay(
    const ServerWindow* window) const {
  const ServerWindow* last = window;
  while (window && window->parent()) {
    last = window;
    window = window->parent();
  }
  for (Display* display : displays_) {
    if (window == display->root_window()) {
      WindowManagerAndDisplayConst result;
      result.display = display;
      result.window_manager_state =
          display->GetWindowManagerStateWithRoot(last);
      return result;
    }
  }
  return WindowManagerAndDisplayConst();
}

WindowManagerAndDisplay DisplayManager::GetWindowManagerAndDisplay(
    const ServerWindow* window) {
  WindowManagerAndDisplayConst result_const =
      const_cast<const DisplayManager*>(this)->GetWindowManagerAndDisplay(
          window);
  WindowManagerAndDisplay result;
  result.display = const_cast<Display*>(result_const.display);
  result.window_manager_state =
      const_cast<WindowManagerState*>(result_const.window_manager_state);
  return result;
}

WindowId DisplayManager::GetAndAdvanceNextRootId() {
  // TODO(sky): handle wrapping!
  const uint16_t id = next_root_id_++;
  DCHECK_LT(id, next_root_id_);
  return RootWindowId(id);
}

uint32_t DisplayManager::GetAndAdvanceNextDisplayId() {
  // TODO(sky): handle wrapping!
  const uint32_t id = next_display_id_++;
  DCHECK_LT(id, next_display_id_);
  return id;
}

void DisplayManager::OnDisplayAcceleratedWidgetAvailable(Display* display) {
  DCHECK_NE(0u, pending_displays_.count(display));
  DCHECK_EQ(0u, displays_.count(display));
  const bool is_first_display = displays_.empty();
  displays_.insert(display);
  pending_displays_.erase(display);
  if (is_first_display)
    delegate_->OnFirstDisplayReady();
}

}  // namespace ws
}  // namespace mus
