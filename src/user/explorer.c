#include "user.h"

// File explorer as a user program (EXPLORER.BIN): a window with a toolbar, a list of
// the drives and folders and a status bar, built only from system calls.
//   - starts at the drive list ("This PC"); click selects, double click or Enter opens
//   - Backspace or "Up" goes up, Delete or "Delete" removes the selection (files and
//     empty folders), "New Folder" makes a folder, "Refresh" reads the folder again
//   - Up / Down / PageUp / PageDown / Home / End move the selection
//   - text files open in NOTES.BIN and .BIN programs run, both through spawn()
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
#define COL_BTN      0x00DADDE8
#define COL_BTN_OFF  0x00ECEDF2
#define COL_BTN_LINE 0x008C91A5
#define COL_TEXT     0x001E1E28
#define COL_DIM      0x0096989F
#define COL_PATH     0x0032374F
#define COL_PAPER    0x00FFFFFF
#define COL_SELECT   0x00BED6FF
#define COL_FOLDER   0x00EBBE3C
#define COL_FILE     0x00F5F5FA
#define COL_FILELINE 0x0078819A
#define COL_STATUS   0x00E2E4EE
#define COL_STATTXT  0x00464B5F
#define COL_OK       0x0028643C
#define COL_ERR      0x00AA2828

static long g_win;
static char g_path[200];               // "" = the drive list, else "/@N" or "/@N/dir/..."
static dirent_t g_ents[MAX_ENTRIES];
static unsigned char g_order[MAX_ENTRIES];
static int g_count;
static int g_sel = -1;
static int g_scroll;
static char g_status[96];
static int g_status_err;

// ---- small string helpers -------------------------------------------------------

