#include "wm.h"
#include "timer.h"
#include "fb.h"
#include "paging.h"
#include "pmm.h"

#define BTN_W   20
#define BTN_H   18
#define BTN_GAP 2
#define GRIP    14

#define COL_FRAME        FB_RGB(20, 40, 100)
#define COL_FRAME_IDLE   FB_RGB(90, 95, 110)
#define COL_TITLE        FB_RGB(40, 90, 190)
#define COL_TITLE_IDLE   FB_RGB(120, 125, 140)
#define COL_TITLE_TEXT   FB_RGB(255, 255, 255)
#define COL_CLIENT       FB_RGB(244, 245, 248)
#define COL_BTN          FB_RGB(70, 120, 210)
#define COL_BTN_IDLE     FB_RGB(150, 155, 170)
#define COL_CLOSE        FB_RGB(200, 60, 60)
#define COL_SHADOW       FB_RGB(10, 20, 50)
#define DRAG_OPACITY     215

typedef enum {
  CAP_NONE = 0,
  CAP_MOVE,
  CAP_RESIZE,
  CAP_CLIENT
} capture_t;

static wm_window_t g_win[WM_MAX_WINDOWS];
static int g_order[WM_MAX_WINDOWS];   // z-order, g_order[0] is the bottom
static int g_count = 0;

static int g_desk_w = 0, g_desk_h = 0, g_taskbar_h = 0;

// Dirty rectangle, [x0,x1) x [y0,y1)
static int g_dirty = 0;
static int g_dx0, g_dy0, g_dx1, g_dy1;

static capture_t g_cap = CAP_NONE;
static wm_window_t *g_cap_win = 0;
static int g_cap_ox, g_cap_oy;          // move: pointer offset inside the window
static int g_cap_x, g_cap_y;            // resize: pointer position at the start
static int g_cap_w, g_cap_h;            // resize: window size at the start

// ---- helpers ---------------------------------------------------------------

static int work_h(void) { return g_desk_h - g_taskbar_h; }

static int idx_of(const wm_window_t *win) { return (int)(win - g_win); }

static int order_pos(const wm_window_t *win) {
  int i, idx = idx_of(win);
  for (i = 0; i < g_count; i++) {
    if (g_order[i] == idx) {
      return i;
    }
  }
  return -1;
}

static void copy_title(char *dst, const char *src) {
  int i;
  for (i = 0; i < WM_TITLE_CHARS - 1 && src && src[i]; i++) {
    dst[i] = src[i];
  }
  dst[i] = 0;
}

static int in_rect(int px, int py, int x, int y, int w, int h) {
  return px >= x && py >= y && px < x + w && py < y + h;
}

void wm_invalidate(int x, int y, int w, int h) {
  int x1 = x + w, y1 = y + h;

  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x1 > g_desk_w) x1 = g_desk_w;
  if (y1 > g_desk_h) y1 = g_desk_h;
  if (x1 <= x || y1 <= y) {
    return;
  }
  if (!g_dirty) {
    g_dx0 = x; g_dy0 = y; g_dx1 = x1; g_dy1 = y1;
    g_dirty = 1;
    return;
  }
  if (x < g_dx0) g_dx0 = x;
  if (y < g_dy0) g_dy0 = y;
  if (x1 > g_dx1) g_dx1 = x1;
  if (y1 > g_dy1) g_dy1 = y1;
}

void wm_damage_window(const wm_window_t *win) {
  wm_invalidate(win->x, win->y, win->w + WM_SHADOW, win->h + WM_SHADOW);
}

void wm_invalidate_window(const wm_window_t *win) {
  ((wm_window_t *)win)->content_dirty = 1;
  wm_damage_window(win);
}

void wm_set_opacity(wm_window_t *win, int opacity) {
  if (!win) {
    return;
  }
  opacity = opacity < 0 ? 0 : (opacity > 255 ? 255 : opacity);
  if (opacity != win->opacity) {
    win->opacity = opacity;
    wm_damage_window(win);
  }
}

void wm_invalidate_all(void) {
  wm_invalidate(0, 0, g_desk_w, g_desk_h);
}

static void invalidate_taskbar(void) {
  wm_invalidate(0, work_h(), g_desk_w, g_taskbar_h);
}

