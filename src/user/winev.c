#include "user.h"

// Waits for input events from the kernel's self-test: expects the key 'x', then a
// mouse press at (5, 6), then a close request. Exit code 0 = as expected.

int main(void) {
  long id = win_create("Events", 160, 100);
  win_event_t ev;
  int stage = 0;

  if (id < 0) {
    return (int)(100 - id);
  }
  win_fill((int)id, 0, 0, 160, 100, 0x00004060);
  win_text((int)id, 8, 8, "waiting for events", 0x00FFFFFF, 1);
  for (;;) {
    long n = win_event((int)id, &ev, 1);               // blocks

    if (n != 1) {
      return 10;
    }
    if (stage == 0 && ev.type == EV_KEY && ev.ch == 'x') {
      stage = 1;
    } else if (stage == 1 && ev.type == EV_MOUSE_DOWN && ev.x == 5 && ev.y == 6) {
      stage = 2;
    } else if (stage == 2 && ev.type == EV_CLOSE) {
      stage = 3;
      break;
    } else {
      return 20 + stage;
    }
  }
  // After the close request the window is gone: more reads fail, and so does drawing.
  if (win_event((int)id, &ev, 1) != -9) {
    return 30;
  }
  if (win_fill((int)id, 0, 0, 5, 5, 0) != -9) {
    return 31;
  }
  return win_close((int)id) == 0 ? 0 : 32;
}