static int scopy(char *dst, const char *src, int max) {
  int n = 0;

  while (src[n] != 0 && n < max - 1) {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = 0;
  return n;
}

static char upper(char c) {
  return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static int seq_nocase(const char *a, const char *b) {
  while (*a != 0 && *b != 0) {
    if (upper(*a) != upper(*b)) {
      return 0;
    }
    ++a;
    ++b;
  }
  return *a == *b;
}

static int has_ext(const char *name, const char *ext) {
  int n = (int)strlen(name);
  int e = (int)strlen(ext);

  return n > e && name[n - e - 1] == '.' && seq_nocase(name + n - e, ext);
}

static int is_text_name(const char *name) {
  return has_ext(name, "TXT") || has_ext(name, "MD") || has_ext(name, "LOG") ||
         has_ext(name, "INI") || has_ext(name, "C") || has_ext(name, "H") ||
         has_ext(name, "CSV") || has_ext(name, "CFG");
}

static int uint_text(unsigned long v, char *out) {
  char tmp[24];
  int n = 0;
  int i;

  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v != 0);
  for (i = 0; i < n; ++i) {
    out[i] = tmp[n - 1 - i];
  }
  out[n] = 0;
  return n;
}

static void set_status(const char *a, const char *b, int err) {
  int n = scopy(g_status, a, (int)sizeof(g_status));

  if (b != 0) {
    scopy(g_status + n, b, (int)sizeof(g_status) - n);
  }
  g_status_err = err;
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
    char x = upper(a->name[i]);
    char y = upper(b->name[i]);

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

static dirent_t *selected(void) {
  if (g_sel < 0 || g_sel >= g_count) {
    return 0;
  }
  return &g_ents[g_order[g_sel]];
}

static void load_view(void) {
  g_count = 0;
  g_sel = -1;
  g_scroll = 0;
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
}

// g_path + "/" + name into out. Returns 0 when it does not fit.
static int child_path(char *out, int max, const char *name) {
  int n = (int)strlen(g_path);

  if (n + 1 + (int)strlen(name) >= max) {
    return 0;
  }
  scopy(out, g_path, max);
  out[n] = '/';
  scopy(out + n + 1, name, max - n - 1);
  return 1;
}

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
  if (g_path[0] == '/' && g_path[1] == '@' && n <= 2) {
    g_path[0] = 0;
  }
  load_view();
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
    uint_text((unsigned long)g_order[g_sel], g_path + 2);
    load_view();
    return;
  }
  if (!child_path(full, (int)sizeof(full), e->name)) {
    set_status("Path too long", 0, 1);
    return;
  }
  if ((e->attr & 0x10) != 0) {
    scopy(g_path, full, (int)sizeof(g_path));
    load_view();
  } else if (strlen(full) >= 63) {                 // getarg() buffers are 64 bytes
    set_status("Path too long to open", 0, 1);
  } else if (is_text_name(e->name)) {
    set_status(spawn("/APPS/NOTES.BIN", full) == 0 ? "Opened in Notes: " : "No free program slot: ", e->name, 0);
  } else if (has_ext(e->name, "BIN")) {
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
    int k = scopy(name, "New Folder", (int)sizeof(name));

    if (n > 1) {
      name[k++] = ' ';
      uint_text((unsigned long)n, name + k);
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

static void delete_selected(void) {
  dirent_t *e = selected();
  char full[200];
  char name[128];
  int keep;

  if (g_path[0] == 0 || e == 0 || !child_path(full, (int)sizeof(full), e->name)) {
    return;
  }
  scopy(name, e->name, (int)sizeof(name));
  keep = g_sel;
  if (unlink(full) == 0) {
    load_view();
    select_index(keep);
    set_status("Deleted ", name, 0);
  } else {
    set_status("Cannot delete (read-only, or folder not empty)", 0, 1);
  }
}

// ---- painting ---------------------------------------------------------------------------

static void draw_button(int x, int w, const char *label, int enabled) {
  win_fill((int)g_win, x, BTN_Y, w, BTN_H, enabled ? COL_BTN : COL_BTN_OFF);
  win_fill((int)g_win, x, BTN_Y, w, 1, COL_BTN_LINE);
  win_fill((int)g_win, x, BTN_Y + BTN_H - 1, w, 1, COL_BTN_LINE);
  win_fill((int)g_win, x, BTN_Y, 1, BTN_H, COL_BTN_LINE);
  win_fill((int)g_win, x + w - 1, BTN_Y, 1, BTN_H, COL_BTN_LINE);
  win_text((int)g_win, x + (w - 8 * (int)strlen(label)) / 2, BTN_Y + 6, label, enabled ? COL_TEXT : COL_DIM, 1);
}

static void size_text(unsigned long size, char *out) {
  int n;

  if (size < 1024UL) {
    n = uint_text(size, out);
    scopy(out + n, " B", 4);
  } else if (size < 1024UL * 1024UL) {
    n = uint_text((size + 1023UL) / 1024UL, out);
    scopy(out + n, " KB", 4);
  } else {
    n = uint_text((size + 1048575UL) / 1048576UL, out);
    scopy(out + n, " MB", 4);
  }
}

static void redraw(void) {
  char path[80];
  int i;

  win_fill((int)g_win, 0, 0, W, TOOLBAR_H, COL_BAR);
  draw_button(BTN_UP_X, BTN_UP_W, "Up", g_path[0] != 0);
  draw_button(BTN_NEW_X, BTN_NEW_W, "New Folder", g_path[0] != 0);
  draw_button(BTN_DEL_X, BTN_DEL_W, "Delete", g_path[0] != 0 && selected() != 0);
  draw_button(BTN_REF_X, BTN_REF_W, "Refresh", 1);
  if (g_path[0] == 0) {
    scopy(path, "This PC", (int)sizeof(path));
  } else {
    scopy(path, g_path, (int)sizeof(path));
  }
  win_text((int)g_win, PATH_X, 10, path, COL_PATH, 1);

  win_fill((int)g_win, 0, TOOLBAR_H, W, H - TOOLBAR_H - STATUS_H, COL_PAPER);
  for (i = 0; i < ROWS; ++i) {
    int idx = g_scroll + i;
    int y = LIST_TOP + i * ROW_H;
    const dirent_t *e;
    int is_dir;
    char shown[64];

    if (idx >= g_count) {
      break;
    }
    e = &g_ents[g_order[idx]];
    is_dir = (e->attr & 0x10) != 0;
    if (idx == g_sel) {
      win_fill((int)g_win, 2, y - 1, W - 4, ROW_H, COL_SELECT);
    }
    if (is_dir) {
      win_fill((int)g_win, 6, y + 1, 12, 9, COL_FOLDER);
      win_fill((int)g_win, 6, y, 6, 2, COL_FOLDER);
    } else {
      win_fill((int)g_win, 8, y, 9, 11, COL_FILE);
      win_fill((int)g_win, 8, y, 9, 1, COL_FILELINE);
      win_fill((int)g_win, 8, y + 10, 9, 1, COL_FILELINE);
      win_fill((int)g_win, 8, y, 1, 11, COL_FILELINE);
      win_fill((int)g_win, 16, y, 1, 11, COL_FILELINE);
    }
    scopy(shown, e->name, (int)sizeof(shown));      // long names are cut to fit
    win_text((int)g_win, 24, y + 2, shown, COL_TEXT, 1);
    if (!is_dir || g_path[0] == 0) {
      char size[24];

      size_text(g_path[0] == 0 ? (unsigned long)e->size * 1048576UL : (unsigned long)e->size, size);
      if (g_path[0] == 0 || !is_dir) {
        win_text((int)g_win, W - 12 - 8 * (int)strlen(size), y + 2, size, COL_DIM, 1);
      }
    }
  }

  win_fill((int)g_win, 0, H - STATUS_H, W, STATUS_H, COL_STATUS);
  if (g_status[0] != 0) {
    win_text((int)g_win, 6, H - STATUS_H + 4, g_status, g_status_err ? COL_ERR : COL_OK, 1);
  } else {
    char text[32];
    int n = uint_text((unsigned long)g_count, text);

    scopy(text + n, g_count == 1 ? " item" : " items", (int)sizeof(text) - n);
    win_text((int)g_win, 6, H - STATUS_H + 4, text, COL_STATTXT, 1);
  }
}

// ---- input ---------------------------------------------------------------------------------

static void click(int x, int y) {
  if (y < TOOLBAR_H) {
    if (y < BTN_Y || y >= BTN_Y + BTN_H) {
      return;
    }
    if (x >= BTN_UP_X && x < BTN_UP_X + BTN_UP_W) {
      go_up();
    } else if (x >= BTN_NEW_X && x < BTN_NEW_X + BTN_NEW_W) {
      new_folder();
    } else if (x >= BTN_DEL_X && x < BTN_DEL_X + BTN_DEL_W) {
      delete_selected();
    } else if (x >= BTN_REF_X && x < BTN_REF_X + BTN_REF_W) {
      load_view();
    }
    return;
  }
  if (y >= LIST_TOP && y < H - STATUS_H) {
    int row = g_scroll + (y - LIST_TOP) / ROW_H;

    if (row < g_count) {
      select_index(row);
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

    if (n != 1 || ev.type == EV_CLOSE) {
      break;
    }
    if (ev.type == EV_RESIZE) {
      g_w = ev.x;
      g_h = ev.y;
      select_index(g_sel);                          // keep the selection in view
    } else if (ev.type == EV_MOUSE_DOWN) {
      click(ev.x, ev.y);
    } else if (ev.type == EV_MOUSE_DOUBLE) {
      if (ev.y >= LIST_TOP && ev.y < H - STATUS_H && g_sel >= 0 &&
          g_sel == g_scroll + (ev.y - LIST_TOP) / ROW_H) {
        open_selected();
      }
    } else if (ev.type == EV_KEY) {
      if (ev.ch == KEY_UP) {
        select_index(g_sel - 1);
      } else if (ev.ch == KEY_DOWN) {
        select_index(g_sel + 1);
      } else if (ev.ch == KEY_PGUP) {
        select_index(g_sel - ROWS);
      } else if (ev.ch == KEY_PGDN) {
        select_index(g_sel + ROWS);
      } else if (ev.ch == KEY_HOME) {
        select_index(0);
      } else if (ev.ch == KEY_END) {
        select_index(g_count - 1);
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
