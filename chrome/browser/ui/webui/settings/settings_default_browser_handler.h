// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_SETTINGS_SETTINGS_DEFAULT_BROWSER_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_SETTINGS_SETTINGS_DEFAULT_BROWSER_HANDLER_H_

#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/shell_integration.h"
#include "chrome/browser/ui/webui/settings/settings_page_ui_handler.h"
#include "components/prefs/pref_member.h"

namespace base {
class ListValue;
}

namespace content {
class WebUI;
}

namespace settings {

// The application used by the OS to open web documents (e.g. *.html)
// is the "default browser".  This class is an API for the JavaScript
// settings code to change the default browser settings.
class DefaultBrowserHandler : public SettingsPageUIHandler {
 public:
  explicit DefaultBrowserHandler(content::WebUI* webui);
  ~DefaultBrowserHandler() override;

  // SettingsPageUIHandler implementation.
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

 private:
  // Called from WebUI to request the current state.
  void RequestDefaultBrowserState(const base::ListValue* args);

  // Makes this the default browser. Called from WebUI.
  void SetAsDefaultBrowser(const base::ListValue* args);

  // Called with the default browser state when the DefaultBrowserWorker is
  // done.
  void OnDefaultBrowserWorkerFinished(
      shell_integration::DefaultWebClientState state);

  // Reference to a background worker that handles default browser settings.
  scoped_refptr<shell_integration::DefaultBrowserWorker>
      default_browser_worker_;

  // Policy setting to determine if default browser setting is managed.
  BooleanPrefMember default_browser_policy_;

  // Used to invalidate the DefaultBrowserWorker callback.
  base::WeakPtrFactory<DefaultBrowserHandler> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(DefaultBrowserHandler);
};

}  // namespace settings

#endif  // CHROME_BROWSER_UI_WEBUI_SETTINGS_SETTINGS_DEFAULT_BROWSER_HANDLER_H_
