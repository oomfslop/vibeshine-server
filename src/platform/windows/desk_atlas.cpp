/**
 * @file src/platform/windows/desk_atlas.cpp
 * @brief See desk_atlas.h.
 */
#include "desk_atlas.h"

#include <dwmapi.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <iterator>

#include "src/logging.h"

namespace desk_atlas {

  namespace {
    constexpr UINT sync_message = WM_APP + 7;
    constexpr UINT_PTR topmost_timer = 1;

    struct sync_request_t {
      const std::vector<source_t> *sources;
      std::vector<slot_t> *slots;
      bool *moved;
    };

    /** How long a thumbnail stays where a picture was: longer than a layout
     *  takes to reach the client and its frames to be drawn */
    constexpr auto ghost_time = std::chrono::milliseconds(150);

    /** Capacity for a window: a little room to grow in a live resize before
     *  it needs another slot (a step that outgrows its slot moves it). */
    int capacity(int size, int limit, bool room) {
      return std::min(limit, room ? (size + 64 + 15) / 16 * 16 : (size + 7) / 8 * 8);
    }

    bool overlaps(int x, int y, int w, int h, int ox, int oy, int ow, int oh) {
      return x < ox + ow && ox < x + w && y < oy + oh && oy < y + h;
    }

    constexpr int piece_margin = 8;

    /** The copy on white only carries alpha (soft at the edges anyway, the
     *  inside opaque): half size, a quarter of the room */
    constexpr double white_ratio = 0.5;
    /** ...inset in its white cell, so filtering at its edges reads white
     *  backing, not whatever is next to the cell */
    constexpr int white_inset = 2;

    bool same_rects(const std::vector<RECT> &a, const std::vector<RECT> &b) {
      return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const RECT &x, const RECT &y) {
               return EqualRect(&x, &y);
             });
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

  std::vector<slot_t> atlas_t::sync(const std::vector<source_t> &sources, bool *moved) {
    std::vector<slot_t> slots;
    bool changed = false;
    if (moved) *moved = false;
    if (!window_) return slots;
    sync_request_t request {&sources, &slots, moved ? moved : &changed};
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
          *request->slots = self->apply(*request->sources, *request->moved);
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
          for (auto &r : self->retired_) DwmUnregisterThumbnail(r.thumb);
          self->retired_.clear();
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
    retire(item.black, item.black_at, false);
    retire(item.white, item.white_at, true);
    item.black = item.white = nullptr;
    for (auto &p : item.pieces) retire(p.thumb, p.cell, false);
    item.pieces.clear();
  }

  void atlas_t::retire(HTHUMBNAIL thumb, const cell_t &cell, bool white, HWND hwnd, int ox, int oy) {
    if (thumb) retired_.push_back({thumb, cell, white, std::chrono::steady_clock::now() + ghost_time, hwnd, ox, oy});
  }

  /** The cells drawing this window 1:1 with its frame's origin at (ox, oy):
   *  where they overlap, they show the very same pixels */
  std::vector<atlas_t::cell_t> atlas_t::same_pixels(HWND hwnd, const item_t &item, int ox, int oy) const {
    std::vector<cell_t> cells;
    if (item.black && item.black_scale == 1.0 && item.black_at.x == ox && item.black_at.y == oy) cells.push_back(item.black_at);
    for (const auto &p : item.pieces) {
      const RECT m = margined(p.part, item);
      if (p.thumb && p.cell.x - m.left == ox && p.cell.y - m.top == oy) cells.push_back(p.cell);
    }
    for (const auto &r : retired_) {
      if (r.hwnd == hwnd && r.ox == ox && r.oy == oy) cells.push_back(r.cell);
    }
    return cells;
  }

