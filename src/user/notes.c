#include "user.h"

// Notes as a real user program: a text editor in its own window.
//   - type to insert text at the cursor; Backspace and Delete remove text
//   - arrows move the cursor (Up/Down keep the column), Home / End go to the start / end
//     of the row (with Ctrl: of the text), PageUp / PageDown move a page, a click places
//     the cursor and dragging selects
//   - Shift with any movement key selects; Ctrl+A selects all; Ctrl+C / Ctrl+X / Ctrl+V
//     copy, cut and paste through the system clipboard (shared with other programs);
//     typing or pasting replaces the selection
//   - Ctrl+S saves; closing the window quits
//   - started with an argument (a file path) it opens that file and saves back to
//     it; without one it saves to the first free /NOTEn.TXT
// It uses only the window syscalls (win_*), the clipboard and the file syscalls.

static int g_w = 480;                               // client size; follows the window (EV_RESIZE)
static int g_h = 300;
#define W        g_w
#define H        g_h
#define MAX_W    800
#define MAX      4096
#define STATUS_H 16
#define ROW_H    10
#define COLS     ((W - 16) / 8)
#define ROWS     ((H - STATUS_H - 12) / ROW_H)

#define COL_PAPER  0x00FFFFFF
#define COL_TEXT   0x001E1E1E
#define COL_CURSOR 0x002850BE
#define COL_SEL    0x00B4D2FF
#define COL_BAR    0x00E4E6EE
#define COL_BARTXT 0x00464B5F

static char g_text[MAX + 1];
static int g_len;
static char g_file[64];
static int g_has_file;
static char g_status[64];
static unsigned char g_io[MAX * 2 + 16];
static long g_win;

static void set_status(const char *a, const char *b) {
  int n = 0;

  while (a[n] != 0 && n < (int)sizeof(g_status) - 1) {
    g_status[n] = a[n];
    ++n;
  }
  if (b != 0) {
    int i = 0;

    while (b[i] != 0 && n < (int)sizeof(g_status) - 1) {
      g_status[n++] = b[i++];
    }
  }
  g_status[n] = 0;
}

static void set_title(void) {
  char title[32] = "Notes - ";
  const char *name = g_file;
  int n = 8;
  int i;

  for (i = 0; g_file[i] != 0; ++i) {
    if (g_file[i] == '/') {
      name = g_file + i + 1;
    }
  }
  for (i = 0; name[i] != 0 && n < (int)sizeof(title) - 1; ++i) {
    title[n++] = name[i];
  }
  title[n] = 0;
  win_title((int)g_win, title);
}

// ---- layout --------------------------------------------------------------------
// The text is shown in rows of at most COLS characters; a newline ends a row (and
// belongs to it, but is not drawn). g_rs[r] is the index of the first character of
// row r. A cursor at index p is on the last row that starts at or before p.

static int g_rs[MAX + 3];
static int g_nrows;
static int g_cur;                                   // cursor: index into g_text, 0..g_len
static int g_want;                                  // column Up/Down try to keep
static int g_top;                                   // first visible row
static int g_anchor = -1;                           // selection start (-1 = none); the cursor is the other end
static char g_clip[MAX + 1];

static void build_rows(void) {
  int i = 0;

  g_nrows = 0;
  for (;;) {
    int col = 0;
    int cont = 0;                                   // 1 = another row follows, even at the end of the text

    g_rs[g_nrows++] = i;
    while (i < g_len) {
      char c = g_text[i];

      if (c == '\n') {
        ++i;
        cont = 1;
        break;
      }
      if (col >= COLS) {
        cont = 1;
        break;
      }
      ++col;
      ++i;
    }
    if (i >= g_len && col >= COLS) {
      cont = 1;
    }
    if (!cont) {
      break;
    }
    if (i >= g_len) {                               // empty last row (after a newline or a full row)
      g_rs[g_nrows++] = i;
      break;
    }
  }
}

static int row_of(int pos) {
  int r = 0;

  while (r + 1 < g_nrows && g_rs[r + 1] <= pos) {
    ++r;
  }
  return r;
}

static int row_len(int r) {                         // characters shown on row r
  int end = (r + 1 < g_nrows) ? g_rs[r + 1] : g_len;
  int n = end - g_rs[r];

  if (n > 0 && g_text[end - 1] == '\n') {
    --n;
  }
  return n;
}

