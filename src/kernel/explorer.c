#include "apps.h"
#include "fb.h"
#include "fs.h"
#include "ps2.h"
#include "string.h"
#include "wm.h"

// File explorer: a drive list, then directories on the mounted FAT32 volumes.
// Single instance. Click selects, double click (or Enter) opens, Backspace goes
// up, Delete removes the selected item, and the toolbar has Up / New Folder /
// Delete buttons. Text files open in Notes.

#define MAX_ENTRIES   96
#define MAX_DEPTH     16
#define TOOLBAR_H     28
#define ROW_H         14
#define STATUS_H      16
#define LIST_TOP      (TOOLBAR_H + 4)

#define BTN_UP_X      4
#define BTN_UP_W      40
#define BTN_NEW_X     48
#define BTN_NEW_W     96
#define BTN_DEL_X     148
#define BTN_DEL_W     64
#define BTN_Y         4
#define BTN_H         20

static wm_window_t *g_win = 0;
static int g_vol = -1;                       // -1 = list of drives
static uint32_t g_dir = 0;
static uint32_t g_stack[MAX_DEPTH];
static char g_stack_name[MAX_DEPTH][16];
static int g_depth = 0;

static fat_dirent_t g_ents[MAX_ENTRIES];
static uint8_t g_order[MAX_ENTRIES];         // display order: folders first, then by name
static int g_count = 0;
static int g_sel = -1;                       // index into g_order
static int g_scroll = 0;
static char g_status[64];
static int g_status_error = 0;

// ---- helpers ---------------------------------------------------------------

static void set_status(const char *a, const char *b) {
  int n = (int)str_copy(g_status, a, sizeof(g_status));
  g_status_error = 0;
  if (b) {
    str_copy(g_status + n, b, sizeof(g_status) - (size_t)n);
  }
}

static void set_error(const char *a) {
  set_status(a, 0);
  g_status_error = 1;
}

static int visible_rows(int ch) {
  int rows = (ch - LIST_TOP - STATUS_H) / ROW_H;
  return rows < 1 ? 1 : rows;
}

static int is_text_file(const char *name) {
  size_t n = str_len(name), i = n;
  char ext[8];
  while (i > 0 && name[i - 1] != '.') {
    i--;
  }
  if (i == 0 || n - i >= sizeof(ext)) {
    return 0;
  }
  str_copy(ext, name + i, sizeof(ext));
  return str_eq_nocase(ext, "txt") || str_eq_nocase(ext, "md") || str_eq_nocase(ext, "log") ||
         str_eq_nocase(ext, "ini") || str_eq_nocase(ext, "c") || str_eq_nocase(ext, "h") ||
         str_eq_nocase(ext, "csv") || str_eq_nocase(ext, "cfg");
}

// "less than" for the display order
static int entry_before(const fat_dirent_t *a, const fat_dirent_t *b) {
  int ad = (a->attr & FAT_ATTR_DIR) != 0, bd = (b->attr & FAT_ATTR_DIR) != 0;
  size_t i = 0;
  if (ad != bd) {
    return ad;
  }
  while (a->name[i] && b->name[i]) {
    char x = char_upper(a->name[i]), y = char_upper(b->name[i]);
    if (x != y) {
      return x < y;
    }
    i++;
  }
  return a->name[i] == 0 && b->name[i] != 0;
}

static void sort_entries(void) {
  int i, j;
  for (i = 0; i < g_count; i++) {
    g_order[i] = (uint8_t)i;
  }
  for (i = 1; i < g_count; i++) {          // insertion sort on indices (no struct copies)
    uint8_t key = g_order[i];
    j = i - 1;
    while (j >= 0 && entry_before(&g_ents[key], &g_ents[g_order[j]])) {
      g_order[j + 1] = g_order[j];
      j--;
    }
    g_order[j + 1] = key;
  }
}

static fat_dirent_t *selected(void) {
  if (g_sel < 0 || g_sel >= g_count) {
    return 0;
  }
  return &g_ents[g_order[g_sel]];
}

