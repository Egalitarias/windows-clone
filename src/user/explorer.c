#include "ulib.h"

// File explorer as a user program (EXPLORER.BIN): a window with a toolbar, a list of
// the drives and folders and a status bar, built only from system calls.
//   - starts at the drive list ("This PC"); click selects, double click or Enter opens
//   - Ctrl+click adds or removes an item from the selection, Shift+click (or
//     Shift+Up / Shift+Down / Shift+PageUp...) selects a range, Ctrl+A selects all
//   - Backspace or "Up" goes up, Delete or "Delete" removes the selected items (files
//     and empty folders), "New Folder" makes a folder, "Refresh" reads the folder again
//   - Up / Down / PageUp / PageDown / Home / End move the selection
//   - text files open in NOTES.BIN and .BIN programs run, both through spawn()
//   - the user can resize the window; the list follows its size
// Drives are addressed with "/@N" path prefixes (see volinfo in user.h).

static int g_w = 560;                               // client size; follows the window (EV_RESIZE)
static int g_h = 360;
#define W          g_w
#define H          g_h
#define TOOLBAR_H  28
#define ROW_H      14
#define STATUS_H   16
#define LIST_TOP   (TOOLBAR_H + 4)
#define ROWS       ((H - LIST_TOP - STATUS_H) / ROW_H)
#define MAX_ENTRIES 96
#define MAX_VOLUMES 8

#define BTN_Y 4
#define BTN_H 20
#define BTN_UP_X   4
#define BTN_UP_W   40
#define BTN_NEW_X  48
#define BTN_NEW_W  96
#define BTN_DEL_X  148
#define BTN_DEL_W  64
#define BTN_REF_X  216
#define BTN_REF_W  64
#define PATH_X     292

#define COL_BAR      0x00E8EAF2
#define COL_TEXT     0x001E1E28
#define COL_DIM      0x0096989F
#define COL_PATH     0x0032374F
#define COL_PAPER    0x00FFFFFF
#define COL_SELECT   0x00BED6FF
#define COL_MARK     0x00DCE8FF
#define COL_FOLDER   0x00EBBE3C
#define COL_FILE     0x00F5F5FA
#define COL_FILELINE 0x0078819A
#define COL_STATUS   0x00E2E4EE
#define COL_STATTXT  0x00464B5F
#define COL_OK       0x0028643C
#define COL_ERR      0x00AA2828

static const ulib_button_style_t kButton = {0x00DADDE8, 0x00ECEDF2, 0x008C91A5, COL_TEXT, COL_DIM};

static long g_win;
static char g_path[200];               // "" = the drive list, else "/@N" or "/@N/dir/..."
static dirent_t g_ents[MAX_ENTRIES];
static unsigned char g_order[MAX_ENTRIES];
static unsigned char g_mark[MAX_ENTRIES];   // by display index: part of the multi-selection
static int g_count;
static int g_sel = -1;                 // the current item (keyboard focus)
static int g_anchor;                   // where a Shift range starts
static int g_scroll;
static char g_status[96];
static int g_status_err;

static void set_status(const char *a, const char *b, int err) {
  ulib_cat(g_status, (int)sizeof(g_status), a, b);
  g_status_err = err;
}

// ---- selection ------------------------------------------------------------------------------

static void clear_marks(void) {
  int i;

  for (i = 0; i < MAX_ENTRIES; ++i) {
    g_mark[i] = 0;
  }
}

static int mark_count(void) {
  int i, n = 0;

  for (i = 0; i < g_count; ++i) {
    n += g_mark[i] != 0;
  }
  return n;
}

// Mark exactly the items from a to b (either order).
static void mark_range(int a, int b) {
  int i;

  if (a > b) {
    int t = a;
    a = b;
    b = t;
  }
  clear_marks();
  for (i = a < 0 ? 0 : a; i <= b && i < g_count; ++i) {
    g_mark[i] = 1;
  }
}

