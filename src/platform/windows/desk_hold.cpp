/**
 * @file src/platform/windows/desk_hold.cpp
 * @brief See desk_hold.h.
 */
#include "desk_hold.h"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace desk_hold {

  namespace {
    struct spec_t {
      RECT monitor {};
      std::uint64_t generation = 0;
      std::vector<region_t> regions;
      std::function<void(int)> request_frame;
      bool active = false;
    };

    std::mutex g_mutex;
    spec_t g_spec;

    // The capture's state: its pixels kept, and when each region took them
    struct held_t {
      ID3D11Device *device = nullptr;  ///< the texture's device (not owned: compared only)
      ID3D11Texture2D *texture = nullptr;
      D3D11_TEXTURE2D_DESC desc {};
      std::uint64_t generation = ~0ull;
      std::vector<std::chrono::steady_clock::time_point> refreshed;  ///< per region
    };

    std::mutex g_held_mutex;  ///< one capture at a time uses it (backends can overlap while one is rebuilt)
    held_t g_held;

    void release_texture() {
      if (g_held.texture) g_held.texture->Release();
      g_held.texture = nullptr;
      g_held.device = nullptr;
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
  }  // namespace

  void set(const RECT &monitor, std::uint64_t generation, std::vector<region_t> regions, std::function<void(int)> request_frame) {
    std::lock_guard lock(g_mutex);
    g_spec.monitor = monitor;
    g_spec.generation = generation;
    g_spec.regions = std::move(regions);
    g_spec.request_frame = std::move(request_frame);
    g_spec.active = true;
  }

  void clear() {
    {
      std::lock_guard lock(g_mutex);
      g_spec = {};
    }
    std::lock_guard lock(g_held_mutex);
    release_texture();
    g_held.refreshed.clear();
  }

  void apply(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *dst, int x, int y, int w, int h, int fps) {
    spec_t spec;
    {
      std::lock_guard lock(g_mutex);
      if (!g_spec.active || g_spec.monitor.left != x || g_spec.monitor.top != y || g_spec.monitor.right - g_spec.monitor.left != w ||
          g_spec.monitor.bottom - g_spec.monitor.top != h) {
        return;
      }
      spec = g_spec;
    }
    if (!device || !ctx || !dst || fps <= 0) return;
    std::lock_guard held_lock(g_held_mutex);
    bool any = false;
    for (const auto &r : spec.regions) any = any || r.divisor > 1;
    if (!any) return;

    D3D11_TEXTURE2D_DESC desc {};
    dst->GetDesc(&desc);
    // A new capture device (the backend was rebuilt): a texture of its own
    if (!g_held.texture || g_held.device != device || g_held.desc.Width != desc.Width || g_held.desc.Height != desc.Height || g_held.desc.Format != desc.Format) {
      release_texture();
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
      g_held.desc = desc;
      g_held.device = device;
      g_held.generation = ~0ull;
    }

    const auto now = std::chrono::steady_clock::now();
    // Another layout: the cells hold other pictures now, all current at once
    if (g_held.generation != spec.generation || g_held.refreshed.size() != spec.regions.size()) {
      g_held.generation = spec.generation;
      g_held.refreshed.assign(spec.regions.size(), now);
      for (const auto &r : spec.regions) {
        if (r.divisor <= 1) continue;
        const auto b = box_of(r.rect, desc);
        if (b.right > b.left && b.bottom > b.top) ctx->CopySubresourceRegion(g_held.texture, 0, b.left, b.top, 0, dst, 0, &b);
      }
      return;
    }

    const auto frame = std::chrono::duration<double>(1.0 / fps);
    auto next_due = std::chrono::steady_clock::time_point::max();
    for (std::size_t i = 0; i < spec.regions.size(); ++i) {
      const auto &r = spec.regions[i];
      if (r.divisor <= 1) continue;
      const auto b = box_of(r.rect, desc);
      if (b.right <= b.left || b.bottom <= b.top) continue;
      // Due a little early rather than a frame late (frames are not exactly periodic)
      const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame * (r.divisor - 0.5));
      if (now - g_held.refreshed[i] >= interval) {
        ctx->CopySubresourceRegion(g_held.texture, 0, b.left, b.top, 0, dst, 0, &b);
        g_held.refreshed[i] = now;
      } else {
        ctx->CopySubresourceRegion(dst, 0, b.left, b.top, 0, g_held.texture, 0, &b);
        next_due = std::min(next_due, g_held.refreshed[i] + interval);
      }
    }
    // Something was held back: a frame when it is due, in case nothing else
    // changes by then
    if (next_due != std::chrono::steady_clock::time_point::max() && spec.request_frame) {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next_due - now).count();
      spec.request_frame(static_cast<int>(std::max<long long>(1, ms + 1)));
    }
  }

}  // namespace desk_hold
