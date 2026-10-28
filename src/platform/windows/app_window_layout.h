/**
 * @file src/platform/windows/app_window_layout.h
 * @brief Window geometry helpers for app-window streaming (the desk): a
 *        window's outer frame rect, cloaking and the OS corner rounding.
 *
 * Header-only and Windows-API-only.
 */
#pragma once

#include <windows.h>
#include <dwmapi.h>

namespace app_window_layout {

  /** Outer (DWM extended frame) rect of a window, or GetWindowRect as a fallback. */
  inline bool frame_rect(HWND hwnd, RECT &out) {
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &out, sizeof(out)))) {
      return out.right > out.left && out.bottom > out.top;
    }
    return GetWindowRect(hwnd, &out) && out.right > out.left && out.bottom > out.top;
  }

  inline bool is_cloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
  }

  /** Windows 11 (build 22000+) rounds top-level windows and popups by 8 DIP. */
  inline bool rounded_corners_os() {
    static const bool rounded = [] {
      using fn_t = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
      RTL_OSVERSIONINFOW info {};
      info.dwOSVersionInfoSize = sizeof(info);
      if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<fn_t>(GetProcAddress(ntdll, "RtlGetVersion")); fn && fn(&info) == 0) {
          return info.dwMajorVersion > 10 || (info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000);
        }
      }
      return false;
    }();
    return rounded;
  }

}  // namespace app_window_layout