// ---- directory view -------------------------------------------------------------------

static int entry_before(const dirent_t *a, const dirent_t *b) {
  int ad = (a->attr & 0x10) != 0;
  int bd = (b->attr & 0x10) != 0;
  int i = 0;

  if (ad != bd) {
    return ad;
  }
  while (a->name[i] != 0 && b->name[i] != 0) {
    char x = ulib_upper(a->name[i]);
    char y = ulib_upper(b->name[i]);

    if (x != y) {
      return x < y;
    }
    ++i;
  }
  return a->name[i] == 0 && b->name[i] != 0;
}

static void sort_entries(void) {
  int i, j;

  for (i = 0; i < g_count; ++i) {
    g_order[i] = (unsigned char)i;
  }
  for (i = 1; i < g_count; ++i) {
    unsigned char key = g_order[i];

    j = i - 1;
    while (j >= 0 && entry_before(&g_ents[key], &g_ents[g_order[j]])) {
      g_order[j + 1] = g_order[j];
      --j;
    }
    g_order[j + 1] = key;
  }
}

static dirent_t *entry_at(int idx) {
  return &g_ents[g_order[idx]];
}

static dirent_t *selected(void) {
  if (g_sel < 0 || g_sel >= g_count) {
    return 0;
  }
  return entry_at(g_sel);
}

static void load_view(void) {
  g_count = 0;
  g_sel = -1;
  g_anchor = 0;
  g_scroll = 0;
  clear_marks();
  if (g_path[0] == 0) {                            // the drive list
    int i;

    for (i = 0; i < MAX_VOLUMES && g_count < MAX_ENTRIES; ++i) {
      if (volinfo(i, &g_ents[g_count]) != 0) {
        break;
      }
      g_order[g_count] = (unsigned char)g_count;   // drives keep their index order
      ++g_count;
    }
    if (g_count == 0) {
      set_status("No FAT32 volumes found", 0, 1);
    } else {
      set_status("", 0, 0);
    }
  } else {
    long fd = open(g_path, O_RDONLY);
    dirent_t extra;

    if (fd < 0) {
      set_status("Cannot open this folder", 0, 1);
      return;
    }
    while (g_count < MAX_ENTRIES && readdir((int)fd, &g_ents[g_count]) == 1) {
      ++g_count;
    }
    if (g_count == MAX_ENTRIES && readdir((int)fd, &extra) == 1) {
      set_status("Showing the first 96 items", 0, 0);
    } else {
      set_status("", 0, 0);
    }
    close((int)fd);
    sort_entries();
  }
  if (g_count > 0) {
    g_sel = 0;
  }
}

// Make idx the current item and scroll it into view (no change to the marks).
static void select_index(int idx) {
  if (g_count == 0) {
    return;
  }
  if (idx < 0) {
    idx = 0;
  }
  if (idx >= g_count) {
    idx = g_count - 1;
  }
  g_sel = idx;
  if (g_sel < g_scroll) {
    g_scroll = g_sel;
  }
  if (g_sel >= g_scroll + ROWS) {
    g_scroll = g_sel - ROWS + 1;
  }
  if (g_scroll < 0) {
    g_scroll = 0;
  }
}

// Move the current item; with Shift the items between the anchor and it are marked,
// without it the marks are dropped and the anchor moves along.
static void move_selection(int idx, int shift) {
  select_index(idx);
  if (shift) {
    mark_range(g_anchor, g_sel);
  } else {
    clear_marks();
    g_anchor = g_sel;
  }
}

// g_path + "/" + name into out. Returns 0 when it does not fit.
static int child_path(char *out, int max, const char *name) {
  int n = (int)strlen(g_path);

  if (n + 1 + (int)strlen(name) >= max) {
    return 0;
  }
  ulib_copy(out, g_path, max);
  out[n] = '/';
  ulib_copy(out + n + 1, name, max - n - 1);
  return 1;
}

// ---- actions --------------------------------------------------------------------------------

