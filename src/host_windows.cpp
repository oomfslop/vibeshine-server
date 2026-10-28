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

  #include "platform/windows/app_window_layout.h"
  #include "platform/windows/image_convert.h"
  #include "platform/windows/virtual_display.h"
  #include <atomic>
  #include <map>
  #include <mutex>
  #include <thread>
  #include <chrono>
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

  layout_t window_layout(std::uint64_t handle, int canvas_w, int canvas_h) {
    layout_t out;
    auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    if (!IsWindow(hwnd) || !frame_rect(handle, out.rect)) return out;
    out.valid = true;
    app_window_layout::canvas_anchor(canvas_w, canvas_h, out.rect[2], out.rect[3], out.anchor[0], out.anchor[1]);
    const bool minimized = IsIconic(hwnd);
    out.state = minimized ? "minimized" : (IsZoomed(hwnd) ? "maximized" : "normal");
    out.focused = GetForegroundWindow() == hwnd;
    out.title = window_title(hwnd);
    if (!minimized) {
      RECT client {};
      POINT origin {0, 0};
      if (GetClientRect(hwnd, &client) && ClientToScreen(hwnd, &origin)) {
        out.caption = std::max(0L, static_cast<LONG>(origin.y - out.rect[1]));
      }
      // Every popup the helper gave a slot on the canvas; menus and dialogs alike
      // are windows of their own on the client.
      const auto slots = popup_slots(handle);
      for (const auto &p : app_window_layout::popup_windows(hwnd)) {
        const auto id = reinterpret_cast<std::uintptr_t>(p.hwnd);
        auto slot = std::find_if(slots.begin(), slots.end(), [&](const popup_slot_t &sl) { return sl.handle == id; });
        if (slot == slots.end()) continue;  // not composited yet
        layout_t::child_t child;
        child.handle = id;
        child.rect[0] = p.rect.left;
        child.rect[1] = p.rect.top;
        child.rect[2] = p.rect.right - p.rect.left;
        child.rect[3] = p.rect.bottom - p.rect.top;
        std::copy(std::begin(slot->canvas), std::end(slot->canvas), child.canvas);
        child.dialog = p.dialog;
        if (p.dialog) {
          child.title = window_title(p.hwnd);
          RECT cr {};
          POINT co {0, 0};
          if (GetClientRect(p.hwnd, &cr) && ClientToScreen(p.hwnd, &co)) child.caption = std::max(0L, static_cast<LONG>(co.y - p.rect.top));
        }
        out.popups.push_back(std::move(child));
      }
      if (app_window_layout::rounded_corners_os() && !IsZoomed(hwnd)) {
        const UINT dpi = GetDpiForWindow(hwnd);
        out.radius = static_cast<int>(8 * (dpi ? dpi : 96) / 96);
      }
    }
    if (const UINT dpi = GetDpiForWindow(hwnd)) out.dpi = static_cast<int>(dpi);
    // FNV-1a over the fields the client reacts to.
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::uint64_t v) {
      for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xff;
        h *= 1099511628211ull;
      }
    };
    for (int v : out.rect) mix(static_cast<std::uint32_t>(v));
    mix(static_cast<std::uint32_t>(out.anchor[0]));
    mix(static_cast<std::uint32_t>(out.anchor[1]));
    for (const auto &c : out.popups) {
      mix(c.handle);
      for (int v : c.rect) mix(static_cast<std::uint32_t>(v));
      for (int v : c.canvas) mix(static_cast<std::uint32_t>(v));
      mix(c.dialog);
      mix(c.caption);
      for (unsigned char ch : c.title) mix(ch);
    }
    mix(out.radius);
    mix(out.dpi);
    mix(out.caption);
    mix(out.focused);
    for (unsigned char c : out.state) mix(c);
    for (unsigned char c : out.title) mix(c);
    out.etag = h ? h : 1;
    return out;
  }


  namespace {
    std::mutex g_slots_mutex;
    std::map<std::uint64_t, std::vector<popup_slot_t>> g_slots;  ///< target hwnd -> slots
    std::map<std::uint64_t, POINT> g_session_origins;  ///< target hwnd -> pinned origin (for input remapping)
    struct aim_t {
      HWND intended = nullptr;
      POINT point {};
      std::vector<HWND> family;
    };
    aim_t g_aim;  ///< what the last mapped pointer position aimed at
  }  // namespace

  void set_popup_slots(std::uint64_t target, std::vector<popup_slot_t> slots) {
    std::lock_guard lock(g_slots_mutex);
    g_slots[target] = std::move(slots);
  }

  std::vector<popup_slot_t> popup_slots(std::uint64_t target) {
    std::lock_guard lock(g_slots_mutex);
    auto it = g_slots.find(target);
    return it == g_slots.end() ? std::vector<popup_slot_t> {} : it->second;
  }

  bool remap_canvas_point(int origin_x, int origin_y, int cx, int cy, int &hx, int &hy) {
    HWND intended = nullptr;
    std::vector<HWND> family;  // the window and its popups: the only windows we reorder
    {
      std::lock_guard lock(g_slots_mutex);
      for (const auto &[target, origin] : g_session_origins) {
        if (origin.x != origin_x || origin.y != origin_y) continue;
        intended = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(target));
        family.push_back(intended);
        hx = origin.x + cx;
        hy = origin.y + cy;
        if (auto it = g_slots.find(target); it != g_slots.end()) {
          for (auto sl = it->second.rbegin(); sl != it->second.rend(); ++sl) {
            family.push_back(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(sl->handle)));
            if (intended == reinterpret_cast<HWND>(static_cast<std::uintptr_t>(target)) &&
                cx >= sl->canvas[0] && cy >= sl->canvas[1] && cx < sl->canvas[0] + sl->canvas[2] && cy < sl->canvas[1] + sl->canvas[3]) {
              intended = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(sl->handle));
              hx = sl->host[0] + (cx - sl->canvas[0]);
              hy = sl->host[1] + (cy - sl->canvas[1]);
            }
          }
        }
        break;
      }
    }
    if (!intended) return false;
    // Remember what the pointer aims at; the button path reorders (once per
    // press), never the move path, which runs at the mouse rate.
    {
      std::lock_guard lock(g_slots_mutex);
      g_aim = {intended, {hx, hy}, std::move(family)};
    }
    return true;
  }

  void raise_aimed_window() {
    aim_t aim;
    {
      std::lock_guard lock(g_slots_mutex);
      aim = g_aim;
    }
    if (!aim.intended || !IsWindow(aim.intended)) return;
    // A window disabled by its modal dialog must stay below that dialog: the
    // click will be ignored anyway, and raising it would only hide the dialog.
    if (!IsWindowEnabled(aim.intended)) return;
    // On the host the window and its dialogs overlap freely (their positions
    // mean nothing to the client), so make sure the window the client aimed
    // at is the one on top at that point; only ever reorder within the family.
    if (HWND under = WindowFromPoint(aim.point)) {
      under = GetAncestor(under, GA_ROOT);
      if (under && under != aim.intended && std::find(aim.family.begin(), aim.family.end(), under) != aim.family.end()) {
        SetWindowPos(aim.intended, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
      }
    }
  }

  namespace {
    int monitor_refresh_hz(HMONITOR mon) {
      MONITORINFOEXW mi {};
      mi.cbSize = sizeof(mi);
      if (!mon || !GetMonitorInfoW(mon, &mi)) return 0;
      DEVMODEW mode {};
      mode.dmSize = sizeof(mode);
      if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0;
      return static_cast<int>(mode.dmDisplayFrequency);
    }

    struct fast_display_state_t {
      std::mutex mutex;
      int leases = 0;
      bool created = false;  ///< we created the virtual display below
      unsigned idle_generation = 0;  ///< bumps whenever the last lease goes away
      GUID guid {};
      std::wstring device;  ///< GDI device name of the fast display ("\\.\\DISPLAYn")
    };
    fast_display_state_t &fast_display_state() {
      static fast_display_state_t state;
      return state;
    }

    /** Attach a (detached) display to the desktop at the given mode, or
     *  switch an attached one to that mode: right of everything else. */
    void attach_display(const std::wstring &device, UINT w, UINT h, int fps) {
      DEVMODEW current {};
      current.dmSize = sizeof(current);
      const bool attached = EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &current) && current.dmPelsWidth > 0;
      DEVMODEW want {};
      want.dmSize = sizeof(want);
      want.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
      want.dmPelsWidth = w;
      want.dmPelsHeight = h;
      want.dmDisplayFrequency = static_cast<DWORD>(fps);
      if (!attached) {
        want.dmFields |= DM_POSITION;
        want.dmPosition.x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        want.dmPosition.y = 0;
      }
      const LONG set = ChangeDisplaySettingsExW(device.c_str(), &want, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
      const LONG commit = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
      BOOST_LOG(info) << "App windows: " << (attached ? "retuned" : "attached") << " display " << utf_utils::to_utf8(device)
                      << " to " << w << 'x' << h << '@' << fps << " (set=" << set << ", commit=" << commit << ')';
    }

    void log_monitors(const char *when) {
      EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFOEXW mi {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
          BOOST_LOG(info) << "App windows: monitor " << utf_utils::to_utf8(mi.szDevice) << ' ' << (mi.rcMonitor.right - mi.rcMonitor.left) << 'x'
                          << (mi.rcMonitor.bottom - mi.rcMonitor.top) << " @" << monitor_refresh_hz(mon) << " Hz at " << mi.rcMonitor.left << ',' << mi.rcMonitor.top
                          << " (" << reinterpret_cast<const char *>(lp) << ')';
        }
        return TRUE;
      },
                          reinterpret_cast<LPARAM>(const_cast<char *>(when)));
    }

    /** Any monitor that refreshes at least `fps`; prefers our own virtual display. */
    HMONITOR find_fast_monitor(int fps, const std::wstring &prefer_device) {
      struct ctx_t {
        int fps;
        const std::wstring &prefer;
        HMONITOR best = nullptr;
        HMONITOR preferred = nullptr;
      } ctx {fps, prefer_device};
      EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<ctx_t *>(lp);
        MONITORINFOEXW mi {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi)) return TRUE;
        if (monitor_refresh_hz(mon) + 1 >= c->fps) {
          if (!c->prefer.empty() && c->prefer == mi.szDevice) c->preferred = mon;
          if (!c->best) c->best = mon;
        }
        return TRUE;
      },
                          reinterpret_cast<LPARAM>(&ctx));
      return ctx.preferred ? ctx.preferred : ctx.best;
    }

    struct fast_display_lease_t {
      HWND hwnd = nullptr;
      RECT original {};  ///< raw window rect before the move
      bool moved = false;
      ~fast_display_lease_t() {
        if (moved && IsWindow(hwnd)) {
          SetWindowPos(hwnd, nullptr, original.left, original.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
        auto &st = fast_display_state();
        std::lock_guard lock(st.mutex);
        if (--st.leases <= 0) {
          st.leases = 0;
          if (st.created) {
            // Creating the display costs seconds and window sessions come and
            // go in bursts: keep it for a while and only then remove it (off
            // this thread: the teardown holds the stream lifecycle lock).
            const auto generation = ++st.idle_generation;
            std::thread([generation] {
              std::this_thread::sleep_for(std::chrono::minutes(10));
              auto &st = fast_display_state();
              GUID guid {};
              {
                std::lock_guard lock(st.mutex);
                if (st.leases > 0 || !st.created || st.idle_generation != generation) return;
                st.created = false;
                st.device.clear();
                guid = st.guid;
              }
              if (VDISPLAY::removeVirtualDisplay(guid)) {
                BOOST_LOG(info) << "App windows: removed the idle fast virtual display";
              } else {
                BOOST_LOG(warning) << "App windows: failed to remove the fast virtual display";
              }
            }).detach();
          }
        }
      }
    };
  }  // namespace

  std::shared_ptr<void> lease_fast_display(std::uint64_t handle, int fps) {
    auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    if (!IsWindow(hwnd) || fps <= 0) return {};
    HMONITOR current = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    const int current_hz = monitor_refresh_hz(current);
    if (current_hz + 1 >= fps) return {};  // already fast enough
    auto &st = fast_display_state();
    std::lock_guard lock(st.mutex);
    HMONITOR target = find_fast_monitor(fps, st.device);
    if (!target && st.created) {
      // Gone (the end-of-stream cleanup removes every virtual display).
      st.created = false;
      st.device.clear();
    }
    if (!target) {
      // Create one the size of the window's monitor, at the window's DPI.
      MONITORINFOEXW mi {};
      mi.cbSize = sizeof(mi);
      GetMonitorInfoW(current, &mi);
      const UINT w = std::max<LONG>(640, mi.rcMonitor.right - mi.rcMonitor.left);
      const UINT h = std::max<LONG>(480, mi.rcMonitor.bottom - mi.rcMonitor.top);
      const UINT dpi = GetDpiForWindow(hwnd);
      if (!VDISPLAY::ensure_driver_is_ready()) {
        BOOST_LOG(warning) << "App windows: no virtual display driver; the window stays on its " << current_hz << " Hz display";
        return {};
      }
      const auto uuid = VDISPLAY::virtualDisplayUuidFromStableId("vibeshine-app-windows");
      std::memcpy(&st.guid, uuid.b8, sizeof(st.guid));
      VDISPLAY::setWatchdogFeedingEnabled(true);
      auto created = VDISPLAY::createVirtualDisplay("app-windows", "App windows", nullptr, w, h, static_cast<uint32_t>(fps), st.guid,
                                                    0, false, 1, std::nullopt, false, true, true, false);
      if (!created) {
        BOOST_LOG(warning) << "App windows: could not create a " << fps << " Hz virtual display; the window stays on its " << current_hz << " Hz display";
        return {};
      }
      st.created = true;
      if (created->display_name) st.device = *created->display_name;
      if (created->monitor_device_path && dpi) {
        (void) VDISPLAY::set_display_scale_percent(*created->monitor_device_path, dpi * 100 / 96);
      }
      // The monitor shows up a moment later; if Windows leaves it detached
      // (nothing applied a topology), attach it ourselves.
      for (int i = 0; i < 160 && !target; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        target = find_fast_monitor(fps, st.device);
        if (!target && (i == 20 || i == 80) && !st.device.empty()) {
          log_monitors("before attach");
          attach_display(st.device, w, h, fps);
        }
      }
      if (!target) {
        log_monitors("gave up");
        BOOST_LOG(warning) << "App windows: the virtual display did not come up; the window stays where it is";
        VDISPLAY::removeVirtualDisplay(st.guid);
        st.created = false;
        st.device.clear();
        return {};
      }
      BOOST_LOG(info) << "App windows: created a " << w << 'x' << h << '@' << fps << " virtual display for window sessions";
    }
    auto lease = std::make_shared<fast_display_lease_t>();
    lease->hwnd = hwnd;
    ++st.leases;
    MONITORINFOEXW tmi {};
    tmi.cbSize = sizeof(tmi);
    GetMonitorInfoW(target, &tmi);
    if (st.device.empty()) st.device = tmi.szDevice;
    if (target != current && GetWindowRect(hwnd, &lease->original)) {
      // Keep the window's offset from its monitor's origin, clamped so it
      // stays inside the fast display's work area.
      MONITORINFOEXW cmi {};
      cmi.cbSize = sizeof(cmi);
      GetMonitorInfoW(current, &cmi);
      const LONG ww = lease->original.right - lease->original.left, wh = lease->original.bottom - lease->original.top;
      LONG x = tmi.rcWork.left + (lease->original.left - cmi.rcMonitor.left);
      LONG y = tmi.rcWork.top + (lease->original.top - cmi.rcMonitor.top);
      x = std::max(tmi.rcWork.left, std::min(x, tmi.rcWork.right - ww));
      y = std::max(tmi.rcWork.top, std::min(y, tmi.rcWork.bottom - wh));
      if (SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS)) {
        lease->moved = true;
        // The move is asynchronous; whoever pins the window next must see it
        // on the new display, so wait (briefly) for it to land.
        for (int i = 0; i < 100 && MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != target; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        BOOST_LOG(info) << "App windows: moved window 0x" << std::hex << handle << std::dec << " to the " << monitor_refresh_hz(target) << " Hz display for a " << fps << " fps session"
                        << (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) == target ? "" : " (move still pending)");
      }
    }
    return lease;
  }


  namespace {
    struct pin_t {
      HWND hwnd = nullptr;
      POINT origin {};  ///< pinned frame-rect origin
      POINT before {};  ///< where the window was before the session parked it
      int canvas_w = 0, canvas_h = 0;  ///< canvas the client can show (0 = unknown)
      std::atomic<bool> stop {false};
    };
    std::mutex g_pins_mutex;
    std::map<HWND, std::shared_ptr<pin_t>> g_pins;

    void move_frame_to(HWND hwnd, int x, int y) {
      RECT frame {}, win {};
      int dx = 0, dy = 0;
      if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) && GetWindowRect(hwnd, &win)) {
        dx = frame.left - win.left;
        dy = frame.top - win.top;
      }
      // Asynchronous: never wait on the window's thread (it may be inside a
      // modal loop or waiting on us), so nothing here can deadlock a teardown.
      SetWindowPos(hwnd, nullptr, x - dx, y - dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }

    void set_pin_origin(HWND hwnd, int x, int y) {
      std::lock_guard lock(g_pins_mutex);
      if (auto it = g_pins.find(hwnd); it != g_pins.end()) {
        it->second->origin = {x, y};
      }
    }
  }  // namespace

  std::shared_ptr<void> pin_position(std::uint64_t handle, int canvas_w, int canvas_h) {
    auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    int rect[4];
    if (!frame_rect(handle, rect)) return {};
    const POINT before {rect[0], rect[1]};
    // Park the window at the top-left of its display's work area, once. The
    // client shows it wherever it likes; on the host the only thing that
    // matters is that every pixel of it stays reachable by injected input,
    // and a window no larger than the display always fits from the corner.
    if (!IsIconic(hwnd) && !IsZoomed(hwnd)) {
      if (HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)) {
        MONITORINFO mi {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
          // A spot of its own per session (16 px apart): the input path finds
          // the session by this origin.
          int taken = 0;
          {
            std::lock_guard lock(g_pins_mutex);
            for (const auto &[other, pin] : g_pins) {
              if (pin->origin.y == mi.rcWork.top && pin->origin.x >= mi.rcWork.left && pin->origin.x < mi.rcWork.left + 16 * 64) ++taken;
            }
          }
          const int x = mi.rcWork.left + 16 * taken, y = mi.rcWork.top;
          if (rect[0] != x || rect[1] != y) {
            move_frame_to(hwnd, x, y);
            // The move is asynchronous: pin the intended spot, not a re-read.
            rect[0] = x;
            rect[1] = y;
            BOOST_LOG(info) << "App windows: parked window 0x" << std::hex << handle << std::dec << " at " << rect[0] << ',' << rect[1];
          }
        }
      }
    }
    auto pin = std::make_shared<pin_t>();
    pin->hwnd = hwnd;
    pin->origin = {rect[0], rect[1]};
    pin->before = before;
    pin->canvas_w = canvas_w;
    pin->canvas_h = canvas_h;
    {
      std::lock_guard lock(g_pins_mutex);
      g_pins[hwnd] = pin;
    }
    {
      std::lock_guard lock(g_slots_mutex);
      g_session_origins[handle] = pin->origin;
    }
    std::thread([pin] {
      unsigned restores = 0;
      auto last_log = std::chrono::steady_clock::now() - std::chrono::seconds(2);
      while (!pin->stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        if (!IsWindow(pin->hwnd) || IsIconic(pin->hwnd) || IsZoomed(pin->hwnd)) continue;
        RECT frame {};
        if (FAILED(DwmGetWindowAttribute(pin->hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) continue;
        const POINT want = pin->origin;
        if (!MonitorFromPoint(POINT {want.x + 8, want.y + 8}, MONITOR_DEFAULTTONULL)) {
          // The display we parked on is gone (virtual display removed):
          // re-park on whatever the window landed on.
          if (HMONITOR mon = MonitorFromWindow(pin->hwnd, MONITOR_DEFAULTTONEAREST)) {
            MONITORINFO mi {};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
              pin->origin = {mi.rcWork.left, mi.rcWork.top};
              std::lock_guard lock(g_slots_mutex);
              g_session_origins[reinterpret_cast<std::uintptr_t>(pin->hwnd)] = pin->origin;
              BOOST_LOG(info) << "App windows: window 0x" << std::hex << reinterpret_cast<std::uintptr_t>(pin->hwnd) << std::dec << " re-parked at " << pin->origin.x << ',' << pin->origin.y;
              continue;
            }
          }
        }
        if (frame.left != want.x || frame.top != want.y) {
          move_frame_to(pin->hwnd, want.x, want.y);
          ++restores;
          const auto now = std::chrono::steady_clock::now();
          if (now - last_log >= std::chrono::seconds(1)) {
            BOOST_LOG(info) << "App windows: window 0x" << std::hex << reinterpret_cast<std::uintptr_t>(pin->hwnd) << std::dec
                            << " was moved to " << frame.left << ',' << frame.top << " by something else; put back (" << restores << " so far)";
            last_log = now;
          }
        }
      }
      // Give the host user their window back where it was.
      if (IsWindow(pin->hwnd) && !IsIconic(pin->hwnd) && !IsZoomed(pin->hwnd) &&
          (pin->before.x != pin->origin.x || pin->before.y != pin->origin.y)) {
        move_frame_to(pin->hwnd, pin->before.x, pin->before.y);
      }
    }).detach();
    struct token_t {
      std::shared_ptr<pin_t> pin;
      explicit token_t(std::shared_ptr<pin_t> p):
          pin(std::move(p)) {}
      token_t(const token_t &) = delete;
      token_t &operator=(const token_t &) = delete;
      ~token_t() {
        // The watcher thread exits on its own (it is detached) and moves the
        // window back where it was; nothing here waits on anything.
        {
          std::lock_guard lock(g_pins_mutex);
          if (auto it = g_pins.find(pin->hwnd); it != g_pins.end() && it->second == pin) g_pins.erase(it);
        }
        {
          std::lock_guard lock(g_slots_mutex);
          g_session_origins.erase(reinterpret_cast<std::uintptr_t>(pin->hwnd));
          g_slots.erase(reinterpret_cast<std::uintptr_t>(pin->hwnd));
        }
        pin->stop.store(true, std::memory_order_release);
      }
    };
    BOOST_LOG(info) << "App windows: pinned window 0x" << std::hex << handle << std::dec << " at " << rect[0] << ',' << rect[1];
    return std::make_shared<token_t>(pin);
  }

  bool pinned_position(std::uint64_t handle, int &x, int &y) {
    auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    std::lock_guard lock(g_pins_mutex);
    if (auto it = g_pins.find(hwnd); it != g_pins.end()) {
      x = it->second->origin.x;
      y = it->second->origin.y;
      return true;
    }
    return false;
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
    const POINT screen {frame[0] + x, frame[1] + y};
    const LPARAM pt = MAKELPARAM(static_cast<WORD>(screen.x), static_cast<WORD>(screen.y));
    DWORD_PTR result = HTNOWHERE;
    if (!SendMessageTimeoutW(hwnd, WM_NCHITTEST, 0, pt, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &result)) return "none";
    if (static_cast<LRESULT>(result) == HTCLIENT) {
      // Apps that draw their own title bar in a child window (XAML islands:
      // Explorer, many WinUI apps) answer HTCLIENT at the top level and put
      // the real caption/button hit-test in the island's window. Ask the
      // windows under the point, deepest first.
      HWND child = hwnd;
      std::vector<HWND> chain;
      for (int depth = 0; depth < 8; ++depth) {
        POINT client = screen;
        ScreenToClient(child, &client);
        HWND next = RealChildWindowFromPoint(child, client);
        if (!next || next == child) break;
        chain.push_back(next);
        child = next;
      }
      for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        DWORD_PTR r = HTNOWHERE;
        if (SendMessageTimeoutW(*it, WM_NCHITTEST, 0, pt, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &r) &&
            r != HTCLIENT && r != HTNOWHERE && r != static_cast<DWORD_PTR>(HTTRANSPARENT) && r != HTERROR) {
          result = r;
          break;
        }
      }
    }
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
        // request worker. SwitchToThisWindow is the fallback the shell uses.
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
          SwitchToThisWindow(focus, TRUE);
          ok = GetForegroundWindow() == focus;
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
        set_pin_origin(hwnd, x, y);
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

  layout_t window_layout(std::uint64_t, int, int) {
    return {};
  }

  std::shared_ptr<void> lease_fast_display(std::uint64_t, int) {
    return {};
  }

  std::shared_ptr<void> pin_position(std::uint64_t, int, int) {
    return {};
  }

  bool pinned_position(std::uint64_t, int &, int &) {
    return false;
  }

  void set_popup_slots(std::uint64_t, std::vector<popup_slot_t>) {}

  std::vector<popup_slot_t> popup_slots(std::uint64_t) {
    return {};
  }

  bool remap_canvas_point(int, int, int, int, int &, int &) {
    return false;
  }

  void raise_aimed_window() {}

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
