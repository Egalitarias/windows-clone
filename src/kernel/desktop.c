#include "desktop.h"
#include "fb.h"

// First on-screen milestone: a static desktop mock-up drawn with the fb
// primitives (wallpaper, desktop icon, window, taskbar). It is not interactive
// yet; it exists to prove pixels, rectangles and text work end to end.

#define COL_TASKBAR      FB_RGB(24, 28, 40)
#define COL_TASKBAR_EDGE FB_RGB(70, 80, 110)
#define COL_START        FB_RGB(40, 110, 220)
#define COL_WHITE        FB_RGB(255, 255, 255)
#define COL_TEXT         FB_RGB(30, 30, 30)
#define COL_WINDOW       FB_RGB(244, 245, 248)
#define COL_TITLE        FB_RGB(40, 90, 190)

static void draw_window(int x, int y, int w, int h, const char *title) {
  fb_fill_rect(x + 6, y + 6, w, h, FB_RGB(10, 20, 50));          // drop shadow
  fb_fill_rect(x, y, w, h, COL_WINDOW);
  fb_fill_rect(x, y, w, 28, COL_TITLE);
  fb_draw_rect(x, y, w, h, FB_RGB(20, 50, 120));
  fb_draw_text(x + 10, y + 10, title, COL_WHITE, 1);
  fb_fill_rect(x + w - 26, y + 6, 16, 16, FB_RGB(200, 60, 60));  // close button
  fb_draw_text(x + w - 22, y + 10, "x", COL_WHITE, 1);
}

void desktop_draw_demo(void) {
  int w, h, bar_h, win_x, win_y;

  if (!fb_available()) {
    return;
  }
  w = (int)fb_width();
  h = (int)fb_height();
  bar_h = 40;

  // Wallpaper
  fb_fill_gradient(0, 0, w, h - bar_h, FB_RGB(20, 70, 160), FB_RGB(90, 170, 230));

  // Desktop icon
  fb_fill_rect(24, 24, 40, 32, FB_RGB(230, 230, 235));
  fb_fill_rect(28, 28, 32, 22, FB_RGB(60, 120, 200));
  fb_draw_text(16, 64, "Computer", COL_WHITE, 1);

  // Window
  win_x = w / 2 - 220;
  win_y = (h - bar_h) / 2 - 110;
  if (win_x < 80) win_x = 80;
  if (win_y < 20) win_y = 20;
  draw_window(win_x, win_y, 440, 220, "WinClone");
  fb_draw_text(win_x + 20, win_y + 50, "Hello, framebuffer!", COL_TEXT, 2);
  fb_draw_text(win_x + 20, win_y + 100, "Pixels, rectangles, gradients and text", COL_TEXT, 1);
  fb_draw_text(win_x + 20, win_y + 116, "are now drawn by the kernel itself.", COL_TEXT, 1);
  fb_draw_text(win_x + 20, win_y + 148, "Next: keyboard, mouse, windows.", FB_RGB(90, 90, 100), 1);

  // Taskbar
  fb_fill_rect(0, h - bar_h, w, bar_h, COL_TASKBAR);
  fb_fill_rect(0, h - bar_h, w, 1, COL_TASKBAR_EDGE);
  fb_fill_rect(6, h - bar_h + 6, 88, bar_h - 12, COL_START);
  fb_draw_text(18, h - bar_h + 16, "Start", COL_WHITE, 1);
  fb_draw_text(w - 8 * 5 - 14, h - bar_h + 16, "12:00", COL_WHITE, 1);
}