int wm_take_dirty(int *x, int *y, int *w, int *h) {
  if (!g_dirty) {
    return 0;
  }
  *x = g_dx0;
  *y = g_dy0;
  *w = g_dx1 - g_dx0;
  *h = g_dy1 - g_dy0;
  g_dirty = 0;
  return 1;
}

// ---- surfaces --------------------------------------------------------------

static void free_surface(wm_window_t *win) {
  uint32_t i;

  if (win->surf) {
    uint64_t phys = (uint64_t)(uintptr_t)win->surf;
    for (i = 0; i < win->surf_pages; i++) {
      pmm_free_page(phys + (uint64_t)i * PAGE_SIZE);
    }
  }
  win->surf = 0;
  win->surf_pages = 0;
  win->surf_w = 0;
  win->surf_h = 0;
}

// Make sure the window has a surface of its current size. Returns 0 if there is no memory
// (the window is then drawn straight onto the screen). A new or resized surface is stale.
static int ensure_surface(wm_window_t *win) {
  uint64_t bytes = fb_surface_pixels_bytes(win->w, win->h);
  uint32_t pages = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
  uint64_t phys;

  if (pages == 0) {
    return 0;
  }
  if (win->surf && win->surf_pages >= pages && win->surf_pages <= 4 * pages + 4) {
    if (win->surf_w != win->w || win->surf_h != win->h) {
      win->surf_w = win->w;
      win->surf_h = win->h;
      win->content_dirty = 1;
      win->frame_dirty = 1;
    }
    return 1;
  }
  free_surface(win);
  phys = pmm_alloc_pages(pages);
  if (phys == 0) {
    return 0;
  }
  if (!paging_map_range(phys, (uint64_t)pages * PAGE_SIZE)) {
    uint32_t i;
    for (i = 0; i < pages; i++) {
      pmm_free_page(phys + (uint64_t)i * PAGE_SIZE);
    }
    return 0;
  }
  win->surf = (uint32_t *)(uintptr_t)phys;
  win->surf_pages = pages;
  win->surf_w = win->w;
  win->surf_h = win->h;
  win->content_dirty = 1;
  win->frame_dirty = 1;
  return 1;
}

// ---- lifetime and z-order --------------------------------------------------

void wm_init(int desk_w, int desk_h, int taskbar_h) {
  int i;
  g_desk_w = desk_w;
  g_desk_h = desk_h;
  g_taskbar_h = taskbar_h;
  g_count = 0;
  g_cap = CAP_NONE;
  g_cap_win = 0;
  for (i = 0; i < WM_MAX_WINDOWS; i++) {
    g_win[i].used = 0;
  }
  wm_invalidate_all();
}

wm_window_t *wm_slot(int i) {
  if (i < 0 || i >= WM_MAX_WINDOWS || !g_win[i].used) {
    return 0;
  }
  return &g_win[i];
}

wm_window_t *wm_create(const char *title, int x, int y, int w, int h) {
  int i;
  wm_window_t *win = 0;

  for (i = 0; i < WM_MAX_WINDOWS; i++) {
    if (!g_win[i].used) {
      win = &g_win[i];
      break;
    }
  }
  if (!win) {
    return 0;
  }
  win->used = 1;
  win->minimized = 0;
  win->maximized = 0;
  win->x = x;
  win->y = y;
  win->w = w;
  win->h = h;
  win->min_w = 160;
  win->min_h = WM_TITLE_H + 60;
  win->data = 0;
  win->surf = 0;
  win->surf_w = 0;
  win->surf_h = 0;
  win->surf_pages = 0;
  win->content_dirty = 1;
  win->frame_dirty = 1;
  win->frame_focused = -1;
  win->opacity = 255;
  win->paint = 0;
  win->key = 0;
  win->mouse = 0;
  win->closing = 0;
  win->resized = 0;
  win->tick = 0;
  copy_title(win->title, title);
  g_order[g_count++] = idx_of(win);
  wm_invalidate_window(win);
  invalidate_taskbar();
  return win;
}

