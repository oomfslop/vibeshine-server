/**
 * @file src/host_windows.h
 * @brief Enumerate, inspect and act on the host's top-level windows for
 *        per-window ("app window") streaming. Windows only; other platforms
 *        report supported() == false and empty results.
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory>
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
  /** Stable cache key of the window's icon ("" when none): equal keys mean equal pictures. */
  std::string icon_key(std::uint64_t handle);
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
  /** Live WM_NCHITTEST for one point (relative to the frame rect): "drag", "min",
   *  "max", "close", "border", "client" or "none". */
  std::string hit_test_point(std::uint64_t handle, int x, int y);

  /** Everything a client needs to show one streamed window as a native
   *  window: its rect and where that rect sits on the encoder canvas, plus the
   *  popups (menus, drop-downs, tooltips, owned dialogs) composited with it. */
  struct layout_t {
    bool valid = false;
    int rect[4] {0, 0, 0, 0};  ///< outer rect (restored placement when minimized)
    int anchor[2] {0, 0};  ///< canvas position of rect's top-left (always 0,0 now)
    struct child_t {
      std::uint64_t handle = 0;
      int rect[4] {0, 0, 0, 0};  ///< outer rect in virtual-desktop pixels
      int canvas[4] {0, 0, 0, 0};  ///< where its picture sits on the canvas (the helper's slot)
      bool dialog = false;  ///< owned window with a caption: movable window of its own; otherwise a menu/tooltip pinned to the parent
      std::string title;
      int caption = 0;
    };
    std::vector<child_t> popups;  ///< every popup of the window with a slot on the canvas, bottom-most first
    int radius = 0;  ///< corner radius in pixels the OS applies to the window and its popups (0 when square)
    int dpi = 96;  ///< the window's current DPI (changes when it lands on a differently scaled display)
    int caption = 0;
    std::string state;
    std::string title;
    bool focused = false;
    std::uint64_t etag = 0;  ///< changes whenever any field above changes
  };
  /** Cheap enough to call every few milliseconds. `canvas_w/h` is the encoder
   *  frame the client launched the window with. */
  layout_t window_layout(std::uint64_t handle, int canvas_w, int canvas_h);

  /** WGC delivers one frame per composition of the window's monitor, so a
   *  window on a 60 Hz display can never stream faster. This moves the window
   *  onto a display that refreshes at least `fps` times per second, creating
   *  a dedicated Sunshine virtual display (at the window's own DPI) when the
   *  host has none, and returns a token that moves the window back and drops
   *  the display again once the last session using it lets go. Returns an
   *  empty token when the window's display is already fast enough. */
  std::shared_ptr<void> lease_fast_display(std::uint64_t handle, int fps);

  /** Popup slots the capture helper reported for this window (canvas placement). */
  struct popup_slot_t {
    std::uint64_t handle = 0;
    int host[4] {0, 0, 0, 0};
    int canvas[4] {0, 0, 0, 0};
    bool dialog = false;
  };
  void set_popup_slots(std::uint64_t target, std::vector<popup_slot_t> slots);
  std::vector<popup_slot_t> popup_slots(std::uint64_t target);
  /** Maps a canvas point of the session whose window is pinned at (origin_x, origin_y)
   *  to a host point: inside a popup's slot it lands in that popup. */
  bool remap_canvas_point(int origin_x, int origin_y, int cx, int cy, int &hx, int &hy);
  /** Before a button press: if another window of the same family covers the
   *  window the last mapped point aimed at, bring the aimed-at one on top. */
  void raise_aimed_window();

  /** Keep the window where it is for the session (and its owned dialogs on
   *  the canvas_w x canvas_h canvas around it): input is mapped from the
   *  pinned origin and anything that moves the window on the host (a drag
   *  the app handles itself from forwarded input, edge snapping) is undone
   *  within milliseconds, so there is no feedback between cursor and window.
   *  Our own moves (slide-to-fit on resize) update the pin. */
  std::shared_ptr<void> pin_position(std::uint64_t handle, int canvas_w = 0, int canvas_h = 0);
  /** Pinned outer-rect origin, when pinned. */
  bool pinned_position(std::uint64_t handle, int &x, int &y);

}  // namespace host_windows
