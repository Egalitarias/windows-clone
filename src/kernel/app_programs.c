#include "apps.h"
#include "fb.h"
#include "process.h"
#include "string.h"
#include "wm.h"

// Programs: launches user-mode programs from /APPS and shows what they print.
// One program runs at a time (until per-process address spaces exist).

#define PROG_COUNT   4
#define BTN_W        96
#define BTN_H        22
#define BTN_Y        6
#define STATUS_H     16
#define OUT_MAX      2048
#define ROW_H        10

// The kernel is loaded without relocations, so a table of string pointers in
// static data is not valid (and the compiler happily builds one out of a switch
// or if-chain). A 2D char array holds the characters themselves.
static const char kProgNames[PROG_COUNT][6] = {"HELLO", "COUNT", "CRASH", "ECHO"};

static const char *prog_name(int i) {
  return kProgNames[i];
}

static wm_window_t *g_win = 0;
static int g_handle = -1;           // process_spawn handle, -1 when idle
static uint64_t g_cursor = 0;       // position in the program output stream
static char g_out[OUT_MAX + 1];
static int g_out_len = 0;
static char g_status[64];
static int g_btn_hot = -1;
static char g_line[128];            // line being typed for the program
static int g_line_len = 0;

static void set_status(const char *a, const char *b) {
  size_t n = str_copy(g_status, a, sizeof(g_status));
  if (b) {
    str_copy(g_status + n, b, sizeof(g_status) - n);
  }
}

static void out_append(const char *s, int n) {
  int i;
  for (i = 0; i < n; ++i) {
    char c = s[i];
    if (c == '\r') {
      continue;
    }
    if (c != '\n' && (c < 0x20 || c > 0x7E)) {
      c = '?';
    }
    if (g_out_len >= OUT_MAX) {              // drop the oldest half
      int keep = OUT_MAX / 2, k;
      for (k = 0; k < keep; ++k) {
        g_out[k] = g_out[g_out_len - keep + k];
      }
      g_out_len = keep;
    }
    g_out[g_out_len++] = c;
  }
  g_out[g_out_len] = 0;
}

static void redraw(void) {
  if (g_win) {
    wm_invalidate_window(g_win);
  }
}

static char g_view[OUT_MAX + 132];

static void program_paint(wm_window_t *win, int cx, int cy, int cw, int ch) {
  char *view = g_view;
  int vlen = 0;
  int i, cols, rows, total_rows = 0, skip, row = 0, col = 0, area_y, area_h;
  (void)win;

  for (i = 0; i < g_out_len; ++i) {
    view[vlen++] = g_out[i];
  }
  if (g_handle >= 0) {                       // the line being typed, with a cursor
    for (i = 0; i < g_line_len; ++i) {
      view[vlen++] = g_line[i];
    }
    view[vlen++] = '_';
  }
  view[vlen] = 0;

  fb_fill_rect(cx, cy, cw, ch, FB_RGB(240, 241, 246));
  for (i = 0; i < PROG_COUNT; ++i) {
    int bx = cx + 6 + i * (BTN_W + 6);
    char label[16];
    size_t n = str_copy(label, prog_name(i), sizeof(label));
    str_copy(label + n, ".BIN", sizeof(label) - n);
    fb_fill_rect(bx, cy + BTN_Y, BTN_W, BTN_H, i == g_btn_hot ? FB_RGB(70, 140, 245) : FB_RGB(40, 110, 220));
    fb_draw_text(bx + (BTN_W - fb_text_width(label, 1)) / 2, cy + BTN_Y + 7, label, FB_RGB(255, 255, 255), 1);
  }

  area_y = cy + BTN_Y + BTN_H + 8;
  area_h = ch - (area_y - cy) - STATUS_H;
  fb_fill_rect(cx + 4, area_y, cw - 8, area_h, FB_RGB(16, 18, 26));
  cols = (cw - 16) / FB_FONT_WIDTH;
  rows = (area_h - 8) / ROW_H;
  if (cols >= 1 && rows >= 1) {
    // Wrap to the window width, then show the last `rows` rows.
    for (i = 0; i < vlen; ++i) {
      if (view[i] == '\n' || col >= cols) {
        total_rows++;
        col = 0;
        if (view[i] == '\n') {
          continue;
        }
      }
      col++;
    }
    total_rows++;
    skip = total_rows > rows ? total_rows - rows : 0;
    col = 0;
    for (i = 0; i < vlen; ++i) {
      if (view[i] == '\n' || col >= cols) {
        row++;
        col = 0;
        if (view[i] == '\n') {
          continue;
        }
      }
      if (row >= skip && row - skip < rows) {
        fb_draw_char(cx + 8 + col * FB_FONT_WIDTH, area_y + 4 + (row - skip) * ROW_H, view[i],
                     FB_RGB(120, 230, 140), 1);
      }
      col++;
    }
  }

  fb_fill_rect(cx, cy + ch - STATUS_H, cw, STATUS_H, FB_RGB(228, 230, 238));
  fb_draw_text(cx + 6, cy + ch - STATUS_H + 4, g_status[0] ? g_status : "Click a program to run it",
               FB_RGB(70, 75, 95), 1);
}