void wm_close(wm_window_t *win) {
  int pos, i;

  if (!win || !win->used) {
    return;
  }
  if (win->closing) {
    win->closing(win);
  }
  if (g_cap_win == win) {
    g_cap = CAP_NONE;
    g_cap_win = 0;
  }
  wm_damage_window(win);
  free_surface(win);
  pos = order_pos(win);
  if (pos >= 0) {
    for (i = pos; i < g_count - 1; i++) {
      g_order[i] = g_order[i + 1];
    }
    g_count--;
  }
  win->used = 0;
  invalidate_taskbar();
}

void wm_raise(wm_window_t *win) {
  int pos, i, idx;

  if (!win) {
    return;
  }
  pos = order_pos(win);
  if (pos < 0) {
    return;
  }
  if (pos != g_count - 1) {
    idx = g_order[pos];
    for (i = pos; i < g_count - 1; i++) {
      g_order[i] = g_order[i + 1];
    }
    g_order[g_count - 1] = idx;
    wm_damage_window(win);
  }
  invalidate_taskbar();    // focus highlight moves
}

void wm_minimize(wm_window_t *win) {
  if (!win || win->minimized) {
    return;
  }
  win->minimized = 1;
  wm_damage_window(win);
  invalidate_taskbar();
  // The focus ring changes for whichever window is now on top.
  if (wm_focused()) {
    wm_damage_window(wm_focused());
  }
}

void wm_restore(wm_window_t *win) {
  if (!win) {
    return;
  }
  win->minimized = 0;
  wm_raise(win);
  wm_damage_window(win);
  invalidate_taskbar();
}

void wm_toggle_maximize(wm_window_t *win) {
  if (!win) {
    return;
  }
  wm_invalidate_window(win);
  if (win->maximized) {
    win->x = win->restore_x;
    win->y = win->restore_y;
    win->w = win->restore_w;
    win->h = win->restore_h;
    win->maximized = 0;
  } else {
    win->restore_x = win->x;
    win->restore_y = win->y;
    win->restore_w = win->w;
    win->restore_h = win->h;
    win->x = 0;
    win->y = 0;
    win->w = g_desk_w;
    win->h = work_h();
    win->maximized = 1;
  }
  wm_invalidate_window(win);
  if (win->resized) {
    win->resized(win);
  }
}

void wm_set_title(wm_window_t *win, const char *title) {
  if (!win) {
    return;
  }
  copy_title(win->title, title);
  win->frame_dirty = 1;
  wm_invalidate(win->x, win->y, win->w, WM_TITLE_H);
  invalidate_taskbar();
}

void wm_move(wm_window_t *win, int x, int y) {
  int max_x = g_desk_w - 40;
  int max_y = work_h() - WM_TITLE_H;

  if (!win) {
    return;
  }
  if (x < 40 - win->w) {
    x = 40 - win->w;
  }
  if (x > max_x) {
    x = max_x;
  }
  if (y < 0) {
    y = 0;
  }
  if (y > max_y) {
    y = max_y;
  }
  wm_damage_window(win);
  win->maximized = 0;
  win->x = x;
  win->y = y;
  wm_damage_window(win);
}

void wm_set_client_size(wm_window_t *win, int cw, int ch) {
  if (!win) {
    return;
  }
  wm_invalidate_window(win);
  win->maximized = 0;
  win->w = cw + 2;
  win->h = ch + WM_TITLE_H + 1;
  wm_invalidate_window(win);
}

wm_window_t *wm_focused(void) {
  int i;
  for (i = g_count - 1; i >= 0; i--) {
    wm_window_t *win = &g_win[g_order[i]];
    if (!win->minimized) {
      return win;
    }
  }
  return 0;
}

void wm_client_rect(const wm_window_t *win, int *cx, int *cy, int *cw, int *ch) {
  *cx = win->x + 1;
  *cy = win->y + WM_TITLE_H;
  *cw = win->w - 2;
  *ch = win->h - WM_TITLE_H - 1;
}

// ---- painting --------------------------------------------------------------

static int btn_x_close(const wm_window_t *win) { return win->x + win->w - BTN_W - 4; }
static int btn_x_max(const wm_window_t *win)   { return btn_x_close(win) - BTN_W - BTN_GAP; }
static int btn_x_min(const wm_window_t *win)   { return btn_x_max(win) - BTN_W - BTN_GAP; }
static int btn_y(const wm_window_t *win)       { return win->y + 3; }

