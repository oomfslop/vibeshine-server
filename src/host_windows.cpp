/**
 * @file src/host_windows.cpp
 * @brief Host window enumeration and actions (see host_windows.h).
 */
#include "host_windows.h"

#include "logging.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
  #include <dwmapi.h>

  #include <algorithm>
  #include <chrono>
  #include <filesystem>
  #include <thread>

  #include "platform/windows/image_convert.h"
  #include "platform/windows/utf_utils.h"
  #include "tools/playnite_launcher/focus_utils.h"
#endif

namespace host_windows {

  bool parse_op(const std::string &name, op_e &out) {
    static const struct {
      const char *name;
      op_e op;
    } table[] = {
      {"activate", op_e::activate},
      {"move", op_e::move},
      {"resize", op_e::resize},
      {"minimize", op_e::minimize},
      {"restore", op_e::restore},
      {"maximize", op_e::maximize},
      {"close", op_e::close},
      {"lower", op_e::lower},
      {"dismiss", op_e::dismiss},
    };
    for (const auto &entry : table) {
      if (name == entry.name) {
        out = entry.op;
        return true;
      }
    }
    return false;
  }

#ifdef _WIN32
  namespace {
    /** A visible menu of this process: a Win32 menu (#32768) or a XAML
     *  popup host (Win11 context menus and flyouts). */
    bool has_open_menu(DWORD pid) {
      struct ctx_t {
        DWORD pid;
        bool found = false;
      } ctx {pid};
      EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<ctx_t *>(lp);
        if (!IsWindowVisible(w)) return TRUE;
        DWORD p = 0;
        GetWindowThreadProcessId(w, &p);
        if (p != c->pid) return TRUE;
        wchar_t cls[64] {}, title[32] {};
        GetClassNameW(w, cls, 64);
        GetWindowTextW(w, title, 32);
        if (wcscmp(cls, L"#32768") == 0 || wcscmp(title, L"PopupHost") == 0 || wcsstr(cls, L"PopupWindowSiteBridge") || wcscmp(cls, L"Xaml_WindowedPopupClass") == 0) {
          c->found = true;
          return FALSE;
        }
        return TRUE;
      },
                  reinterpret_cast<LPARAM>(&ctx));
      return ctx.found;
    }

    bool window_is_cloaked(HWND hwnd) {
      DWORD cloaked = 0;
      return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
    }

    std::string window_title(HWND hwnd) {
      const auto length = GetWindowTextLengthW(hwnd);
      if (length <= 0) return {};
      std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
      const auto copied = GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()));
      if (copied <= 0) return {};
      title.resize(static_cast<std::size_t>(copied));
      try {
        return utf_utils::to_utf8(title);
      } catch (...) {
        return {};
      }
    }

    std::wstring process_image(DWORD pid) {
      std::wstring path;
      if (!playnite_launcher::focus::get_process_image_path(pid, path)) return {};
      return path;
    }

    // Streamable: visible, not cloaked, not a tool window, has a title, is a
    // real top-level window (no owner) that is not our own process.
    bool is_candidate(HWND hwnd) {
      if (!IsWindowVisible(hwnd) || window_is_cloaked(hwnd)) return false;
      if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;
      const auto ex_style = static_cast<std::uintptr_t>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
      if ((ex_style & WS_EX_TOOLWINDOW) != 0) return false;
      wchar_t cls[64] {};
      GetClassNameW(hwnd, cls, 64);
      // The shell's untitled frame windows host system dialogs such as "Open
      // with" (the modern picker); they are streamable, nothing else untitled is.
      const bool frame_host = wcscmp(cls, L"ApplicationFrameWindow") == 0;
      if (GetWindowTextLengthW(hwnd) <= 0 && !frame_host) return false;
      if (hwnd == GetShellWindow() || hwnd == GetDesktopWindow()) return false;
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      if (pid == GetCurrentProcessId()) return false;
      // Skip the Windows shell surfaces (Start, search, taskbar hosts).
      static const wchar_t *shell_classes[] = {L"Windows.UI.Core.CoreWindow", L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"ApplicationFrameWindow"};
      for (auto *c : shell_classes) {
        if (wcscmp(cls, c) == 0 && wcscmp(c, L"ApplicationFrameWindow") != 0) return false;
      }
      return true;
    }

    HICON window_icon_handle(HWND hwnd) {
      HICON icon = nullptr;
      DWORD_PTR result = 0;
      if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &result) && result) icon = reinterpret_cast<HICON>(result);
      if (!icon && SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &result) && result) icon = reinterpret_cast<HICON>(result);
      if (!icon) icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
      if (!icon) icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
      return icon;
    }

    bool fill_geometry(HWND hwnd, window_info_t &info) {
      RECT frame {};
      if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        if (!GetWindowRect(hwnd, &frame)) return false;
      }
      RECT client {};
      if (!GetClientRect(hwnd, &client)) return false;
      POINT origin {client.left, client.top};
      ClientToScreen(hwnd, &origin);
      const int cw = client.right - client.left, ch = client.bottom - client.top;
      info.rect[0] = frame.left;
      info.rect[1] = frame.top;
      info.rect[2] = frame.right - frame.left;
      info.rect[3] = frame.bottom - frame.top;
      RECT win {};
      if (GetWindowRect(hwnd, &win)) {
        info.bounds[0] = win.left;
        info.bounds[1] = win.top;
        info.bounds[2] = win.right - win.left;
        info.bounds[3] = win.bottom - win.top;
      }
      info.client[0] = origin.x;
      info.client[1] = origin.y;
      info.client[2] = cw;
      info.client[3] = ch;
      info.borders[0] = std::max(0L, origin.x - frame.left);
      info.borders[2] = std::max(0L, frame.right - (origin.x + cw));
      info.borders[3] = std::max(0L, frame.bottom - (origin.y + ch));
      // The whole band above the client area is the caption; the top border
      // is folded into it (DWM draws no separate top edge on Windows 10/11).
      info.caption = std::max(0L, origin.y - frame.top);
      info.borders[1] = 0;
      return info.rect[2] > 0 && info.rect[3] > 0;
    }
  }  // namespace

  bool supported() {
    return true;
  }

  bool exists(std::uint64_t handle) {
    return IsWindow(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle))) != 0;
  }

  bool frame_rect(std::uint64_t handle, int out[4]) {
    auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    if (!IsWindow(hwnd)) return false;
    if (IsIconic(hwnd)) {
      // A minimized window sits at -32000,-32000; keep the capture geometry
      // of its restored placement so the session survives minimize/restore.
      WINDOWPLACEMENT placement {};
      placement.length = sizeof(placement);
      if (GetWindowPlacement(hwnd, &placement)) {
        const auto &r = placement.rcNormalPosition;
        out[0] = r.left;
        out[1] = r.top;
        out[2] = r.right - r.left;
        out[3] = r.bottom - r.top;
        return out[2] > 0 && out[3] > 0;
      }
    }
    RECT frame {};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) && !GetWindowRect(hwnd, &frame)) return false;
    out[0] = frame.left;
    out[1] = frame.top;
    out[2] = frame.right - frame.left;
    out[3] = frame.bottom - frame.top;
    return out[2] > 0 && out[3] > 0;
  }

  bool window_rect(std::uint64_t handle, int out[4]) {
    auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    RECT win {};
    if (!IsWindow(hwnd) || !GetWindowRect(hwnd, &win)) return false;
    out[0] = win.left;
    out[1] = win.top;
    out[2] = win.right - win.left;
    out[3] = win.bottom - win.top;
    return out[2] > 0 && out[3] > 0;
  }

  std::string icon_key(std::uint64_t handle) {
    auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    if (!IsWindow(hwnd)) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (HICON icon = window_icon_handle(hwnd)) {
      char key[48];
      snprintf(key, sizeof(key), "%llx-%lu", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(icon)), static_cast<unsigned long>(pid));
      return key;
    }
    if (!process_image(pid).empty()) return "exe-" + std::to_string(pid);
    return {};
  }

  std::vector<std::string> list_all() {
    std::vector<std::string> out;
    const HWND foreground = GetForegroundWindow();
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
      auto &out = *reinterpret_cast<std::vector<std::string> *>(param);
      wchar_t cls[128] {}, title[128] {};
      GetClassNameW(hwnd, cls, 128);
      GetWindowTextW(hwnd, title, 128);
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      RECT r {};
      GetWindowRect(hwnd, &r);
      DWORD cloaked = 0;
      DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
      char line[1024];
      snprintf(line, sizeof(line), "0x%llx vis=%d cloak=%lu owner=0x%llx style=0x%lx ex=0x%lx rect=%ld,%ld %ldx%ld pid=%lu", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(hwnd)), IsWindowVisible(hwnd) ? 1 : 0, cloaked,
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(GetWindow(hwnd, GW_OWNER))), GetWindowLongW(hwnd, GWL_STYLE), GetWindowLongW(hwnd, GWL_EXSTYLE), r.left, r.top, r.right - r.left, r.bottom - r.top, pid);
      out.push_back(std::string(line) + " cls=" + utf_utils::to_utf8(std::wstring(cls)) + " title=" + utf_utils::to_utf8(std::wstring(title)) + " exe=" + utf_utils::to_utf8(std::filesystem::path(process_image(pid)).filename().wstring()));
      return TRUE;
    },
                reinterpret_cast<LPARAM>(&out));
    char fg[64];
    snprintf(fg, sizeof(fg), "fg=0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(foreground)));
    std::string fg_line = fg;
    if (foreground) {
      // Windows in other z-order bands (system UI) are not enumerated: the
      // foreground one is shown here regardless
      wchar_t cls[128] {}, title[128] {};
      GetClassNameW(foreground, cls, 128);
      GetWindowTextW(foreground, title, 128);
      DWORD pid = 0;
      GetWindowThreadProcessId(foreground, &pid);
      RECT r {};
      GetWindowRect(foreground, &r);
      DWORD band = 0;
      using get_band_t = BOOL(WINAPI *)(HWND, DWORD *);
      if (auto get_band = reinterpret_cast<get_band_t>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowBand"))) get_band(foreground, &band);
      char line[512];
      snprintf(line, sizeof(line), " band=%lu vis=%d rect=%ld,%ld %ldx%ld pid=%lu owner=0x%llx", band, IsWindowVisible(foreground) ? 1 : 0, r.left, r.top, r.right - r.left, r.bottom - r.top, pid, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(GetWindow(foreground, GW_OWNER))));
      fg_line += std::string(line) + " cls=" + utf_utils::to_utf8(std::wstring(cls)) + " title=" + utf_utils::to_utf8(std::wstring(title)) + " exe=" + utf_utils::to_utf8(std::filesystem::path(process_image(pid)).filename().wstring());
    }
    // Which enumerations see it
    bool by_find = false, by_walk = false;
    for (HWND h = FindWindowExW(nullptr, nullptr, nullptr, nullptr); h; h = FindWindowExW(nullptr, h, nullptr, nullptr)) by_find = by_find || h == foreground;
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) by_walk = by_walk || h == foreground;
    fg_line += std::string(" find=") + (by_find ? "1" : "0") + " walk=" + (by_walk ? "1" : "0");
    out.insert(out.begin(), fg_line);
    return out;
  }

  std::vector<window_info_t> list() {
    std::vector<window_info_t> out;
    const HWND foreground = GetForegroundWindow();
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
      auto &out = *reinterpret_cast<std::vector<window_info_t> *>(param);
      if (!is_candidate(hwnd)) return TRUE;
      window_info_t info;
      info.handle = reinterpret_cast<std::uintptr_t>(hwnd);
      info.title = window_title(hwnd);
      if (info.title.empty()) info.title = "System dialog";
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      info.pid = pid;
      const auto image = process_image(pid);
      if (!image.empty()) {
        try {
          info.process = utf_utils::to_utf8(std::filesystem::path(image).filename().wstring());
        } catch (...) {
        }
      }
      if (IsIconic(hwnd)) {
        info.state = "minimized";
        // Geometry of a minimized window is meaningless; report the restored placement.
        WINDOWPLACEMENT placement {};
        placement.length = sizeof(placement);
        if (GetWindowPlacement(hwnd, &placement)) {
          const auto &r = placement.rcNormalPosition;
          info.rect[0] = r.left;
          info.rect[1] = r.top;
          info.rect[2] = r.right - r.left;
          info.rect[3] = r.bottom - r.top;
        }
      } else {
        info.state = IsZoomed(hwnd) ? "maximized" : "normal";
        if (!fill_geometry(hwnd, info)) return TRUE;
      }
      if (HICON icon = window_icon_handle(hwnd)) {
        char key[48];
        snprintf(key, sizeof(key), "%llx-%lu", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(icon)), static_cast<unsigned long>(pid));
        info.icon_key = key;
      } else if (!image.empty()) {
        info.icon_key = "exe-" + std::to_string(pid);
      }
      if (HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)) {
        MONITORINFOEXW mi {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
          try {
            info.monitor = utf_utils::to_utf8(mi.szDevice);
          } catch (...) {
          }
          info.monitor_rect[0] = mi.rcMonitor.left;
          info.monitor_rect[1] = mi.rcMonitor.top;
          info.monitor_rect[2] = mi.rcMonitor.right - mi.rcMonitor.left;
          info.monitor_rect[3] = mi.rcMonitor.bottom - mi.rcMonitor.top;
        }
        if (const UINT dpi = GetDpiForWindow(hwnd)) info.dpi = static_cast<int>(dpi);
      }
      out.push_back(std::move(info));
      return TRUE;
    },
                reinterpret_cast<LPARAM>(&out));
    for (auto &w : out) {
      w.focused = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(w.handle)) == foreground;
    }
    return out;
  }

  std::string hit_test_point(std::uint64_t handle, int x, int y) {
    auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    int frame[4];
    if (!IsWindow(hwnd) || IsIconic(hwnd) || !frame_rect(handle, frame)) return "none";
    const POINT screen {frame[0] + x, frame[1] + y};
    const LPARAM pt = MAKELPARAM(static_cast<WORD>(screen.x), static_cast<WORD>(screen.y));
    // Ask like the system routes a click: the deepest visible child under the
    // point first; one that answers HTTRANSPARENT passes it to its parent.
    // (Asking the top-level first is wrong for apps whose top level claims
    // the whole title strip as caption while child windows on top of it, the
    // tabs, take the clicks: Windows Terminal.)
    std::vector<HWND> chain {hwnd};
    for (int depth = 0; depth < 8; ++depth) {
      POINT client = screen;
      ScreenToClient(chain.back(), &client);
      HWND next = ChildWindowFromPointEx(chain.back(), client, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
      if (!next || next == chain.back()) break;
      chain.push_back(next);
    }
    DWORD_PTR result = HTNOWHERE;
    bool answered = false;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      DWORD_PTR r = HTNOWHERE;
      if (!SendMessageTimeoutW(*it, WM_NCHITTEST, 0, pt, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &r)) continue;
      if (r == static_cast<DWORD_PTR>(HTTRANSPARENT) || r == HTNOWHERE || r == static_cast<DWORD_PTR>(HTERROR)) continue;
      result = r;
      answered = true;
      break;
    }
    if (!answered) return "none";
    switch (static_cast<LRESULT>(result)) {
      case HTCAPTION:
      case HTSYSMENU:
        return "drag";
      case HTMINBUTTON:
        return "min";
      case HTMAXBUTTON:
        return "max";
      case HTCLOSE:
        return "close";
      case HTCLIENT:
        return "client";
      case HTLEFT:
      case HTRIGHT:
      case HTTOP:
      case HTTOPLEFT:
      case HTTOPRIGHT:
      case HTBOTTOM:
      case HTBOTTOMLEFT:
      case HTBOTTOMRIGHT:
        return "border";
      default:
        return "none";
    }
  }

  std::string icon_png(std::uint64_t handle) {
    auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    if (!IsWindow(hwnd)) return {};
    std::string png;
    if (HICON icon = window_icon_handle(hwnd)) {
      if (platf::img::encode_hicon_to_png_bytes(icon, png) && !png.empty()) return png;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    const auto image = process_image(pid);
    if (image.empty()) return {};
    wchar_t tmp_dir[MAX_PATH] {};
    if (!GetTempPathW(MAX_PATH, tmp_dir)) return {};
    const std::wstring tmp = std::wstring(tmp_dir) + L"vibeshine-window-icon-" + std::to_wstring(pid) + L".png";
    int native = 0;
    if (!platf::img::extract_largest_icon_png(image, tmp, native)) return {};
    std::string bytes;
    if (FILE *f = _wfopen(tmp.c_str(), L"rb")) {
      char buf[8192];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0) bytes.append(buf, n);
      fclose(f);
    }
    DeleteFileW(tmp.c_str());
    return bytes;
  }

  bool action(std::uint64_t handle, op_e op, int x, int y, int w, int h) {
    auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    if (!IsWindow(hwnd)) return false;
    switch (op) {
      case op_e::activate: {
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        // No AttachThreadInput here: it blocks for as long as the foreground
        // window's thread is busy (a drag in progress) and this runs on a
        // request worker. SwitchToThisWindow is the shell's last resort.
        // A window disabled by its modal dialog cannot take focus; activating
        // it would only shuffle the z-order and leave the dialog behind it.
        // Activate the enabled popup of the chain instead (what Windows does
        // on a click).
        HWND focus = hwnd;
        if (!IsWindowEnabled(hwnd)) {
          if (HWND popup = GetLastActivePopup(hwnd); popup && popup != hwnd && IsWindowEnabled(popup)) focus = popup;
        }
        AllowSetForegroundWindow(ASFW_ANY);
        bool ok = SetForegroundWindow(focus) != 0;
        BringWindowToTop(focus);
        if (!ok || GetForegroundWindow() != focus) {
          // The foreground lock: only the process with the last input may
          // set the foreground. A synthetic tap of an unassigned key makes
          // that us. (Not Alt: it puts the focused app in menu mode and
          // can bring up the Alt-Tab peek, which draws the other windows
          // as glass outlines.)
          INPUT tap[2] {};
          tap[0].type = tap[1].type = INPUT_KEYBOARD;
          tap[0].ki.wVk = tap[1].ki.wVk = 0xE8;  // unassigned
          tap[1].ki.dwFlags = KEYEVENTF_KEYUP;
          SendInput(2, tap, sizeof(INPUT));
          ok = SetForegroundWindow(focus) != 0;
          BringWindowToTop(focus);
        }
        if (!ok || GetForegroundWindow() != focus) {
          SwitchToThisWindow(focus, FALSE);  // TRUE plays the Alt-Tab switch (peek)
          ok = GetForegroundWindow() == focus;
        }
        if (!ok) {
          BOOST_LOG(info) << "App windows: could not focus 0x" << std::hex << reinterpret_cast<std::uintptr_t>(focus) << std::dec
                          << " (foreground is 0x" << std::hex << reinterpret_cast<std::uintptr_t>(GetForegroundWindow()) << std::dec << ')';
        }
        return ok;
      }
      case op_e::move: {
        // x/y are DWM frame coordinates; SetWindowPos positions the raw window
        // rect, which sits a few pixels outside the visible frame.
        RECT frame {}, win {};
        int dx = 0, dy = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) && GetWindowRect(hwnd, &win)) {
          dx = frame.left - win.left;
          dy = frame.top - win.top;
        }
        return SetWindowPos(hwnd, nullptr, x - dx, y - dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != 0;
      }
      case op_e::resize: {
        // w/h are the DWM frame size; SetWindowPos wants the GetWindowRect
        // size, which on Windows 10/11 includes invisible resize borders.
        RECT frame {}, win {};
        int dw = 0, dh = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) && GetWindowRect(hwnd, &win)) {
          dw = (win.right - win.left) - (frame.right - frame.left);
          dh = (win.bottom - win.top) - (frame.bottom - frame.top);
        }
        if (!SetWindowPos(hwnd, nullptr, 0, 0, std::max(1, w + dw), std::max(1, h + dh), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS)) return false;
        return true;
      }
      case op_e::minimize:
        return ShowWindow(hwnd, SW_MINIMIZE) != 0 || IsIconic(hwnd);
      case op_e::restore:
        return ShowWindow(hwnd, SW_RESTORE) != 0 || !IsIconic(hwnd);
      case op_e::maximize:
        return ShowWindow(hwnd, SW_MAXIMIZE) != 0 || IsZoomed(hwnd);
      case op_e::close:
        return PostMessageW(hwnd, WM_CLOSE, 0, 0) != 0;
      case op_e::lower:
        return SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS) != 0;
      case op_e::dismiss: {
        // Escape closes one menu level, Win32 menus and XAML flyouts alike;
        // sent only while the window's process has the focus and one of its
        // menus is open, so it never reaches the app itself
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        for (int level = 0; level < 5; ++level) {
          DWORD fg_pid = 0;
          GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
          if (fg_pid != pid || !has_open_menu(pid)) return true;
          INPUT esc[2] {};
          esc[0].type = esc[1].type = INPUT_KEYBOARD;
          esc[0].ki.wVk = esc[1].ki.wVk = VK_ESCAPE;
          esc[1].ki.dwFlags = KEYEVENTF_KEYUP;
          SendInput(2, esc, sizeof(INPUT));
          std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        return !has_open_menu(pid);
      }
    }
    return false;
  }
#else
  bool supported() {
    return false;
  }

  bool exists(std::uint64_t) {
    return false;
  }

  bool frame_rect(std::uint64_t, int[4]) {
    return false;
  }

  bool window_rect(std::uint64_t, int[4]) {
    return false;
  }

  std::string hit_test_point(std::uint64_t, int, int) {
    return "none";
  }

  std::vector<window_info_t> list() {
    return {};
  }

  std::vector<std::string> list_all() {
    return {};
  }

  std::string icon_png(std::uint64_t) {
    return {};
  }

  std::string icon_key(std::uint64_t) {
    return {};
  }

  bool action(std::uint64_t, op_e, int, int, int, int) {
    return false;
  }
#endif

}  // namespace host_windows
