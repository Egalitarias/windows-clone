#ifndef KERNEL_WM_H
#define KERNEL_WM_H

#include "console.h"

// Window manager: a fixed pool of overlapping top-level windows with z-order,
// focus, move / resize / minimize / maximize / close, and a dirty-rectangle
// model. The shell (desktop.c) owns the wallpaper, taskbar and menus and calls
// wm_paint_windows() between them.
//
// Callbacks are plain function pointers and MUST be assigned at run time (in
// code), never from static initialisers: the kernel is loaded without
// relocations, so addresses stored in static data are not valid.

#define WM_MAX_WINDOWS 16
#define WM_TITLE_H     24
#define WM_SHADOW      6
#define WM_TITLE_CHARS 32

#define WM_MOUSE_DOWN 0
#define WM_MOUSE_MOVE 1   // only while the left button is held
#define WM_MOUSE_UP   2

typedef struct wm_window wm_window_t;

struct wm_window {
  int used;
  int minimized;
  int maximized;
  int x, y, w, h;            // outer rectangle (frame included)
  int restore_x, restore_y, restore_w, restore_h;
  int min_w, min_h;
  char title[WM_TITLE_CHARS];
  void *data;                // owned by the application

  // Paint the client area. (cx, cy, cw, ch) is the client rectangle in screen
  // coordinates; the clip is already limited to it.
  void (*paint)(wm_window_t *win, int cx, int cy, int cw, int ch);
  // Printable key, '\b' or '\n', delivered to the focused window.
  void (*key)(wm_window_t *win, char ch);
  // Mouse in client coordinates. Once pressed inside the client area, move and
  // up events keep coming to the same window until the button is released.
  void (*mouse)(wm_window_t *win, int lx, int ly, int event);
  // Called once just before the window is destroyed.
  void (*closing)(wm_window_t *win);
  // Called about once a second while the window is not minimized (clocks,
  // uptime displays). Call wm_invalidate_window() if something changed.
  void (*tick)(wm_window_t *win);
};

// desk_w x desk_h is the full screen; the bottom `taskbar_h` rows belong to
// the shell and windows are kept above them.
void wm_init(int desk_w, int desk_h, int taskbar_h);

// Create a window (raised and focused). Returns 0 if the pool is full.
wm_window_t *wm_create(const char *title, int x, int y, int w, int h);
void wm_close(wm_window_t *win);
void wm_raise(wm_window_t *win);
void wm_minimize(wm_window_t *win);
void wm_restore(wm_window_t *win);
void wm_toggle_maximize(wm_window_t *win);

wm_window_t *wm_focused(void);
// Pool slot i (for taskbar buttons); returns 0 for an unused slot.
wm_window_t *wm_slot(int i);

// Client rectangle of a window in screen coordinates.
void wm_client_rect(const wm_window_t *win, int *cx, int *cy, int *cw, int *ch);

// Dirty region (a single bounding rectangle). The shell repaints it with the
// fb clip set to it, then clears it.
void wm_invalidate(int x, int y, int w, int h);
void wm_invalidate_window(const wm_window_t *win);
void wm_invalidate_all(void);
int wm_take_dirty(int *x, int *y, int *w, int *h);

// Paint all visible windows back to front. Call with the clip already set.
void wm_paint_windows(void);

// Drive the per-window tick callbacks; the shell calls this once a second.
void wm_tick(void);

// Input routing. wm_mouse_down returns 1 if the click landed on a window.
// `double_click` is set by the shell when this press completes a double click;
// on a title bar it toggles maximize.
int wm_mouse_down(int x, int y, int double_click);
void wm_mouse_move(int x, int y, unsigned int buttons);
void wm_mouse_up(int x, int y);
void wm_key(char ch);

#endif // KERNEL_WM_H