static void set_cursor(int pos) {
  build_rows();
  g_cur = pos < 0 ? 0 : (pos > g_len ? g_len : pos);
  g_want = g_cur - g_rs[row_of(g_cur)];
}

static void move_row(int delta) {                   // Up / Down by delta rows, keeping the column
  int r;
  int len;

  build_rows();
  r = row_of(g_cur) + delta;
  if (r < 0) {
    g_cur = 0;                                      // Up on the first row: start of the text
    return;
  }
  if (r >= g_nrows) {
    g_cur = g_len;                                  // Down on the last row: end of the text
    return;
  }
  len = row_len(r);
  g_cur = g_rs[r] + (g_want < len ? g_want : len);
}

// The selection as [*a, *b); returns 0 when nothing is selected.
static int sel_range(int *a, int *b) {
  if (g_anchor < 0 || g_anchor == g_cur) {
    return 0;
  }
  *a = g_anchor < g_cur ? g_anchor : g_cur;
  *b = g_anchor < g_cur ? g_cur : g_anchor;
  return 1;
}

static void redraw(void) {
  int cr, cc, r, x, y, sa, sb;
  int has_sel;
  char line[(MAX_W - 16) / 8 + 2];

  build_rows();
  cr = row_of(g_cur);
  cc = g_cur - g_rs[cr];
  if (cr < g_top) {
    g_top = cr;
  }
  if (cr >= g_top + ROWS) {
    g_top = cr - ROWS + 1;
  }
  has_sel = sel_range(&sa, &sb);
  win_fill((int)g_win, 0, 0, W, H - STATUS_H, COL_PAPER);
  for (r = g_top; r < g_nrows && r < g_top + ROWS; ++r) {
    int n = row_len(r);
    int i;

    if (has_sel) {                                  // highlight behind the text
      int lo = sa > g_rs[r] ? sa : g_rs[r];
      int hi = sb < g_rs[r] + n ? sb : g_rs[r] + n;
      int nl = sb > g_rs[r] + n && sa <= g_rs[r] + n && g_rs[r] + n < g_len && g_text[g_rs[r] + n] == '\n';

      if (hi > lo || nl) {
        int w = (hi > lo ? hi - lo : 0) * 8 + (nl ? 4 : 0);

        win_fill((int)g_win, 8 + (hi > lo ? lo - g_rs[r] : n) * 8, 6 + (r - g_top) * ROW_H - 1, w, ROW_H, COL_SEL);
      }
    }
    for (i = 0; i < n && i < COLS; ++i) {
      line[i] = g_text[g_rs[r] + i];
    }
    line[i] = 0;
    if (n > 0) {
      win_text((int)g_win, 8, 6 + (r - g_top) * ROW_H, line, COL_TEXT, 1);
    }
  }
  x = 8 + cc * 8;
  y = 6 + (cr - g_top) * ROW_H;
  win_fill((int)g_win, x, y - 1, 2, ROW_H, COL_CURSOR);
  win_fill((int)g_win, 0, H - STATUS_H, W, STATUS_H, COL_BAR);
  win_text((int)g_win, 6, H - STATUS_H + 4, g_status[0] ? g_status : "Ctrl+S saves", COL_BARTXT, 1);
}

static void insert_char(char c) {
  int i;

  if (g_len >= MAX) {
    return;
  }
  for (i = g_len; i > g_cur; --i) {
    g_text[i] = g_text[i - 1];
  }
  g_text[g_cur] = c;
  ++g_len;
  g_text[g_len] = 0;
  set_cursor(g_cur + 1);
}

static void delete_range(int a, int b) {            // remove g_text[a..b)
  int i;

  if (a < 0 || b > g_len || a >= b) {
    return;
  }
  for (i = a; i + (b - a) < g_len; ++i) {
    g_text[i] = g_text[i + (b - a)];
  }
  g_len -= b - a;
  g_text[g_len] = 0;
}

// Delete the selection, leaving the cursor where it was. Returns 1 if there was one.
static int delete_selection(void) {
  int a, b;

  if (!sel_range(&a, &b)) {
    g_anchor = -1;
    return 0;
  }
  delete_range(a, b);
  g_anchor = -1;
  set_cursor(a);
  return 1;
}