// The frame: title bar with buttons, and a one-pixel border. The client area is not
// touched, so the frame can be redrawn without repainting the content. Works on whatever
// target is current (the window's surface, or the screen).
static void draw_frame(const wm_window_t *win, int focused) {
  int x = win->x, y = win->y, w = win->w, h = win->h;
  int by = btn_y(win);
  int max_chars;
  char shown[WM_TITLE_CHARS];
  int i;
  uint32_t frame = focused ? COL_FRAME : COL_FRAME_IDLE;

  fb_fill_rect(x, y, w, WM_TITLE_H, frame);
  fb_fill_rect(x, y + WM_TITLE_H, 1, h - WM_TITLE_H, frame);
  fb_fill_rect(x + w - 1, y + WM_TITLE_H, 1, h - WM_TITLE_H, frame);
  fb_fill_rect(x, y + h - 1, w, 1, frame);
  fb_fill_rect(x + 1, y + 1, w - 2, WM_TITLE_H - 1, focused ? COL_TITLE : COL_TITLE_IDLE);

  max_chars = (btn_x_min(win) - (x + 8) - 4) / FB_FONT_WIDTH;
  for (i = 0; i < max_chars && i < WM_TITLE_CHARS - 1 && win->title[i]; i++) {
    shown[i] = win->title[i];
  }
  shown[i] = 0;
  fb_draw_text(x + 8, y + 9, shown, COL_TITLE_TEXT, 1);

  // Buttons: minimize, maximize, close
  fb_fill_rect(btn_x_min(win), by, BTN_W, BTN_H, focused ? COL_BTN : COL_BTN_IDLE);
  fb_fill_rect(btn_x_min(win) + 5, by + 12, 10, 2, COL_TITLE_TEXT);
  fb_fill_rect(btn_x_max(win), by, BTN_W, BTN_H, focused ? COL_BTN : COL_BTN_IDLE);
  fb_draw_rect(btn_x_max(win) + 5, by + 4, 10, 9, COL_TITLE_TEXT);
  fb_fill_rect(btn_x_max(win) + 5, by + 4, 10, 2, COL_TITLE_TEXT);
  fb_fill_rect(btn_x_close(win), by, BTN_W, BTN_H, focused ? COL_CLOSE : COL_BTN_IDLE);
  fb_draw_text(btn_x_close(win) + 6, by + 5, "x", COL_TITLE_TEXT, 1);
}

// The client area: background, then the application's paint callback with the clip
// narrowed to the client rectangle.
static void draw_client(wm_window_t *win) {
  int cx, cy, cw, ch;

  wm_client_rect(win, &cx, &cy, &cw, &ch);
  fb_fill_rect(cx, cy, cw, ch, COL_CLIENT);
  if (win->paint && cw > 0 && ch > 0) {
    int ox0, oy0, ow, oh, nx0, ny0, nx1, ny1;
    fb_get_clip(&ox0, &oy0, &ow, &oh);
    nx0 = cx > ox0 ? cx : ox0;
    ny0 = cy > oy0 ? cy : oy0;
    nx1 = (cx + cw) < (ox0 + ow) ? (cx + cw) : (ox0 + ow);
    ny1 = (cy + ch) < (oy0 + oh) ? (cy + ch) : (oy0 + oh);
    if (nx1 > nx0 && ny1 > ny0) {
      fb_set_clip(nx0, ny0, nx1 - nx0, ny1 - ny0);
      win->paint(win, cx, cy, cw, ch);
      fb_set_clip(ox0, oy0, ow, oh);
    }
  }
}

// Triangle of six dots in the bottom-right corner (drawn over the client area).
static void draw_grip(const wm_window_t *win) {
  int sx, sy;

  for (sy = 0; sy < 3; sy++) {
    for (sx = 0; sx + sy < 3; sx++) {
      fb_fill_rect(win->x + win->w - 6 - sx * 4, win->y + win->h - 6 - sy * 4, 2, 2, FB_RGB(120, 125, 140));
    }
  }
}