static void refresh(void) {
  if (g_win) {
    wm_invalidate_window(g_win);
  }
}

static void load_view(void) {
  g_count = 0;
  g_sel = -1;
  g_scroll = 0;
  if (g_vol < 0) {                           // drive list
    int i;
    for (i = 0; i < fs_volume_count() && g_count < MAX_ENTRIES; i++) {
      fat_dirent_t *e = &g_ents[g_count++];
      memset(e, 0, sizeof(*e));
      str_copy(e->name, fs_volume_name(i), FAT_NAME_MAX);
      e->attr = FAT_ATTR_DIR;
      e->first_cluster = (uint32_t)i;        // volume index
    }
    if (g_count == 0) {
      set_error("No FAT32 volumes found");
    } else {
      set_status("", 0);
    }
  } else {
    fat_volume_t *v = fs_volume(g_vol);
    fat_iter_t it;
    fat_dirent_t tmp;
    int truncated = 0;
    if (!v) {
      return;
    }
    fat_iter_open(v, g_dir, &it);
    while (fat_iter_next(&it, &tmp)) {
      if (g_count >= MAX_ENTRIES) {
        truncated = 1;
        break;
      }
      memcpy(&g_ents[g_count++], &tmp, sizeof(tmp));
    }
    set_status(truncated ? "Showing the first 96 items" : "", 0);
  }
  sort_entries();
  if (g_count > 0) {
    g_sel = 0;
  }
  refresh();
}

static void path_text(char *out, size_t max) {
  int n, i;
  if (g_vol < 0) {
    str_copy(out, "This PC", max);
    return;
  }
  n = (int)str_copy(out, fs_volume_name(g_vol), max);
  if (n + 2 < (int)max) {
    out[n++] = ':';
    out[n++] = '/';
    out[n] = 0;
  }
  for (i = 0; i < g_depth; i++) {
    n += (int)str_copy(out + n, g_stack_name[i], max - (size_t)n);
    if (n + 1 < (int)max) {
      out[n++] = '/';
      out[n] = 0;
    }
  }
}

// ---- actions ---------------------------------------------------------------

static void go_up(void) {
  fat_volume_t *v;
  if (g_vol < 0) {
    return;
  }
  if (g_depth > 0) {
    g_depth--;
    v = fs_volume(g_vol);
    g_dir = g_depth > 0 ? g_stack[g_depth - 1] : (v ? v->root_cluster : 0);
  } else {
    g_vol = -1;
  }
  load_view();
}

static void open_selected(void) {
  fat_dirent_t *e = selected();
  fat_volume_t *v;

  if (!e) {
    return;
  }
  if (g_vol < 0) {                           // open a drive
    v = fs_volume((int)e->first_cluster);
    if (!v) {
      return;
    }
    g_vol = (int)e->first_cluster;
    g_dir = v->root_cluster;
    g_depth = 0;
    load_view();
    return;
  }
  v = fs_volume(g_vol);
  if (e->attr & FAT_ATTR_DIR) {
    if (g_depth >= MAX_DEPTH || e->first_cluster < 2) {
      return;
    }
    g_stack[g_depth] = e->first_cluster;
    str_copy(g_stack_name[g_depth], e->name, sizeof(g_stack_name[g_depth]));
    g_depth++;
    g_dir = e->first_cluster;
    load_view();
  } else if (v && is_text_file(e->name)) {
    app_notes_open_file(v, e);
  } else {
    set_error("No app for this file type");
    refresh();
  }
}

static void new_folder(void) {
  fat_volume_t *v = (g_vol >= 0) ? fs_volume(g_vol) : 0;
  fat_dirent_t probe;
  int n;

  if (!v) {
    return;
  }
  for (n = 1; n < 1000; n++) {
    char name[24];
    int k = (int)str_copy(name, "New Folder", sizeof(name));
    if (n > 1) {
      name[k++] = ' ';
      k += u32_to_str((uint32_t)n, name + k);
    }
    if (!fat_lookup(v, g_dir, name, &probe)) {
      if (fat_mkdir(v, g_dir, name, 0)) {
        load_view();
        set_status("Created ", name);
      } else {
        set_error("Could not create folder");
      }
      refresh();
      return;
    }
  }
}

