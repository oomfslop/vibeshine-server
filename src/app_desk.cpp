/**
 * @file src/app_desk.cpp
 * @brief One private virtual display per client that mirrors the client's screen.
 */
#include "app_desk.h"

#include "host_windows.h"
#include "logging.h"

#ifdef _WIN32
  #include <windows.h>
  #include <dwmapi.h>

  #include <algorithm>
  #include <chrono>
  #include <cstring>
  #include <map>
  #include <mutex>
  #include <thread>

  #include "platform/windows/app_window_layout.h"
  #include "platform/windows/utf_utils.h"
  #include "platform/windows/virtual_display.h"

namespace app_desk {

  namespace {

    struct desk_state_t {
      GUID guid {};
      std::wstring device;  ///< "\\\\.\\DISPLAYn"
      std::wstring monitor_path;  ///< device path the DPI scale is set through
      desk_t info;
      std::map<HWND, POINT> adopted;  ///< window -> frame origin before adoption
      bool open = false;  ///< a client is using it (false = idle, kept for a while)
      std::chrono::steady_clock::time_point idle_since {};
      unsigned idle_generation = 0;
    };

    std::mutex g_mutex;
    std::map<std::string, desk_state_t> g_desks;  ///< client uuid -> desk

    int monitor_refresh_hz(const wchar_t *device) {
      DEVMODEW mode {};
      mode.dmSize = sizeof(mode);
      if (!EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &mode)) return 0;
      return static_cast<int>(mode.dmDisplayFrequency);
    }

