#include "user.h"

// Resize test. Creates a resizable 200x120 window and expects, in order: EV_RESIZE 300x200,
// EV_RESIZE 800x600 (a drag beyond the limits is clamped), then the key 'q'. The kernel's
// self-test triggers the resizes and checks the surface size in between.
// Exit 0 = as expected, otherwise a number saying which step failed.

int main(void) {
  long id = win_create("Resize", 200, 120);
  win_event_t ev;

  if (id < 0) {
    return 1;
  }
  if (win_resizable((int)id, 1) != 0) {
    return 2;
  }
  win_fill((int)id, 0, 0, 200, 120, 0x00203040);
  if (win_event((int)id, &ev, 1) != 1 || ev.type != EV_RESIZE || ev.x != 300 || ev.y != 200) {
    return 3;
  }
  if (win_fill((int)id, 0, 0, 300, 200, 0x00306040) != 0) {
    return 4;
  }
  if (win_event((int)id, &ev, 1) != 1 || ev.type != EV_RESIZE || ev.x != 800 || ev.y != 600) {
    return 5;
  }
  if (win_text((int)id, 8, 8, "big", 0x00FFFFFF, 2) != 0) {
    return 6;
  }
  if (win_event((int)id, &ev, 1) != 1 || ev.type != EV_KEY || ev.ch != 'q') {
    return 7;
  }
  return win_close((int)id) == 0 ? 0 : 8;
}
