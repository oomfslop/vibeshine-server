/**
 * @file src/platform/windows/desk_atlas.cpp
 * @brief See desk_atlas.h.
 */
#include "desk_atlas.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>

#include "src/logging.h"

namespace desk_atlas {

  namespace {
    constexpr UINT sync_message = WM_APP + 7;
    constexpr UINT_PTR topmost_timer = 1;

    struct sync_request_t {
      const std::vector<source_t> *sources;
      std::vector<slot_t> *slots;
    };

    /** Capacity for a window: room to grow in a live resize before it needs
     *  another slot (each step that outgrows its slot moves it). */
    int capacity(int size, int limit) {
      const int with_room = size + std::max(128, size / 4);
      return std::min(limit, (with_room + 63) / 64 * 64);
    }

    bool overlaps(int x, int y, int w, int h, int ox, int oy, int ow, int oh) {
      return x < ox + ow && ox < x + w && y < oy + oh && oy < y + h;
    }
  }  // namespace

  atlas_t::atlas_t(const RECT &monitor):
      monitor_(monitor) {
    thread_ = std::thread([this] {
      run();
    });
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this] {
      return started_;
    });
  }

  atlas_t::~atlas_t() {
    if (window_) PostMessageW(window_, WM_CLOSE, 0, 0);
    if (thread_.joinable()) thread_.join();
  }

  std::vector<slot_t> atlas_t::sync(const std::vector<source_t> &sources) {
    std::vector<slot_t> slots;
    if (!window_) return slots;
    sync_request_t request {&sources, &slots};
    // The window's thread does the work: thumbnails and the backings it
    // paints change together, in the order the next composition shows them
    SendMessageW(window_, sync_message, 0, reinterpret_cast<LPARAM>(&request));
    return slots;
  }

  LRESULT CALLBACK atlas_t::window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams));
    }
    auto *self = reinterpret_cast<atlas_t *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
      case sync_message:
        if (self) {
          auto *request = reinterpret_cast<sync_request_t *>(lparam);
          *request->slots = self->apply(*request->sources);
        }
        return 0;
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (self) self->paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_ERASEBKGND:
        return 1;
      case WM_TIMER:
        if (wparam == topmost_timer) {
          // Nothing may cover the atlas: its display is the picture
          SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        return 0;
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
      case WM_CLOSE:
        if (self) {
          for (auto &[source, item] : self->items_) self->release(item);
          self->items_.clear();
        }
        DestroyWindow(hwnd);
        return 0;
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  void atlas_t::run() {
    // Thumbnails are sized in physical pixels only for a per-monitor aware window
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc {};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VibeshineDeskAtlas";
    wc.hCursor = nullptr;
    RegisterClassW(&wc);  // fails harmlessly when already registered
    window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, monitor_.left, monitor_.top,
                              monitor_.right - monitor_.left, monitor_.bottom - monitor_.top, nullptr, nullptr, wc.hInstance, this);
    if (window_) {
      ShowWindow(window_, SW_SHOWNOACTIVATE);
      SetTimer(window_, topmost_timer, 250, nullptr);
      BOOST_LOG(info) << "Desk atlas: window on " << monitor_.left << ',' << monitor_.top << ' ' << monitor_.right - monitor_.left << 'x'
                      << monitor_.bottom - monitor_.top;
    } else {
      BOOST_LOG(error) << "Desk atlas: could not create its window (" << GetLastError() << ')';
    }
    {
      std::lock_guard lock(mutex_);
      started_ = true;
    }
    ready_.notify_all();
    MSG msg;
    while (window_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    window_ = nullptr;
  }

  void atlas_t::paint(HDC dc) {
    RECT client;
    GetClientRect(window_, &client);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    for (const auto &r : white_) FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  }

  void atlas_t::release(item_t &item) {
    if (item.black) DwmUnregisterThumbnail(item.black);
    if (item.white) DwmUnregisterThumbnail(item.white);
    item.black = item.white = nullptr;
  }

  bool atlas_t::place(std::vector<cell_t> &taken, int cw, int ch, cell_t &out) const {
    const int W = monitor_.right - monitor_.left, H = monitor_.bottom - monitor_.top;
    if (cw > W || ch > H) return false;
    std::vector<int> xs {0}, ys {0};
    for (const auto &c : taken) {
      xs.push_back(c.x + c.cw);
      ys.push_back(c.y + c.ch);
    }
    std::sort(xs.begin(), xs.end());
    std::sort(ys.begin(), ys.end());
    // Bottom-left-most free spot: lowest row first, then leftmost
    for (int y : ys) {
      if (y + ch > H) break;
      for (int x : xs) {
        if (x + cw > W) break;
        bool free = true;
        for (const auto &c : taken) {
          if (overlaps(x, y, cw, ch, c.x, c.y, c.cw, c.ch)) {
            free = false;
            break;
          }
        }
        if (free) {
          out = {x, y, cw, ch};
          taken.push_back(out);
          return true;
        }
      }
    }
    return false;
  }

  bool atlas_t::update_thumbnail(HTHUMBNAIL thumb, const cell_t &cell, const item_t &item) {
    DWM_THUMBNAIL_PROPERTIES p {};
    p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    p.rcDestination = {cell.x, cell.y, cell.x + cell.cw, cell.y + cell.ch};
    // The capacity as source, from the frame's origin: 1:1 (or the slot's
    // scale), the window anchored top-left, its growth room left empty
    const int sw = static_cast<int>(std::lround(cell.cw / item.scale)), sh = static_cast<int>(std::lround(cell.ch / item.scale));
    p.rcSource = {item.source_offset.left, item.source_offset.top, item.source_offset.left + sw, item.source_offset.top + sh};
    p.opacity = 255;
    p.fVisible = TRUE;
    p.fSourceClientAreaOnly = FALSE;
    return SUCCEEDED(DwmUpdateThumbnailProperties(thumb, &p));
  }

  std::vector<slot_t> atlas_t::apply(const std::vector<source_t> &sources) {
    const int W = monitor_.right - monitor_.left, H = monitor_.bottom - monitor_.top;
    for (auto &[hwnd, item] : items_) item.used = false;
    std::vector<HWND> to_place;
    for (const auto &s : sources) {
      if (!s.hwnd || s.w <= 0 || s.h <= 0 || !IsWindow(s.hwnd)) continue;
      auto &item = items_[s.hwnd];
      item.used = true;
      RECT wr {}, fr {};
      GetWindowRect(s.hwnd, &wr);
      if (FAILED(DwmGetWindowAttribute(s.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr)))) fr = wr;
      const RECT offset {fr.left - wr.left, fr.top - wr.top, 0, 0};
      const bool moved_origin = offset.left != item.source_offset.left || offset.top != item.source_offset.top;
      item.source_offset = offset;
      const bool fits = item.black && item.scale == 1.0 && item.cell.cw >= s.w && item.cell.ch >= s.h && item.has_white == s.alpha;
      const bool shrunk_far = item.black && item.scale == 1.0 && item.cell.cw > capacity(s.w, W) * 2 && item.cell.ch > capacity(s.h, H) * 2;
      if (!fits || shrunk_far) to_place.push_back(s.hwnd);
      else if (moved_origin || item.w != s.w || item.h != s.h) {
        item.w = s.w;
        item.h = s.h;
        if (moved_origin) {
          update_thumbnail(item.black, item.cell, item);
          if (item.white) update_thumbnail(item.white, item.white_cell, item);
        }
      }
      item.w = s.w;
      item.h = s.h;
      item.has_white = s.alpha;
    }
    for (auto it = items_.begin(); it != items_.end();) {
      if (!it->second.used) {
        release(it->second);
        it = items_.erase(it);
      } else {
        ++it;
      }
    }

    if (!to_place.empty()) {
      auto priority = [&](HWND h) {
        for (const auto &s : sources) {
          if (s.hwnd == h) return s.priority;
        }
        return 0;
      };
      std::vector<cell_t> taken;
      for (const auto &[hwnd, item] : items_) {
        if (std::find(to_place.begin(), to_place.end(), hwnd) != to_place.end() || !item.black) continue;
        taken.push_back(item.cell);
        if (item.has_white) taken.push_back(item.white_cell);
      }
      std::sort(to_place.begin(), to_place.end(), [&](HWND a, HWND b) {
        return priority(a) > priority(b);
      });
      auto place_item = [&](HWND hwnd, item_t &item, double scale) {
        const int cw = capacity(static_cast<int>(std::ceil(item.w * scale)), W), ch = capacity(static_cast<int>(std::ceil(item.h * scale)), H);
        std::vector<cell_t> trial = taken;
        cell_t cell, white;
        if (!place(trial, cw, ch, cell)) return false;
        if (item.has_white && !place(trial, cw, ch, white)) return false;
        taken = std::move(trial);
        item.cell = cell;
        item.white_cell = item.has_white ? white : cell_t {};
        item.scale = scale;
        return true;
      };
      bool all = true;
      for (HWND hwnd : to_place) {
        if (!place_item(hwnd, items_[hwnd], 1.0)) {
          all = false;
          break;
        }
      }
      if (!all) {
        // Full: everything again, front-most first at full size; what does
        // not fit then goes smaller (the client scales it back up)
        taken.clear();
        std::vector<HWND> order;
        for (const auto &[hwnd, item] : items_) order.push_back(hwnd);
        std::sort(order.begin(), order.end(), [&](HWND a, HWND b) {
          return priority(a) > priority(b);
        });
        to_place = order;
        for (HWND hwnd : order) {
          auto &item = items_[hwnd];
          bool ok = false;
          for (double scale : {1.0, 0.75, 0.5, 0.35, 0.25, 0.15}) {
            if ((ok = place_item(hwnd, item, scale))) break;
          }
          if (!ok) item.cell = {};
        }
        BOOST_LOG(info) << "Desk atlas: repacked " << order.size() << " windows";
      }
      for (HWND hwnd : to_place) {
        auto &item = items_[hwnd];
        if (item.cell.cw <= 0) continue;
        if (!item.black && FAILED(DwmRegisterThumbnail(window_, hwnd, &item.black))) item.black = nullptr;
        if (item.has_white && !item.white && FAILED(DwmRegisterThumbnail(window_, hwnd, &item.white))) item.white = nullptr;
        if (!item.has_white && item.white) {
          DwmUnregisterThumbnail(item.white);
          item.white = nullptr;
        }
        if (item.black) update_thumbnail(item.black, item.cell, item);
        if (item.white) update_thumbnail(item.white, item.white_cell, item);
      }
    }

    // The white backings first in the frame order: painted now, synchronously
    std::vector<RECT> white;
    for (const auto &[hwnd, item] : items_) {
      if (item.white && item.white_cell.cw > 0) {
        white.push_back({item.white_cell.x, item.white_cell.y, item.white_cell.x + item.white_cell.cw, item.white_cell.y + item.white_cell.ch});
      }
    }
    const bool repaint = white.size() != white_.size() || !std::equal(white.begin(), white.end(), white_.begin(), [](const RECT &a, const RECT &b) {
                           return EqualRect(&a, &b);
                         });
    if (repaint) {
      white_ = std::move(white);
      InvalidateRect(window_, nullptr, FALSE);
      UpdateWindow(window_);
    }

    std::vector<slot_t> out;
    for (const auto &[hwnd, item] : items_) {
      if (!item.black || item.cell.cw <= 0) continue;
      slot_t s;
      s.hwnd = hwnd;
      s.x = item.cell.x;
      s.y = item.cell.y;
      s.w = static_cast<int>(std::lround(item.w * item.scale));
      s.h = static_cast<int>(std::lround(item.h * item.scale));
      s.scale = item.scale;
      if (item.white) {
        s.ax = item.white_cell.x;
        s.ay = item.white_cell.y;
      }
      out.push_back(s);
    }
    return out;
  }

}  // namespace desk_atlas