// Called before a movement key: with Shift start (or keep) a selection, else drop it.
static void begin_move(int shift) {
  if (shift) {
    if (g_anchor < 0) {
      g_anchor = g_cur;
    }
  } else {
    g_anchor = -1;
  }
}

// Text position under a point in the window.
static int pos_at(int x, int y) {
  int r = y < 6 ? g_top : g_top + (y - 6) / ROW_H;
  int c = (x - 8 + 4) / 8;

  build_rows();
  if (r >= g_nrows) {
    r = g_nrows - 1;
  }
  if (c < 0) {
    c = 0;
  }
  if (c > row_len(r)) {
    c = row_len(r);
  }
  return g_rs[r] + c;
}

static void copy_selection(void) {
  int a, b;

  if (sel_range(&a, &b)) {
    clip_set(g_text + a, (unsigned long)(b - a));
    set_status("Copied", 0);
  }
}

static void paste(void) {
  long n = clip_get(g_clip, MAX);
  long i;

  if (n <= 0) {
    return;
  }
  delete_selection();
  for (i = 0; i < n && g_len < MAX; ++i) {
    unsigned char c = (unsigned char)g_clip[i];

    if (c == '\n' || (c >= 0x20 && c <= 0x7E)) {
      insert_char((char)c);
    } else if (c == '\t') {
      insert_char(' ');
    }
  }
  g_status[0] = 0;
}

static void load(void) {
  long fd = open(g_file, O_RDONLY);
  long got = 0;
  long n;
  long i;

  if (fd < 0) {
    set_status("New file ", g_file);
    return;
  }
  while ((n = read((int)fd, g_io + got, (unsigned long)(sizeof(g_io) - (unsigned long)got))) > 0) {
    got += n;
  }
  close((int)fd);
  for (i = 0; i < got && g_len < MAX; ++i) {
    unsigned char c = g_io[i];

    if (c == '\r') {
      continue;
    }
    if (c == '\n' || c == '\t' || (c >= 0x20 && c <= 0x7E)) {
      g_text[g_len++] = (c == '\t') ? ' ' : (char)c;
    }
  }
  g_text[g_len] = 0;
  if (got > MAX) {
    set_status("File truncated to 4096 bytes: do not save", 0);
  }
}

// Pick /NOTEn.TXT for the first n that does not exist yet.
static void choose_name(void) {
  int n;

  for (n = 1; n < 1000; ++n) {
    char num[8];
    int k = 0;
    int i = 0;
    int v = n;
    long fd;

    num[k++] = (char)('0' + v / 100 % 10);
    num[k++] = (char)('0' + v / 10 % 10);
    num[k++] = (char)('0' + v % 10);
    num[k] = 0;
    {
      const char *pre = "/NOTE";
      const char *post = ".TXT";
      int p = 0;

      for (i = 0; pre[i] != 0; ++i) {
        g_file[p++] = pre[i];
      }
      for (i = (n < 10 ? 2 : (n < 100 ? 1 : 0)); num[i] != 0; ++i) {
        g_file[p++] = num[i];
      }
      for (i = 0; post[i] != 0; ++i) {
        g_file[p++] = post[i];
      }
      g_file[p] = 0;
    }
    fd = open(g_file, O_RDONLY);
    if (fd < 0) {
      return;
    }
    close((int)fd);
  }
}

static void save(void) {
  long len = 0;
  long fd;
  long done = 0;
  int i;

  if (!g_has_file) {
    choose_name();
  }
  for (i = 0; i < g_len; ++i) {                    // text uses LF; files use CR LF
    if (g_text[i] == '\n') {
      g_io[len++] = '\r';
    }
    g_io[len++] = (unsigned char)g_text[i];
  }
  fd = open(g_file, O_WRONLY);
  if (fd < 0) {
    set_status("Save failed", 0);
    return;
  }
  while (done < len) {
    long n = write((int)fd, g_io + done, (unsigned long)(len - done));

    if (n <= 0) {
      break;
    }
    done += n;
  }
  if (done == len && close((int)fd) == 0) {
    g_has_file = 1;
    set_title();
    set_status("Saved ", g_file);
  } else {
    set_status("Save failed", 0);
  }
}

