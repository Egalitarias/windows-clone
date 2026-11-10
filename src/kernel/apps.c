#include "apps.h"
#include "fb.h"
#include "fs.h"
#include "string.h"
#include "timer.h"
#include "wm.h"

#define COL_TEXT FB_RGB(30, 30, 30)

static int g_cascade = 0;

static void next_position(int *x, int *y) {
  *x = 120 + g_cascade * 32;
  *y = 50 + g_cascade * 28;
  g_cascade = (g_cascade + 1) % 8;
}

// ---- Message ---------------------------------------------------------------
// Shown when a program that should open (Notes, Files) cannot be started. One window,
// reused if it is already open.

#define MSG_LINES 3
#define MSG_LEN   100

static char g_msg[MSG_LINES][MSG_LEN];
static wm_window_t *g_msg_win = 0;

static void msg_paint(wm_window_t *win, int cx, int cy, int cw, int ch) {
  int i;

  (void)win;
  (void)cw;
  (void)ch;
  for (i = 0; i < MSG_LINES; i++) {
    fb_draw_text(cx + 16, cy + 16 + i * 16, g_msg[i], i == 0 ? FB_RGB(170, 40, 40) : COL_TEXT, 1);
  }
}

static void msg_closing(wm_window_t *win) {
  (void)win;
  g_msg_win = 0;
}

void app_message_open(const char *title, const char *line1, const char *line2, const char *line3) {
  int x, y;

  str_copy(g_msg[0], line1 ? line1 : "", MSG_LEN);
  str_copy(g_msg[1], line2 ? line2 : "", MSG_LEN);
  str_copy(g_msg[2], line3 ? line3 : "", MSG_LEN);
  if (g_msg_win) {
    wm_set_title(g_msg_win, title);
    wm_restore(g_msg_win);
    return;
  }
  next_position(&x, &y);
  g_msg_win = wm_create(title, x, y, 520, 110);
  if (!g_msg_win) {
    return;
  }
  g_msg_win->paint = msg_paint;
  g_msg_win->closing = msg_closing;
  g_msg_win->min_w = 200;
  g_msg_win->min_h = 80;
}

// ---- Paint -----------------------------------------------------------------

#define PAINT_W       400
#define PAINT_H       260
#define PAINT_CX      4          // canvas origin inside the client area
#define PAINT_CY      30
#define PAINT_COLORS  8
#define SWATCH_W      22
#define SWATCH_PITCH  26

static uint8_t g_canvas[PAINT_W * PAINT_H];
static int g_paint_color = 1;
static int g_paint_open = 0;
static int g_last_x = -1, g_last_y = -1;

static uint32_t paint_color(int i) {
  switch (i) {
    case 0:  return FB_RGB(255, 255, 255);   // eraser
    case 1:  return FB_RGB(0, 0, 0);
    case 2:  return FB_RGB(200, 40, 40);
    case 3:  return FB_RGB(230, 140, 20);
    case 4:  return FB_RGB(240, 210, 40);
    case 5:  return FB_RGB(40, 160, 70);
    case 6:  return FB_RGB(40, 100, 220);
    default: return FB_RGB(140, 60, 190);
  }
}

static void paint_paint(wm_window_t *win, int cx, int cy, int cw, int ch) {
  int i, row, col, run_start;

  (void)win;
  (void)cw;
  (void)ch;
  // Palette strip
  for (i = 0; i < PAINT_COLORS; i++) {
    int sx = cx + 4 + i * SWATCH_PITCH;
    fb_fill_rect(sx, cy + 4, SWATCH_W, 20, paint_color(i));
    fb_draw_rect(sx, cy + 4, SWATCH_W, 20,
                 i == g_paint_color ? FB_RGB(40, 90, 190) : FB_RGB(120, 120, 130));
    if (i == g_paint_color) {
      fb_draw_rect(sx + 1, cy + 5, SWATCH_W - 2, 18, FB_RGB(40, 90, 190));
    }
  }
  fb_fill_rect(cx + 4 + PAINT_COLORS * SWATCH_PITCH + 8, cy + 4, 50, 20, FB_RGB(210, 212, 220));
  fb_draw_rect(cx + 4 + PAINT_COLORS * SWATCH_PITCH + 8, cy + 4, 50, 20, FB_RGB(120, 120, 130));
  fb_draw_text(cx + 4 + PAINT_COLORS * SWATCH_PITCH + 17, cy + 10, "Clear", COL_TEXT, 1);

  // Canvas, drawn as horizontal runs of equal colour.
  fb_draw_rect(cx + PAINT_CX - 1, cy + PAINT_CY - 1, PAINT_W + 2, PAINT_H + 2, FB_RGB(120, 120, 130));
  for (row = 0; row < PAINT_H; row++) {
    run_start = 0;
    for (col = 1; col <= PAINT_W; col++) {
      if (col == PAINT_W || g_canvas[row * PAINT_W + col] != g_canvas[row * PAINT_W + run_start]) {
        fb_fill_rect(cx + PAINT_CX + run_start, cy + PAINT_CY + row, col - run_start, 1,
                     paint_color(g_canvas[row * PAINT_W + run_start]));
        run_start = col;
      }
    }
  }
}

static void stamp(wm_window_t *win, int px, int py) {
  int dx, dy, cx, cy, cw, ch;

  for (dy = -1; dy <= 1; dy++) {
    for (dx = -1; dx <= 1; dx++) {
      int x = px + dx, y = py + dy;
      if (x >= 0 && y >= 0 && x < PAINT_W && y < PAINT_H) {
        g_canvas[y * PAINT_W + x] = (uint8_t)g_paint_color;
      }
    }
  }
  wm_client_rect(win, &cx, &cy, &cw, &ch);
  wm_invalidate(cx + PAINT_CX + px - 3, cy + PAINT_CY + py - 3, 7, 7);
}