static void go_up(void) {
  int n = (int)strlen(g_path);

  if (n == 0) {
    return;
  }
  while (n > 0 && g_path[n - 1] != '/') {
    --n;
  }
  if (n > 0) {
    --n;                                           // drop the slash too
  }
  g_path[n] = 0;                                   // "/@N" -> "" (the drive list)
  load_view();
}

static int is_text_name(const char *name) {
  return ulib_has_ext(name, "TXT") || ulib_has_ext(name, "MD") || ulib_has_ext(name, "LOG") ||
         ulib_has_ext(name, "INI") || ulib_has_ext(name, "C") || ulib_has_ext(name, "H") ||
         ulib_has_ext(name, "CSV") || ulib_has_ext(name, "CFG");
}

static void open_selected(void) {
  dirent_t *e = selected();
  char full[200];

  if (e == 0) {
    return;
  }
  if (g_path[0] == 0) {                            // a drive: its index is its position
    g_path[0] = '/';
    g_path[1] = '@';
    ulib_utoa((unsigned long)g_order[g_sel], g_path + 2);
    load_view();
    return;
  }
  if (!child_path(full, (int)sizeof(full), e->name)) {
    set_status("Path too long", 0, 1);
    return;
  }
  if ((e->attr & 0x10) != 0) {
    ulib_copy(g_path, full, (int)sizeof(g_path));
    load_view();
  } else if (strlen(full) >= 63) {                 // getarg() buffers are 64 bytes
    set_status("Path too long to open", 0, 1);
  } else if (is_text_name(e->name)) {
    set_status(spawn("/APPS/NOTES.BIN", full) == 0 ? "Opened in Notes: " : "No free program slot: ", e->name, 0);
  } else if (ulib_has_ext(e->name, "BIN")) {
    if (spawn(full, 0) == 0) {
      set_status("Started ", e->name, 0);
    } else {
      set_status("No free program slot", 0, 1);
    }
  } else {
    set_status("No app for this file type", 0, 1);
  }
}

static void new_folder(void) {
  int n;

  if (g_path[0] == 0) {
    return;
  }
  for (n = 1; n < 1000; ++n) {
    char name[32];
    char full[200];
    dirent_t probe;
    int k = ulib_copy(name, "New Folder", (int)sizeof(name));

    if (n > 1) {
      name[k++] = ' ';
      ulib_utoa((unsigned long)n, name + k);
    }
    if (!child_path(full, (int)sizeof(full), name)) {
      set_status("Path too long", 0, 1);
      return;
    }
    if (stat(full, &probe) == 0) {
      continue;
    }
    if (mkdir(full) == 0) {
      load_view();
      set_status("Created ", name, 0);
    } else {
      set_status("Could not create folder", 0, 1);
    }
    return;
  }
}

// Delete the marked items, or the current one when nothing is marked.
static void delete_selected(void) {
  int multi = mark_count() > 0;
  int done = 0;
  int failed = 0;
  int keep = g_sel;
  char name[128];
  int i;

  if (g_path[0] == 0 || selected() == 0) {
    return;
  }
  name[0] = 0;
  for (i = 0; i < g_count; ++i) {
    char full[200];

    if (multi ? !g_mark[i] : i != g_sel) {
      continue;
    }
    if (!child_path(full, (int)sizeof(full), entry_at(i)->name)) {
      ++failed;
      continue;
    }
    if (unlink(full) == 0) {
      ++done;
      ulib_copy(name, entry_at(i)->name, (int)sizeof(name));
    } else {
      ++failed;
    }
  }
  load_view();
  select_index(keep);
  g_anchor = g_sel;
  if (failed == 0 && done == 1) {
    set_status("Deleted ", name, 0);
  } else if (failed == 0) {
    char text[40];
    int n = ulib_utoa((unsigned long)done, text);

    ulib_copy(text + n, " items deleted", (int)sizeof(text) - n);
    set_status(text, 0, 0);
  } else {
    set_status("Some items could not be deleted (read-only, or folder not empty)", 0, 1);
  }
}

