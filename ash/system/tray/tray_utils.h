// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_SYSTEM_TRAY_TRAY_UTILS_H_
#define ASH_SYSTEM_TRAY_TRAY_UTILS_H_

#include <vector>

#include "ash/shelf/shelf_types.h"
#include "base/strings/string16.h"

namespace views {
class Label;
class View;
}

namespace ash {

class TrayItemView;

// Sets up a Label properly for the tray (sets color, font etc.).
void SetupLabelForTray(views::Label* label);

// TODO(jennyz): refactor these two functions to SystemTrayItem.
// Sets the empty border of an image tray item for adjusting the space
// around it.
void SetTrayImageItemBorder(views::View* tray_view,
                            wm::ShelfAlignment alignment);
// Sets the empty border around a label tray item for adjusting the space
// around it.
void SetTrayLabelItemBorder(TrayItemView* tray_view,
                            wm::ShelfAlignment alignment);

// Computes an accessible label for this button based on all descendant view
// labels by concatenating them in depth-first order.
void GetAccessibleLabelFromDescendantViews(
    views::View* view,
    std::vector<base::string16>& out_labels);

}  // namespace ash

#endif  // ASH_SYSTEM_TRAY_TRAY_UTILS_H_