static void paint_line(wm_window_t *win, int x0, int y0, int x1, int y1) {
  int dx = x1 - x0, dy = y1 - y0;
  int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
  int i;

  if (steps == 0) {
    stamp(win, x0, y0);
    return;
  }
  for (i = 0; i <= steps; i++) {
    stamp(win, x0 + dx * i / steps, y0 + dy * i / steps);
  }
}

static void paint_mouse(wm_window_t *win, int lx, int ly, int event) {
  int i, px, py;

  if (event == WM_MOUSE_UP) {
    g_last_x = -1;
    return;
  }
  if (event == WM_MOUSE_DOWN && ly >= 4 && ly < 24) {
    for (i = 0; i < PAINT_COLORS; i++) {
      int sx = 4 + i * SWATCH_PITCH;
      if (lx >= sx && lx < sx + SWATCH_W) {
        g_paint_color = i;
        wm_invalidate_window(win);
        return;
      }
    }
    if (lx >= 4 + PAINT_COLORS * SWATCH_PITCH + 8 && lx < 4 + PAINT_COLORS * SWATCH_PITCH + 58) {
      for (i = 0; i < PAINT_W * PAINT_H; i++) {
        g_canvas[i] = 0;
      }
      wm_invalidate_window(win);
    }
    return;
  }
  px = lx - PAINT_CX;
  py = ly - PAINT_CY;
  if (event == WM_MOUSE_DOWN) {
    if (px < 0 || py < 0 || px >= PAINT_W || py >= PAINT_H) {
      return;
    }
    g_last_x = px;
    g_last_y = py;
    stamp(win, px, py);
  } else if (g_last_x >= 0) {
    paint_line(win, g_last_x, g_last_y, px, py);
    g_last_x = px;
    g_last_y = py;
  }
}

static void paint_closing(wm_window_t *win) {
  (void)win;
  g_paint_open = 0;
}

void app_paint_open(void) {
  int x, y, i;
  wm_window_t *win;

  if (g_paint_open) {
    for (i = 0; i < WM_MAX_WINDOWS; i++) {
      wm_window_t *w = wm_slot(i);
      if (w && w->mouse == paint_mouse) {
        wm_restore(w);
        return;
      }
    }
    return;
  }
  next_position(&x, &y);
  win = wm_create("Paint", x, y, PAINT_W + 2 * PAINT_CX + 2, PAINT_H + PAINT_CY + 4 + WM_TITLE_H + 1);
  if (!win) {
    return;
  }
  g_paint_open = 1;
  win->paint = paint_paint;
  win->mouse = paint_mouse;
  win->closing = paint_closing;
  win->min_w = 240;
  win->min_h = 160;
}

// ---- About -----------------------------------------------------------------

static void put_int(char *dst, unsigned int v) {
  char tmp[12];
  int n = 0, i = 0;
  if (v == 0) {
    tmp[n++] = '0';
  }
  while (v > 0) {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  }
  while (n > 0) {
    dst[i++] = tmp[--n];
  }
  dst[i] = 0;
}

static void about_paint(wm_window_t *win, int cx, int cy, int cw, int ch) {
  char res[40];
  int n = 0, i;
  char num[12];

  (void)win;
  (void)cw;
  (void)ch;
  fb_draw_text(cx + 16, cy + 16, "WinClone", FB_RGB(40, 90, 190), 3);
  fb_draw_text(cx + 16, cy + 56, "A from-scratch x86-64 UEFI operating system.", COL_TEXT, 1);

  put_int(num, fb_width());
  for (i = 0; num[i]; i++) res[n++] = num[i];
  res[n++] = 'x';
  put_int(num, fb_height());
  for (i = 0; num[i]; i++) res[n++] = num[i];
  res[n] = 0;
  fb_draw_text(cx + 16, cy + 80, "Display:", COL_TEXT, 1);
  fb_draw_text(cx + 16 + 9 * FB_FONT_WIDTH, cy + 80, res, COL_TEXT, 1);
  fb_draw_text(cx + 16, cy + 96, "Input:   PS/2 keyboard and mouse (IRQ1/IRQ12)", COL_TEXT, 1);
  fb_draw_text(cx + 16, cy + 112, "Timer:   PIT at 1000 Hz, interrupt-driven", COL_TEXT, 1);
  {
    uint64_t secs = timer_get_ms() / 1000;
    char up[32];
    int n = 0, i2;
    char part[12];

    put_int(part, (unsigned int)(secs / 3600));
    for (i2 = 0; part[i2]; i2++) up[n++] = part[i2];
    up[n++] = 'h';
    up[n++] = ' ';
    put_int(part, (unsigned int)((secs / 60) % 60));
    for (i2 = 0; part[i2]; i2++) up[n++] = part[i2];
    up[n++] = 'm';
    up[n++] = ' ';
    put_int(part, (unsigned int)(secs % 60));
    for (i2 = 0; part[i2]; i2++) up[n++] = part[i2];
    up[n++] = 's';
    up[n] = 0;
    fb_draw_text(cx + 16, cy + 128, "Uptime:", COL_TEXT, 1);
    fb_draw_text(cx + 16 + 9 * FB_FONT_WIDTH, cy + 128, up, COL_TEXT, 1);
  }
}

static void about_tick(wm_window_t *win) {
  wm_invalidate_window(win);
}

void app_about_open(void) {
  int x, y;
  wm_window_t *win;

  next_position(&x, &y);
  win = wm_create("About WinClone", x, y, 470, 190);
  if (!win) {
    return;
  }
  win->paint = about_paint;
  win->tick = about_tick;
  win->min_w = 200;
  win->min_h = 100;
}