// ---- painting ---------------------------------------------------------------------------------

static void redraw(void) {
  char path[80];
  int i;
  int win = (int)g_win;

  win_fill(win, 0, 0, W, TOOLBAR_H, COL_BAR);
  ulib_button(win, BTN_UP_X, BTN_Y, BTN_UP_W, BTN_H, "Up", g_path[0] != 0, &kButton);
  ulib_button(win, BTN_NEW_X, BTN_Y, BTN_NEW_W, BTN_H, "New Folder", g_path[0] != 0, &kButton);
  ulib_button(win, BTN_DEL_X, BTN_Y, BTN_DEL_W, BTN_H, "Delete", g_path[0] != 0 && selected() != 0, &kButton);
  ulib_button(win, BTN_REF_X, BTN_Y, BTN_REF_W, BTN_H, "Refresh", 1, &kButton);
  ulib_copy(path, g_path[0] == 0 ? "This PC" : g_path, (int)sizeof(path));
  ulib_text_fit(win, PATH_X, 10, path, COL_PATH, (W - PATH_X - 4) / 8);

  win_fill(win, 0, TOOLBAR_H, W, H - TOOLBAR_H - STATUS_H, COL_PAPER);
  for (i = 0; i < ROWS; ++i) {
    int idx = g_scroll + i;
    int y = LIST_TOP + i * ROW_H;
    const dirent_t *e;
    int is_dir;

    if (idx >= g_count) {
      break;
    }
    e = entry_at(idx);
    is_dir = (e->attr & 0x10) != 0;
    if (idx == g_sel) {
      win_fill(win, 2, y - 1, W - 4, ROW_H, COL_SELECT);
    } else if (g_mark[idx]) {
      win_fill(win, 2, y - 1, W - 4, ROW_H, COL_MARK);
    }
    if (is_dir) {
      win_fill(win, 6, y + 1, 12, 9, COL_FOLDER);
      win_fill(win, 6, y, 6, 2, COL_FOLDER);
    } else {
      win_fill(win, 8, y, 9, 11, COL_FILE);
      ulib_outline(win, 8, y, 9, 11, COL_FILELINE);
    }
    {
      char size[24];
      int size_w = 0;

      if (g_path[0] == 0) {                        // drives: capacity (volinfo gives megabytes)
        ulib_size_text((unsigned long)e->size * 1048576UL, size);
        size_w = ulib_text_width(size);
      } else if (!is_dir) {
        ulib_size_text((unsigned long)e->size, size);
        size_w = ulib_text_width(size);
      }
      ulib_text_fit(win, 24, y + 2, e->name, COL_TEXT, (W - 24 - 20 - size_w) / 8);
      if (size_w != 0) {
        win_text(win, W - 12 - size_w, y + 2, size, COL_DIM, 1);
      }
    }
  }

  win_fill(win, 0, H - STATUS_H, W, STATUS_H, COL_STATUS);
  if (g_status[0] != 0) {
    win_text(win, 6, H - STATUS_H + 4, g_status, g_status_err ? COL_ERR : COL_OK, 1);
  } else {
    char text[48];
    int marks = mark_count();
    int n = 0;

    if (marks > 0) {
      n += ulib_utoa((unsigned long)marks, text + n);
      n += ulib_copy(text + n, " of ", (int)sizeof(text) - n);
    }
    n += ulib_utoa((unsigned long)g_count, text + n);
    ulib_copy(text + n, marks > 0 ? " selected" : (g_count == 1 ? " item" : " items"), (int)sizeof(text) - n);
    win_text(win, 6, H - STATUS_H + 4, text, COL_STATTXT, 1);
  }
}

// ---- input ------------------------------------------------------------------------------------