  /** The cell is inside the atlas and overlaps only cells in `own` */
  bool atlas_t::fits_over(const std::vector<cell_t> &taken, const cell_t &cell, const std::vector<cell_t> &own) const {
    const int W = monitor_.right - monitor_.left, H = monitor_.bottom - monitor_.top;
    if (cell.cw <= 0 || cell.x < 0 || cell.y < 0 || cell.x + cell.cw > W || cell.y + cell.ch > H) return false;
    for (const auto &t : taken) {
      if (!overlaps(cell.x, cell.y, cell.cw, cell.ch, t.x, t.y, t.cw, t.ch)) continue;
      const bool mine = std::any_of(own.begin(), own.end(), [&](const cell_t &o) {
        return o.x == t.x && o.y == t.y && o.cw == t.cw && o.ch == t.ch;
      });
      if (!mine) return false;
    }
    return true;
  }

  /** This very cell, if nothing taken overlaps it */
  bool atlas_t::take(std::vector<cell_t> &taken, const cell_t &cell) const {
    const int W = monitor_.right - monitor_.left, H = monitor_.bottom - monitor_.top;
    if (cell.cw <= 0 || cell.x < 0 || cell.y < 0 || cell.x + cell.cw > W || cell.y + cell.ch > H) return false;
    for (const auto &t : taken) {
      if (overlaps(cell.x, cell.y, cell.cw, cell.ch, t.x, t.y, t.cw, t.ch)) return false;
    }
    taken.push_back(cell);
    return true;
  }

  std::vector<atlas_t::cell_t> atlas_t::reserved() const {
    std::vector<cell_t> cells;
    for (const auto &r : retired_) cells.push_back(r.cell);
    return cells;
  }

  /** The visible parts of a picture drawn smaller, each 1:1 in a cell of its
   *  own; a part with no room as a whole is split along its long side (the
   *  free space of a full atlas is in strips). With `required`, all of them
   *  or nothing. */
  bool atlas_t::place_pieces(std::vector<cell_t> &taken, HWND hwnd, item_t &item, bool required, bool soft) const {
    constexpr int most = 192, smallest = 128;
    // Each part's cell holds a margin of real pixels around it, so filtering
    // and chroma at its edges never take in a neighbour's
    const int W = monitor_.right - monitor_.left, H = monitor_.bottom - monitor_.top;
    item.plan.clear();
    // Where the window was drawn whole, its parts go to the very place they
    // had in it: wherever they overlap what is (or was just) drawn there, the
    // pixels are the same, so the switch shows in no frame
    const bool aligned = item.full_at.cw > 0;
    std::vector<cell_t> own = aligned ? same_pixels(hwnd, item, item.full_at.x, item.full_at.y) : std::vector<cell_t> {};
    if (item.scale < 1.0) {
      std::vector<RECT> parts(item.want.rbegin(), item.want.rend());  // a stack, first part on top
      while (!parts.empty()) {
        const RECT r = parts.back();
        parts.pop_back();
        const int w = r.right - r.left, h = r.bottom - r.top;
        // A part already drawn stays where it is (soft: its cell is taken
        // for it already)
        auto same = std::find_if(item.pieces.begin(), item.pieces.end(), [&](const piece_item_t &p) {
          return p.thumb && EqualRect(&p.part, &r);
        });
        if (same != item.pieces.end() && (soft || take(taken, same->cell))) {
          item.plan.push_back({r, same->cell});
          continue;
        }
        if (aligned) {
          const RECT m = margined(r, item);
          const cell_t at {item.full_at.x + m.left, item.full_at.y + m.top, capacity(w + 2 * piece_margin, W, false), capacity(h + 2 * piece_margin, H, false)};
          if (fits_over(taken, at, own)) {
            taken.push_back(at);
            own.push_back(at);
            item.plan.push_back({r, at});
            continue;
          }
        }
        cell_t cell;
        if (place(taken, capacity(w + 2 * piece_margin, W, false), capacity(h + 2 * piece_margin, H, false), cell)) {
          item.plan.push_back({r, cell});
          continue;
        }
        if (std::max(w, h) >= 2 * smallest && item.plan.size() + parts.size() + 2 <= most) {
          RECT a = r, b = r;
          if (w >= h) a.right = b.left = r.left + w / 2 / 8 * 8;
          else a.bottom = b.top = r.top + h / 2 / 8 * 8;
          parts.push_back(b);
          parts.push_back(a);
          continue;
        }
        if (required) return false;
      }
    }
    return true;
  }