static void delete_selected(void) {
  fat_volume_t *v = (g_vol >= 0) ? fs_volume(g_vol) : 0;
  fat_dirent_t *e = selected();
  char name[FAT_NAME_MAX];

  if (!v || !e) {
    return;
  }
  str_copy(name, e->name, sizeof(name));
  if (fat_delete(v, e)) {
    load_view();
    set_status("Deleted ", name);
  } else {
    set_error("Cannot delete (folder not empty?)");
  }
  refresh();
}

// ---- painting --------------------------------------------------------------

static void draw_button(int x, int y, int w, const char *label, int enabled) {
  fb_fill_rect(x, y, w, BTN_H, enabled ? FB_RGB(218, 221, 232) : FB_RGB(236, 237, 242));
  fb_draw_rect(x, y, w, BTN_H, FB_RGB(140, 145, 165));
  fb_draw_text(x + (w - fb_text_width(label, 1)) / 2, y + 6, label,
               enabled ? FB_RGB(30, 30, 40) : FB_RGB(150, 152, 165), 1);
}

static void format_size(uint32_t size, char *out) {
  int n;
  if (size < 1024) {
    n = u32_to_str(size, out);
    str_copy(out + n, " B", 4);
  } else if (size < 1024u * 1024u) {
    n = u32_to_str((size + 1023) / 1024, out);
    str_copy(out + n, " KB", 4);
  } else {
    n = u32_to_str((size + 1024u * 1024u - 1) / (1024u * 1024u), out);
    str_copy(out + n, " MB", 4);
  }
}

static void explorer_paint(wm_window_t *win, int cx, int cy, int cw, int ch) {
  char path[96];
  int rows = visible_rows(ch);
  int i;

  (void)win;
  // Toolbar
  fb_fill_rect(cx, cy, cw, TOOLBAR_H, FB_RGB(232, 234, 242));
  draw_button(cx + BTN_UP_X, cy + BTN_Y, BTN_UP_W, "Up", g_vol >= 0);
  draw_button(cx + BTN_NEW_X, cy + BTN_Y, BTN_NEW_W, "New Folder", g_vol >= 0);
  draw_button(cx + BTN_DEL_X, cy + BTN_Y, BTN_DEL_W, "Delete", g_vol >= 0 && selected() != 0);
  path_text(path, sizeof(path));
  fb_draw_text(cx + BTN_DEL_X + BTN_DEL_W + 14, cy + 10, path, FB_RGB(50, 55, 80), 1);

  // List
  fb_fill_rect(cx, cy + TOOLBAR_H, cw, ch - TOOLBAR_H - STATUS_H, FB_RGB(255, 255, 255));
  for (i = 0; i < rows; i++) {
    int idx = g_scroll + i;
    int y = cy + LIST_TOP + i * ROW_H;
    const fat_dirent_t *e;
    int is_dir;

    if (idx >= g_count) {
      break;
    }
    e = &g_ents[g_order[idx]];
    is_dir = (e->attr & FAT_ATTR_DIR) != 0;
    if (idx == g_sel) {
      fb_fill_rect(cx + 2, y - 1, cw - 4, ROW_H, FB_RGB(190, 214, 255));
    }
    if (is_dir) {
      fb_fill_rect(cx + 6, y + 1, 12, 9, FB_RGB(235, 190, 60));
      fb_fill_rect(cx + 6, y, 6, 2, FB_RGB(235, 190, 60));
    } else {
      fb_fill_rect(cx + 8, y, 9, 11, FB_RGB(245, 245, 250));
      fb_draw_rect(cx + 8, y, 9, 11, FB_RGB(120, 125, 145));
    }
    fb_draw_text(cx + 24, y + 2, e->name, FB_RGB(25, 25, 35), 1);
    if (!is_dir && g_vol >= 0) {
      char size[16];
      format_size(e->size, size);
      fb_draw_text(cx + cw - 12 - fb_text_width(size, 1), y + 2, size, FB_RGB(90, 95, 115), 1);
    }
  }

  // Status bar
  fb_fill_rect(cx, cy + ch - STATUS_H, cw, STATUS_H, FB_RGB(226, 228, 238));
  if (g_status[0]) {
    fb_draw_text(cx + 6, cy + ch - STATUS_H + 4, g_status,
                 g_status_error ? FB_RGB(170, 40, 40) : FB_RGB(40, 100, 60), 1);
  } else {
    char text[32];
    int n = u32_to_str((uint32_t)g_count, text);
    str_copy(text + n, g_count == 1 ? " item" : " items", sizeof(text) - (size_t)n);
    fb_draw_text(cx + 6, cy + ch - STATUS_H + 4, text, FB_RGB(70, 75, 95), 1);
  }
}

