#include "apps.h"
#include "console.h"
#include "fb.h"
#include "shell.h"
#include "string.h"
#include "timer.h"
#include "wm.h"

// Console: an on-screen view of the kernel log (everything printed with
// console_printf, which also goes to the serial port) with a command prompt at the
// bottom (see shell.c for the commands). It follows the newest output unless scrolled
// back, and wraps long lines to the window width. The log itself lives in console.c;
// this file only draws it and collects the typed line.

#define TOOL_H       26
#define BTN_W        52
#define BTN_H        18
#define BTN_Y        4
#define ROW_H        10
#define PAD          6
#define MAX_LINES    4096          // most lines kept in view for scrolling
#define REFRESH_MS   100
#define INPUT_MAX    120

#define COL_BG       FB_RGB(16, 18, 26)
#define COL_TEXT     FB_RGB(205, 222, 205)
#define COL_WARN     FB_RGB(255, 190, 90)
#define COL_BAR      FB_RGB(226, 228, 238)

static wm_window_t *g_win = 0;
static int g_scroll = 0;                    // lines above the newest (0 = follow the end)
static uint64_t g_seen_total = 0;
static uint64_t g_last_refresh_ms = 0;
static int g_page_rows = 10;                // rows in view at the last paint
static char g_input[INPUT_MAX + 1];         // the line being typed
static int g_input_len = 0;

static char g_snap[CONSOLE_LOG_SIZE];       // copy of the log being drawn
static int g_line_start[MAX_LINES];         // ring of the last MAX_LINES lines
static int g_line_len[MAX_LINES];

// Split the snapshot into lines of at most `cols` characters. Returns how many lines
// are kept (at most MAX_LINES); line i of the kept lines is slot (first + i) % MAX_LINES.
static int split_lines(int len, int cols, int *first) {
  int pos = 0, count = 0, start = 0;

  *first = 0;
  while (pos <= len) {
    int end_of_line = (pos == len) || g_snap[pos] == '\n' || (pos - start) >= cols;
    if (end_of_line) {
      int slot = count % MAX_LINES;
      int line_len = pos - start;

      if (pos == len && line_len == 0) {
        break;                              // nothing after the last newline
      }
      g_line_start[slot] = start;
      g_line_len[slot] = line_len;
      count++;
      if (pos < len && g_snap[pos] == '\n') {
        pos++;
      }
      start = pos;
      if (pos == len) {
        break;
      }
      continue;
    }
    pos++;
  }
  if (count > MAX_LINES) {
    *first = count % MAX_LINES;
    count = MAX_LINES;
  }
  return count;
}

static void draw_button(int x, int y, int w, const char *label) {
  fb_fill_rect(x, y, w, BTN_H, FB_RGB(236, 238, 246));
  fb_draw_rect(x, y, w, BTN_H, FB_RGB(120, 126, 150));
  fb_draw_text(x + (w - fb_text_width(label, 1)) / 2, y + 5, label, FB_RGB(30, 34, 50), 1);
}

static void console_paint(wm_window_t *win, int cx, int cy, int cw, int ch) {
  uint64_t from = 0, got;
  int len, cols, rows, count, first, last, i, row;
  char line[160];

  (void)win;
  // Toolbar
  fb_fill_rect(cx, cy, cw, TOOL_H, COL_BAR);
  draw_button(cx + 6, cy + BTN_Y, BTN_W, "Up");
  draw_button(cx + 6 + (BTN_W + 4), cy + BTN_Y, BTN_W, "Down");
  draw_button(cx + 6 + 2 * (BTN_W + 4), cy + BTN_Y, BTN_W, "End");
  draw_button(cx + 6 + 3 * (BTN_W + 4), cy + BTN_Y, BTN_W + 8, "Clear");
  if (g_scroll > 0) {
    fb_draw_text(cx + 6 + 4 * (BTN_W + 4) + 16, cy + BTN_Y + 5, "scrolled back", FB_RGB(150, 80, 20), 1);
  }

  // Text area
  fb_fill_rect(cx, cy + TOOL_H, cw, ch - TOOL_H, COL_BG);
  cols = (cw - 2 * PAD) / 8;
  rows = (ch - TOOL_H - PAD - ROW_H - 4) / ROW_H;     // the last row is the prompt
  if (cols < 8 || rows < 1) {
    return;
  }
  if (cols > (int)sizeof(line) - 1) {
    cols = (int)sizeof(line) - 1;
  }
  g_page_rows = rows;

  got = console_log_read(0, g_snap, sizeof(g_snap), &from);
  len = (int)got;
  g_seen_total = console_log_total();
  count = split_lines(len, cols, &first);

  if (g_scroll > count - rows) {
    g_scroll = count - rows;
  }
  if (g_scroll < 0) {
    g_scroll = 0;
  }
  last = count - g_scroll;                  // one past the last line shown
  row = 0;
  for (i = last - rows; i < last; i++, row++) {
    int slot, n, k;

    if (i < 0) {
      continue;
    }
    slot = (first + i) % MAX_LINES;
    n = g_line_len[slot];
    for (k = 0; k < n; k++) {
      char c = g_snap[g_line_start[slot] + k];
      line[k] = (c >= 32 && c < 127) ? c : ' ';
    }
    line[n] = 0;
    // Failures and kills stand out.
    fb_draw_text(cx + PAD, cy + TOOL_H + PAD / 2 + row * ROW_H, line,
                 (n > 0 && (line[0] == '!' ||
                            (n > 6 && line[1] == 'P' && line[2] == 'R' && line[3] == 'O' && line[4] == 'C')))
                     ? COL_WARN : COL_TEXT, 1);
  }

  // Prompt: "folder> typed text_", scrolled left when it does not fit.
  {
    char prompt[sizeof(line) + INPUT_MAX + 8];
    int n = (int)str_copy(prompt, shell_cwd(), sizeof(prompt));
    int skip = 0;

    n += (int)str_copy(prompt + n, "> ", sizeof(prompt) - (size_t)n);
    n += (int)str_copy(prompt + n, g_input, sizeof(prompt) - (size_t)n);
    prompt[n++] = '_';
    prompt[n] = 0;
    if (n > cols) {
      skip = n - cols;
    }
    fb_fill_rect(cx, cy + ch - ROW_H - 4, cw, 1, FB_RGB(60, 66, 90));
    fb_draw_text(cx + PAD, cy + ch - ROW_H - 1, prompt + skip, FB_RGB(150, 235, 170), 1);
  }
}