  /** A part with its margin, inside the frame */
  RECT atlas_t::margined(const RECT &part, const item_t &item) {
    return {std::max<LONG>(0, part.left - piece_margin), std::max<LONG>(0, part.top - piece_margin), std::min<LONG>(item.w, part.right + piece_margin), std::min<LONG>(item.h, part.bottom + piece_margin)};
  }

  /** Pieces as planned: parts already drawn in their cell stay, the rest get
   *  thumbnails of their own; ones no longer planned are retired (hard:
   *  dropped). Answers whether a picture changed place. */
  bool atlas_t::show_pieces(HWND hwnd, item_t &item, bool hard) {
    const bool shown = item.black && item.cell.cw > 0;
    bool moved = false;
    std::vector<piece_item_t> next;
    std::vector<bool> kept(item.pieces.size(), false);
    for (const auto &pl : shown ? item.plan : std::vector<plan_t> {}) {
      const RECT part = margined(pl.part, item);
      const RECT source {item.source_offset.left + part.left, item.source_offset.top + part.top, item.source_offset.left + part.right, item.source_offset.top + part.bottom};
      piece_item_t p;
      std::size_t k = 0;
      while (k < item.pieces.size() && (kept[k] || !item.pieces[k].thumb || !EqualRect(&item.pieces[k].part, &pl.part) || item.pieces[k].cell.x != pl.cell.x || item.pieces[k].cell.y != pl.cell.y)) ++k;
      if (k < item.pieces.size()) {
        p = item.pieces[k];
        kept[k] = true;
      } else {
        if (FAILED(DwmRegisterThumbnail(window_, hwnd, &p.thumb))) continue;
        p.part = pl.part;
        p.cell = pl.cell;
      }
      if (!EqualRect(&source, &p.source)) {
        DWM_THUMBNAIL_PROPERTIES props {};
        props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        props.rcDestination = {p.cell.x, p.cell.y, p.cell.x + (part.right - part.left), p.cell.y + (part.bottom - part.top)};
        props.rcSource = source;
        props.opacity = 255;
        props.fVisible = TRUE;
        props.fSourceClientAreaOnly = FALSE;
        if (SUCCEEDED(DwmUpdateThumbnailProperties(p.thumb, &props))) p.source = source;
      }
      next.push_back(p);
    }
    for (std::size_t k = 0; k < item.pieces.size(); ++k) {
      if (kept[k] || !item.pieces[k].thumb) continue;
      moved = true;
      const RECT m = margined(item.pieces[k].part, item);
      if (hard) DwmUnregisterThumbnail(item.pieces[k].thumb);
      else retire(item.pieces[k].thumb, item.pieces[k].cell, false, hwnd, item.pieces[k].cell.x - m.left, item.pieces[k].cell.y - m.top);
    }
    item.pieces = std::move(next);
    return moved;
  }

