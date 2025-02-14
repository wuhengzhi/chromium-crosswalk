// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_UI_FOCUS_RING_CONTROLLER_H_
#define CHROME_BROWSER_CHROMEOS_UI_FOCUS_RING_CONTROLLER_H_

#include <memory>

#include "base/macros.h"
#include "chrome/browser/chromeos/ui/focus_ring_layer.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/focus/widget_focus_manager.h"
#include "ui/views/widget/widget_observer.h"

namespace views {
class View;
class Widget;
}

namespace chromeos {

// FocusRingController manages the focus ring around the focused view. It
// follows widget focus change and update the focus ring layer when the focused
// view of the widget changes.
class FocusRingController : public FocusRingLayerDelegate,
                            public views::WidgetObserver,
                            public views::WidgetFocusChangeListener,
                            public views::FocusChangeListener {
 public:
  FocusRingController();
  ~FocusRingController() override;

  // Turns on/off the focus ring.
  void SetVisible(bool visible);

 private:
  // FocusRingLayerDelegate.
  void OnDeviceScaleFactorChanged() override;

  // Sets the focused |widget|.
  void SetWidget(views::Widget* widget);

  // Updates the focus ring to the focused view of |widget_|. If |widget_| is
  // NULL or has no focused view, removes the focus ring. Otherwise, draws it.
  void UpdateFocusRing();

  // views::WidgetObserver overrides:
  void OnWidgetDestroying(views::Widget* widget) override;
  void OnWidgetBoundsChanged(views::Widget* widget,
                             const gfx::Rect& new_bounds) override;

  // views::WidgetFocusChangeListener overrides:
  void OnNativeFocusChanged(gfx::NativeView focused_now) override;

  // views::FocusChangeListener overrides:
  void OnWillChangeFocus(views::View* focused_before,
                         views::View* focused_now) override;
  void OnDidChangeFocus(views::View* focused_before,
                        views::View* focused_now) override;

  bool visible_;

  views::Widget* widget_;
  std::unique_ptr<FocusRingLayer> focus_ring_layer_;

  DISALLOW_COPY_AND_ASSIGN(FocusRingController);
};

}  // namespace chromeos

#endif  // CHROME_BROWSER_CHROMEOS_UI_FOCUS_RING_CONTROLLER_H_
