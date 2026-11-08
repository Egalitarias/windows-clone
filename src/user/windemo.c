#include "user.h"

// A small interactive window: drag the mouse to scribble, type to add text,
// Backspace deletes. Close the window to quit.

#define W 320
#define H 220

int main(void) {
  long id = win_create("Window demo", W, H);
  win_event_t ev;
  char typed[40];
  int len = 0;
  int drawing = 0;

  if (id < 0) {
    puts("windemo: could not create a window\r\n");
    return 1;
  }
  typed[0] = 0;
  win_fill((int)id, 0, 0, W, H, 0x00F4F4F8);
  win_text((int)id, 8, 8, "Drag to draw. Type below.", 0x00303050, 1);
  win_fill((int)id, 0, H - 24, W, 24, 0x00DDDDE8);

  for (;;) {
    long n = win_event((int)id, &ev, 1);

    if (n != 1 || ev.type == EV_CLOSE) {
      break;
    }
    if (ev.type == EV_MOUSE_DOWN) {
      drawing = 1;
    } else if (ev.type == EV_MOUSE_UP) {
      drawing = 0;
    }
    if ((ev.type == EV_MOUSE_DOWN || ev.type == EV_MOUSE_MOVE) && drawing && ev.y > 20 && ev.y < H - 26) {
      win_fill((int)id, ev.x - 2, ev.y - 2, 5, 5, 0x00C03030);
    } else if (ev.type == EV_KEY) {
      if (ev.ch == '\b') {
        if (len > 0) {
          typed[--len] = 0;
        }
      } else if (ev.ch >= 0x20 && ev.ch < 0x7F && len < (int)sizeof(typed) - 1) {
        typed[len++] = (char)ev.ch;
        typed[len] = 0;
      }
      win_fill((int)id, 0, H - 24, W, 24, 0x00DDDDE8);
      win_text((int)id, 8, H - 16, typed, 0x00202040, 1);
    }
  }
  win_close((int)id);
  return 0;
}
