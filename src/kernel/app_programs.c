#include "apps.h"
#include "fb.h"
#include "process.h"
#include "string.h"
#include "syswin.h"
#include "wm.h"

// Programs: launches user-mode programs from /APPS and shows what they print.
// Every Programs window runs its own program with its own output and input.

#define PROG_COUNT   7
#define BTN_W        84
#define BTN_H        22
#define BTN_Y        6
#define STATUS_H     16
#define OUT_MAX      2048
#define ROW_H        10
#define MAX_WINS     4

// The kernel is loaded without relocations, so a table of string pointers in
// static data is not valid (and the compiler happily builds one out of a switch
// or if-chain). A 2D char array holds the characters themselves.
static const char kProgNames[PROG_COUNT][9] = {"HELLO", "COUNT", "CRASH", "ECHO", "TICK", "NOTES", "EXPLORER"};

static const char *prog_name(int i) {
  return kProgNames[i];
}

typedef struct {
  int used;
  wm_window_t *win;
  int handle;                 // process_spawn handle, -1 when idle
  uint64_t cursor;            // position in the program output stream
  char out[OUT_MAX + 1];
  int out_len;
  char status[64];
  int btn_hot;
  char line[128];             // line being typed for the program
  int line_len;
} prog_t;

static prog_t g_progs[MAX_WINS];
static char g_view[OUT_MAX + 132];

static void set_status(prog_t *pr, const char *a, const char *b) {
  size_t n = str_copy(pr->status, a, sizeof(pr->status));
  if (b) {
    str_copy(pr->status + n, b, sizeof(pr->status) - n);
  }
}

static void out_append(prog_t *pr, const char *s, int n) {
  int i;
  for (i = 0; i < n; ++i) {
    char c = s[i];
    if (c == '\r') {
      continue;
    }
    if (c != '\n' && (c < 0x20 || c > 0x7E)) {
      c = '?';
    }
    if (pr->out_len >= OUT_MAX) {              // drop the oldest half
      int keep = OUT_MAX / 2, k;
      for (k = 0; k < keep; ++k) {
        pr->out[k] = pr->out[pr->out_len - keep + k];
      }
      pr->out_len = keep;
    }
    pr->out[pr->out_len++] = c;
  }
  pr->out[pr->out_len] = 0;
}

static void redraw(prog_t *pr) {
  if (pr->win) {
    wm_invalidate_window(pr->win);
  }
}