// ---- input -----------------------------------------------------------------

static void select_index(int idx) {
  int rows;
  int cx, cy, cw, ch;
  if (g_count == 0) {
    return;
  }
  if (idx < 0) idx = 0;
  if (idx >= g_count) idx = g_count - 1;
  g_sel = idx;
  wm_client_rect(g_win, &cx, &cy, &cw, &ch);
  rows = visible_rows(ch);
  if (g_sel < g_scroll) g_scroll = g_sel;
  if (g_sel >= g_scroll + rows) g_scroll = g_sel - rows + 1;
  refresh();
}

static void explorer_mouse(wm_window_t *win, int lx, int ly, int event) {
  int cx, cy, cw, ch, row;

  if (event == WM_MOUSE_UP) {
    return;
  }
  wm_client_rect(win, &cx, &cy, &cw, &ch);
  if (ly < TOOLBAR_H) {
    if (event != WM_MOUSE_DOWN || ly < BTN_Y || ly >= BTN_Y + BTN_H || g_vol < 0) {
      return;
    }
    if (lx >= BTN_UP_X && lx < BTN_UP_X + BTN_UP_W) {
      go_up();
    } else if (lx >= BTN_NEW_X && lx < BTN_NEW_X + BTN_NEW_W) {
      new_folder();
    } else if (lx >= BTN_DEL_X && lx < BTN_DEL_X + BTN_DEL_W) {
      delete_selected();
    }
    return;
  }
  if (ly < LIST_TOP || ly >= ch - STATUS_H) {
    return;
  }
  row = g_scroll + (ly - LIST_TOP) / ROW_H;
  if (row >= g_count) {
    return;
  }
  if (event == WM_MOUSE_DOWN) {
    select_index(row);
  } else if (event == WM_MOUSE_DOUBLE) {
    select_index(row);
    open_selected();
  }
}

static void explorer_key(wm_window_t *win, char ch) {
  (void)win;
  if (ch == PS2_KEY_UP) {
    select_index(g_sel - 1);
  } else if (ch == PS2_KEY_DOWN) {
    select_index(g_sel + 1);
  } else if (ch == '\n') {
    open_selected();
  } else if (ch == '\b') {
    go_up();
  } else if (ch == PS2_KEY_DELETE) {
    delete_selected();
  }
}

static void explorer_closing(wm_window_t *win) {
  (void)win;
  g_win = 0;
}

void app_files_open(void) {
  wm_window_t *win;

  if (g_win) {
    wm_restore(g_win);
    return;
  }
  win = wm_create("Files", 80, 70, 540, 340);
  if (!win) {
    return;
  }
  g_win = win;
  win->paint = explorer_paint;
  win->mouse = explorer_mouse;
  win->key = explorer_key;
  win->closing = explorer_closing;
  win->min_w = 300;
  win->min_h = 160;
  g_vol = -1;
  g_depth = 0;
  load_view();
  // With a single volume, open it straight away.
  if (fs_volume_count() == 1) {
    g_sel = 0;
    open_selected();
  }
}
