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

// ---- Notes -----------------------------------------------------------------

#define NOTES_INSTANCES 4
#define NOTES_MAX       4096
#define NOTES_STATUS_H  16

typedef struct {
  int in_use;
  int len;
  int has_file;                 // loaded from / saved to a file
  fat_volume_t *vol;
  uint32_t dir_cluster;
  char file[FAT_NAME_MAX];
  char status[48];
  char text[NOTES_MAX + 1];
} NotesData;

static NotesData g_notes[NOTES_INSTANCES];
static uint8_t g_io_buf[NOTES_MAX * 2 + 16];     // file <-> text conversion buffer

static void notes_paint(wm_window_t *win, int cx, int cy, int cw, int ch) {
  NotesData *d = (NotesData *)win->data;
  int text_h = ch - NOTES_STATUS_H;
  int cols = (cw - 16) / FB_FONT_WIDTH;
  int rows = (text_h - 16) / 12;
  int i, row = 0, col = 0;

  fb_fill_rect(cx + 4, cy + 4, cw - 8, text_h - 4, FB_RGB(255, 255, 255));

  // Status line
  fb_fill_rect(cx, cy + ch - NOTES_STATUS_H, cw, NOTES_STATUS_H, FB_RGB(228, 230, 238));
  fb_draw_text(cx + 6, cy + ch - NOTES_STATUS_H + 4,
               d->status[0] ? d->status : "Ctrl+S saves", FB_RGB(70, 75, 95), 1);

  if (cols < 1 || rows < 1) {
    return;
  }
  for (i = 0; i < d->len; i++) {
    if (d->text[i] == '\n') {
      row++;
      col = 0;
      continue;
    }
    if (col >= cols) {
      row++;
      col = 0;
    }
    if (row >= rows) {
      return;
    }
    fb_draw_char(cx + 8 + col * FB_FONT_WIDTH, cy + 8 + row * 12, d->text[i], COL_TEXT, 1);
    col++;
  }
  if (col >= cols) {
    row++;
    col = 0;
  }
  if (row < rows) {
    fb_draw_char(cx + 8 + col * FB_FONT_WIDTH, cy + 8 + row * 12, '_', FB_RGB(40, 90, 190), 1);
  }
}

static void notes_set_title(wm_window_t *win, const char *file) {
  char title[WM_TITLE_CHARS];
  int n = (int)str_copy(title, "Notes - ", sizeof(title));
  str_copy(title + n, file, sizeof(title) - (size_t)n);
  wm_set_title(win, title);
}

static void notes_set_status(NotesData *d, const char *a, const char *b) {
  int n = (int)str_copy(d->status, a, sizeof(d->status));
  if (b) {
    str_copy(d->status + n, b, sizeof(d->status) - (size_t)n);
  }
}

// Save to the associated file, or to a new NOTEn.TXT on the first volume.
static void notes_save(wm_window_t *win) {
  NotesData *d = (NotesData *)win->data;
  fat_volume_t *vol = d->has_file ? d->vol : fs_volume(0);
  uint32_t dir, len = 0;
  int i;

  if (!vol) {
    notes_set_status(d, "No disk available", 0);
    wm_invalidate_window(win);
    return;
  }
  dir = d->has_file ? d->dir_cluster : vol->root_cluster;
  if (!d->has_file) {
    fat_dirent_t probe;
    int n;
    for (n = 1; n < 1000; n++) {
      char name[16];
      int k = (int)str_copy(name, "NOTE", sizeof(name));
      k += u32_to_str((uint32_t)n, name + k);
      str_copy(name + k, ".TXT", sizeof(name) - (size_t)k);
      if (!fat_lookup(vol, dir, name, &probe)) {
        str_copy(d->file, name, sizeof(d->file));
        break;
      }
    }
  }
  for (i = 0; i < d->len; i++) {            // text uses LF; files use CR LF
    if (d->text[i] == '\n') {
      g_io_buf[len++] = '\r';
    }
    g_io_buf[len++] = (uint8_t)d->text[i];
  }
  if (fat_write_file(vol, dir, d->file, g_io_buf, len)) {
    d->has_file = 1;
    d->vol = vol;
    d->dir_cluster = dir;
    notes_set_title(win, d->file);
    notes_set_status(d, "Saved ", d->file);
  } else {
    notes_set_status(d, "Save failed", 0);
  }
  wm_invalidate_window(win);
}

static void notes_key(wm_window_t *win, char ch) {
  NotesData *d = (NotesData *)win->data;

  if (ch == 0x13) {                         // Ctrl+S
    notes_save(win);
    return;
  }
  if (ch == '\b') {
    if (d->len > 0) {
      d->len--;
      d->text[d->len] = 0;
    }
  } else if ((ch == '\n' || (ch >= 0x20 && ch <= 0x7E)) && d->len < NOTES_MAX) {
    d->text[d->len++] = ch;
    d->text[d->len] = 0;
  } else {
    return;
  }
  d->status[0] = 0;
  wm_invalidate_window(win);
}

static void notes_closing(wm_window_t *win) {
  NotesData *d = (NotesData *)win->data;
  if (d) {
    d->in_use = 0;
  }
}

static NotesData *notes_alloc(void) {
  int i;
  for (i = 0; i < NOTES_INSTANCES; i++) {
    if (!g_notes[i].in_use) {
      g_notes[i].in_use = 1;
      g_notes[i].len = 0;
      g_notes[i].text[0] = 0;
      g_notes[i].has_file = 0;
      g_notes[i].vol = 0;
      g_notes[i].dir_cluster = 0;
      g_notes[i].file[0] = 0;
      g_notes[i].status[0] = 0;
      return &g_notes[i];
    }
  }
  return 0;
}

static wm_window_t *notes_window(NotesData *d, const char *title) {
  int x, y;
  wm_window_t *win;

  next_position(&x, &y);
  win = wm_create(title, x, y, 460, 300);
  if (!win) {
    d->in_use = 0;
    return 0;
  }
  win->data = d;
  win->paint = notes_paint;
  win->key = notes_key;
  win->closing = notes_closing;
  win->min_w = 220;
  win->min_h = 140;
  return win;
}

void app_notes_open(void) {
  NotesData *d = notes_alloc();
  if (d) {
    notes_window(d, "Notes");
  }
}

void app_notes_open_file(fat_volume_t *vol, const fat_dirent_t *ent) {
  NotesData *d = notes_alloc();
  uint32_t got = 0, i;
  wm_window_t *win;

  if (!d) {
    return;
  }
  win = notes_window(d, "Notes");
  if (!win) {
    return;
  }
  d->has_file = 1;
  d->vol = vol;
  d->dir_cluster = ent->dir_cluster;
  str_copy(d->file, ent->name, sizeof(d->file));
  notes_set_title(win, d->file);

  if (fat_read_file(vol, ent, g_io_buf, NOTES_MAX * 2, &got)) {
    for (i = 0; i < got && d->len < NOTES_MAX; i++) {
      uint8_t c = g_io_buf[i];
      if (c == '\r') {
        continue;
      }
      if (c == '\n' || c == '\t' || (c >= 0x20 && c <= 0x7E)) {
        d->text[d->len++] = (c == '\t') ? ' ' : (char)c;
      }
    }
    d->text[d->len] = 0;
    if (got > NOTES_MAX) {
      notes_set_status(d, "File truncated to 4096 bytes: do not save", 0);
    }
  } else {
    notes_set_status(d, "Could not read file", 0);
  }
  wm_invalidate_window(win);
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
