/**
 * @file src/platform/windows/desk_hold.cpp
 * @brief See desk_hold.h.
 */
#include "desk_hold.h"

#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <tuple>

namespace desk_hold {

  namespace {
    using clock = std::chrono::steady_clock;

    struct layout_t {
      int seq = -1;
      std::vector<region_t> regions;
    };

    struct spec_t {
      RECT monitor {};
      std::deque<layout_t> layouts;  ///< the last few, newest last
      std::function<void(int)> request_frame;
      bool active = false;
    };

    constexpr std::size_t layouts_kept = 8;

    std::mutex g_mutex;
    spec_t g_spec;
    int g_background_qp = 0;  ///< under g_mutex
    stats_t g_stats;  ///< under g_mutex

    // A cell with one window's picture at one size
    using key_t = std::tuple<HWND, LONG, LONG, LONG, LONG, int, int>;

    key_t key_of(const region_t &r) {
      return {r.window, r.rect.left, r.rect.top, r.rect.right, r.rect.bottom, r.w, r.h};
    }

    struct cell_t {
      bool taken = false;  ///< its pixels are in the texture
      clock::time_point refreshed;
    };

    // The capture's state: the pixels kept, per cell
    struct held_t {
      ID3D11Device *device = nullptr;  ///< the textures' device (not owned: compared only)
      ID3D11Texture2D *texture = nullptr;
      ID3D11Texture2D *marker = nullptr;  ///< staging: the layout number's row, read back
      ID3D11Texture2D *black = nullptr;  ///< the number's rows, all black: a frame that carries no number
      D3D11_TEXTURE2D_DESC desc {};
      std::map<key_t, cell_t> cells;
    };

    std::mutex g_held_mutex;  ///< one capture at a time uses it (backends can overlap while one is rebuilt)
    held_t g_held;

    void release_textures() {
      if (g_held.texture) g_held.texture->Release();
      if (g_held.marker) g_held.marker->Release();
      if (g_held.black) g_held.black->Release();
      g_held.texture = nullptr;
      g_held.marker = nullptr;
      g_held.black = nullptr;
      g_held.device = nullptr;
      g_held.cells.clear();
    }

    D3D11_BOX box_of(const RECT &r, const D3D11_TEXTURE2D_DESC &desc) {
      return D3D11_BOX {
        static_cast<UINT>(std::clamp<LONG>(r.left, 0, desc.Width)),
        static_cast<UINT>(std::clamp<LONG>(r.top, 0, desc.Height)),
        0,
        static_cast<UINT>(std::clamp<LONG>(r.right, 0, desc.Width)),
        static_cast<UINT>(std::clamp<LONG>(r.bottom, 0, desc.Height)),
        1,
      };
    }

    bool readable(DXGI_FORMAT f) {
      return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM;
    }

    /** The layout number `dst` carries, or -1 */
    int read_seq(ID3D11DeviceContext *ctx, ID3D11Texture2D *dst) {
      const auto &d = g_held.desc;
      if (!g_held.marker || d.Height < static_cast<UINT>(marker_rows)) return -1;
      const D3D11_BOX row {0, d.Height - marker_rows / 2, 0, static_cast<UINT>(marker_block * 16), d.Height - marker_rows / 2 + 1, 1};
      ctx->CopySubresourceRegion(g_held.marker, 0, 0, 0, 0, dst, 0, &row);
      D3D11_MAPPED_SUBRESOURCE m {};
      if (FAILED(ctx->Map(g_held.marker, 0, D3D11_MAP_READ, 0, &m))) return -1;
      const auto *px = static_cast<const std::uint8_t *>(m.pData);
      std::uint16_t code = 0;
      for (int i = 0; i < 16; ++i) {
        // green: the same byte in BGRA and RGBA
        if (px[(i * marker_block + marker_block / 2) * 4 + 1] > 128) code |= static_cast<std::uint16_t>(1u << i);
      }
      ctx->Unmap(g_held.marker, 0);
      return marker_seq(code);
    }
  }  // namespace

  void set(const RECT &monitor, int seq, std::vector<region_t> regions, std::function<void(int)> request_frame) {
    std::lock_guard lock(g_mutex);
    if (!EqualRect(&g_spec.monitor, &monitor)) g_spec.layouts.clear();
    g_spec.monitor = monitor;
    auto it = std::find_if(g_spec.layouts.begin(), g_spec.layouts.end(), [&](const layout_t &l) {
      return l.seq == seq;
    });
    if (it != g_spec.layouts.end()) g_spec.layouts.erase(it);
    g_spec.layouts.push_back({seq, std::move(regions)});
    while (g_spec.layouts.size() > layouts_kept) g_spec.layouts.pop_front();
    g_spec.request_frame = std::move(request_frame);
    g_spec.active = true;
  }

