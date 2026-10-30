/**
 * @file src/app_desk.h
 * @brief The "desk": one private virtual display per client that mirrors the
 *        client's screen. Host windows the client streams are moved onto it at
 *        the position and z-order they have on the client, so the display's
 *        picture is exactly the client's layout, composed by DWM: windows,
 *        layered skins, menus, tooltips and dialogs alike. One capture, one
 *        stream; the client cuts one native window per host window out of it.
 *
 * Windows only; elsewhere every call fails.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <array>
#include <string>
#include <vector>

namespace app_desk {

  struct desk_t {
    bool valid = false;
    std::string display;  ///< GDI device name ("\\\\.\\DISPLAYn") of the desk, where the windows are
    std::string capture;  ///< the atlas display the session captures (thumbnails of the desk's windows)
    int capture_size[2] {0, 0};  ///< its size: the stream's resolution
    int rect[4] {0, 0, 0, 0};  ///< the desk on the host's virtual screen: x, y, w, h
    int fps = 0;
    int dpi = 96;
  };

  struct options_t {
    int unfocused_divisor = 1;  ///< windows other than the focused one (and its popups, dialogs) at 1/n of the frame rate: 1, 2, 4, 8
    bool skip_covered = true;  ///< windows covered whole by others are not in the stream (the client keeps their last picture)
    int atlas_w = 0, atlas_h = 0;  ///< the atlas (what is streamed) at this size; 0: 3840x2160. Smaller saves bandwidth on slow links (fewer pixels to fill when windows change)
    int background_qp = 0;  ///< background windows at lower quality: this much added to their quantizer (0-20)
    bool efficient = false;  ///< encoder settings that spend fewer bits for the same picture (slower preset, a pre-pass, a quality ceiling) at a few ms more encode time
    bool quiet_idle = false;  ///< while nothing changes, a frame a second (not the minimum frame rate's repeats), and no blinking text cursor on the host
  };

  /** Make sure the client's desk exists with this size (host px), refresh rate
   *  and DPI; creates or retunes the display. Takes a few seconds the first time. */
  desk_t open(const std::string &client_uuid, int w, int h, int fps, int dpi, const options_t &options = {});
  std::optional<desk_t> get(const std::string &client_uuid);
  /** The options the client's desk was opened with */
  std::optional<options_t> options(const std::string &client_uuid);
  /** Where absolute pointer input for this client's desk session goes: the
   *  desk's origin relative to the virtual desktop's (the session captures
   *  the atlas, of the same size). */
  std::optional<std::pair<int, int>> input_origin(const std::string &client_uuid);
  /** Added to where absolute pointer input goes while a secure-desktop prompt
   *  (UAC) is up: it is shown where the client draws it, but is elsewhere. */
  std::pair<int, int> pointer_shift();
  /** Put every adopted window back and let the display go idle (it is removed
   *  after ten minutes without a desk). */
  void close(const std::string &client_uuid);
  /** True while any client's desk display exists (used to keep it out of the
   *  generic end-of-stream display cleanup). */
  bool any_open();

  /** A desk stream of this client started / ended. When the last one ends
   *  and none starts again within a short grace period (reconnects), the desk
   *  is closed: windows never stay stranded on a display nobody sees. */
  void session_started(const std::string &client_uuid);
  void session_ended(const std::string &client_uuid);

  /** Move a host window onto the desk at desk-relative (x, y); its original
   *  place is remembered and restored by release/close. */
  bool adopt(const std::string &client_uuid, std::uint64_t hwnd, int x, int y, int w = 0, int h = 0);
  /** Give a window on the desk this frame size, in step with its painting
   *  (the next resize waits until it has drawn this one; only the newest
   *  waiting one is applied). False: not on the client's desk. */
  bool resize(const std::string &client_uuid, std::uint64_t hwnd, int w, int h);
  bool release(const std::string &client_uuid, std::uint64_t hwnd);
  /** Out of the way while the client drags it: just past the desk's right
   *  edge, beyond what any client window shows. Adopt puts it back. */
  bool park(const std::string &client_uuid, std::uint64_t hwnd);
  /** Stack the desk's windows in this order, top-most first (the client's
   *  window order is the truth); with focus, the first one is activated. */
  bool order(const std::string &client_uuid, const std::vector<std::uint64_t> &top_first, bool focus);

  struct window_t {
    std::uint64_t handle = 0;
    std::uint64_t owner = 0;  ///< owning top-level window (dialogs, popups), 0 for app windows
    std::uint32_t pid = 0;
    std::string process;
    std::string title;
    std::string kind;  ///< "window" (captioned, unowned), "dialog" (captioned, owned), "popup" (caption-less)
    int rect[4] {0, 0, 0, 0};  ///< desk-relative outer rect (may hang off the desk)
    std::string state;  ///< "normal", "minimized", "maximized"
    bool focused = false;
    int caption = 0;  ///< caption height in px (0 = unknown / custom)
    int radius = 0;  ///< corner radius the OS applies (Windows 11: 8 DIP)
    int z = 0;  ///< stacking order, 0 = bottom-most
    std::string icon_key;
    int slot[4] {0, 0, 0, 0};  ///< where its picture is in the captured atlas: x, y, w, h (0 size = none)
    int alpha_slot[2] {-1, -1};  ///< the same picture on white (x, y), for exact alpha; -1 = opaque
    double alpha_scale = 1.0;  ///< that copy's size relative to the picture
    std::vector<int> alpha_strips;
    bool settled = true;  ///< done with an opening animation (popups)
    /** Its picture shows it fully painted at its current size (after a
     *  resize the app needs a moment; until then the client keeps showing the
     *  last picture that was) */
    bool stable = true;  ///< or as edges: width, then the atlas origin of top, bottom, left, right
    double scale = 1.0;  ///< the picture's scale in the atlas (below 1 only when it is full)
    std::vector<std::array<int, 6>> pieces;  ///< with a scale below 1: the parts the user sees 1:1 (x, y, w, h in the frame; x, y in the atlas)
  };
  struct layout_t {
    bool valid = false;
    std::vector<window_t> windows;  ///< bottom-most first
    std::uintptr_t pointer_lock = 0;  ///< the window that has confined the pointer to itself (games), or 0
    std::string cursor = "arrow";  ///< the host pointer's shape: arrow, ibeam, hand, we, ns, nwse, nesw, all, cross, no, wait, progress, help, uparrow
    std::uint64_t etag = 0;
    int atlas_us = 0;  ///< what syncing the atlas took (diagnostics)
    bool moved = false;  ///< a picture changed place in the atlas
    unsigned seq = 0;  ///< the layout number frames showing this layout carry
    unsigned hard_repacks = 0;  ///< diagnostics
    int ghosts = 0;
  };
  /** Everything visible on the desk right now. Caption-less popups that lie
   *  entirely inside their owner are omitted: they are already part of the
   *  owner's picture. */
  layout_t layout(const std::string &client_uuid);

}  // namespace app_desk