int main(void) {
  win_event_t ev;

  g_win = win_create("Notes", W, H);
  if (g_win < 0) {
    puts("notes: could not create a window\r\n");
    return 1;
  }
  win_resizable((int)g_win, 1);
  if (getarg(g_file, sizeof(g_file)) > 0) {
    g_has_file = 1;
    load();
    set_title();
  }
  set_cursor(g_len);                                // start at the end of the text
  redraw();

  for (;;) {
    long n = win_event((int)g_win, &ev, 1);
    int shift = (ev.x & MOD_SHIFT) != 0;
    int ctrl = (ev.x & MOD_CTRL) != 0;
    int a, b;

    if (n != 1 || ev.type == EV_CLOSE) {
      break;
    }
    if (ev.type == EV_RESIZE) {                     // the window was resized: new surface, redraw
      g_w = ev.x;
      g_h = ev.y;
      redraw();
      continue;
    }
    if (ev.type == EV_MOUSE_DOWN) {                 // click: place the cursor; drag selects
      set_cursor(pos_at(ev.x, ev.y));
      g_anchor = g_cur;
      g_status[0] = 0;
      redraw();
      continue;
    }
    if (ev.type == EV_MOUSE_MOVE) {
      if (g_anchor >= 0) {
        set_cursor(pos_at(ev.x, ev.y));
        redraw();
      }
      continue;
    }
    if (ev.type == EV_MOUSE_UP) {
      if (g_anchor == g_cur) {
        g_anchor = -1;
      }
      continue;
    }
    if (ev.type != EV_KEY) {
      continue;
    }
    shift = (ev.x & MOD_SHIFT) != 0;
    ctrl = (ev.x & MOD_CTRL) != 0;

    if (ev.ch == 0x13) {                            // Ctrl+S
      save();
    } else if (ev.ch == KEY_LEFT) {
      if (!shift && sel_range(&a, &b)) {
        g_anchor = -1;
        set_cursor(a);
      } else {
        begin_move(shift);
        set_cursor(g_cur - 1);
      }
    } else if (ev.ch == KEY_RIGHT) {
      if (!shift && sel_range(&a, &b)) {
        g_anchor = -1;
        set_cursor(b);
      } else {
        begin_move(shift);
        set_cursor(g_cur + 1);
      }
    } else if (ev.ch == KEY_UP) {
      begin_move(shift);
      move_row(-1);
    } else if (ev.ch == KEY_DOWN) {
      begin_move(shift);
      move_row(1);
    } else if (ev.ch == KEY_PGUP) {
      begin_move(shift);
      move_row(-(ROWS - 1));
    } else if (ev.ch == KEY_PGDN) {
      begin_move(shift);
      move_row(ROWS - 1);
    } else if (ev.ch == KEY_HOME) {
      begin_move(shift);
      build_rows();
      set_cursor(ctrl ? 0 : g_rs[row_of(g_cur)]);
    } else if (ev.ch == KEY_END) {
      int r;

      begin_move(shift);
      build_rows();
      r = row_of(g_cur);
      set_cursor(ctrl ? g_len : g_rs[r] + row_len(r));
    } else if (ev.ch == 0x01) {                     // Ctrl+A: select all
      g_anchor = 0;
      set_cursor(g_len);
    } else if (ev.ch == 0x03) {                     // Ctrl+C
      copy_selection();
    } else if (ev.ch == 0x18) {                     // Ctrl+X
      copy_selection();
      delete_selection();
    } else if (ev.ch == 0x16) {                     // Ctrl+V
      paste();
    } else if (ev.ch == '\b') {
      if (!delete_selection() && g_cur > 0) {
        delete_range(g_cur - 1, g_cur);
        set_cursor(g_cur - 1);
      }
      g_status[0] = 0;
    } else if (ev.ch == KEY_DELETE) {
      if (!delete_selection()) {
        delete_range(g_cur, g_cur + 1);
        set_cursor(g_cur);
      }
      g_status[0] = 0;
    } else if (ev.ch == '\n' || (ev.ch >= 0x20 && ev.ch <= 0x7E)) {
      delete_selection();
      insert_char((char)ev.ch);
      g_status[0] = 0;
    } else {
      continue;
    }
    redraw();
  }
  win_close((int)g_win);
  return 0;
}