static void click(int x, int y, unsigned int mods) {
  if (y < TOOLBAR_H) {
    if (y < BTN_Y || y >= BTN_Y + BTN_H) {
      return;
    }
    if (ulib_in_rect(x, y, BTN_UP_X, BTN_Y, BTN_UP_W, BTN_H)) {
      go_up();
    } else if (ulib_in_rect(x, y, BTN_NEW_X, BTN_Y, BTN_NEW_W, BTN_H)) {
      new_folder();
    } else if (ulib_in_rect(x, y, BTN_DEL_X, BTN_Y, BTN_DEL_W, BTN_H)) {
      delete_selected();
    } else if (ulib_in_rect(x, y, BTN_REF_X, BTN_Y, BTN_REF_W, BTN_H)) {
      load_view();
    }
    return;
  }
  if (y >= LIST_TOP && y < H - STATUS_H) {
    int row = g_scroll + (y - LIST_TOP) / ROW_H;

    if (row >= g_count) {
      return;
    }
    if ((mods & MOD_CTRL) != 0 && g_path[0] != 0) {          // add / remove one item
      if (mark_count() == 0 && g_sel >= 0) {
        g_mark[g_sel] = 1;                                   // the current item joins the set
      }
      g_mark[row] = !g_mark[row];
      select_index(row);
      g_anchor = row;
    } else if ((mods & MOD_SHIFT) != 0 && g_path[0] != 0) {  // a range from the anchor
      select_index(row);
      mark_range(g_anchor, row);
    } else {
      select_index(row);
      clear_marks();
      g_anchor = row;
    }
  }
}

int main(void) {
  win_event_t ev;

  g_win = win_create("Files", W, H);
  if (g_win < 0) {
    puts("explorer: could not create a window\r\n");
    return 1;
  }
  win_resizable((int)g_win, 1);
  load_view();
  redraw();

  for (;;) {
    long n = win_event((int)g_win, &ev, 1);
    int shift;

    if (n != 1 || ev.type == EV_CLOSE) {
      break;
    }
    shift = (ulib_mods(&ev) & MOD_SHIFT) != 0;
    if (ev.type == EV_RESIZE) {
      g_w = ev.x;
      g_h = ev.y;
      select_index(g_sel);                          // keep the selection in view
    } else if (ev.type == EV_MOUSE_DOWN) {
      click(ev.x, ev.y, ulib_mods(&ev));
    } else if (ev.type == EV_WHEEL) {
      int max = g_count - ROWS;

      g_scroll -= (int)ev.ch * 3;
      if (g_scroll > max) {
        g_scroll = max;
      }
      if (g_scroll < 0) {
        g_scroll = 0;
      }
    } else if (ev.type == EV_MOUSE_DOUBLE) {
      if (ev.y >= LIST_TOP && ev.y < H - STATUS_H && g_sel >= 0 &&
          g_sel == g_scroll + (ev.y - LIST_TOP) / ROW_H) {
        open_selected();
      }
    } else if (ev.type == EV_KEY) {
      if (ev.ch == KEY_UP) {
        move_selection(g_sel - 1, shift);
      } else if (ev.ch == KEY_DOWN) {
        move_selection(g_sel + 1, shift);
      } else if (ev.ch == KEY_PGUP) {
        move_selection(g_sel - ROWS, shift);
      } else if (ev.ch == KEY_PGDN) {
        move_selection(g_sel + ROWS, shift);
      } else if (ev.ch == KEY_HOME) {
        move_selection(0, shift);
      } else if (ev.ch == KEY_END) {
        move_selection(g_count - 1, shift);
      } else if (ev.ch == 0x01 && g_path[0] != 0) {   // Ctrl+A: select all
        mark_range(0, g_count - 1);
      } else if (ev.ch == '\n') {
        open_selected();
      } else if (ev.ch == '\b') {
        go_up();
      } else if (ev.ch == KEY_DELETE) {
        delete_selected();
      } else {
        continue;
      }
    } else {
      continue;
    }
    redraw();
  }
  win_close((int)g_win);
  return 0;
}