  void set_background_qp(int qp_delta) {
    std::lock_guard lock(g_mutex);
    g_background_qp = std::clamp(qp_delta, 0, 20);
  }

  std::uint64_t background_cells(std::vector<std::array<int, 5>> &out) {
    out.clear();
    std::lock_guard lock(g_mutex);
    if (!g_spec.active || g_spec.layouts.empty() || g_background_qp <= 0) return 0;
    std::uint64_t sig = 1469598103934665603ull;
    auto mix = [&sig](std::int64_t v) {
      sig = (sig ^ static_cast<std::uint64_t>(v)) * 1099511628211ull;
    };
    mix(g_background_qp);
    // A cell just placed (a window moved across one behind it uncovers new
    // parts, which go in new cells) is coded at the usual quality first:
    // its content did not change, it only has to arrive sharp. Lower
    // quality is for what changes in it later.
    static std::map<key_t, clock::time_point> placed;
    const auto now = clock::now();
    const auto &regions = g_spec.layouts.back().regions;
    std::erase_if(placed, [&](const auto &e) {
      return std::none_of(regions.begin(), regions.end(), [&](const region_t &r) {
        return key_of(r) == e.first;
      });
    });
    for (const auto &r : regions) {
      if (!r.background) continue;
      const auto since = placed.try_emplace(key_of(r), now).first->second;
      if (now - since < std::chrono::milliseconds(1500)) {
        mix(-1);  // (the map changes again when it stops being new)
        continue;
      }
      out.push_back({r.rect.left, r.rect.top, r.rect.right - r.rect.left, r.rect.bottom - r.rect.top, g_background_qp});
      for (int v : out.back()) mix(v);
    }
    return sig;
  }

  void clear() {
    {
      std::lock_guard lock(g_mutex);
      g_spec = {};
      g_background_qp = 0;
    }
    std::lock_guard lock(g_held_mutex);
    release_textures();
  }

