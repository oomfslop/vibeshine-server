/**
 * @file src/platform/windows/app_window_layout.h
 * @brief App-window streaming geometry shared by Sunshine and the WGC capture
 *        helper: where a window sits on its canvas and which popup windows
 *        (menus, drop-downs, tooltips, owned dialogs) belong to it.
 *
 * Header-only and Windows-API-only so both processes compute exactly the
 * same answers.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include <windows.h>
#include <dwmapi.h>

namespace app_window_layout {

  /**
   * @brief Canvas position of a window's top-left corner.
   *
   * The canvas is larger than the window so popups that stick out past the
   * window's edges still fit. The window is anchored at a fixed fraction of
   * the canvas (an eighth from the left, a tenth from the top: popups mostly
   * open to the right and below, flipped ones need less) and only pushed
   * towards the origin when it would not fit otherwise, so moves never shift
   * the picture and resizes only shift it once the window nearly fills the
   * canvas. Even coordinates keep chroma planes aligned.
   */
  inline void canvas_anchor(int canvas_w, int canvas_h, int win_w, int win_h, int &ax, int &ay) {
    ax = std::clamp(canvas_w / 8, 0, std::max(0, canvas_w - win_w)) & ~1;
    ay = std::clamp(canvas_h / 10, 0, std::max(0, canvas_h - win_h)) & ~1;
  }

  /**
   * @brief The encoder frame of an app-window session is two bands: the
   *        composited picture on top and, below it, the same pixels' alpha
   *        as grey (video has no alpha channel). Both bands are this tall.
   */
  inline int color_height(int frame_height) {
    return frame_height / 2;
  }

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

  /**
   * @brief Whether `hwnd` is a popup that visually belongs to `target`:
   *        anything in its owner chain (dialogs, WinUI PopupHost, tooltips),
   *        or an unowned caption-less WS_POPUP window on the same thread
   *        (Win32 menus, combo-box lists, tooltips of classic apps).
   */
  inline bool is_popup_of(HWND target, HWND hwnd) {
    if (hwnd == target || !IsWindowVisible(hwnd) || IsIconic(hwnd) || is_cloaked(hwnd)) {
      return false;
    }
    for (HWND owner = GetWindow(hwnd, GW_OWNER); owner; owner = GetWindow(owner, GW_OWNER)) {
      if (owner == target) {
        return true;
      }
    }
    if (GetWindow(hwnd, GW_OWNER)) {
      return false;  // owned by some other window
    }
    DWORD target_pid = 0, pid = 0;
    const DWORD target_tid = GetWindowThreadProcessId(target, &target_pid);
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (tid != target_tid || pid != target_pid) {
      return false;
    }
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (!(style & WS_POPUP) || (style & WS_CAPTION) == WS_CAPTION) {
      return false;
    }
    return true;
  }

  struct popup_t {
    HWND hwnd;
    RECT rect;  ///< outer rect in virtual-desktop pixels
  };

  /**
   * @brief Visible popups of `target`, bottom-most first (composition order).
   */
  inline std::vector<popup_t> popup_windows(HWND target) {
    struct ctx_t {
      HWND target;
      std::vector<popup_t> out;  // top-most first, as EnumWindows walks the z-order
    } ctx {target, {}};
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
      auto *c = reinterpret_cast<ctx_t *>(lp);
      if (is_popup_of(c->target, hwnd)) {
        RECT r {};
        if (frame_rect(hwnd, r)) {
          c->out.push_back({hwnd, r});
        }
      }
      return TRUE;
    },
                reinterpret_cast<LPARAM>(&ctx));
    std::reverse(ctx.out.begin(), ctx.out.end());
    return ctx.out;
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
