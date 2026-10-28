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
   * @brief The encoder frame of an app-window session: a colour canvas on top
   *        (the window band, where the window sits at 0,0, followed by the
   *        popup band where menus and dialogs get slots of their own) and,
   *        below it at half resolution, the canvas' alpha as grey (video has
   *        no alpha channel). frame_h = 3/2 * canvas_h.
   */
  struct frame_bands_t {
    int canvas_h = 0;  ///< colour canvas height (window band + popup band)
    int window_h = 0;  ///< window band: the streamed window lives in rows [0, window_h)
    int popup_y = 0;  ///< popup band starts here, ends at canvas_h
    int alpha_y = 0;  ///< alpha band (half resolution: canvas_w/2 x canvas_h/2) starts here
  };
  inline frame_bands_t frame_bands(int frame_h) {
    frame_bands_t b;
    b.canvas_h = frame_h * 2 / 3;
    const int popup = (b.canvas_h / 3) & ~63;
    b.window_h = b.canvas_h - popup;
    b.popup_y = b.window_h;
    b.alpha_y = b.canvas_h;
    return b;
  }
  inline int color_height(int frame_height) {
    return frame_bands(frame_height).canvas_h;
  }
  /** The window sits at the canvas origin; kept for the callers' shape. */
  inline void canvas_anchor(int, int, int, int, int &ax, int &ay) {
    ax = 0;
    ay = 0;
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
    DWORD target_pid = 0, pid = 0;
    const DWORD target_tid = GetWindowThreadProcessId(target, &target_pid);
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (pid != target_pid) {
      return false;
    }
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (HWND owner = GetWindow(hwnd, GW_OWNER)) {
      // Owned by something else in this process. Dialogs that hang off an
      // invisible helper window (Explorer's property sheets, many shell
      // dialogs) belong to the app's window the user is working in.
      if ((style & WS_CAPTION) != WS_CAPTION) {
        return false;
      }
      for (HWND o = owner; o; o = GetWindow(o, GW_OWNER)) {
        if (IsWindowVisible(o)) {
          return false;  // a visible window of its own owns it
        }
      }
      return true;
    }
    if (tid != target_tid) {
      return false;
    }
    if (!(style & WS_POPUP) || (style & WS_CAPTION) == WS_CAPTION) {
      return false;
    }
    return true;
  }

  /** An owned window with its own caption (dialog, tool window): the client
   *  shows these as windows of their own rather than as part of the owner. */
  inline bool is_owned_dialog(HWND target, HWND hwnd) {
    if (!is_popup_of(target, hwnd)) {
      return false;
    }
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    return (style & WS_CAPTION) == WS_CAPTION;
  }

  struct popup_t {
    HWND hwnd;
    RECT rect;  ///< outer rect in virtual-desktop pixels
    bool dialog = false;  ///< is_owned_dialog
  };

  /**
   * @brief A caption-less popup that lies entirely inside its parent and
   *        covers a good part of it is a piece of the app's own UI (Media
   *        Player Legacy's skin parts, docked tool panes), not a menu: it is
   *        drawn in place over the parent, where it also sits on the host.
   */
  inline bool is_inline_part(const RECT &parent, const popup_t &p) {
    if (p.dialog) {
      return false;
    }
    if (p.rect.left < parent.left || p.rect.top < parent.top || p.rect.right > parent.right || p.rect.bottom > parent.bottom) {
      return false;
    }
    const LONGLONG area = static_cast<LONGLONG>(p.rect.right - p.rect.left) * (p.rect.bottom - p.rect.top);
    const LONGLONG parent_area = static_cast<LONGLONG>(parent.right - parent.left) * (parent.bottom - parent.top);
    return parent_area > 0 && area * 4 >= parent_area;
  }

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
          const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
          c->out.push_back({hwnd, r, (style & WS_CAPTION) == WS_CAPTION});
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
