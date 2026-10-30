/**
 * @file src/platform/windows/desk_hold.h
 * @brief Keeps parts of a captured desk atlas at the pixels of an earlier
 *        frame, applied to each frame right after it is captured:
 *
 * - A window's cells while the window is not as the layout says (another
 *   size, or hidden). DWM draws a window's thumbnail as the window is at the
 *   composition, and that can be ahead of the layout the frame carries: a
 *   menu that closes or is reused for another menu shows DWM's placeholder (a
 *   gradient with the app's icon) or the window at another size for a frame
 *   or two before the next layout says so. Its cells keep their last pixels
 *   that matched the layout instead, so every frame is exactly its layout.
 * - Windows in the background at a lower frame rate: their cells take the
 *   current pixels only every so often. The encoder spends next to nothing on
 *   pixels that did not change, so a window at a quarter of the rate costs
 *   about a quarter of the bits. A region that held back a change asks the
 *   atlas for one more frame when it is due, so the last change of a window
 *   that then stops is always shown.
 *
 * Each frame is matched to its own layout by the number it carries (read
 * back from the captured frame: 256 pixels).
 */
#pragma once

#include <windows.h>

#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace desk_hold {

  /** The layout number in the atlas's bottom rows: 16 blocks, white for a
   *  set bit, 12 bits of number and a 4-bit check (the inverted bit count: an
   *  all-black row never checks) */
  constexpr int marker_rows = 8;
  constexpr int marker_block = 16;

  inline std::uint16_t marker_code(std::uint16_t seq) {
    seq &= 0xfff;
    int bits = 0;
    for (int i = 0; i < 12; ++i) bits += (seq >> i) & 1;
    return static_cast<std::uint16_t>(seq | ((~bits & 0xf) << 12));
  }

  /** The number a code carries, or -1 when it does not check */
  inline int marker_seq(std::uint16_t code) {
    return marker_code(code & 0xfff) == code ? code & 0xfff : -1;
  }

  struct region_t {
    RECT rect {};  ///< in the atlas (monitor-relative pixels)
    int divisor = 1;  ///< 1, or 2, 4, 8: that fraction of the frame rate
    HWND window = nullptr;  ///< whose picture it is
    int w = 0, h = 0;  ///< the window's frame size in the layout
  };

  /** The regions of the atlas covering `monitor` (virtual-desktop pixels) in
   *  its layout `seq`, set before a frame can carry that number (the last few
   *  layouts are kept: a frame is matched to its own). `request_frame(ms)`
   *  makes the atlas change a pixel after ms. */
  void set(const RECT &monitor, int seq, std::vector<region_t> regions, std::function<void(int)> request_frame);
  void clear();

  /** Capture thread: `dst` is the frame just captured of the display at
   *  (x, y) w x h (virtual-desktop pixels), at `fps`. */
  void apply(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Texture2D *dst, int x, int y, int w, int h, int fps);

  /** Diagnostics: frames seen, frames of the atlas, frames whose layout was
   *  known, regions held back, regions refreshed (lower rates), regions held
   *  because their window was not as laid out, frames left without a number
   *  (such a window had no earlier pixels), and the last frame's display
   *  rect */
  struct stats_t {
    std::uint64_t frames = 0, matched = 0, known = 0, held = 0, refreshed = 0, unlike = 0, unnumbered = 0;
    int x = 0, y = 0, w = 0, h = 0, fps = 0, regions = 0, seq = -1;
  };
  stats_t stats();

}  // namespace desk_hold
