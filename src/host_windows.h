/**
 * @file src/host_windows.h
 * @brief Enumerate, inspect and act on the host's top-level windows for
 *        per-window ("app window") streaming. Windows only; other platforms
 *        report supported() == false and empty results.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace host_windows {

  struct window_info_t {
    std::uint64_t handle = 0;  ///< HWND as an integer
    std::string title;
    std::string process;  ///< executable file name (no path)
    std::uint32_t pid = 0;
    int rect[4] {0, 0, 0, 0};  ///< outer rect (DWM extended frame bounds): x, y, w, h in virtual-desktop pixels
    int bounds[4] {0, 0, 0, 0};  ///< raw window rect (GetWindowRect) = the captured image's extent
    int client[4] {0, 0, 0, 0};  ///< client area rect, same space
    int caption = 0;  ///< caption bar height in pixels (client top - outer top)
    int borders[4] {0, 0, 0, 0};  ///< frame thickness left, top (excluding caption), right, bottom
    std::string state;  ///< "normal", "minimized" or "maximized"
    bool focused = false;
    std::string icon_key;  ///< stable cache key for the window icon ("" when none)
    std::string monitor;  ///< GDI device name of the monitor holding most of the window
    int monitor_rect[4] {0, 0, 0, 0};  ///< that monitor's rect (virtual-desktop pixels)
    int dpi = 96;
  };

  enum class op_e {
    activate,
    move,
    resize,
    minimize,
    restore,
    maximize,
    close,
  };

  bool supported();
  bool exists(std::uint64_t handle);
  /** Outer (DWM frame) rect in virtual-desktop pixels: x, y, w, h. */
  bool frame_rect(std::uint64_t handle, int out[4]);
  /** Raw window rect (GetWindowRect, includes invisible resize borders): what WGC captures. */
  bool window_rect(std::uint64_t handle, int out[4]);
  std::vector<window_info_t> list();
  /** PNG bytes of the window's icon (large), empty when unavailable. */
  std::string icon_png(std::uint64_t handle);
  /** x/y/w/h are used by move and resize (outer size). */
  bool action(std::uint64_t handle, op_e op, int x = 0, int y = 0, int w = 0, int h = 0);
  bool parse_op(const std::string &name, op_e &out);

  /** A region of the window's non-client hit-test map (rect relative to the frame rect). */
  struct hit_region_t {
    std::string kind;  ///< "drag", "min", "max", "close"
    int rect[4] {0, 0, 0, 0};
  };
  /** Probe WM_NCHITTEST over the top of the window to find the caption/drag
   *  areas and the caption buttons, including apps that draw their own title bar. */
  std::vector<hit_region_t> hit_regions(std::uint64_t handle);

}  // namespace host_windows
