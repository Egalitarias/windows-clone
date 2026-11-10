#include "user.h"

// Checks the window syscalls without needing input. Exit code 0 = all passed,
// otherwise the number of the first failed check. Leaves two windows open on
// purpose: the kernel must release them when the program exits.

static int g_failed = 0;

static void check(int ok, int number, const char *what) {
  if (!ok) {
    puts("wintest: FAIL ");
    put_uint((unsigned long)number);
    puts(" ");
    puts(what);
    puts("\r\n");
    if (g_failed == 0) {
      g_failed = number;
    }
  }
}

static uint32_t g_pixels[64 * 48];

int main(void) {
  long a, b;
  int x, y;
  win_event_t ev;

  // Bad requests first.
  check(win_create("tiny", 4, 4) == -22, 1, "too small is EINVAL");
  check(win_create("huge", 5000, 100) == -22, 2, "too large is EINVAL");
  check(win_create((const char *)1, 100, 100) == -14, 3, "bad title pointer is EFAULT");
  check(win_fill(0, 0, 0, 10, 10, 0) == -9, 4, "no window yet is EBADF");

  a = win_create("Window test", 200, 120);
  check(a >= 0, 5, "create");
  b = win_create("Second", 100, 80);
  check(b >= 0 && b != a, 6, "second window");
  check(win_create("Third", 100, 80) == -24, 7, "per-program limit is EMFILE");

  check(win_fill((int)a, 0, 0, 200, 120, 0x00203040) == 0, 8, "fill");
  check(win_fill((int)a, -50, -50, 100, 100, 0x00FF0000) == 0, 9, "fill is clipped, not rejected");
  check(win_fill((int)a, 500, 500, 10, 10, 0x00FF0000) == 0, 10, "fill outside is a no-op");
  for (y = 0; y < 48; ++y) {
    for (x = 0; x < 64; ++x) {
      g_pixels[y * 64 + x] = (uint32_t)((x * 4) << 16 | (y * 5) << 8 | 0x80);
    }
  }
  check(win_blit((int)a, 10, 10, 64, 48, g_pixels) == 0, 11, "blit");
  check(win_blit((int)a, 190, 110, 64, 48, g_pixels) == 0, 12, "blit partly outside is clipped");
  check(win_blit((int)a, 0, 0, 64, 48, (const uint32_t *)1) == -14, 13, "bad pixel pointer is EFAULT");
  check(win_blit((int)a, 0, 0, 5000, 5000, g_pixels) == -22, 14, "oversized blit is EINVAL");
  check(win_text((int)a, 8, 70, "Hello from ring 3", 0x00FFFFFF, 1) == 0, 15, "text");
  check(win_text((int)a, 8, 90, "Big", 0x0000FF00, 3) == 0, 16, "scaled text");
  check(win_text((int)a, 0, 0, (const char *)1, 0, 1) == -14, 17, "bad text pointer is EFAULT");

  // Move, resize and timed wait.
  check(win_move((int)a, 120, 90) == 0, 25, "move");
  check(win_move((int)a, -100000, 5) == 0, 26, "move far off screen is clamped, not an error");
  check(win_move((int)a, 120, 90) == 0, 27, "move back");
  check(win_move(3, 0, 0) == -9, 28, "move of a foreign window is EBADF");
  check(win_resize((int)a, 4, 4) == -22, 29, "resize too small is EINVAL");
  check(win_resize((int)a, 5000, 100) == -22, 30, "resize too large is EINVAL");
  check(win_resize(3, 100, 100) == -9, 31, "resize of a foreign window is EBADF");
  check(win_resize((int)a, 240, 160) == 0, 32, "resize");
  check(win_fill((int)a, 0, 0, 240, 160, 0x00304050) == 0, 33, "fill the whole new surface");
  check(win_text((int)a, 8, 140, "resized", 0x00FFFFFF, 1) == 0, 34, "text near the new bottom edge");
  check(win_resize((int)a, 200, 120) == 0, 35, "resize back");
  check(win_fill((int)a, 0, 0, 200, 120, 0x00203040) == 0, 36, "fill after shrinking");
  check(win_wait((int)a, &ev, 0) == 0, 37, "win_wait with 0 ms polls");
  check(win_wait((int)a, &ev, 40) == 0, 38, "win_wait times out");
  check(win_wait((int)a, &ev, 40) == 0, 39, "win_wait times out again");
  check(win_wait((int)a, (win_event_t *)1, 40) == -14, 40, "win_wait bad pointer is EFAULT");
  check(win_wait(3, &ev, 40) == -9, 41, "win_wait on a foreign window is EBADF");

  check(win_event((int)a, &ev, 0) == 0, 18, "no event pending");
  check(win_event((int)a, (win_event_t *)1, 0) == -14, 19, "bad event pointer is EFAULT");
  check(win_event(3, &ev, 0) == -9, 20, "foreign window id is EBADF");

  check(win_close((int)b) == 0, 21, "close");
  check(win_close((int)b) == -9, 22, "second close is EBADF");
  check(win_fill((int)b, 0, 0, 5, 5, 0) == -9, 23, "drawing in a closed window is EBADF");
  {
    long c = win_create("Again", 100, 80);          // the closed slot becomes free soon
    int tries = 0;
    while (c < 0 && tries++ < 200) {
      sleep_ms(10);
      c = win_create("Again", 100, 80);
    }
    check(c >= 0, 24, "a closed window's slot can be reused");
  }

  if (g_failed != 0) {
    return g_failed;
  }
  puts("wintest: all checks passed\r\n");
  return 0;                                          // two windows are still open
}