// Soft drop shadow along the right and bottom edges, blended over what is below.
static void draw_shadow(const wm_window_t *win) {
  int x = win->x, y = win->y, w = win->w, h = win->h, i;

  for (i = 0; i < WM_SHADOW; i++) {
    int a = 90 - i * 90 / WM_SHADOW;                       // darkest at the window edge

    if (h - WM_SHADOW - i > 0) {                           // right of the window
      fb_fill_rect_alpha(x + w + i, y + WM_SHADOW + i, 1, h - WM_SHADOW - i, COL_SHADOW, a);
    }
    if (w - 2 * i > 0) {                                   // below it, rounding the corner
      fb_fill_rect_alpha(x + WM_SHADOW + i, y + h + i, w - 2 * i, 1, COL_SHADOW, a);
    }
  }
}

static void paint_window(wm_window_t *win, int focused) {
  if (ensure_surface(win)) {
    int stale_frame = win->frame_dirty || win->frame_focused != focused;

    if (stale_frame || win->content_dirty) {
      fb_set_target(win->surf, win->w, win->h, win->x, win->y);
      if (stale_frame) {
        draw_frame(win, focused);
      }
      if (win->content_dirty) {
        draw_client(win);
      }
      if (!win->maximized) {
        draw_grip(win);
      }
      fb_reset_target();
      win->frame_dirty = 0;
      win->frame_focused = focused;
      win->content_dirty = 0;
    }
    if (!win->maximized) {
      draw_shadow(win);
    }
    fb_composite(win->x, win->y, win->w, win->h, win->surf, win->w, win->opacity);
    return;
  }
  // No memory for a surface: draw straight onto the screen.
  if (!win->maximized) {
    draw_shadow(win);
  }
  draw_frame(win, focused);
  draw_client(win);
  if (!win->maximized) {
    draw_grip(win);
  }
}

void wm_paint_windows(void) {
  int i, cx, cy, cw, ch;
  wm_window_t *top = wm_focused();

  fb_get_clip(&cx, &cy, &cw, &ch);
  for (i = 0; i < g_count; i++) {
    wm_window_t *win = &g_win[g_order[i]];

    // Windows that do not touch the area being repainted are left alone (their surfaces
    // stay as they are until they are next shown).
    if (!win->minimized && win->x < cx + cw && win->x + win->w + WM_SHADOW > cx &&
        win->y < cy + ch && win->y + win->h + WM_SHADOW > cy) {
      paint_window(win, win == top);
    }
  }
}

// ---- input -----------------------------------------------------------------

static wm_window_t *window_at(int x, int y) {
  int i;
  for (i = g_count - 1; i >= 0; i--) {
    wm_window_t *win = &g_win[g_order[i]];
    if (!win->minimized && in_rect(x, y, win->x, win->y, win->w, win->h)) {
      return win;
    }
  }
  return 0;
}

void wm_tick(void) {
  int i;
  for (i = 0; i < WM_MAX_WINDOWS; i++) {
    if (g_win[i].used && !g_win[i].minimized && g_win[i].tick) {
      g_win[i].tick(&g_win[i]);
    }
  }
}

int wm_mouse_down(int x, int y, int double_click) {
  wm_window_t *win = window_at(x, y);
  wm_window_t *prev_top;
  int by;

  if (!win) {
    return 0;
  }
  prev_top = wm_focused();
  if (prev_top && prev_top != win) {
    wm_damage_window(prev_top);        // old focus loses its highlight
  }
  wm_raise(win);
  wm_damage_window(win);

  by = btn_y(win);
  if (in_rect(x, y, btn_x_close(win), by, BTN_W, BTN_H)) {
    wm_close(win);
    return 1;
  }
  if (in_rect(x, y, btn_x_max(win), by, BTN_W, BTN_H)) {
    wm_toggle_maximize(win);
    return 1;
  }
  if (in_rect(x, y, btn_x_min(win), by, BTN_W, BTN_H)) {
    wm_minimize(win);
    return 1;
  }
  if (!win->maximized && x >= win->x + win->w - GRIP && y >= win->y + win->h - GRIP) {
    g_cap = CAP_RESIZE;
    g_cap_win = win;
    g_cap_x = x;
    g_cap_y = y;
    g_cap_w = win->w;
    g_cap_h = win->h;
    return 1;
  }
  if (y < win->y + WM_TITLE_H) {
    if (double_click) {
      wm_toggle_maximize(win);
      return 1;
    }
    if (!win->maximized) {
      g_cap = CAP_MOVE;
      g_cap_win = win;
      g_cap_ox = x - win->x;
      g_cap_oy = y - win->y;
      wm_set_opacity(win, DRAG_OPACITY);     // see-through while it is carried
    }
    return 1;
  }
  {
    int cx, cy, cw, ch;
    wm_client_rect(win, &cx, &cy, &cw, &ch);
    if (in_rect(x, y, cx, cy, cw, ch) && win->mouse) {
      g_cap = CAP_CLIENT;
      g_cap_win = win;
      win->mouse(win, x - cx, y - cy, WM_MOUSE_DOWN);
      if (double_click && g_cap == CAP_CLIENT && g_cap_win == win) {
        win->mouse(win, x - cx, y - cy, WM_MOUSE_DOUBLE);
      }
    }
  }
  return 1;
}

