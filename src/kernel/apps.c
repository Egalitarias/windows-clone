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