static void console_mouse(wm_window_t *win, int lx, int ly, int event) {
  int step;

  if (event != WM_MOUSE_DOWN || ly < BTN_Y || ly >= BTN_Y + BTN_H) {
    return;
  }
  step = g_page_rows > 2 ? g_page_rows - 1 : 1;
  if (lx >= 6 && lx < 6 + BTN_W) {
    g_scroll += step;                       // paint clamps it
  } else if (lx >= 6 + (BTN_W + 4) && lx < 6 + (BTN_W + 4) + BTN_W) {
    g_scroll -= step;
  } else if (lx >= 6 + 2 * (BTN_W + 4) && lx < 6 + 2 * (BTN_W + 4) + BTN_W) {
    g_scroll = 0;
  } else if (lx >= 6 + 3 * (BTN_W + 4) && lx < 6 + 3 * (BTN_W + 4) + BTN_W + 8) {
    console_log_clear();
    g_scroll = 0;
  } else {
    return;
  }
  wm_invalidate_window(win);
}

// Wheel: three lines per notch; up goes back in the log (paint clamps g_scroll).
static void console_wheel(wm_window_t *win, int lx, int ly, int delta) {
  (void)lx;
  (void)ly;
  g_scroll += delta * 3;
  wm_invalidate_window(win);
}

// Typing: printable characters build the line, Backspace edits it, Enter runs it.
static void console_key(wm_window_t *win, char ch) {
  if (ch == '\n' || ch == '\r') {
    char cmd[INPUT_MAX + 1];

    str_copy(cmd, g_input, sizeof(cmd));
    g_input_len = 0;
    g_input[0] = 0;
    g_scroll = 0;
    console_printf("%s> %s\r\n", shell_cwd(), cmd);
    shell_exec(cmd);
  } else if (ch == '\b' || ch == 127) {
    if (g_input_len > 0) {
      g_input[--g_input_len] = 0;
    }
  } else if (ch >= 32 && ch < 127) {
    if (g_input_len < INPUT_MAX) {
      g_input[g_input_len++] = ch;
      g_input[g_input_len] = 0;
    }
    g_scroll = 0;
  } else {
    return;
  }
  wm_invalidate_window(win);
}

static void console_closing(wm_window_t *win) {
  (void)win;
  g_win = 0;
}

void app_console_open(void) {
  wm_window_t *win;

  if (g_win) {
    wm_restore(g_win);
    wm_raise(g_win);
    return;
  }
  win = wm_create("Console", 70, 60, 700, 420);
  if (!win) {
    return;
  }
  g_win = win;
  g_scroll = 0;
  g_seen_total = 0;
  win->paint = console_paint;
  win->mouse = console_mouse;
  win->wheel = console_wheel;
  win->key = console_key;
  win->closing = console_closing;
  win->min_w = 320;
  win->min_h = 160;
}

// Called by the desktop loop: redraw when new output arrived (at most every
// REFRESH_MS, and only while following the end, since scrolled-back text stays put).
void app_console_poll(void) {
  uint64_t now;

  if (!g_win || g_win->minimized || g_scroll != 0 || console_log_total() == g_seen_total) {
    return;
  }
  now = timer_get_ms();
  if (now - g_last_refresh_ms < REFRESH_MS) {
    return;
  }
  g_last_refresh_ms = now;
  wm_invalidate_window(g_win);
}