#define LIVE_RESIZE_MS 60U
static uint64_t g_live_resize_ms = 0;

void wm_mouse_move(int x, int y, unsigned int buttons) {
  wm_window_t *win = g_cap_win;

  if (g_cap == CAP_NONE || !win) {
    return;
  }
  if (!(buttons & 1u)) {
    return;
  }
  if (g_cap == CAP_MOVE) {
    int nx = x - g_cap_ox, ny = y - g_cap_oy;
    if (ny < 0) ny = 0;
    if (ny > work_h() - WM_TITLE_H) ny = work_h() - WM_TITLE_H;
    if (nx < 80 - win->w) nx = 80 - win->w;
    if (nx > g_desk_w - 80) nx = g_desk_w - 80;
    if (nx != win->x || ny != win->y) {
      wm_damage_window(win);
      win->x = nx;
      win->y = ny;
      wm_damage_window(win);
    }
  } else if (g_cap == CAP_RESIZE) {
    int nw = g_cap_w + (x - g_cap_x), nh = g_cap_h + (y - g_cap_y);
    if (nw < win->min_w) nw = win->min_w;
    if (nh < win->min_h) nh = win->min_h;
    if (win->x + nw > g_desk_w) nw = g_desk_w - win->x;
    if (win->y + nh > work_h()) nh = work_h() - win->y;
    if (nw != win->w || nh != win->h) {
      wm_invalidate_window(win);
      win->w = nw;
      win->h = nh;
      wm_invalidate_window(win);
      // Let the program follow the drag, but not on every mouse packet: giving it a new
      // surface costs a memory allocation and a full redraw on its side.
      if (win->resized && timer_get_ms() - g_live_resize_ms >= LIVE_RESIZE_MS) {
        g_live_resize_ms = timer_get_ms();
        win->resized(win);
      }
    }
  } else if (g_cap == CAP_CLIENT && win->mouse) {
    int cx, cy, cw, ch;
    wm_client_rect(win, &cx, &cy, &cw, &ch);
    win->mouse(win, x - cx, y - cy, WM_MOUSE_MOVE);
  }
}

void wm_mouse_up(int x, int y) {
  if (g_cap == CAP_MOVE && g_cap_win) {
    wm_set_opacity(g_cap_win, 255);
  }
  if (g_cap == CAP_CLIENT && g_cap_win && g_cap_win->mouse) {
    int cx, cy, cw, ch;
    wm_client_rect(g_cap_win, &cx, &cy, &cw, &ch);
    g_cap_win->mouse(g_cap_win, x - cx, y - cy, WM_MOUSE_UP);
  }
  if (g_cap == CAP_RESIZE && g_cap_win && g_cap_win->resized) {
    wm_window_t *win = g_cap_win;

    g_cap = CAP_NONE;
    g_cap_win = 0;
    win->resized(win);
    return;
  }
  g_cap = CAP_NONE;
  g_cap_win = 0;
}

static unsigned int g_key_mods = 0;

void wm_set_key_mods(unsigned int mods) { g_key_mods = mods; }
unsigned int wm_key_mods(void) { return g_key_mods; }

void wm_key(char ch) {
  wm_window_t *win = wm_focused();
  if (win && win->key) {
    win->key(win, ch);
  }
}