    /** The monitor whose GDI device name is `device`, if attached. */
    HMONITOR find_monitor(const std::wstring &device, RECT *rect_out = nullptr) {
      struct ctx_t {
        const std::wstring &device;
        HMONITOR found = nullptr;
        RECT rect {};
      } ctx {device};
      EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<ctx_t *>(lp);
        MONITORINFOEXW mi {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi) && c->device == mi.szDevice) {
          c->found = mon;
          c->rect = mi.rcMonitor;
          return FALSE;
        }
        return TRUE;
      },
                          reinterpret_cast<LPARAM>(&ctx));
      if (ctx.found && rect_out) *rect_out = ctx.rect;
      return ctx.found;
    }

    /** Effective DPI of a monitor (0 when unknown). */
    int monitor_dpi(HMONITOR mon) {
      using fn_t = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);
      static fn_t fn = [] {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        return shcore ? reinterpret_cast<fn_t>(GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
      }();
      UINT x = 0, y = 0;
      if (!mon || !fn || FAILED(fn(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y))) return 0;
      return static_cast<int>(x);
    }

    /** Attach a (detached) display at the given mode, right of everything else,
     *  or switch an attached one to that mode. */
    void attach_display(const std::wstring &device, int w, int h, int fps) {
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
      BOOST_LOG(info) << "Desk: " << (attached ? "retuned" : "attached") << " display " << utf_utils::to_utf8(device) << " to " << w << 'x' << h << '@' << fps
                      << " (set=" << set << ", commit=" << commit << ')';
    }

    struct mode_t {
      int w = 0, h = 0, hz = 0;
    };

    /** The driver offers a fixed catalog of modes: the smallest one that
     *  covers the requested size, at the requested rate or the fastest below
     *  it (the desk is then a little larger than the client's screen, which
     *  the client simply does not use). */
    std::optional<mode_t> pick_mode(const std::wstring &device, int w, int h, int fps) {
      std::vector<mode_t> modes;
      DEVMODEW dm {};
      dm.dmSize = sizeof(dm);
      for (DWORD i = 0; EnumDisplaySettingsW(device.c_str(), i, &dm); ++i) {
        modes.push_back({static_cast<int>(dm.dmPelsWidth), static_cast<int>(dm.dmPelsHeight), static_cast<int>(dm.dmDisplayFrequency)});
      }
      if (modes.empty()) return std::nullopt;
      auto better = [&](const mode_t &a, const mode_t &b) {  // is a better than b?
        const bool fa = a.w >= w && a.h >= h, fb = b.w >= w && b.h >= h;
        if (fa != fb) return fa;
        const long long area_a = 1LL * a.w * a.h, area_b = 1LL * b.w * b.h;
        if (fa) {
          if (area_a != area_b) return area_a < area_b;  // smallest that fits
        } else if (area_a != area_b) {
          return area_a > area_b;  // nothing fits: the largest
        }
        const bool ra = a.hz >= fps, rb = b.hz >= fps;
        if (ra != rb) return ra;
        return ra ? a.hz < b.hz : a.hz > b.hz;  // at or just above the rate, else the fastest
      };
      mode_t best = modes.front();
      for (const auto &m : modes) {
        if (better(m, best)) best = m;
      }
      std::string sizes;
      int shown = 0;
      for (const auto &m : modes) {
        if (m.hz != best.hz) continue;
        if (shown++ < 24) sizes += std::to_string(m.w) + "x" + std::to_string(m.h) + " ";
      }
      BOOST_LOG(info) << "Desk: " << modes.size() << " modes; picked " << best.w << 'x' << best.h << '@' << best.hz << " for " << w << 'x' << h << '@' << fps
                      << " (sizes at " << best.hz << " Hz: " << sizes << ')';
      return best;
    }

    void move_frame_to(HWND hwnd, int x, int y) {
      RECT frame {}, win {};
      int dx = 0, dy = 0;
      if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) && GetWindowRect(hwnd, &win)) {
        dx = frame.left - win.left;
        dy = frame.top - win.top;
      }
      SetWindowPos(hwnd, nullptr, x - dx, y - dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }

    /** A window that crosses to a display of another DPI gets WM_DPICHANGED
     *  with a suggested, scaled frame; some apps (Explorer) take it, some
     *  (Notepad) rescale their content but keep the frame. Once the move has
     *  settled, give the ones that kept it the scaled frame, as a monitor
     *  drag would have. Off this thread: the window may be slow. */
    void settle_dpi_change(HWND hwnd, UINT dpi_before, RECT frame_before) {
      std::thread([hwnd, dpi_before, frame_before] {
        // The move is posted; wait for the window to be on the other DPI.
        UINT dpi_after = dpi_before;
        for (int i = 0; i < 20 && dpi_after == dpi_before; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          if (!IsWindow(hwnd)) return;
          dpi_after = GetDpiForWindow(hwnd);
        }
        if (!dpi_before || !dpi_after || dpi_after == dpi_before || IsIconic(hwnd)) {
          BOOST_LOG(debug) << "Desk: window 0x" << std::hex << reinterpret_cast<std::uintptr_t>(hwnd) << std::dec << " dpi " << dpi_before << " -> " << dpi_after << ", nothing to settle";
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let it answer WM_DPICHANGED first
        RECT frame {};
        if (!app_window_layout::frame_rect(hwnd, frame)) return;
        const LONG w0 = frame_before.right - frame_before.left, h0 = frame_before.bottom - frame_before.top;
        const LONG w1 = frame.right - frame.left, h1 = frame.bottom - frame.top;
        if (w0 <= 0 || h0 <= 0 || std::abs(w1 - w0) > w0 / 20 || std::abs(h1 - h0) > h0 / 20) return;  // it rescaled itself
        RECT win {};
        if (!GetWindowRect(hwnd, &win)) return;
        const LONG ww = MulDiv(win.right - win.left, static_cast<int>(dpi_after), static_cast<int>(dpi_before));
        const LONG wh = MulDiv(win.bottom - win.top, static_cast<int>(dpi_after), static_cast<int>(dpi_before));
        SetWindowPos(hwnd, nullptr, 0, 0, ww, wh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        BOOST_LOG(info) << "Desk: window 0x" << std::hex << reinterpret_cast<std::uintptr_t>(hwnd) << std::dec << " kept its frame across " << dpi_before << "->" << dpi_after
                        << " dpi; resized " << w1 << 'x' << h1 << " -> " << ww << 'x' << wh;
      }).detach();
    }

    std::string window_title(HWND hwnd) {
      const int length = GetWindowTextLengthW(hwnd);
      if (length <= 0) return {};
      std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
      const int copied = GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()));
      title.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
      try {
        return utf_utils::to_utf8(title);
      } catch (...) {
        return {};
      }
    }

    std::string process_name(DWORD pid) {
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!process) return {};
      wchar_t path[MAX_PATH] {};
      DWORD size = MAX_PATH;
      std::string out;
      if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        std::wstring p(path, size);
        const auto slash = p.find_last_of(L"\\/");
        try {
          out = utf_utils::to_utf8(slash == std::wstring::npos ? p : p.substr(slash + 1));
        } catch (...) {
        }
      }
      CloseHandle(process);
      return out;
    }

    /** Remove a desk's display once it has been idle long enough (detached thread). */
    void schedule_idle_removal(const std::string &client_uuid, unsigned generation) {
      std::thread([client_uuid, generation] {
        std::this_thread::sleep_for(std::chrono::minutes(10));
        GUID guid {};
        {
          std::lock_guard lock(g_mutex);
          auto it = g_desks.find(client_uuid);
          if (it == g_desks.end() || it->second.open || it->second.idle_generation != generation) return;
          guid = it->second.guid;
          g_desks.erase(it);
        }
        VDISPLAY::set_virtual_display_protected(guid, {}, false);
        if (VDISPLAY::removeVirtualDisplay(guid)) {
          BOOST_LOG(info) << "Desk: removed the idle display of client " << client_uuid;
        } else {
          BOOST_LOG(warning) << "Desk: failed to remove the idle display of client " << client_uuid;
        }
      }).detach();
    }

  }  // namespace

  desk_t open(const std::string &client_uuid, int w, int h, int fps, int dpi) {
    w = std::max(640, w & ~1);
    h = std::max(480, h & ~1);
    fps = std::clamp(fps, 24, 240);
    dpi = std::clamp(dpi, 96, 384);
    std::lock_guard lock(g_mutex);
    auto &desk = g_desks[client_uuid];
    RECT rect {};
    // Newer drivers expose exactly the requested mode; older ones only a catalog.
    const bool custom = VDISPLAY::supportsRequestedResolutionCatalog();
    bool present = !desk.device.empty() && find_monitor(desk.device, &rect);
    if (present && custom && (rect.right - rect.left != w || rect.bottom - rect.top != h)) {
      // A different size needs a display created for it: recreate.
      BOOST_LOG(info) << "Desk: recreating the display for " << w << 'x' << h;
      VDISPLAY::set_virtual_display_protected(desk.guid, desk.device, false);
      VDISPLAY::removeVirtualDisplay(desk.guid);
      desk.device.clear();
      desk.monitor_path.clear();
      present = false;
    }
    if (present) {
      if (!custom) {
        if (auto mode = pick_mode(desk.device, w, h, fps)) {
          w = mode->w;
          h = mode->h;
          fps = mode->hz;
        }
      }
      if (rect.right - rect.left != w || rect.bottom - rect.top != h || monitor_refresh_hz(desk.device.c_str()) != fps) {
        // Different size or rate: retune the existing display.
        attach_display(desk.device, w, h, fps);
        for (int i = 0; i < 100; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          if (find_monitor(desk.device, &rect) && rect.right - rect.left == w && rect.bottom - rect.top == h) break;
        }
        present = find_monitor(desk.device, &rect);
      }
    }
    if (!present) {
      if (!VDISPLAY::ensure_driver_is_ready()) {
        BOOST_LOG(warning) << "Desk: no virtual display driver";
        return {};
      }
      // A fresh monitor identity every time: Windows "remembers window
      // locations based on monitor connection" and would otherwise drag
      // windows that once sat on the desk back onto it when it reappears,
      // with placements scaled by whatever DPI it had then.
      const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      const auto uuid = VDISPLAY::virtualDisplayUuidFromStableId("vibeshine-desk-" + client_uuid + "-" + std::to_string(stamp));
      std::memcpy(&desk.guid, uuid.b8, sizeof(desk.guid));
      VDISPLAY::setWatchdogFeedingEnabled(true);
      auto created = VDISPLAY::createVirtualDisplay(("desk-" + client_uuid).c_str(), "App windows desk", nullptr,
                                                    static_cast<uint32_t>(w), static_cast<uint32_t>(h), static_cast<uint32_t>(fps), desk.guid,
                                                    0, false, 1, std::nullopt, false, true, true, custom);
      if (!created) {
        BOOST_LOG(warning) << "Desk: the virtual display driver could not create a " << w << 'x' << h << '@' << fps << " display";
        g_desks.erase(client_uuid);
        return {};
      }
      desk.device = created->display_name.value_or(L"");
      VDISPLAY::set_virtual_display_protected(desk.guid, desk.device, true);
      if (created->monitor_device_path) desk.monitor_path = *created->monitor_device_path;
      // The driver brings it up detached or at a default mode: attach it at ours.
      present = false;
      bool picked = false;
      for (int i = 0; i < 200 && !present; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!desk.device.empty() && !picked && !custom) {
          if (auto mode = pick_mode(desk.device, w, h, fps)) {
            w = mode->w;
            h = mode->h;
            fps = mode->hz;
            picked = true;
          }
        }
        if (!desk.device.empty() && (i == 10 || i == 60)) attach_display(desk.device, w, h, fps);
        present = !desk.device.empty() && find_monitor(desk.device, &rect) && rect.right - rect.left == w && rect.bottom - rect.top == h &&
                  monitor_refresh_hz(desk.device.c_str()) + 1 >= fps;
      }
      if (!present) {
        BOOST_LOG(warning) << "Desk: the display did not come up at " << w << 'x' << h << '@' << fps;
        VDISPLAY::set_virtual_display_protected(desk.guid, desk.device, false);
        VDISPLAY::removeVirtualDisplay(desk.guid);
        g_desks.erase(client_uuid);
        return {};
      }
      BOOST_LOG(info) << "Desk: created " << utf_utils::to_utf8(desk.device) << ' ' << w << 'x' << h << '@' << fps << " at " << dpi << " dpi for client " << client_uuid;
    }
    desk.open = true;
    desk.info.valid = true;
    try {
      desk.info.display = utf_utils::to_utf8(desk.device);
    } catch (...) {
    }
    desk.info.rect[0] = rect.left;
    desk.info.rect[1] = rect.top;
    desk.info.rect[2] = rect.right - rect.left;
    desk.info.rect[3] = rect.bottom - rect.top;
    desk.info.fps = fps;
    desk.info.dpi = dpi;
    // The scale can only be set on an attached display; apply it now and
    // wait for it to show, then report what the desk really runs at.
    if (monitor_dpi(find_monitor(desk.device)) != dpi && !desk.monitor_path.empty()) {
      const auto r = VDISPLAY::set_display_scale_percent(desk.monitor_path, static_cast<std::uint32_t>(dpi * 100 / 96));
      BOOST_LOG(info) << "Desk: scale " << dpi * 100 / 96 << "% requested: found=" << r.target_found << " applied=" << r.applied << " current=" << r.current_percent
                      << " recommended=" << r.recommended_percent << " status=" << r.status;
    }
    for (int i = 0; i < 30; ++i) {
      const int actual = monitor_dpi(find_monitor(desk.device));
      if (actual == dpi) break;
      if (i == 29 && actual > 0) {
        BOOST_LOG(warning) << "Desk: asked for " << dpi << " dpi, the display runs at " << actual;
        desk.info.dpi = actual;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    BOOST_LOG(info) << "Desk: open " << desk.info.display << ' ' << desk.info.rect[2] << 'x' << desk.info.rect[3] << '@' << fps << " at " << desk.info.dpi << " dpi"
                    << (custom ? " (exact mode)" : " (catalog mode)");
    return desk.info;
  }

  std::optional<desk_t> get(const std::string &client_uuid) {
    std::lock_guard lock(g_mutex);
    auto it = g_desks.find(client_uuid);
    if (it == g_desks.end() || !it->second.open) return std::nullopt;
    RECT rect {};
    if (!find_monitor(it->second.device, &rect)) return std::nullopt;
    it->second.info.rect[0] = rect.left;
    it->second.info.rect[1] = rect.top;
    return it->second.info;
  }

  void close(const std::string &client_uuid) {
    std::lock_guard lock(g_mutex);
    auto it = g_desks.find(client_uuid);
    if (it == g_desks.end()) return;
    for (const auto &[hwnd, origin] : it->second.adopted) {
      if (IsWindow(hwnd)) move_frame_to(hwnd, origin.x, origin.y);
    }
    it->second.adopted.clear();
    it->second.open = false;
    it->second.idle_since = std::chrono::steady_clock::now();
    schedule_idle_removal(client_uuid, ++it->second.idle_generation);
  }

  bool any_open() {
    std::lock_guard lock(g_mutex);
    return !g_desks.empty();
  }

  bool adopt(const std::string &client_uuid, std::uint64_t handle, int x, int y) {
    auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    if (!IsWindow(hwnd)) return false;
    std::lock_guard lock(g_mutex);
    auto it = g_desks.find(client_uuid);
    if (it == g_desks.end() || !it->second.open) return false;
    RECT rect {};
    if (!find_monitor(it->second.device, &rect)) return false;
    if (it->second.adopted.contains(hwnd)) {
      // Already on the desk: a plain move. Kept on the desk: past its edge
      // the window would land on the host's own displays (the client shows
      // the part of it that is on screen either way).
      RECT frame {};
      if (app_window_layout::frame_rect(hwnd, frame)) {
        const LONG w = frame.right - frame.left, h = frame.bottom - frame.top;
        const LONG dw = rect.right - rect.left, dh = rect.bottom - rect.top;
        x = static_cast<int>(std::max<LONG>(0, std::min<LONG>(x, std::max<LONG>(0, dw - w))));
        y = static_cast<int>(std::max<LONG>(0, std::min<LONG>(y, std::max<LONG>(0, dh - h))));
      }
      move_frame_to(hwnd, rect.left + x, rect.top + y);
      return true;
    }
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    RECT frame {};
    if (!app_window_layout::frame_rect(hwnd, frame)) return false;
    it->second.adopted[hwnd] = {frame.left, frame.top};
    settle_dpi_change(hwnd, GetDpiForWindow(hwnd), frame);
    // First time: keep it inside the desk.
    const LONG w = frame.right - frame.left, h = frame.bottom - frame.top;
    const LONG dw = rect.right - rect.left, dh = rect.bottom - rect.top;
    x = static_cast<int>(std::max<LONG>(0, std::min<LONG>(x, dw - std::min(w, dw))));
    y = static_cast<int>(std::max<LONG>(0, std::min<LONG>(y, dh - std::min(h, dh))));
    move_frame_to(hwnd, rect.left + x, rect.top + y);
    return true;
  }

  bool release(const std::string &client_uuid, std::uint64_t handle) {
    auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    std::lock_guard lock(g_mutex);
    auto it = g_desks.find(client_uuid);
    if (it == g_desks.end()) return false;
    auto a = it->second.adopted.find(hwnd);
    if (a == it->second.adopted.end()) return false;
    if (IsWindow(hwnd)) {
      RECT frame {};
      if (app_window_layout::frame_rect(hwnd, frame)) settle_dpi_change(hwnd, GetDpiForWindow(hwnd), frame);
      move_frame_to(hwnd, a->second.x, a->second.y);
    }
    it->second.adopted.erase(a);
    return true;
  }

  layout_t layout(const std::string &client_uuid) {
    layout_t out;
    RECT desk {};
    std::vector<HWND> adopted;
    {
      std::lock_guard lock(g_mutex);
      auto it = g_desks.find(client_uuid);
      if (it == g_desks.end() || !it->second.open || !find_monitor(it->second.device, &desk)) return out;
      for (const auto &[hwnd, origin] : it->second.adopted) adopted.push_back(hwnd);
    }
    out.valid = true;
    struct raw_t {
      HWND hwnd;
      RECT rect;
      LONG style, ex_style;
      HWND owner;
      DWORD pid, tid;
    };
    struct ctx_t {
      RECT desk;
      std::vector<raw_t> top_first;
    } ctx {desk, {}};
    const DWORD self = GetCurrentProcessId();
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
      auto *c = reinterpret_cast<ctx_t *>(lp);
      if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || app_window_layout::is_cloaked(hwnd)) return TRUE;
      RECT r {};
      if (!app_window_layout::frame_rect(hwnd, r)) return TRUE;
      RECT x {};
      if (!IntersectRect(&x, &r, &c->desk)) return TRUE;
      wchar_t cls[64] {};
      GetClassNameW(hwnd, cls, 64);
      static const wchar_t *shell[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow", L"IME"};
      for (auto *s : shell) {
        if (wcscmp(cls, s) == 0) return TRUE;
      }
      raw_t raw {hwnd, r, GetWindowLongW(hwnd, GWL_STYLE), GetWindowLongW(hwnd, GWL_EXSTYLE), GetWindow(hwnd, GW_OWNER), 0, 0};
      raw.tid = GetWindowThreadProcessId(hwnd, &raw.pid);
      c->top_first.push_back(raw);
      return TRUE;
    },
                reinterpret_cast<LPARAM>(&ctx));
    const HWND foreground = GetForegroundWindow();
    const bool rounded = app_window_layout::rounded_corners_os();
    // Bottom-most first
    std::vector<raw_t> windows(ctx.top_first.rbegin(), ctx.top_first.rend());
    for (std::size_t i = 0; i < windows.size(); ++i) {
      const auto &w = windows[i];
      if (w.pid == self) continue;
      const bool captioned = (w.style & WS_CAPTION) == WS_CAPTION;
      HWND owner = w.owner;
      if (!captioned && !owner) {
        // Unowned popup (Win32 menu, combo list): belongs to the top-level
        // window of its thread, if one is on the desk.
        for (const auto &o : windows) {
          if (o.hwnd != w.hwnd && o.tid == w.tid && (o.style & WS_CAPTION) == WS_CAPTION && !o.owner) {
            owner = o.hwnd;
            break;
          }
        }
      }
      if (!captioned) {
        // Inside its owner it is already part of the owner's picture.
        if (owner) {
          for (const auto &o : windows) {
            if (o.hwnd == owner) {
              RECT x {};
              if (w.rect.left >= o.rect.left && w.rect.top >= o.rect.top && w.rect.right <= o.rect.right && w.rect.bottom <= o.rect.bottom) {
                owner = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1));  // marker: inline
              }
              (void) x;
              break;
            }
          }
          if (owner == reinterpret_cast<HWND>(static_cast<std::uintptr_t>(1))) continue;
        }
      }
      window_t win;
      win.handle = reinterpret_cast<std::uintptr_t>(w.hwnd);
      win.owner = reinterpret_cast<std::uintptr_t>(owner);
      win.pid = w.pid;
      win.process = process_name(w.pid);
      win.title = window_title(w.hwnd);
      win.kind = !captioned ? "popup" : (w.owner ? "dialog" : "window");
      // Desk-relative, unclipped: a window the client pushed partly off its
      // screen hangs off the desk by the same amount.
      win.rect[0] = static_cast<int>(w.rect.left - desk.left);
      win.rect[1] = static_cast<int>(w.rect.top - desk.top);
      win.rect[2] = static_cast<int>(w.rect.right - w.rect.left);
      win.rect[3] = static_cast<int>(w.rect.bottom - w.rect.top);
      win.state = IsZoomed(w.hwnd) ? "maximized" : "normal";
      win.focused = w.hwnd == foreground;
      if (captioned) {
        RECT cr {};
        POINT co {0, 0};
        if (GetClientRect(w.hwnd, &cr) && ClientToScreen(w.hwnd, &co)) win.caption = static_cast<int>(std::max(0L, co.y - w.rect.top));
      }
      if (rounded && !IsZoomed(w.hwnd)) {
        const UINT dpi = GetDpiForWindow(w.hwnd);
        win.radius = static_cast<int>(8 * (dpi ? dpi : 96) / 96);
      }
      win.z = static_cast<int>(out.windows.size());
      win.icon_key = host_windows::icon_key(win.handle);
      out.windows.push_back(std::move(win));
    }
    // Adopted windows that are minimized are off the desk but still the
    // client's: list them so their client windows live on (minimized too).
    for (HWND hwnd : adopted) {
      if (!IsWindow(hwnd) || !IsIconic(hwnd)) continue;
      WINDOWPLACEMENT placement {};
      placement.length = sizeof(placement);
      if (!GetWindowPlacement(hwnd, &placement)) continue;
      const auto &r = placement.rcNormalPosition;
      window_t win;
      win.handle = reinterpret_cast<std::uintptr_t>(hwnd);
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      win.pid = pid;
      win.process = process_name(pid);
      win.title = window_title(hwnd);
      win.kind = "window";
      win.rect[0] = static_cast<int>(r.left - desk.left);
      win.rect[1] = static_cast<int>(r.top - desk.top);
      win.rect[2] = static_cast<int>(r.right - r.left);
      win.rect[3] = static_cast<int>(r.bottom - r.top);
      win.state = "minimized";
      win.radius = rounded ? 8 : 0;
      win.z = -1;
      win.icon_key = host_windows::icon_key(win.handle);
      out.windows.insert(out.windows.begin(), std::move(win));
    }
    for (std::size_t i = 0; i < out.windows.size(); ++i) out.windows[i].z = static_cast<int>(i);
    // FNV-1a over what the client reacts to
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::uint64_t v) {
      for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xff;
        h *= 1099511628211ull;
      }
    };
    for (const auto &w : out.windows) {
      mix(w.handle);
      mix(w.owner);
      for (int v : w.rect) mix(static_cast<std::uint32_t>(v));
      mix(w.focused);
      mix(w.caption);
      mix(w.radius);
      mix(w.z);
      for (unsigned char c : w.state) mix(c);
      for (unsigned char c : w.title) mix(c);
      for (unsigned char c : w.kind) mix(c);
    }
    out.etag = h ? h : 1;
    return out;
  }

}  // namespace app_desk

#else

namespace app_desk {
  desk_t open(const std::string &, int, int, int, int) {
    return {};
  }

  std::optional<desk_t> get(const std::string &) {
    return std::nullopt;
  }

  void close(const std::string &) {}

  bool any_open() {
    return false;
  }

  bool adopt(const std::string &, std::uint64_t, int, int) {
    return false;
  }

  bool release(const std::string &, std::uint64_t) {
    return false;
  }

  layout_t layout(const std::string &) {
    return {};
  }
}  // namespace app_desk

#endif