  void apply(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *dst, int x, int y, int w, int h, int fps) {
    spec_t spec;
    {
      std::lock_guard lock(g_mutex);
      ++g_stats.frames;
      g_stats.x = x;
      g_stats.y = y;
      g_stats.w = w;
      g_stats.h = h;
      g_stats.fps = fps;
      if (!g_spec.active || g_spec.monitor.left != x || g_spec.monitor.top != y || g_spec.monitor.right - g_spec.monitor.left != w ||
          g_spec.monitor.bottom - g_spec.monitor.top != h) {
        return;
      }
      spec = g_spec;
      ++g_stats.matched;
    }
    if (!device || !ctx || !dst || fps <= 0 || spec.layouts.empty()) return;
    std::lock_guard held_lock(g_held_mutex);

    D3D11_TEXTURE2D_DESC desc {};
    dst->GetDesc(&desc);
    // A new capture device (the backend was rebuilt): textures of its own
    if (!g_held.texture || g_held.device != device || g_held.desc.Width != desc.Width || g_held.desc.Height != desc.Height || g_held.desc.Format != desc.Format) {
      release_textures();
      D3D11_TEXTURE2D_DESC d = desc;
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = 0;
      d.CPUAccessFlags = 0;
      d.MiscFlags = 0;
      d.MipLevels = 1;
      d.ArraySize = 1;
      if (FAILED(device->CreateTexture2D(&d, nullptr, &g_held.texture))) {
        g_held.texture = nullptr;
        return;
      }
      if (readable(desc.Format)) {
        D3D11_TEXTURE2D_DESC m = d;
        m.Width = marker_block * 16;
        m.Height = 1;
        m.Usage = D3D11_USAGE_STAGING;
        m.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&m, nullptr, &g_held.marker))) g_held.marker = nullptr;
        // (zero is black in the 8-bit formats)
        std::vector<std::uint8_t> zeros(static_cast<std::size_t>(marker_block) * 16 * marker_rows * 4);
        D3D11_SUBRESOURCE_DATA init {zeros.data(), static_cast<UINT>(marker_block * 16 * 4), 0};
        D3D11_TEXTURE2D_DESC k = d;
        k.Width = marker_block * 16;
        k.Height = marker_rows;
        if (FAILED(device->CreateTexture2D(&k, &init, &g_held.black))) g_held.black = nullptr;
      }
      g_held.desc = desc;
      g_held.device = device;
    }

    // The frame's own layout (a frame composed while the pictures change
    // carries none, and is not drawn); a format the number cannot be read
    // from: the newest
    const int seq = g_held.marker ? read_seq(ctx, dst) : spec.layouts.back().seq;
    const auto layout = std::find_if(spec.layouts.begin(), spec.layouts.end(), [&](const layout_t &l) {
      return l.seq == seq;
    });
    {
      std::lock_guard lock(g_mutex);
      g_stats.seq = seq;
      if (layout != spec.layouts.end()) {
        ++g_stats.known;
        g_stats.regions = static_cast<int>(layout->regions.size());
      }
    }
    if (layout == spec.layouts.end()) return;

    // A window not in this frame's layout loses its pixels: shown again
    // (a menu reusing its window), it starts over from what it shows then
    std::erase_if(g_held.cells, [&](const auto &e) {
      return std::none_of(layout->regions.begin(), layout->regions.end(), [&](const region_t &r) {
        return r.window == std::get<0>(e.first);
      });
    });

    // Each window as it is now (DWM drew this frame's thumbnails from that,
    // or from a state before it): is it as the layout says?
    std::map<HWND, bool> like;
    auto as_laid_out = [&](const region_t &r) {
      auto [it, fresh] = like.try_emplace(r.window, true);
      if (fresh && r.window) {
        RECT frame {};
        const bool sized = SUCCEEDED(DwmGetWindowAttribute(r.window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) || GetWindowRect(r.window, &frame);
        DWORD cloaked = 0;
        if (FAILED(DwmGetWindowAttribute(r.window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) cloaked = 0;
        it->second = sized && IsWindowVisible(r.window) && !IsIconic(r.window) && !cloaked && frame.right - frame.left == r.w && frame.bottom - frame.top == r.h;
      }
      return it->second;
    };

    // Per window, so its cells (picture, copy on white, pieces) always show
    // the same moment: held all or none, refreshed all at once
    struct window_t {
      std::vector<std::pair<const region_t *, cell_t *>> cells;
      bool taken = true;  ///< every cell has pixels
      bool due = false;  ///< a cell is due for current pixels
    };
    std::map<HWND, window_t> windows;
    const auto now = clock::now();
    const auto frame = std::chrono::duration<double>(1.0 / fps);
    auto interval_of = [&](const region_t &r) {
      // Due a little early rather than a frame late (frames are not exactly periodic)
      return std::chrono::duration_cast<clock::duration>(frame * (std::max(1, r.divisor) - 0.5));
    };
    for (const auto &r : layout->regions) {
      const auto b = box_of(r.rect, desc);
      if (b.right <= b.left || b.bottom <= b.top) continue;
      auto &cell = g_held.cells[key_of(r)];
      auto &w = windows[r.window];
      w.cells.push_back({&r, &cell});
      w.taken = w.taken && cell.taken;
      w.due = w.due || r.divisor <= 1 || !cell.taken || now - cell.refreshed >= interval_of(r);
    }

    auto next_due = clock::time_point::max();
    std::uint64_t held = 0, refreshed = 0, unlike = 0, unnumbered = 0;
    for (auto &[hwnd, w] : windows) {
      const bool ok = as_laid_out(*w.cells.front().first);
      if (ok && w.due) {
        for (auto &[r, cell] : w.cells) {
          const auto b = box_of(r->rect, desc);
          ctx->CopySubresourceRegion(g_held.texture, 0, b.left, b.top, 0, dst, 0, &b);
          cell->taken = true;
          cell->refreshed = now;
          if (r->divisor > 1) ++refreshed;
        }
        continue;
      }
      if (!ok) {
        ++unlike;
        // The layout follows within a few ms; a frame then, in case nothing
        // else changes
        next_due = std::min(next_due, now + std::chrono::milliseconds(8));
      }
      // Its last pixels; without them for every cell, the frame is not
      // drawn at all (it carries no layout number)
      if (!w.taken) {
        if (!ok && g_held.black && desc.Height >= static_cast<UINT>(marker_rows)) {
          ctx->CopySubresourceRegion(dst, 0, 0, desc.Height - marker_rows, 0, g_held.black, 0, nullptr);
          ++unnumbered;
        }
        continue;
      }
      for (auto &[r, cell] : w.cells) {
        const auto b = box_of(r->rect, desc);
        ctx->CopySubresourceRegion(dst, 0, b.left, b.top, 0, g_held.texture, 0, &b);
        ++held;
        if (ok) next_due = std::min(next_due, cell->refreshed + interval_of(*r));
      }
    }
    {
      std::lock_guard lock(g_mutex);
      g_stats.held += held;
      g_stats.refreshed += refreshed;
      g_stats.unlike += unlike;
      g_stats.unnumbered += unnumbered;
    }
    // Something was held back: a frame when it is due, in case nothing else
    // changes by then
    if (next_due != clock::time_point::max() && spec.request_frame) {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next_due - now).count();
      spec.request_frame(static_cast<int>(std::max<long long>(1, ms + 1)));
    }
  }

  stats_t stats() {
    std::lock_guard lock(g_mutex);
    return g_stats;
  }

}  // namespace desk_hold