static void program_paint(wm_window_t *win, int cx, int cy, int cw, int ch) {
  char *view = g_view;
  int vlen = 0;
  int i, cols, rows, total_rows = 0, skip, row = 0, col = 0, area_y, area_h;
  prog_t *pr = (prog_t *)win->data;

  for (i = 0; i < pr->out_len; ++i) {
    view[vlen++] = pr->out[i];
  }
  if (pr->handle >= 0) {                       // the line being typed, with a cursor
    for (i = 0; i < pr->line_len; ++i) {
      view[vlen++] = pr->line[i];
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
    fb_fill_rect(bx, cy + BTN_Y, BTN_W, BTN_H, i == pr->btn_hot ? FB_RGB(70, 140, 245) : FB_RGB(40, 110, 220));
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
  fb_draw_text(cx + 6, cy + ch - STATUS_H + 4, pr->status[0] ? pr->status : "Click a program to run it",
               FB_RGB(70, 75, 95), 1);
}

// Start a program in this window. Returns 1 if it was started.
static int start_program(prog_t *pr, const char *path, const char *arg) {
  int h;

  if (pr->handle >= 0) {
    set_status(pr, "Another program is still running", 0);
    redraw(pr);
    return 0;
  }
  h = process_spawn_arg(path, arg);
  if (h < 0) {
    set_status(pr, "Could not start ", path);
    redraw(pr);
    return 0;
  }
  pr->handle = h;
  pr->cursor = 0;
  pr->out_len = 0;
  pr->out[0] = 0;
  pr->line_len = 0;
  set_status(pr, "Running ", path);
  redraw(pr);
  return 1;
}

static void program_mouse(wm_window_t *win, int lx, int ly, int event) {
  prog_t *pr = (prog_t *)win->data;
  int i, hot = -1;

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
    start_program(pr, path, 0);
  }
  if (hot != pr->btn_hot) {
    pr->btn_hot = hot;
    redraw(pr);
  }
}

// Keyboard: cooked line mode. Characters are echoed and edited here; a whole line
// is handed to the program when Enter is pressed. Ctrl+D ends the input.
static void program_key(wm_window_t *win, char ch) {
  prog_t *pr = (prog_t *)win->data;

  if (pr->handle < 0) {
    return;
  }
  if (ch == 0x04) {
    process_input_close(pr->handle);
    set_status(pr, "End of input sent", 0);
  } else if (ch == '\n') {
    int i;
    for (i = 0; i < pr->line_len; ++i) {
      process_input_push(pr->handle, pr->line[i]);
    }
    process_input_push(pr->handle, '\n');
    out_append(pr, pr->line, pr->line_len);
    out_append(pr, "\n", 1);
    pr->line_len = 0;
  } else if (ch == '\b') {
    if (pr->line_len > 0) {
      pr->line_len--;
    }
  } else if (ch >= 0x20 && ch <= 0x7E && pr->line_len < (int)sizeof(pr->line) - 2) {
    pr->line[pr->line_len++] = ch;
  } else {
    return;
  }
  redraw(pr);
}

// The window is going away. A program still running keeps its slot until app_programs_poll
// sees it finish (that releases the spawn handle); it just sees end of input.
static void program_closing(wm_window_t *win) {
  prog_t *pr = (prog_t *)win->data;

  pr->win = 0;
  if (pr->handle >= 0) {
    process_input_close(pr->handle);
  } else {
    pr->used = 0;
  }
}

static prog_t *new_window(void) {
  int i;

  for (i = 0; i < MAX_WINS; ++i) {
    prog_t *pr = &g_progs[i];
    if (!pr->used) {
      pr->used = 1;
      pr->handle = -1;
      pr->cursor = 0;
      pr->out_len = 0;
      pr->out[0] = 0;
      pr->status[0] = 0;
      pr->btn_hot = -1;
      pr->line_len = 0;
      pr->win = wm_create("Programs", 150 + 24 * i, 70 + 24 * i, 640, 300);
      if (!pr->win) {
        pr->used = 0;
        return 0;
      }
      pr->win->data = pr;
      pr->win->paint = program_paint;
      pr->win->mouse = program_mouse;
      pr->win->closing = program_closing;
      pr->win->key = program_key;
      pr->win->min_w = 3 * (BTN_W + 6) + 12;
      pr->win->min_h = 140;
      return pr;
    }
  }
  return 0;
}

// Start menu: raise an open Programs window, or open the first one.
void app_programs_open(void) {
  int i;

  for (i = 0; i < MAX_WINS; ++i) {
    if (g_progs[i].used && g_progs[i].win) {
      wm_raise(g_progs[i].win);
      wm_invalidate_window(g_progs[i].win);
      return;
    }
  }
  new_window();
}

// Run `path` in an idle Programs window, or in a new one if all are busy.
void app_programs_run(const char *path, const char *arg) {
  prog_t *pr = 0;
  int i;

  for (i = 0; i < MAX_WINS && !pr; ++i) {
    if (g_progs[i].used && g_progs[i].win && g_progs[i].handle < 0) {
      pr = &g_progs[i];
    }
  }
  if (!pr) {
    pr = new_window();
  }
  if (pr) {
    wm_raise(pr->win);
    start_program(pr, path, arg);
  }
}

static void poll_one(prog_t *pr) {
  char tmp[128];
  uint64_t n, code = 0;
  int changed = 0;
  int rc = 1;
  int finished;

  if (pr->handle < 0) {
    return;
  }
  // Look at "finished" before draining: a program stops writing before it is marked
  // done, so a drain after seeing done is complete (process_poll frees the console).
  finished = process_finished(pr->handle);
  while ((n = process_output_read(pr->handle, &pr->cursor, tmp, sizeof(tmp))) > 0) {
    out_append(pr, tmp, (int)n);
    changed = 1;
  }
  if (finished) {
    rc = process_poll(pr->handle, &code);
  }
  if (rc != 1) {
    char num[24];
    pr->handle = -1;
    if (rc != PROCESS_OK) {
      set_status(pr, "Program not found", 0);
    } else if (code >= PROCESS_EXIT_FAULT_BASE) {
      u32_to_str((uint32_t)(code - PROCESS_EXIT_FAULT_BASE), num);
      set_status(pr, "Crashed: CPU exception ", num);
    } else {
      u32_to_str((uint32_t)code, num);
      set_status(pr, "Finished, exit code ", num);
    }
    changed = 1;
    if (!pr->win) {
      pr->used = 0;                          // window was closed while it ran
      return;
    }
  }
  if (changed) {
    redraw(pr);
  }
}

// Called by the desktop loop: move new program output into each window and notice
// when a program has finished.
void app_programs_poll(void) {
  int i;

  for (i = 0; i < MAX_WINS; ++i) {
    if (g_progs[i].used) {
      poll_one(&g_progs[i]);
    }
  }
}

// ---- default Notes and Files: user programs, with the built-in apps as a fallback ----

#define NOTES_PROGRAM    "/APPS/NOTES.BIN"
#define EXPLORER_PROGRAM "/APPS/EXPLORER.BIN"

// Start a windowed user program. 0 when that is not possible right now.
static int launch_user_program(const char *path) {
  if (!process_program_exists(path) || syswin_free_slots() == 0) {
    return 0;
  }
  return process_spawn_detached(path, 0) == PROCESS_OK;
}

// Tell the user why `path` did not start.
static void launch_failed(const char *what, const char *path) {
  char line[100];
  size_t n = str_copy(line, "Cannot start ", sizeof(line));

  str_copy(line + n, path, sizeof(line) - n);
  if (!process_program_exists(path)) {
    app_message_open(what, line, "The program file was not found on any disk.",
                     "Programs live in the \\APPS folder next to the kernel.");
  } else {
    app_message_open(what, line, "No free window or program slot: close something and try again.", 0);
  }
}

// A single-instance program: raise it if it runs, else start it, else say why not.
static void launch_single(const char *path, const char *what) {
  uint64_t pid = 0;

  if (process_spawn_active(path, &pid)) {
    syswin_raise_pid(pid);
    return;
  }
  if (!launch_user_program(path)) {
    launch_failed(what, path);
  }
}

void app_launch_notes(void) {
  if (!launch_user_program(NOTES_PROGRAM)) {
    launch_failed("Notes", NOTES_PROGRAM);
  }
}

void app_launch_files(void) {
  launch_single(EXPLORER_PROGRAM, "Files");
}

void app_launch_paint(void) {
  launch_single("/APPS/PAINT.BIN", "Paint");
}

void app_launch_about(void) {
  launch_single("/APPS/ABOUT.BIN", "About");
}
