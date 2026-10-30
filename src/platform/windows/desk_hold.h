/**
 * @file src/platform/windows/desk_hold.h
 * @brief Lower frame rates for parts of a captured desk atlas: regions keep
 *        the pixels of an earlier frame and take the current ones only every
 *        so often. The encoder spends next to nothing on pixels that did not
 *        change, so a window in the background at a quarter of the rate costs
 *        about a quarter of the bits.
 *
 * The atlas says which regions (its unfocused windows' cells) and how much
 * lower; the capture backends call apply() right after each frame is copied
 * into the image the encoder gets. A region that held back a change asks the
 * atlas for one more frame when it is due, so the last change of a window
 * that then stops is always shown.
 */
#pragma once

#include <windows.h>

#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace desk_hold {

  struct region_t {
    RECT rect {};  ///< in the atlas (monitor-relative pixels)
    int divisor = 1;  ///< 2, 4, 8: that fraction of the frame rate
  };

  /** The regions of the atlas covering `monitor` (virtual-desktop pixels),
   *  for its layout `generation` (regions of another generation are refreshed
   *  at once: their cells now hold other pictures). `request_frame(ms)` makes
   *  the atlas change a pixel after ms. */
  void set(const RECT &monitor, std::uint64_t generation, std::vector<region_t> regions, std::function<void(int)> request_frame);
  void clear();

  /** Capture thread: `dst` is the frame just captured of the display at
   *  (x, y) w x h (virtual-desktop pixels), at `fps`. */
  void apply(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *dst, int x, int y, int w, int h, int fps);

  /** Diagnostics: frames seen, frames matching the atlas, regions held back,
   *  regions refreshed, and the last frame's display rect */
  struct stats_t {
    std::uint64_t frames = 0, matched = 0, held = 0, refreshed = 0;
    int x = 0, y = 0, w = 0, h = 0, fps = 0, regions = 0;
  };
  stats_t stats();

}  // namespace desk_hold