  /** MaxRects, best short side fit: the free space is the atlas minus every
   *  taken cell, as maximal free rectangles; the cell goes where it leaves
   *  the least on its shorter side. */
  bool atlas_t::place(std::vector<cell_t> &taken, int cw, int ch, cell_t &out) const {
    const int W = monitor_.right - monitor_.left, H = monitor_.bottom - monitor_.top;
    if (cw > W || ch > H) return false;
    std::vector<cell_t> free {{0, 0, W, H}};
    for (const auto &t : taken) {
      std::vector<cell_t> next;
      for (const auto &f : free) {
        if (!overlaps(f.x, f.y, f.cw, f.ch, t.x, t.y, t.cw, t.ch)) {
          next.push_back(f);
          continue;
        }
        if (t.x > f.x) next.push_back({f.x, f.y, t.x - f.x, f.ch});
        if (t.x + t.cw < f.x + f.cw) next.push_back({t.x + t.cw, f.y, f.x + f.cw - (t.x + t.cw), f.ch});
        if (t.y > f.y) next.push_back({f.x, f.y, f.cw, t.y - f.y});
        if (t.y + t.ch < f.y + f.ch) next.push_back({f.x, t.y + t.ch, f.cw, f.y + f.ch - (t.y + t.ch)});
      }
      // Drop rectangles inside others
      std::vector<cell_t> pruned;
      for (std::size_t i = 0; i < next.size(); ++i) {
        bool inside = false;
        for (std::size_t j = 0; j < next.size() && !inside; ++j) {
          if (i == j) continue;
          const auto &a = next[i], &b = next[j];
          inside = a.x >= b.x && a.y >= b.y && a.x + a.cw <= b.x + b.cw && a.y + a.ch <= b.y + b.ch && (i > j || a.cw != b.cw || a.ch != b.ch || a.x != b.x || a.y != b.y);
        }
        if (!inside) pruned.push_back(next[i]);
      }
      free = std::move(pruned);
    }
    const cell_t *best = nullptr;
    int best_short = INT_MAX, best_long = INT_MAX;
    for (const auto &f : free) {
      if (f.cw < cw || f.ch < ch) continue;
      const int dx = f.cw - cw, dy = f.ch - ch;
      const int sh = std::min(dx, dy), lg = std::max(dx, dy);
      if (sh < best_short || (sh == best_short && lg < best_long)) {
        best = &f;
        best_short = sh;
        best_long = lg;
      }
    }
    if (!best) return false;
    out = {best->x, best->y, cw, ch};
    taken.push_back(out);
    return true;
  }

  bool atlas_t::update_thumbnail(HTHUMBNAIL thumb, const cell_t &cell, const item_t &item, double ratio) {
    DWM_THUMBNAIL_PROPERTIES p {};
    p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    const int inset = ratio < 1.0 ? white_inset : 0;
    p.rcDestination = {cell.x + inset, cell.y + inset, cell.x + cell.cw - inset, cell.y + cell.ch - inset};
    // The capacity as source, from the frame's origin: 1:1 (or the slot's
    // scale), the window anchored top-left, its growth room left empty
    const int sw = static_cast<int>(std::lround((cell.cw - 2 * inset) / (item.scale * ratio))), sh = static_cast<int>(std::lround((cell.ch - 2 * inset) / (item.scale * ratio)));
    p.rcSource = {item.source_offset.left, item.source_offset.top, item.source_offset.left + sw, item.source_offset.top + sh};
    p.opacity = 255;
    p.fVisible = TRUE;
    p.fSourceClientAreaOnly = FALSE;
    return SUCCEEDED(DwmUpdateThumbnailProperties(thumb, &p));
  }

