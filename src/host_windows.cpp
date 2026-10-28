/**
 * @file src/host_windows.cpp
 * @brief Host window enumeration and actions (see host_windows.h).
 */
#include "host_windows.h"

#include "logging.h"

#ifdef _WIN32
  #include <windows.h>
  #include <dwmapi.h>

  #include <algorithm>
  #include <filesystem>

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
      if (GetWindowTextLengthW(hwnd) <= 0) return false;
      if (hwnd == GetShellWindow() || hwnd == GetDesktopWindow()) return false;
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      if (pid == GetCurrentProcessId()) return false;
      // Skip the Windows shell surfaces (Start, search, taskbar hosts).
      wchar_t cls[64] {};
      GetClassNameW(hwnd, cls, 64);
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

  std::vector<window_info_t> list() {
    std::vector<window_info_t> out;
    const HWND foreground = GetForegroundWindow();
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
      auto &out = *reinterpret_cast<std::vector<window_info_t> *>(param);
      if (!is_candidate(hwnd)) return TRUE;
      window_info_t info;
      info.handle = reinterpret_cast<std::uintptr_t>(hwnd);
      info.title = window_title(hwnd);
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

  std::vector<hit_region_t> hit_regions(std::uint64_t handle) {
    std::vector<hit_region_t> out;
    auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    int frame[4];
    if (!IsWindow(hwnd) || IsIconic(hwnd) || !frame_rect(handle, frame)) return out;
    auto kind_of = [](LRESULT hit) -> const char * {
      switch (hit) {
        case HTCAPTION:
        case HTSYSMENU:
          return "drag";
        case HTMINBUTTON:
          return "min";
        case HTMAXBUTTON:
          return "max";
        case HTCLOSE:
          return "close";
        default:
          return nullptr;
      }
    };
    constexpr int step = 8;
    const int probe_h = std::min(frame[3], 64);
    struct run_t {
      const char *kind;
      int x, w;
    };
    std::vector<std::pair<int, std::vector<run_t>>> rows;
    for (int y = step / 2; y < probe_h; y += step) {
      std::vector<run_t> runs;
      const char *cur = nullptr;
      int cur_x = 0;
      for (int x = step / 2; x <= frame[2]; x += step) {
        const char *k = nullptr;
        if (x < frame[2]) {
          const LPARAM pt = MAKELPARAM(static_cast<WORD>(frame[0] + x), static_cast<WORD>(frame[1] + y));
          DWORD_PTR result = HTNOWHERE;
          if (SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0, pt, SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &result)) {
            k = kind_of(static_cast<LRESULT>(result));
          }
        }
        if (k != cur) {
          if (cur) runs.push_back({cur, cur_x - step / 2, x - cur_x});
          cur = k;
          cur_x = x;
        }
      }
      rows.push_back({y - step / 2, std::move(runs)});
    }
    // Merge vertically: a run that repeats on the next row grows down.
    std::vector<hit_region_t> open;
    for (const auto &[y, runs] : rows) {
      std::vector<hit_region_t> next;
      for (const auto &r : runs) {
        bool merged = false;
        for (auto &o : open) {
          if (o.rect[3] > 0 && o.kind == r.kind && o.rect[0] == r.x && o.rect[2] == r.w && o.rect[1] + o.rect[3] == y) {
            o.rect[3] += step;
            next.push_back(o);
            o.rect[3] = -1;  // consumed
            merged = true;
            break;
          }
        }
        if (!merged) {
          hit_region_t h;
          h.kind = r.kind;
          h.rect[0] = r.x;
          h.rect[1] = y;
          h.rect[2] = r.w;
          h.rect[3] = step;
          next.push_back(h);
        }
      }
      for (auto &o : open) {
        if (o.rect[3] > 0) out.push_back(o);
      }
      open = std::move(next);
    }
    for (auto &o : open) {
      if (o.rect[3] > 0) out.push_back(o);
    }
    return out;
  }

  std::string hit_test_point(std::uint64_t handle, int x, int y) {
    auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    int frame[4];
    if (!IsWindow(hwnd) || IsIconic(hwnd) || !frame_rect(handle, frame)) return "none";
    const LPARAM pt = MAKELPARAM(static_cast<WORD>(frame[0] + x), static_cast<WORD>(frame[1] + y));
    DWORD_PTR result = HTNOWHERE;
    if (!SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0, pt, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &result)) return "none";
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
        // Foreground rules: attach to the current foreground thread so the
        // request isn't ignored, then detach.
        const DWORD fg_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const DWORD our_thread = GetCurrentThreadId();
        const bool attached = fg_thread && fg_thread != our_thread && AttachThreadInput(fg_thread, our_thread, TRUE);
        AllowSetForegroundWindow(ASFW_ANY);
        const bool ok = SetForegroundWindow(hwnd) != 0;
        BringWindowToTop(hwnd);
        if (attached) AttachThreadInput(fg_thread, our_thread, FALSE);
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
        return SetWindowPos(hwnd, nullptr, 0, 0, std::max(1, w + dw), std::max(1, h + dh), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != 0;
      }
      case op_e::minimize:
        return ShowWindow(hwnd, SW_MINIMIZE) != 0 || IsIconic(hwnd);
      case op_e::restore:
        return ShowWindow(hwnd, SW_RESTORE) != 0 || !IsIconic(hwnd);
      case op_e::maximize:
        return ShowWindow(hwnd, SW_MAXIMIZE) != 0 || IsZoomed(hwnd);
      case op_e::close:
        return PostMessageW(hwnd, WM_CLOSE, 0, 0) != 0;
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

  std::vector<hit_region_t> hit_regions(std::uint64_t) {
    return {};
  }

  std::string hit_test_point(std::uint64_t, int, int) {
    return "none";
  }

  std::vector<window_info_t> list() {
    return {};
  }

  std::string icon_png(std::uint64_t) {
    return {};
  }

  bool action(std::uint64_t, op_e, int, int, int, int) {
    return false;
  }
#endif

}  // namespace host_windows
