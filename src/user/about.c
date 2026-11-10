#include "ulib.h"

// About WinClone as a user program (ABOUT.BIN): the product name, the screen size and the
// time since boot, refreshed once a second. The refresh uses win_wait with a timeout, so
// the program sleeps until either a second has passed or the user closes the window.

#define W 470
#define H 190

#define COL_BG    0x00F4F5FA
#define COL_TITLE 0x002A5ABE
#define COL_TEXT  0x001E1E28

static long g_win;

// "<number><unit> " appended to out; returns the new length.
static int add_part(char *out, int n, unsigned long v, char unit) {
  n += ulib_utoa(v, out + n);
  out[n++] = unit;
  out[n] = 0;
  return n;
}

static void draw(void) {
  char line[48];
  char num[24];
  int win = (int)g_win;
  int sw = 0, sh = 0;
  unsigned long secs = (unsigned long)uptime_ms() / 1000UL;
  int n;

  win_fill(win, 0, 0, W, H, COL_BG);
  win_text(win, 16, 16, "WinClone", COL_TITLE, 3);
  win_text(win, 16, 56, "A from-scratch x86-64 UEFI operating system.", COL_TEXT, 1);

  screen_size(&sw, &sh);
  n = ulib_utoa((unsigned long)sw, line);
  line[n++] = 'x';
  ulib_utoa((unsigned long)sh, num);
  ulib_copy(line + n, num, (int)sizeof(line) - n);
  win_text(win, 16, 80, "Display:", COL_TEXT, 1);
  win_text(win, 16 + 9 * 8, 80, line, COL_TEXT, 1);
  win_text(win, 16, 96, "Input:   PS/2 keyboard and mouse (IRQ1/IRQ12)", COL_TEXT, 1);
  win_text(win, 16, 112, "Timer:   PIT at 1000 Hz, interrupt-driven", COL_TEXT, 1);

  n = add_part(line, 0, secs / 3600UL, 'h');
  line[n++] = ' ';
  n = add_part(line, n, (secs / 60UL) % 60UL, 'm');
  line[n++] = ' ';
  add_part(line, n, secs % 60UL, 's');
  win_text(win, 16, 128, "Uptime:", COL_TEXT, 1);
  win_text(win, 16 + 9 * 8, 128, line, COL_TEXT, 1);
  win_text(win, 16, 160, "Running as an ordinary user program (ring 3).", COL_TEXT, 1);
}

int main(void) {
  win_event_t ev;

  g_win = win_create("About WinClone", W, H);
  if (g_win < 0) {
    puts("about: could not create a window\r\n");
    return 1;
  }
  draw();
  for (;;) {
    long n = win_wait((int)g_win, &ev, 1000);          // an event, or a second passing

    if (n < 0 || (n == 1 && ev.type == EV_CLOSE)) {
      break;
    }
    if (n == 0) {
      draw();                                          // the uptime moved on
    }
  }
  win_close((int)g_win);
  return 0;
}