  std::vector<slot_t> atlas_t::apply(const std::vector<source_t> &sources, bool &moved) {
    const int W = monitor_.right - monitor_.left, H = monitor_.bottom - monitor_.top;
    moved = false;
    // Thumbnails left where pictures were have served their time
    const auto now = std::chrono::steady_clock::now();
    const bool expired = std::erase_if(retired_, [&](const retired_t &r) {
                           if (r.until > now) return false;
                           DwmUnregisterThumbnail(r.thumb);
                           return true;
                         }) > 0;
    for (auto &[hwnd, item] : items_) item.used = false;
    std::vector<HWND> to_place, pieces_dirty;
    bool hard = false;  ///< pictures that change place leave no thumbnail behind
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
      const bool fits = item.black && item.cell.cw > 0 && item.cell.cw >= static_cast<int>(std::ceil(s.w * item.scale)) &&
                        item.cell.ch >= static_cast<int>(std::ceil(s.h * item.scale)) && item.has_white == s.alpha &&
                        (!s.alpha || (item.white_cell.cw >= static_cast<int>(std::ceil(s.w * item.scale * white_ratio)) + 2 * white_inset &&
                                      item.white_cell.ch >= static_cast<int>(std::ceil(s.h * item.scale * white_ratio)) + 2 * white_inset));
      const bool shrunk_far = item.black && item.scale == 1.0 && item.cell.cw > capacity(s.w, W, true) * 2 && item.cell.ch > capacity(s.h, H, true) * 2;
      if (!fits || shrunk_far) to_place.push_back(s.hwnd);
      else if (moved_origin || item.w != s.w || item.h != s.h) {
        item.w = s.w;
        item.h = s.h;
        if (moved_origin) {
          update_thumbnail(item.black, item.black_at, item);
          if (item.white) update_thumbnail(item.white, item.white_at, item, white_ratio);
        }
      }
      item.w = s.w;
      item.h = s.h;
      item.has_white = s.alpha;
      std::vector<RECT> want;
      if (!s.alpha) {
        for (const auto &r : s.visible) {
          const RECT c {std::max<LONG>(r.left, 0), std::max<LONG>(r.top, 0), std::min<LONG>(r.right, s.w), std::min<LONG>(r.bottom, s.h)};
          if (c.right > c.left && c.bottom > c.top) want.push_back(c);
        }
      }
      if (item.scale < 1.0 && item.black && item.cell.cw > 0 && !same_rects(want, item.want)) pieces_dirty.push_back(s.hwnd);
      item.want = std::move(want);
    }
    bool removed = false;
    for (auto it = items_.begin(); it != items_.end();) {
      if (!it->second.used) {
        release(it->second);
        it = items_.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }
    // Something drawn smaller gets another try at full size once space frees
    // up (a window went or shrank)
    double area = 0;
    bool any_scaled = false;
    for (const auto &[hwnd, item] : items_) {
      area += static_cast<double>(item.w) * item.h * (item.has_white ? 2 : 1);
      any_scaled = any_scaled || item.scale < 1.0;
    }
    // ...or when a window drawn smaller now shows clearly more than one at
    // full size (it came to the front)
    bool outranked = false;
    for (const auto &s : sources) {
      auto it = items_.find(s.hwnd);
      if (it == items_.end() || it->second.scale >= 1.0) continue;
      for (const auto &o : sources) {
        auto jt = items_.find(o.hwnd);
        if (jt != items_.end() && jt->second.scale >= 1.0 && s.priority > o.priority + o.priority / 5 + 20000) {
          outranked = true;
        }
      }
    }
    // ...or when space left behind by moved pictures frees up
    if (any_scaled && (removed || area < last_area_ * 0.97 || outranked || expired)) {
      to_place.clear();
      for (const auto &[hwnd, item] : items_) to_place.push_back(hwnd);
    }
    last_area_ = area;
    auto placing = [&](HWND h) {
      return std::find(to_place.begin(), to_place.end(), h) != to_place.end();
    };
    std::erase_if(pieces_dirty, placing);

    if (!to_place.empty() || !pieces_dirty.empty()) {
      auto priority = [&](HWND h) {
        for (const auto &s : sources) {
          if (s.hwnd == h) return s.priority;
        }
        return 0;
      };
      // Soft: every picture where it is now stays taken (moved, it is left
      // there for a while), so new places never overlap old ones; a part or
      // window that keeps its place reuses it. Hard (when soft does not
      // fit): only the retired thumbnails' places, and moved pictures are
      // dropped from where they were at once.
      auto base = [&](bool soft) {
        std::vector<cell_t> cells = reserved();
        if (!soft) return cells;
        for (const auto &[hwnd, item] : items_) {
          if (item.black) cells.push_back(item.black_at);
          if (item.white) cells.push_back(item.white_at);
          for (const auto &p : item.pieces) cells.push_back(p.cell);
        }
        return cells;
      };
      bool soft = true;
      std::vector<cell_t> taken = base(true);
      std::sort(to_place.begin(), to_place.end(), [&](HWND a, HWND b) {
        return priority(a) > priority(b);
      });
      // pieces: 2 all of them or fail, 1 where they fit, 0 none (placed later)
      auto place_item = [&](HWND hwnd, item_t &item, double scale, int pieces = 2) {
        // Windows get a little room to grow (live resizes); popups never resize
        const bool room = !item.has_white;
        const int cw = capacity(static_cast<int>(std::ceil(item.w * scale)), W, room), ch = capacity(static_cast<int>(std::ceil(item.h * scale)), H, room);
        const int wcw = capacity(static_cast<int>(std::ceil(item.w * scale * white_ratio)) + 2 * white_inset, W, false), wch = capacity(static_cast<int>(std::ceil(item.h * scale * white_ratio)) + 2 * white_inset, H, false);
        std::vector<cell_t> trial = taken;
        cell_t cell, white;
        // Where it is drawn now, if that is still the size it needs
        const bool same = item.black && item.black_at.cw == cw && item.black_at.ch == ch && item.black_scale == scale &&
                          (!item.has_white || (item.white && item.white_at.cw == wcw && item.white_at.ch == wch));
        // Whole again where it was whole, over its own parts there
        const bool back = !same && scale == 1.0 && !item.has_white && item.full_at.cw == cw && item.full_at.ch == ch &&
                          fits_over(trial, item.full_at, same_pixels(hwnd, item, item.full_at.x, item.full_at.y));
        if (same && (soft || (take(trial, item.black_at) && (!item.has_white || take(trial, item.white_at))))) {
          cell = item.black_at;
          white = item.white_at;
        } else if (back) {
          cell = item.full_at;
          trial.push_back(cell);
        } else {
          if (!place(trial, cw, ch, cell)) return false;
          if (item.has_white && !place(trial, wcw, wch, white)) return false;
        }
        item.scale = scale;
        if (pieces == 0) {
          item.plan.clear();
        } else if (!place_pieces(trial, hwnd, item, pieces == 2, soft)) {
          return false;
        }
        taken = std::move(trial);
        item.cell = cell;
        item.white_cell = item.has_white ? white : cell_t {};
        item.scale = scale;
        return true;
      };
      bool all = true;
      for (HWND hwnd : pieces_dirty) {
        std::vector<cell_t> trial = taken;
        if (!place_pieces(trial, hwnd, items_[hwnd], !items_[hwnd].loose, true)) {
          all = false;
          break;
        }
        taken = std::move(trial);
      }
      for (HWND hwnd : all ? to_place : std::vector<HWND> {}) {
        if (!place_item(hwnd, items_[hwnd], 1.0)) {
          all = false;
          break;
        }
      }
      if (!all) {
        // Full: everything again, front-most first at full size; what does
        // not fit then goes smaller (the client scales it back up)
        // Everything at full size, largest first (packs best); if that
        // does not fit, the back-most windows go smaller, one step at a time
        std::vector<HWND> order;
        for (const auto &[hwnd, item] : items_) order.push_back(hwnd);
        std::vector<HWND> by_priority = order;
        std::sort(by_priority.begin(), by_priority.end(), [&](HWND a, HWND b) {
          return priority(a) < priority(b);  // back-most first
        });
        std::map<HWND, double> scales;
        for (HWND h : order) scales[h] = 1.0;
        auto area = [&](HWND h) {
          const auto &it = items_[h];
          return static_cast<double>(it.w) * it.h * scales[h] * scales[h] * (it.has_white ? 2 : 1);
        };
        static const double steps[] = {1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.12};
        // Windows whose pieces are taken only where room is left, after
        // everything else (least visible windows, once nothing else gives)
        std::set<HWND> loose;
        long long loose_area = 0;  ///< of the loose pieces the last packing placed
        auto pack_all = [&] {
          taken = base(soft);
          loose_area = 0;
          std::sort(order.begin(), order.end(), [&](HWND a, HWND b) {
            return area(a) > area(b);
          });
          for (HWND hwnd : order) {
            if (!place_item(hwnd, items_[hwnd], scales[hwnd], loose.contains(hwnd) ? 0 : 2)) return false;
          }
          for (auto h = by_priority.rbegin(); h != by_priority.rend(); ++h) {
            if (!loose.contains(*h)) continue;
            place_pieces(taken, *h, items_[*h], false, soft);
            for (const auto &p : items_[*h].plan) loose_area += static_cast<long long>(p.part.right - p.part.left) * (p.part.bottom - p.part.top);
          }
          return true;
        };
        // Popups and the focused window are the last to give anything up;
        // a focused window that hangs off the screen or is partly covered
        // costs less as its visible parts (just as sharp) than whole
        auto top = [&](HWND h) {
          const auto &it = items_[h];
          if (it.has_white) return true;
          if (priority(h) < INT_MAX / 4) return false;
          long long shown = 0;
          for (const auto &r : it.want) shown += static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
          return shown * 100 >= static_cast<long long>(it.w) * it.h * 85;
        };
        // Soft first, all the way down to the smallest stand-ins (the visible
        // parts stay sharp either way); only if even that does not fit, hard
        bool packed = false;
        auto run = [&](bool soft_mode) {
          soft = soft_mode;
          for (HWND h : order) scales[h] = 1.0;
          loose.clear();
          packed = false;
          for (int round = 0; round < 64 && !packed; ++round) {
            packed = pack_all();
            if (packed) {
              // Then the stand-ins of windows that went smaller as large as
              // still fits (most visible first): what a move uncovers before
              // its pieces come shows nearly sharp
              const long long sharp = loose_area;
              for (auto h = by_priority.rbegin(); h != by_priority.rend(); ++h) {
                if (scales[*h] >= 1.0 || items_[*h].has_white || loose.contains(*h)) continue;
                const double was = scales[*h];
                for (double step : steps) {
                  if (step <= was + 1e-9) break;
                  scales[*h] = step;
                  if (pack_all() && loose_area >= sharp) break;
                  scales[*h] = was;
                }
              }
              // The last trial may have failed: place everything as chosen
              packed = pack_all();
              break;
            }
            // Shrink a step the window showing the fewest pixels at its current
            // scale: the loss spreads over the least visible windows instead
            // of one window going very blurry
            HWND pick = nullptr;
            double least = 0;
            for (int pass = 0; pass < 2 && !pick; ++pass) {
              for (HWND h : by_priority) {
                if (scales[h] <= steps[std::size(steps) - 1] || top(h) != (pass == 1)) continue;
                const double shown = static_cast<double>(priority(h)) * scales[h];
                if (!pick || shown < least) {
                  pick = h;
                  least = shown;
                }
              }
              if (pick || pass == 1) break;
              // Every other window is as small as it goes: the least visible
              // one's pieces go where room is left
              for (HWND h : by_priority) {
                if (!top(h) && !loose.contains(h) && scales[h] < 1.0 && !items_[h].want.empty()) {
                  loose.insert(h);
                  pick = h;
                  break;
                }
              }
              if (pick) break;
            }
            if (!pick) break;
            if (loose.contains(pick) && !top(pick) && scales[pick] <= steps[std::size(steps) - 1]) continue;
            // A window's visible parts stay 1:1 (pieces), so the rest of it
            // goes straight to a small stand-in; popups have no pieces
            if (!items_[pick].has_white) {
              scales[pick] = scales[pick] > 0.25 ? 0.25 : steps[std::size(steps) - 1];
              continue;
            }
            for (std::size_t k = 0; k + 1 < std::size(steps); ++k) {
              if (scales[pick] >= steps[k] - 1e-9) {
                scales[pick] = steps[k + 1];
                break;
              }
            }
          }
        };
        run(true);
        if (!packed) run(false);
        to_place = order;
        for (auto &[hwnd, item] : items_) item.loose = loose.contains(hwnd);
        if (!packed) {
          // Not even then: what fits, pieces where they fit
          soft = false;
          taken = base(false);
          for (HWND hwnd : order) {
            auto &item = items_[hwnd];
            if (!place_item(hwnd, item, scales[hwnd], 1)) {
              item.cell = {};
              item.white_cell = {};
              item.plan.clear();
            }
          }
        }
        BOOST_LOG(info) << "Desk atlas: repacked " << order.size() << " windows";
      }
      hard = !soft;
      if (hard) ++hard_repacks_;
      // A picture that changed place gets a new thumbnail there; the old one
      // stays where it was for a while (hard: goes now)
      auto leave = [&](HWND hwnd, HTHUMBNAIL &thumb, const cell_t &at, bool white, bool whole) {
        if (!thumb) return;
        moved = true;
        if (hard) DwmUnregisterThumbnail(thumb);
        else retire(thumb, at, white, hwnd, whole ? at.x : INT_MIN, whole ? at.y : INT_MIN);
        thumb = nullptr;
      };
      for (HWND hwnd : to_place) {
        auto &item = items_[hwnd];
        const auto same = [](const cell_t &a, const cell_t &b) {
          return a.x == b.x && a.y == b.y && a.cw == b.cw && a.ch == b.ch;
        };
        if (item.cell.cw <= 0 || !same(item.black_at, item.cell) || item.black_scale != item.scale) leave(hwnd, item.black, item.black_at, false, item.black_scale == 1.0 && !item.has_white);
        if (item.cell.cw <= 0 || !item.has_white || !same(item.white_at, item.white_cell)) leave(hwnd, item.white, item.white_at, true, false);
        if (item.cell.cw <= 0) continue;
        if (!item.black && SUCCEEDED(DwmRegisterThumbnail(window_, hwnd, &item.black))) {
          item.black_at = item.cell;
          item.black_scale = item.scale;
          if (item.scale == 1.0 && !item.has_white) item.full_at = item.cell;
          update_thumbnail(item.black, item.cell, item);
        }
        if (item.has_white && !item.white && SUCCEEDED(DwmRegisterThumbnail(window_, hwnd, &item.white))) {
          item.white_at = item.white_cell;
          update_thumbnail(item.white, item.white_cell, item, white_ratio);
        }
      }
    }

    for (auto &[hwnd, item] : items_) moved = show_pieces(hwnd, item, hard) || moved;
    // Dropped at once, nothing is left where pictures were: the new places
    // hold from the next composition on, as precisely as that can be dated
    if (hard) moved = false;

    // The white backings first in the frame order: painted now, synchronously
    std::vector<RECT> white;
    for (const auto &[hwnd, item] : items_) {
      if (item.white && item.white_at.cw > 0) {
        white.push_back({item.white_at.x, item.white_at.y, item.white_at.x + item.white_at.cw, item.white_at.y + item.white_at.ch});
      }
    }
    for (const auto &r : retired_) {
      if (r.white) white.push_back({r.cell.x, r.cell.y, r.cell.x + r.cell.cw, r.cell.y + r.cell.ch});
    }
    const bool repaint = white.size() != white_.size() || !std::equal(white.begin(), white.end(), white_.begin(), [](const RECT &a, const RECT &b) {
                           return EqualRect(&a, &b);
                         });
    if (repaint) {
      white_ = std::move(white);
      InvalidateRect(window_, nullptr, FALSE);
      UpdateWindow(window_);
    }

    ghost_count_ = retired_.size();
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
        s.ax = item.white_cell.x + white_inset;
        s.ay = item.white_cell.y + white_inset;
        s.white_scale = white_ratio;
      }
      for (const auto &p : item.pieces) {
        if (!p.thumb || p.cell.cw <= 0) continue;
        const RECT part = margined(p.part, item);
        s.pieces.push_back({p.part.left, p.part.top, p.part.right - p.part.left, p.part.bottom - p.part.top, p.cell.x + (p.part.left - part.left), p.cell.y + (p.part.top - part.top)});
      }
      out.push_back(s);
    }
    return out;
  }

}  // namespace desk_atlas
