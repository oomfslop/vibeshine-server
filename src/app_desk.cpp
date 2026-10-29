/**
 * @file src/app_desk.cpp
 * @brief One private virtual display per client that mirrors the client's screen.
 */
#include "app_desk.h"

#include "host_windows.h"
#include "logging.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
  #include <dwmapi.h>

  #include <shellapi.h>

  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <memory>
  #include <cstring>
  #include <map>
  #include <mutex>
  #include <thread>

  #include <functional>

  #include "platform/windows/app_window_layout.h"
  #include "platform/windows/desk_atlas.h"
  #include "platform/windows/misc.h"
  #include "platform/windows/utf_utils.h"
  #include "platform/windows/virtual_display.h"

namespace app_desk {

  namespace {

    struct vdisplay_t {
      GUID guid {};
      std::wstring device;  ///< "\\\\.\\DISPLAYn"
      std::wstring monitor_path;  ///< device path the DPI scale is set through
    };

    struct desk_state_t {
      GUID guid {};
      std::wstring device;  ///< "\\\\.\\DISPLAYn"
      std::wstring monitor_path;  ///< device path the DPI scale is set through
      vdisplay_t atlas;  ///< the display the session captures: thumbnails of the desk's windows
      std::shared_ptr<desk_atlas::atlas_t> atlas_window;
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
    /** The host's own displays: every monitor but the desks' and atlases'. */
    std::vector<RECT> own_monitors(const std::vector<std::wstring> &exclude) {
      struct ctx_t {
        const std::vector<std::wstring> &exclude;
        std::vector<RECT> out;
      } ctx {exclude, {}};
      EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<ctx_t *>(lp);
        MONITORINFOEXW mi {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi)) return TRUE;
        for (const auto &d : c->exclude) {
          if (!d.empty() && d == mi.szDevice) return TRUE;
        }
        c->out.push_back(mi.rcMonitor);
        return TRUE;
      },
                          reinterpret_cast<LPARAM>(&ctx));
      return ctx.out;
    }

    /** Where the desk goes: under the bottom-most of the host's own
     *  displays, aligned to its left edge. Only its top edge then touches
     *  another display, and a window the client lets hang off the desk's
     *  left, right or bottom (as far as macOS lets a window go) stays on the
     *  desk alone: beside a real display it would cross onto it and take its
     *  DPI. Nothing goes above the desk's top (macOS keeps windows below its
     *  menu bar). */
    POINT desk_position(const std::vector<std::wstring> &exclude) {
      const auto monitors = own_monitors(exclude);
      if (monitors.empty()) return {0, 0};
      const RECT *bottom_most = &monitors.front();
      for (const auto &m : monitors) {
        if (m.bottom > bottom_most->bottom) bottom_most = &m;
      }
      return {bottom_most->left, bottom_most->bottom};
    }

    /** Where the atlas goes: right of the right-most of the host's own
     *  displays, at its top, away from the desk (under them): no window of
     *  the desk crosses onto it. */
    POINT atlas_position(const std::vector<std::wstring> &exclude) {
      const auto monitors = own_monitors(exclude);
      if (monitors.empty()) return {0, 0};
      const RECT *right_most = &monitors.front();
      for (const auto &m : monitors) {
        if (m.right > right_most->right) right_most = &m;
      }
      return {right_most->right, right_most->top};
    }

    void attach_display(const std::wstring &device, int w, int h, int fps, POINT at) {
      DEVMODEW current {};
      current.dmSize = sizeof(current);
      const bool attached = EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &current) && current.dmPelsWidth > 0;
      DEVMODEW want {};
      want.dmSize = sizeof(want);
      want.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_POSITION;
      want.dmPelsWidth = w;
      want.dmPelsHeight = h;
      want.dmDisplayFrequency = static_cast<DWORD>(fps);
      want.dmPosition.x = at.x;
      want.dmPosition.y = at.y;
      const LONG set = ChangeDisplaySettingsExW(device.c_str(), &want, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
      const LONG commit = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
      BOOST_LOG(info) << "Desk: " << (attached ? "retuned" : "attached") << " display " << utf_utils::to_utf8(device) << " to " << w << 'x' << h << '@' << fps
                      << " at " << at.x << ',' << at.y << " (set=" << set << ", commit=" << commit << ')';
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

    /** Windows puts a taskbar on every display, the desk included; nothing of
     *  the desk but its windows must ever be in the picture. */
    void hide_desk_taskbar(HMONITOR desk) {
      if (!desk) return;
      for (HWND tray = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); tray;
           tray = FindWindowExW(nullptr, tray, L"Shell_SecondaryTrayWnd", nullptr)) {
        if (MonitorFromWindow(tray, MONITOR_DEFAULTTONULL) == desk && IsWindowVisible(tray)) {
          ShowWindow(tray, SW_HIDE);
          BOOST_LOG(info) << "Desk: hid the taskbar on the desk";
        }
      }
    }

    /** Where a window goes when the desk lets go of it: where it was before
     *  it came to the desk, or, for one that never was anywhere else (it
     *  opened on the desk), the same place on the primary display. */
    POINT home_for(const RECT &frame, const RECT &desk) {
      RECT x {};
      if (!IntersectRect(&x, &frame, &desk)) return {frame.left, frame.top};
      MONITORINFO mi {};
      mi.cbSize = sizeof(mi);
      GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
      const RECT &work = mi.rcWork;
      const LONG w = frame.right - frame.left, h = frame.bottom - frame.top;
      const LONG px = work.left + std::clamp<LONG>(frame.left - desk.left, 0, std::max<LONG>(0, (work.right - work.left) - w));
      const LONG py = work.top + std::clamp<LONG>(frame.top - desk.top, 0, std::max<LONG>(0, (work.bottom - work.top) - h));
      return {px, py};
    }

    /** Visible top-level windows on the desk (the desk's own taskbar aside). */
    std::vector<HWND> windows_on(const RECT &desk) {
      struct ctx_t {
        RECT desk;
        std::vector<HWND> out;
      } ctx {desk, {}};
      EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto *c = reinterpret_cast<ctx_t *>(lp);
        if (!IsWindowVisible(hwnd) || app_window_layout::is_cloaked(hwnd)) return TRUE;
        wchar_t cls[64] {};
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 || wcscmp(cls, L"Shell_TrayWnd") == 0) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) return TRUE;
        RECT r {}, x {};
        if (!app_window_layout::frame_rect(hwnd, r) || !IntersectRect(&x, &r, &c->desk)) return TRUE;
        c->out.push_back(hwnd);
        return TRUE;
      },
                  reinterpret_cast<LPARAM>(&ctx));
      return ctx.out;
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

    /** The part of the desk the client does not show (the driver's mode is
     *  larger than the client's screen) taken out of the desk's work area,
     *  the way a taskbar is: hidden app bars on its right and bottom edges.
     *  Windows keeps menus, flyouts and tooltips inside the work area and
     *  maximizes into it, so nothing lands where the client cannot see it. */
    class work_area_reserve_t {
    public:
      work_area_reserve_t(const RECT &desk, int right, int bottom) {
        thread_ = std::thread([this, desk, right, bottom] {
          run(desk, right, bottom);
        });
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] {
          return started_;
        });
      }

      ~work_area_reserve_t() {
        if (thread_id_) PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
        if (thread_.joinable()) thread_.join();
      }

      bool same(const RECT &desk, int right, int bottom) const {
        return EqualRect(&desk_, &desk) && right == right_ && bottom == bottom_;
      }

    private:
      static constexpr UINT callback_message = WM_APP + 1;

      /** Our two bars meet in the bottom right corner: the bottom one ends
       *  where the right one begins (left overlapping, the shell moves the
       *  bottom bar up by the right one's width instead). The shell's
       *  suggestion is asked for, as app bars must, but our rectangles are
       *  exact: nothing else lives on the desk's edges. */
      static void place(HWND bar, UINT edge, const RECT &desk, int extent, int right) {
        APPBARDATA abd {sizeof(abd)};
        abd.hWnd = bar;
        abd.uEdge = edge;
        RECT want = desk;
        if (edge == ABE_RIGHT) {
          want.left = desk.right - extent;
        } else {
          want.top = desk.bottom - extent;
          want.right = desk.right - right;
        }
        abd.rc = want;
        SHAppBarMessage(ABM_QUERYPOS, &abd);
        abd.rc = want;
        SHAppBarMessage(ABM_SETPOS, &abd);
        SetWindowPos(bar, nullptr, want.left, want.top, want.right - want.left, want.bottom - want.top, SWP_NOZORDER | SWP_NOACTIVATE);
      }

      void run(const RECT &desk, int right, int bottom) {
        desk_ = desk;
        right_ = right;
        bottom_ = bottom;
        thread_id_ = GetCurrentThreadId();
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // the thread's queue, for WM_QUIT
        WNDCLASSW wc {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"VibeshineDeskReserve";
        RegisterClassW(&wc);  // fails harmlessly when already registered
        std::vector<std::pair<HWND, std::pair<UINT, int>>> bars;
        for (auto [edge, extent] : {std::pair<UINT, int> {ABE_RIGHT, right}, std::pair<UINT, int> {ABE_BOTTOM, bottom}}) {
          if (extent <= 0) continue;
          // Never shown: an app bar reserves its edge whether visible or not,
          // and a visible one would be in the picture
          HWND bar = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
          if (!bar) continue;
          APPBARDATA abd {sizeof(abd)};
          abd.hWnd = bar;
          abd.uCallbackMessage = callback_message;
          if (!SHAppBarMessage(ABM_NEW, &abd)) {
            DestroyWindow(bar);
            continue;
          }
          place(bar, edge, desk, extent, right);
          bars.push_back({bar, {edge, extent}});
        }
        BOOST_LOG(info) << "Desk: reserving " << right << " px right and " << bottom << " px bottom of the desk's work area (" << bars.size() << " bars)";
        SetTimer(nullptr, 0, 1000, nullptr);  // the shell applies it in a moment: report what it became
        {
          std::lock_guard lock(mutex_);
          started_ = true;
        }
        ready_.notify_all();
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
          if (msg.message == WM_TIMER && !msg.hwnd) {
            KillTimer(nullptr, msg.wParam);
            MONITORINFO mi {sizeof(mi)};
            GetMonitorInfoW(MonitorFromRect(&desk, MONITOR_DEFAULTTONEAREST), &mi);
            BOOST_LOG(info) << "Desk: work area " << mi.rcWork.left << ',' << mi.rcWork.top << ' ' << mi.rcWork.right - mi.rcWork.left << 'x'
                            << mi.rcWork.bottom - mi.rcWork.top << " of " << desk.right - desk.left << 'x' << desk.bottom - desk.top;
            continue;
          }
          if (msg.message == callback_message && msg.wParam == ABN_POSCHANGED) {
            for (auto &[bar, spec] : bars) place(bar, spec.first, desk, spec.second, right);
          }
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
        for (auto &[bar, spec] : bars) {
          APPBARDATA abd {sizeof(abd)};
          abd.hWnd = bar;
          SHAppBarMessage(ABM_REMOVE, &abd);
          DestroyWindow(bar);
        }
      }

      std::thread thread_;
      std::mutex mutex_;
      std::condition_variable ready_;
      bool started_ = false;
      std::atomic<DWORD> thread_id_ {0};
      RECT desk_ {};
      int right_ = 0, bottom_ = 0;
    };

    std::map<std::string, std::unique_ptr<work_area_reserve_t>> g_reserves;  ///< client uuid -> its desk's reserve (under g_mutex)

    void reserve_work_area(const std::string &client_uuid, const RECT &desk, int shown_w, int shown_h) {
      const int right = std::max(0, static_cast<int>(desk.right - desk.left) - shown_w);
      const int bottom = std::max(0, static_cast<int>(desk.bottom - desk.top) - shown_h);
      auto &slot = g_reserves[client_uuid];
      if (slot && slot->same(desk, right, bottom)) return;
      slot.reset();
      if (right > 0 || bottom > 0) slot = std::make_unique<work_area_reserve_t>(desk, right, bottom);
    }

    /** Make sure this virtual display exists at the mode (the driver's
     *  nearest when it only has a catalog; w, h and fps then say what it
     *  got) and at the place `where` says. Creates it when missing. */
    bool ensure_display(vdisplay_t &d, const std::string &id, const char *label, int &w, int &h, int &fps, bool custom,
                        const std::function<POINT()> &where, RECT &rect) {
      bool present = !d.device.empty() && find_monitor(d.device, &rect);
      if (present && custom && (rect.right - rect.left != w || rect.bottom - rect.top != h)) {
        // A different size needs a display created for it: recreate.
        BOOST_LOG(info) << "Desk: recreating " << label << " for " << w << 'x' << h;
        VDISPLAY::set_virtual_display_protected(d.guid, d.device, false);
        VDISPLAY::removeVirtualDisplay(d.guid);
        d.device.clear();
        d.monitor_path.clear();
        present = false;
      }
      if (present) {
        if (!custom) {
          if (auto mode = pick_mode(d.device, w, h, fps)) {
            w = mode->w;
            h = mode->h;
            fps = mode->hz;
          }
        }
        const POINT at = where();
        if (rect.right - rect.left != w || rect.bottom - rect.top != h || monitor_refresh_hz(d.device.c_str()) != fps || rect.left != at.x || rect.top != at.y) {
          // Different size, rate or place: retune the existing display.
          attach_display(d.device, w, h, fps, at);
          for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (find_monitor(d.device, &rect) && rect.right - rect.left == w && rect.bottom - rect.top == h && rect.left == at.x && rect.top == at.y) break;
          }
          present = find_monitor(d.device, &rect);
        }
        return present;
      }
      if (!VDISPLAY::ensure_driver_is_ready()) {
        BOOST_LOG(warning) << "Desk: no virtual display driver";
        return false;
      }
      // A fresh monitor identity every time: Windows "remembers window
      // locations based on monitor connection" and would otherwise drag
      // windows that once sat on it back onto it when it reappears, with
      // placements scaled by whatever DPI it had then.
      const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      const auto uuid = VDISPLAY::virtualDisplayUuidFromStableId(id + "-" + std::to_string(stamp));
      std::memcpy(&d.guid, uuid.b8, sizeof(d.guid));
      VDISPLAY::setWatchdogFeedingEnabled(true);
      auto created = VDISPLAY::createVirtualDisplay(id.c_str(), label, nullptr, static_cast<uint32_t>(w), static_cast<uint32_t>(h), static_cast<uint32_t>(fps),
                                                    d.guid, 0, false, 1, std::nullopt, false, true, true, custom);
      if (!created) {
        BOOST_LOG(warning) << "Desk: the virtual display driver could not create " << label << " at " << w << 'x' << h << '@' << fps;
        return false;
      }
      d.device = created->display_name.value_or(L"");
      VDISPLAY::set_virtual_display_protected(d.guid, d.device, true);
      if (created->monitor_device_path) d.monitor_path = *created->monitor_device_path;
      // The driver brings it up detached or at a default mode: attach it at ours.
      present = false;
      bool picked = false;
      for (int i = 0; i < 200 && !present; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!d.device.empty() && !picked && !custom) {
          if (auto mode = pick_mode(d.device, w, h, fps)) {
            w = mode->w;
            h = mode->h;
            fps = mode->hz;
            picked = true;
          }
        }
        if (!d.device.empty() && (i == 10 || i == 60)) attach_display(d.device, w, h, fps, where());
        present = !d.device.empty() && find_monitor(d.device, &rect) && rect.right - rect.left == w && rect.bottom - rect.top == h &&
                  monitor_refresh_hz(d.device.c_str()) + 1 >= fps;
      }
      if (!present) {
        BOOST_LOG(warning) << "Desk: " << label << " did not come up at " << w << 'x' << h << '@' << fps;
        VDISPLAY::set_virtual_display_protected(d.guid, d.device, false);
        VDISPLAY::removeVirtualDisplay(d.guid);
        d.device.clear();
        d.monitor_path.clear();
        return false;
      }
      BOOST_LOG(info) << "Desk: created " << label << ' ' << utf_utils::to_utf8(d.device) << ' ' << w << 'x' << h << '@' << fps;
      return true;
    }

    void remove_display(vdisplay_t &d) {
      if (d.device.empty()) return;
      VDISPLAY::set_virtual_display_protected(d.guid, d.device, false);
      VDISPLAY::removeVirtualDisplay(d.guid);
      d.device.clear();
      d.monitor_path.clear();
    }

    /** Windows' own window management fights the client's: dragging to an
     *  edge or hovering a maximize button (Snap Layouts) tiles windows on the
     *  host behind the client's back. Off for the logged-in user while any
     *  desk is open, as the Settings app turns it off; the user's values are
     *  kept under HKCU\\Software\\Vibeshine\\DeskTweaks until restored, so a
     *  crash restores them with the next desk. */
    constexpr const wchar_t *tweaks_backup_key = L"Software\\Vibeshine\\DeskTweaks";
    constexpr const wchar_t *explorer_advanced_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
    constexpr const wchar_t *snap_values[] = {L"EnableSnapAssistFlyout", L"EnableSnapBar", L"SnapAssist"};
    bool g_tweaked = false;

    /** Runs `fn` with the logged-in user's HKCU open (we run as SYSTEM). */
    void with_user_hive(const std::function<void(HKEY)> &fn) {
      HANDLE token = platf::retrieve_users_token(false);
      if (!token) {
        BOOST_LOG(warning) << "Desk: no logged-in user for host tweaks";
        return;
      }
      (void) platf::impersonate_current_user(token, [&] {
        HKEY user = nullptr;
        if (RegOpenCurrentUser(KEY_READ | KEY_WRITE, &user) == ERROR_SUCCESS) {
          fn(user);
          RegCloseKey(user);
        }
      });
      CloseHandle(token);
    }

    bool read_dword(HKEY root, const wchar_t *key, const wchar_t *name, DWORD &out) {
      DWORD size = sizeof(out);
      return RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &out, &size) == ERROR_SUCCESS;
    }

    void write_dword(HKEY root, const wchar_t *key, const wchar_t *name, DWORD value) {
      RegSetKeyValueW(root, key, name, REG_DWORD, &value, sizeof(value));
    }

    void announce_settings() {
      DWORD_PTR result = 0;
      SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, SPI_SETWINARRANGING, reinterpret_cast<LPARAM>(L"WindowMetrics"), SMTO_ABORTIFHUNG, 500, &result);
      SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"TraySettings"), SMTO_ABORTIFHUNG, 500, &result);
    }

    void apply_host_tweaks() {
      if (g_tweaked) return;
      g_tweaked = true;
      BOOL arranging = TRUE;
      SystemParametersInfoW(SPI_GETWINARRANGING, 0, &arranging, 0);
      with_user_hive([&](HKEY user) {
        DWORD saved = 0;
        if (!read_dword(user, tweaks_backup_key, L"Saved", saved)) {
          // First time (or restored since): keep the user's values. "absent"
          // values are remembered as such and deleted again on restore.
          for (const wchar_t *name : snap_values) {
            DWORD v = 0;
            const bool present = read_dword(user, explorer_advanced_key, name, v);
            write_dword(user, tweaks_backup_key, name, present ? v : 0xffffffffu);
          }
          write_dword(user, tweaks_backup_key, L"WinArranging", arranging ? 1 : 0);
          write_dword(user, tweaks_backup_key, L"Saved", 1);
        }
        for (const wchar_t *name : snap_values) write_dword(user, explorer_advanced_key, name, 0);
      });
      const BOOL ok = SystemParametersInfoW(SPI_SETWINARRANGING, FALSE, nullptr, SPIF_SENDCHANGE);
      announce_settings();
      BOOST_LOG(info) << "Desk: window snapping off on the host while streaming windows (spi " << (ok ? "ok" : "failed") << ", was " << arranging << ')';
    }

    void restore_host_tweaks() {
      if (!g_tweaked) return;
      g_tweaked = false;
      DWORD arranging = 1;
      with_user_hive([&](HKEY user) {
        DWORD saved = 0;
        if (!read_dword(user, tweaks_backup_key, L"Saved", saved)) return;
        for (const wchar_t *name : snap_values) {
          DWORD v = 0xffffffffu;
          read_dword(user, tweaks_backup_key, name, v);
          if (v == 0xffffffffu) RegDeleteKeyValueW(user, explorer_advanced_key, name);
          else write_dword(user, explorer_advanced_key, name, v);
        }
        read_dword(user, tweaks_backup_key, L"WinArranging", arranging);
        RegDeleteKeyW(user, tweaks_backup_key);
      });
      SystemParametersInfoW(SPI_SETWINARRANGING, arranging ? TRUE : FALSE, nullptr, SPIF_SENDCHANGE);
      announce_settings();
      BOOST_LOG(info) << "Desk: window snapping restored on the host";
    }

    /** Remove a desk's display once it has been idle long enough (detached thread). */
    void schedule_idle_removal(const std::string &client_uuid, unsigned generation) {
      std::thread([client_uuid, generation] {
        std::this_thread::sleep_for(std::chrono::minutes(10));
        GUID guid {};
        vdisplay_t atlas;
        {
          std::lock_guard lock(g_mutex);
          auto it = g_desks.find(client_uuid);
          if (it == g_desks.end() || it->second.open || it->second.idle_generation != generation) return;
          guid = it->second.guid;
          atlas = it->second.atlas;
          g_desks.erase(it);
        }
        remove_display(atlas);
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
    const int shown_w = w, shown_h = h;  // the client's screen; the mode may be larger
    fps = std::clamp(fps, 24, 240);
    dpi = std::clamp(dpi, 96, 384);
    std::lock_guard lock(g_mutex);
    auto &desk = g_desks[client_uuid];
    // Newer drivers expose exactly the requested mode; older ones only a catalog.
    const bool custom = VDISPLAY::supportsRequestedResolutionCatalog();
    auto ours = [&desk] {
      return std::vector<std::wstring> {desk.device, desk.atlas.device};
    };
    vdisplay_t desk_display {desk.guid, desk.device, desk.monitor_path};
    RECT rect {};
    const bool desk_ok = ensure_display(desk_display, "vibeshine-desk-" + client_uuid, "the desk", w, h, fps, custom, [&] {
      return desk_position(ours());
    }, rect);
    desk.guid = desk_display.guid;
    desk.device = desk_display.device;
    desk.monitor_path = desk_display.monitor_path;
    if (!desk_ok) {
      remove_display(desk.atlas);
      g_desks.erase(client_uuid);
      return {};
    }
    // The atlas: the same mode, so the session streams the size the client expects
    int aw = w, ah = h, afps = fps;
    RECT atlas_rect {};
    if (!ensure_display(desk.atlas, "vibeshine-atlas-" + client_uuid, "the atlas", aw, ah, afps, custom, [&] {
          return atlas_position(ours());
        }, atlas_rect) || aw != w || ah != h) {
      BOOST_LOG(warning) << "Desk: no atlas display at " << w << 'x' << h << " (got " << aw << 'x' << ah << ')';
      remove_display(desk.atlas);
      vdisplay_t gone {desk.guid, desk.device, desk.monitor_path};
      remove_display(gone);
      g_desks.erase(client_uuid);
      return {};
    }
    find_monitor(desk.device, &rect);  // the atlas may have shifted the layout
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
    hide_desk_taskbar(find_monitor(desk.device));
    hide_desk_taskbar(find_monitor(desk.atlas.device));
    if (find_monitor(desk.device, &rect)) reserve_work_area(client_uuid, rect, shown_w, shown_h);
    try {
      desk.info.capture = utf_utils::to_utf8(desk.atlas.device);
    } catch (...) {
    }
    find_monitor(desk.atlas.device, &atlas_rect);
    if (!desk.atlas_window || !EqualRect(&desk.atlas_window->monitor(), &atlas_rect)) {
      desk.atlas_window.reset();
      desk.atlas_window = std::make_shared<desk_atlas::atlas_t>(atlas_rect);
    }
    apply_host_tweaks();
    BOOST_LOG(info) << "Desk: open " << desk.info.display << ' ' << desk.info.rect[2] << 'x' << desk.info.rect[3] << '@' << fps << " at " << desk.info.dpi << " dpi"
                    << (custom ? " (exact mode)" : " (catalog mode)") << ", atlas " << desk.info.capture;
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

  namespace {
    std::map<std::string, std::pair<int, unsigned>> g_sessions;  ///< client -> (live desk streams, generation)
  }

  void session_started(const std::string &client_uuid) {
    std::lock_guard lock(g_mutex);
    auto &s = g_sessions[client_uuid];
    ++s.first;
    ++s.second;
  }

  void session_ended(const std::string &client_uuid) {
    unsigned generation = 0;
    {
      std::lock_guard lock(g_mutex);
      auto &s = g_sessions[client_uuid];
      s.first = std::max(0, s.first - 1);
      if (s.first > 0) return;
      generation = ++s.second;
    }
    std::thread([client_uuid, generation] {
      std::this_thread::sleep_for(std::chrono::seconds(20));
      {
        std::lock_guard lock(g_mutex);
        auto &s = g_sessions[client_uuid];
        if (s.first > 0 || s.second != generation) return;
      }
      BOOST_LOG(info) << "Desk: no stream for client " << client_uuid << " since 20 s; closing its desk";
      close(client_uuid);
    }).detach();
  }

  void close(const std::string &client_uuid) {
    std::lock_guard lock(g_mutex);
    auto it = g_desks.find(client_uuid);
    if (it == g_desks.end()) return;
    for (const auto &[hwnd, origin] : it->second.adopted) {
      if (!IsWindow(hwnd)) continue;
      RECT frame {};
      if (app_window_layout::frame_rect(hwnd, frame)) settle_dpi_change(hwnd, GetDpiForWindow(hwnd), frame);
      move_frame_to(hwnd, origin.x, origin.y);
    }
    // Everything else still on the desk (dialogs and windows that opened
    // there) goes to the primary display: nothing stays on a display nobody sees
    RECT desk {};
    if (find_monitor(it->second.device, &desk)) {
      for (HWND hwnd : windows_on(desk)) {
        if (it->second.adopted.contains(hwnd)) continue;
        RECT frame {};
        if (!app_window_layout::frame_rect(hwnd, frame)) continue;
        const POINT home = home_for(frame, desk);
        settle_dpi_change(hwnd, GetDpiForWindow(hwnd), frame);
        move_frame_to(hwnd, home.x, home.y);
        BOOST_LOG(info) << "Desk: moved 0x" << std::hex << reinterpret_cast<std::uintptr_t>(hwnd) << std::dec << " to the primary display";
      }
    }
    it->second.adopted.clear();
    g_reserves.erase(client_uuid);
    it->second.atlas_window.reset();
    it->second.open = false;
    bool any = false;
    for (const auto &[uuid, d] : g_desks) any = any || d.open;
    if (!any) restore_host_tweaks();
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
      // Already on the desk: a plain move, as far off its edges as the Mac
      // lets a window go (a window flicked aside), so both keep agreeing on
      // where it is. Nothing borders the desk but above (desk_position); a
      // sliver stays on it so it remains the window's display.
      RECT frame {};
      if (app_window_layout::frame_rect(hwnd, frame)) {
        const LONG w = frame.right - frame.left;
        const LONG dw = rect.right - rect.left, dh = rect.bottom - rect.top;
        x = static_cast<int>(std::clamp<LONG>(x, -(w - 8), std::max<LONG>(-(w - 8), dw - 8)));
        y = static_cast<int>(std::clamp<LONG>(y, 0, std::max<LONG>(0, dh - 8)));
      }
      move_frame_to(hwnd, rect.left + x, rect.top + y);
      return true;
    }
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    RECT frame {};
    if (!app_window_layout::frame_rect(hwnd, frame)) return false;
    // A window first seen on the desk itself (it opened there, or is parked
    // past its edge mid-drag) has no home elsewhere: the primary display
    it->second.adopted[hwnd] = home_for(frame, rect);
    settle_dpi_change(hwnd, GetDpiForWindow(hwnd), frame);
    // First time: keep it inside the desk.
    const LONG w = frame.right - frame.left, h = frame.bottom - frame.top;
    const LONG dw = rect.right - rect.left, dh = rect.bottom - rect.top;
    x = static_cast<int>(std::max<LONG>(0, std::min<LONG>(x, dw - std::min(w, dw))));
    y = static_cast<int>(std::max<LONG>(0, std::min<LONG>(y, dh - std::min(h, dh))));
    move_frame_to(hwnd, rect.left + x, rect.top + y);
    return true;
  }

  bool order(const std::string &client_uuid, const std::vector<std::uint64_t> &top_first, bool focus) {
    RECT desk {};
    {
      std::lock_guard lock(g_mutex);
      auto it = g_desks.find(client_uuid);
      if (it == g_desks.end() || !it->second.open || !find_monitor(it->second.device, &desk)) return false;
    }
    std::vector<HWND> windows;
    for (auto handle : top_first) {
      auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
      RECT r {}, x {};
      if (!IsWindow(hwnd) || !GetWindowRect(hwnd, &r) || !IntersectRect(&x, &r, &desk)) continue;  // only windows on the desk
      if (IsHungAppWindow(hwnd)) continue;  // DeferWindowPos would wait for it
      windows.push_back(hwnd);
    }
    if (windows.empty()) return false;
    // The client is the active app: its front window gets the focus in the
    // same step, so the host never shows the new order with the old focus.
    // (Unless it, a popup of it or its thread's menu has it already: a
    // click got there first, and activating again would close its menu.)
    if (focus) {
      HWND top = windows.front(), fg = GetForegroundWindow();
      const bool has = fg && (fg == top || GetAncestor(fg, GA_ROOTOWNER) == top ||
                              GetWindowThreadProcessId(fg, nullptr) == GetWindowThreadProcessId(top, nullptr));
      if (!has) host_windows::action(reinterpret_cast<std::uintptr_t>(top), host_windows::op_e::activate);
    }
    if (windows.size() < 2) return true;
    // One batch, each window right below the one before it. The front one
    // is not moved: raising a window above the foreground one is refused
    // to a background process, and only the order among the desk's own
    // windows matters (the desk is a display of its own). Separate
    // SetWindowPos calls on windows of different threads applied in any
    // order and could leave the stack half done.
    HDWP batch = BeginDeferWindowPos(static_cast<int>(windows.size()) - 1);
    for (std::size_t i = 1; batch && i < windows.size(); ++i) {
      batch = DeferWindowPos(batch, windows[i], windows[i - 1], 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    if (!batch || !EndDeferWindowPos(batch)) {
      BOOST_LOG(info) << "App desk: stacking failed (" << GetLastError() << ')';
      return false;
    }
    return true;
  }

  bool park(const std::string &client_uuid, std::uint64_t handle) {
    auto hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
    RECT desk {}, frame {};
    {
      std::lock_guard lock(g_mutex);
      auto it = g_desks.find(client_uuid);
      if (it == g_desks.end() || !it->second.open || !find_monitor(it->second.device, &desk)) return false;
    }
    if (!IsWindow(hwnd) || !app_window_layout::frame_rect(hwnd, frame)) return false;
    // 8 px stay on the desk so Windows keeps it on this display
    move_frame_to(hwnd, desk.right - 8, std::clamp<LONG>(frame.top, desk.top, desk.bottom - 8));
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
    HMONITOR desk_monitor = nullptr, atlas_monitor = nullptr;
    std::shared_ptr<desk_atlas::atlas_t> atlas;
    {
      std::lock_guard lock(g_mutex);
      auto it = g_desks.find(client_uuid);
      if (it == g_desks.end() || !it->second.open || !(desk_monitor = find_monitor(it->second.device, &desk))) return out;
      for (const auto &[hwnd, origin] : it->second.adopted) adopted.push_back(hwnd);
      atlas = it->second.atlas_window;
      atlas_monitor = find_monitor(it->second.atlas.device);
    }
    // Explorer shows it again now and then (display changes, restarts)
    static std::chrono::steady_clock::time_point last_tray_check {};
    if (std::chrono::steady_clock::now() - last_tray_check > std::chrono::seconds(2)) {
      last_tray_check = std::chrono::steady_clock::now();
      hide_desk_taskbar(desk_monitor);
      hide_desk_taskbar(atlas_monitor);
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
      static const wchar_t *shell[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow", L"IME"};
      for (auto *s : shell) {
        if (wcscmp(cls, s) == 0) return TRUE;
      }
      // The shell's XAML hosts are its full-screen overlays (Alt-Tab, task
      // view) but also real dialogs ("Open with"): only the overlays are left out
      if (wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0 && (r.right - r.left) * 10 >= (c->desk.right - c->desk.left) * 9 &&
          (r.bottom - r.top) * 10 >= (c->desk.bottom - c->desk.top) * 9) {
        return TRUE;
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
      // A window of its own: one with a caption, or an unowned one Alt-Tab
      // would list (no tool window, activatable) of window size: dialogs and
      // apps drawing their own frame ("Open with", frameless apps). Menus,
      // tooltips and drop-downs are tool windows, owned, or no-activate.
      const bool has_caption = (w.style & WS_CAPTION) == WS_CAPTION;
      const auto frameless_window = [](const raw_t &r) {
        return !r.owner && !(r.ex_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) && (r.rect.right - r.rect.left) >= 200 && (r.rect.bottom - r.rect.top) >= 150;
      };
      const bool captioned = has_caption || frameless_window(w);
      HWND owner = w.owner;
      if (!captioned && !owner) {
        // Unowned popup (Win32 menu, combo list): belongs to the top-level
        // window of its thread, if one is on the desk.
        for (const auto &o : windows) {
          if (o.hwnd != w.hwnd && o.tid == w.tid && ((o.style & WS_CAPTION) == WS_CAPTION || frameless_window(o)) && !o.owner) {
            owner = o.hwnd;
            break;
          }
        }
      }
      // (A popup inside its owner is listed too: a window's thumbnail never
      // holds its popups, each is a window of its own.)
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
      if (has_caption) {
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
    // Their pictures: a slot each in the atlas (the copy on white for popups,
    // whose transparency matters: menus with their shadows, tooltips)
    if (atlas) {
      std::vector<desk_atlas::source_t> sources;
      for (const auto &w : out.windows) {
        if (w.state == "minimized") continue;
        sources.push_back({reinterpret_cast<HWND>(static_cast<std::uintptr_t>(w.handle)), w.rect[2], w.rect[3], w.kind == "popup", w.z});
      }
      for (const auto &slot : atlas->sync(sources)) {
        for (auto &w : out.windows) {
          if (w.handle != reinterpret_cast<std::uintptr_t>(slot.hwnd)) continue;
          w.slot[0] = slot.x;
          w.slot[1] = slot.y;
          w.slot[2] = slot.w;
          w.slot[3] = slot.h;
          w.alpha_slot[0] = slot.ax;
          w.alpha_slot[1] = slot.ay;
          w.scale = slot.scale;
        }
      }
    }
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
      for (int v : w.slot) mix(static_cast<std::uint32_t>(v));
      for (int v : w.alpha_slot) mix(static_cast<std::uint32_t>(v));
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

  bool park(const std::string &, std::uint64_t) {
    return false;
  }

  bool order(const std::string &, const std::vector<std::uint64_t> &, bool) {
    return false;
  }

  void session_started(const std::string &) {}

  void session_ended(const std::string &) {}

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
