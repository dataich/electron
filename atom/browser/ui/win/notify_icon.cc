// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/ui/win/notify_icon.h"

#include <shobjidl.h>

#include "atom/browser/ui/win/notify_icon_host.h"
#include "base/md5.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/win/windows_version.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/icon_util.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/controls/menu/menu_runner.h"

namespace atom {

NotifyIcon::NotifyIcon(NotifyIconHost* host,
                       UINT id,
                       HWND window,
                       UINT message)
    : host_(host),
      icon_id_(id),
      window_(window),
      message_id_(message),
      menu_model_(NULL),
      has_tray_app_id_hash_(false) {
  // NB: If we have an App Model ID, we should propagate that to the tray.
  // Doing this prevents duplicate items from showing up in the notification
  // preferences (i.e. "Always Show / Show notifications only / etc")
  PWSTR explicit_app_id;
  if (SUCCEEDED(GetCurrentProcessExplicitAppUserModelID(&explicit_app_id))) {
    // GUIDs and MD5 hashes are the same length. So convenient!
    base::MD5Sum(explicit_app_id,
                 sizeof(wchar_t) * wcslen(explicit_app_id),
                 reinterpret_cast<base::MD5Digest*>(&tray_app_id_hash_));
    has_tray_app_id_hash_ = true;
    CoTaskMemFree(explicit_app_id);
  }

  NOTIFYICONDATA icon_data;
  InitIconData(&icon_data);
  icon_data.uFlags |= NIF_MESSAGE;
  icon_data.uCallbackMessage = message_id_;
  BOOL result = Shell_NotifyIcon(NIM_ADD, &icon_data);
  // This can happen if the explorer process isn't running when we try to
  // create the icon for some reason (for example, at startup).
  if (!result)
    LOG(WARNING) << "Unable to create status tray icon.";
}

NotifyIcon::~NotifyIcon() {
  // Remove our icon.
  host_->Remove(this);
  NOTIFYICONDATA icon_data;
  InitIconData(&icon_data);
  Shell_NotifyIcon(NIM_DELETE, &icon_data);
}

void NotifyIcon::HandleClickEvent(const gfx::Point& cursor_pos,
                                  bool left_mouse_click) {
  NOTIFYICONIDENTIFIER icon_id;
  memset(&icon_id, 0, sizeof(NOTIFYICONIDENTIFIER));
  icon_id.uID = icon_id_;
  icon_id.hWnd = window_;
  icon_id.cbSize = sizeof(NOTIFYICONIDENTIFIER);
  RECT rect = { 0 };
  Shell_NotifyIconGetRect(&icon_id, &rect);

  // Pass to the observer if appropriate.
  if (left_mouse_click) {
    NotifyClicked(gfx::Rect(rect));
    return;
  }

  NotifyRightClicked(gfx::Rect(rect));
  PopContextMenu(cursor_pos);
}

void NotifyIcon::ResetIcon() {
  NOTIFYICONDATA icon_data;
  InitIconData(&icon_data);
  // Delete any previously existing icon.
  Shell_NotifyIcon(NIM_DELETE, &icon_data);
  InitIconData(&icon_data);
  icon_data.uFlags |= NIF_MESSAGE;
  icon_data.uCallbackMessage = message_id_;
  icon_data.hIcon = icon_.Get();
  // If we have an image, then set the NIF_ICON flag, which tells
  // Shell_NotifyIcon() to set the image for the status icon it creates.
  if (icon_data.hIcon)
    icon_data.uFlags |= NIF_ICON;
  // Re-add our icon.
  BOOL result = Shell_NotifyIcon(NIM_ADD, &icon_data);
  if (!result)
    LOG(WARNING) << "Unable to re-create status tray icon.";
}

void NotifyIcon::SetImage(const gfx::Image& image) {
  // Create the icon.
  NOTIFYICONDATA icon_data;
  InitIconData(&icon_data);
  icon_data.uFlags |= NIF_ICON;
  icon_.Set(IconUtil::CreateHICONFromSkBitmap(image.AsBitmap()));
  icon_data.hIcon = icon_.Get();
  BOOL result = Shell_NotifyIcon(NIM_MODIFY, &icon_data);
  if (!result)
    LOG(WARNING) << "Error setting status tray icon image";
}

void NotifyIcon::SetPressedImage(const gfx::Image& image) {
  // Ignore pressed images, since the standard on Windows is to not highlight
  // pressed status icons.
}

void NotifyIcon::SetToolTip(const std::string& tool_tip) {
  // Create the icon.
  NOTIFYICONDATA icon_data;
  InitIconData(&icon_data);
  icon_data.uFlags |= NIF_TIP;
  wcscpy_s(icon_data.szTip, base::UTF8ToUTF16(tool_tip).c_str());
  BOOL result = Shell_NotifyIcon(NIM_MODIFY, &icon_data);
  if (!result)
    LOG(WARNING) << "Unable to set tooltip for status tray icon";
}

void NotifyIcon::DisplayBalloon(const gfx::Image& icon,
                                const base::string16& title,
                                const base::string16& contents) {
  NOTIFYICONDATA icon_data;
  InitIconData(&icon_data);
  icon_data.uFlags |= NIF_INFO;
  icon_data.dwInfoFlags = NIIF_INFO;
  wcscpy_s(icon_data.szInfoTitle, title.c_str());
  wcscpy_s(icon_data.szInfo, contents.c_str());
  icon_data.uTimeout = 0;

  base::win::Version win_version = base::win::GetVersion();
  if (!icon.IsEmpty() && win_version != base::win::VERSION_PRE_XP) {
    balloon_icon_.Set(IconUtil::CreateHICONFromSkBitmap(icon.AsBitmap()));
    icon_data.hBalloonIcon = balloon_icon_.Get();
    icon_data.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
  }

  BOOL result = Shell_NotifyIcon(NIM_MODIFY, &icon_data);
  if (!result)
    LOG(WARNING) << "Unable to create status tray balloon.";
}

void NotifyIcon::PopContextMenu(const gfx::Point& pos) {
  // Set our window as the foreground window, so the context menu closes when
  // we click away from it.
  if (!SetForegroundWindow(window_))
    return;

  views::MenuRunner menu_runner(
      menu_model_,
      views::MenuRunner::CONTEXT_MENU | views::MenuRunner::HAS_MNEMONICS);
  ignore_result(menu_runner.RunMenuAt(
      NULL,
      NULL,
      gfx::Rect(pos, gfx::Size()),
      views::MENU_ANCHOR_TOPLEFT,
      ui::MENU_SOURCE_MOUSE));
}

void NotifyIcon::SetContextMenu(ui::SimpleMenuModel* menu_model) {
  menu_model_ = menu_model;
}

void NotifyIcon::InitIconData(NOTIFYICONDATA* icon_data) {
  memset(icon_data, 0, sizeof(NOTIFYICONDATA));
  icon_data->cbSize = sizeof(NOTIFYICONDATA);
  icon_data->hWnd = window_;
  icon_data->uID = icon_id_;

  if (has_tray_app_id_hash_) {
    icon_data->uFlags |= NIF_GUID;
    memcpy(reinterpret_cast<void*>(&icon_data->guidItem),
           &tray_app_id_hash_,
           sizeof(GUID));
  }
}

}  // namespace atom
