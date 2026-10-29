/**
 * @file src/platform/windows/desk_atlas.h
 * @brief The picture side of the desk: a window of our own covering a second
 *        private display (the "atlas"), on which DWM draws a live thumbnail of
 *        every window on the desk, each in a slot of its own, 1:1. The atlas
 *        display is what a desk session captures.
 *
 * DWM thumbnails are the compositor's own copy of a window (taskbar previews
 * use them): every kind of window renders, layered skins and XAML menus
 * included, whole even where it hangs off a display, and untouched by
 * whatever overlaps it. Windows that need transparency (menus with their
 * shadow, tooltips) get a second copy on a white backing: with the copy on
 * black, the client recovers exact alpha (a = 1 - (white - black)).
 *
 * The source rectangle of each thumbnail is the slot's capacity, larger than
 * the window: DWM then draws the window 1:1 from its top-left and leaves the
 * rest empty, so a window growing within its slot never shows scaled.
 *
 * When everything does not fit, the windows showing least go in smaller, and
 * the parts of them the user sees go in 1:1 as well ("pieces", thumbnails of
 * just that part): what is on the client's screen never fills more than the
 * screen, which the atlas is larger than, so it stays sharp.
 */
#pragma once

#include <windows.h>
#include <dwmapi.h>

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace desk_atlas {

  struct source_t {
    HWND hwnd = nullptr;
    int w = 0, h = 0;  ///< the window's frame (DWM extended frame bounds) size
    bool alpha = false;  ///< also draw it on white, for exact transparency
    int priority = 0;  ///< higher keeps full size first when the atlas is full
    std::vector<RECT> visible;  ///< the parts the user sees (frame coordinates), sharp even when the rest goes smaller
  };

  struct piece_t {
    int x = 0, y = 0, w = 0, h = 0;  ///< the part, in the frame (1:1)
    int ax = 0, ay = 0;  ///< where it is in the atlas
  };

  struct slot_t {
    HWND hwnd = nullptr;
    int x = 0, y = 0;  ///< where the window's frame starts in the atlas
    int w = 0, h = 0;  ///< its size there (the frame size times scale)
    int ax = -1, ay = -1;  ///< the copy on white, or -1
    double scale = 1.0;  ///< below 1 only when the atlas is too small for everything
    std::vector<piece_t> pieces;  ///< with a scale below 1: the visible parts, 1:1
  };

  class atlas_t {
  public:
    /** A window covering `monitor` (a display nothing else uses), kept on top. */
    explicit atlas_t(const RECT &monitor);
    ~atlas_t();
    atlas_t(const atlas_t &) = delete;
    atlas_t &operator=(const atlas_t &) = delete;

    /** Show exactly these windows; answers where each one is drawn. Slots are
     *  kept while a window fits its slot. Changes take effect with the next
     *  composition. */
    std::vector<slot_t> sync(const std::vector<source_t> &sources);

    const RECT &monitor() const {
      return monitor_;
    }

  private:
    struct cell_t {
      int x = 0, y = 0, cw = 0, ch = 0;  ///< capacity rect (atlas client coordinates)
    };
    struct piece_item_t {
      HTHUMBNAIL thumb = nullptr;
      RECT part {};  ///< in the frame
      cell_t cell;
      RECT shown_part {};  ///< what the thumbnail was last set to
      cell_t shown_cell;
    };
    struct item_t {
      HTHUMBNAIL black = nullptr, white = nullptr;
      cell_t cell, white_cell;
      bool has_white = false;
      int w = 0, h = 0;
      double scale = 1.0;
      RECT source_offset {};  ///< frame origin inside the window rect
      bool used = false;
      std::vector<RECT> want;  ///< the visible parts, for pieces when drawn smaller
      std::vector<piece_item_t> pieces;
    };

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    void run();
    std::vector<slot_t> apply(const std::vector<source_t> &sources);
    bool place(std::vector<cell_t> &taken, int cw, int ch, cell_t &out) const;
    bool place_pieces(std::vector<cell_t> &taken, item_t &item, bool required) const;
    void show_pieces(HWND hwnd, item_t &item);
    void release(item_t &item);
    void paint(HDC dc);
    bool update_thumbnail(HTHUMBNAIL thumb, const cell_t &cell, const item_t &item);

    RECT monitor_ {};
    HWND window_ = nullptr;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool started_ = false;
    std::map<HWND, item_t> items_;
    std::vector<RECT> white_;  ///< backings to paint (client coordinates)
    unsigned ticks_ = 0;
    double last_area_ = 0;  ///< what the windows asked for at the last sync
  };

}  // namespace desk_atlas