// Start a program. Returns 1 if it was started.
static int start_program(const char *path) {
  int h;

  if (g_handle >= 0) {
    set_status("Another program is still running", 0);
    redraw();
    return 0;
  }
  h = process_spawn(path);
  if (h < 0) {
    set_status("Could not start ", path);
    redraw();
    return 0;
  }
  g_handle = h;
  g_out_len = 0;
  g_out[0] = 0;
  g_line_len = 0;
  process_input_reset();
  {
    char tmp[256];                             // skip output from before this launch
    while (process_output_read(&g_cursor, tmp, sizeof(tmp)) > 0) {
    }
  }
  set_status("Running ", path);
  redraw();
  return 1;
}

static void program_mouse(wm_window_t *win, int lx, int ly, int event) {
  int i, hot = -1;
  (void)win;

  for (i = 0; i < PROG_COUNT; ++i) {
    int bx = 6 + i * (BTN_W + 6);
    if (lx >= bx && lx < bx + BTN_W && ly >= BTN_Y && ly < BTN_Y + BTN_H) {
      hot = i;
    }
  }
  if (event == WM_MOUSE_DOWN && hot >= 0) {
    char path[32];
    size_t n = str_copy(path, "/APPS/", sizeof(path));
    n += str_copy(path + n, prog_name(hot), sizeof(path) - n);
    str_copy(path + n, ".BIN", sizeof(path) - n);
    start_program(path);
  }
  if (hot != g_btn_hot) {
    g_btn_hot = hot;
    redraw();
  }
}

// Keyboard: cooked line mode. Characters are echoed and edited here; a whole line
// is handed to the program when Enter is pressed. Ctrl+D ends the input.
static void program_key(wm_window_t *win, char ch) {
  (void)win;
  if (g_handle < 0) {
    return;
  }
  if (ch == 0x04) {
    process_input_close();
    set_status("End of input sent", 0);
  } else if (ch == '\n') {
    int i;
    for (i = 0; i < g_line_len; ++i) {
      process_input_push(g_line[i]);
    }
    process_input_push('\n');
    out_append(g_line, g_line_len);
    out_append("\n", 1);
    g_line_len = 0;
  } else if (ch == '\b') {
    if (g_line_len > 0) {
      g_line_len--;
    }
  } else if (ch >= 0x20 && ch <= 0x7E && g_line_len < (int)sizeof(g_line) - 2) {
    g_line[g_line_len++] = ch;
  } else {
    return;
  }
  redraw();
}

static void program_closing(wm_window_t *win) {
  (void)win;
  g_win = 0;
  process_input_close();                    // a program waiting for input sees end of input
}

void app_programs_open(void) {
  if (g_win) {
    wm_raise(g_win);
    wm_invalidate_window(g_win);
    return;
  }
  g_win = wm_create("Programs", 150, 70, 520, 300);
  if (!g_win) {
    return;
  }
  g_win->paint = program_paint;
  g_win->mouse = program_mouse;
  g_win->closing = program_closing;
  g_win->key = program_key;
  g_win->min_w = 3 * (BTN_W + 6) + 12;
  g_win->min_h = 140;
}

void app_programs_run(const char *path) {
  app_programs_open();
  if (g_win) {
    start_program(path);
  }
}

// Called by the desktop loop: move new program output into the window and notice
// when the program has finished.
void app_programs_poll(void) {
  char tmp[128];
  uint64_t n, code = 0;
  int changed = 0;
  int rc;

  if (g_handle < 0) {
    return;
  }
  while ((n = process_output_read(&g_cursor, tmp, sizeof(tmp))) > 0) {
    out_append(tmp, (int)n);
    changed = 1;
  }
  rc = process_poll(g_handle, &code);
  if (rc != 1) {
    char num[24];
    g_handle = -1;
    while ((n = process_output_read(&g_cursor, tmp, sizeof(tmp))) > 0) {
      out_append(tmp, (int)n);
    }
    if (rc != PROCESS_OK) {
      set_status("Program not found", 0);
    } else if (code >= PROCESS_EXIT_FAULT_BASE) {
      u32_to_str((uint32_t)(code - PROCESS_EXIT_FAULT_BASE), num);
      set_status("Crashed: CPU exception ", num);
    } else {
      u32_to_str((uint32_t)code, num);
      set_status("Finished, exit code ", num);
    }
    changed = 1;
  }
  if (changed) {
    redraw();
  }
}
