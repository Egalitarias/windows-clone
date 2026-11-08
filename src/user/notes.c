#include "user.h"

// Notes as a real user program: a text editor in its own window.
//   - type to add text, Backspace deletes, Ctrl+S saves, closing the window quits
//   - started with an argument (a file path) it opens that file and saves back to
//     it; without one it saves to the first free /NOTEn.TXT
// It uses only the window syscalls (win_*) and the file syscalls (open/read/write/close).

#define W        480
#define H        300
#define MAX      4096
#define STATUS_H 16
#define ROW_H    10
#define COLS     ((W - 16) / 8)
#define ROWS     ((H - STATUS_H - 12) / ROW_H)

#define COL_PAPER  0x00FFFFFF
#define COL_TEXT   0x001E1E1E
#define COL_CURSOR 0x002850BE
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

// Lay the text out in rows of COLS characters. With draw set, rows from `skip`
// on are drawn. Returns the number of rows; *cur_row / *cur_col get the cursor cell.
static int layout(int skip, int draw, int *cur_row, int *cur_col) {
  char line[COLS + 1];
  int row = 0;
  int col = 0;
  int i;

#define FLUSH_ROW()                                                              \
  do {                                                                           \
    if (draw && row >= skip && row - skip < ROWS && col > 0) {                   \
      line[col] = 0;                                                             \
      win_text((int)g_win, 8, 6 + (row - skip) * ROW_H, line, COL_TEXT, 1);      \
    }                                                                            \
  } while (0)

  for (i = 0; i < g_len; ++i) {
    char c = g_text[i];

    if (c == '\n') {
      FLUSH_ROW();
      ++row;
      col = 0;
      continue;
    }
    if (col >= COLS) {
      FLUSH_ROW();
      ++row;
      col = 0;
    }
    line[col++] = c;
  }
  FLUSH_ROW();
#undef FLUSH_ROW
  if (col >= COLS) {
    ++row;
    col = 0;
  }
  *cur_row = row;
  *cur_col = col;
  return row + 1;
}

static void redraw(void) {
  int cur_row, cur_col, total, skip;

  total = layout(0, 0, &cur_row, &cur_col);
  skip = total > ROWS ? total - ROWS : 0;          // keep the cursor row visible
  win_fill((int)g_win, 0, 0, W, H - STATUS_H, COL_PAPER);
  layout(skip, 1, &cur_row, &cur_col);
  win_text((int)g_win, 8 + cur_col * 8, 6 + (cur_row - skip) * ROW_H, "_", COL_CURSOR, 1);
  win_fill((int)g_win, 0, H - STATUS_H, W, STATUS_H, COL_BAR);
  win_text((int)g_win, 6, H - STATUS_H + 4, g_status[0] ? g_status : "Ctrl+S saves", COL_BARTXT, 1);
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
  if (getarg(g_file, sizeof(g_file)) > 0) {
    g_has_file = 1;
    load();
    set_title();
  }
  redraw();

  for (;;) {
    long n = win_event((int)g_win, &ev, 1);

    if (n != 1 || ev.type == EV_CLOSE) {
      break;
    }
    if (ev.type != EV_KEY) {
      continue;
    }
    if (ev.ch == 0x13) {                            // Ctrl+S
      save();
    } else if (ev.ch == '\b') {
      if (g_len > 0) {
        g_text[--g_len] = 0;
      }
      g_status[0] = 0;
    } else if ((ev.ch == '\n' || (ev.ch >= 0x20 && ev.ch <= 0x7E)) && g_len < MAX) {
      g_text[g_len++] = (char)ev.ch;
      g_text[g_len] = 0;
      g_status[0] = 0;
    } else {
      continue;
    }
    redraw();
  }
  win_close((int)g_win);
  return 0;
}
