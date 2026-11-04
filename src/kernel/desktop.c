#include "desktop.h"
#include "apps.h"
#include "fb.h"
#include "ps2.h"
#include "wm.h"

// The shell: wallpaper, desktop icons, taskbar (Start button plus one button
// per window), Start menu and the mouse cursor. Windows themselves belong to
// the window manager (wm.c). Painting is dirty-rectangle based: anything that
// changes calls wm_invalidate(); the main loop repaints only that region with
// the fb clip set to it, back to front.
//
// NOTE: the kernel is a PIE and the loader applies no relocations, so never
// build static tables of pointers or function pointers. Use flat arrays or code.

#define COL_TASKBAR      FB_RGB(24, 28, 40)
#define COL_TASKBAR_EDGE FB_RGB(70, 80, 110)
#define COL_START        FB_RGB(40, 110, 220)
#define COL_START_HOT    FB_RGB(70, 140, 245)
#define COL_WHITE        FB_RGB(255, 255, 255)
#define COL_MENU         FB_RGB(36, 42, 60)
#define COL_MENU_HOT     FB_RGB(60, 100, 190)
#define COL_TASK_BTN     FB_RGB(55, 65, 95)
#define COL_TASK_ACTIVE  FB_RGB(80, 105, 170)

#define TASKBAR_H    40
#define START_X      6
#define START_W      88
#define TASK_X0      106
#define TASK_W       128
#define TASK_PITCH   132
#define MENU_W       200
#define MENU_ITEM_H  28
#define MENU_COUNT   4
#define ICON_X       24
#define ICON_Y0      24
#define ICON_PITCH   84
#define ICON_COUNT   3

static int g_menu_open = 0;
static int g_menu_hover = -1;
static int g_mouse_x = 0, g_mouse_y = 0;
static unsigned int g_buttons = 0;

// ---- geometry --------------------------------------------------------------

static int in_rect(int px, int py, int x, int y, int w, int h) {
  return px >= x && py >= y && px < x + w && py < y + h;
}

static int bar_y(void) { return (int)fb_height() - TASKBAR_H; }
static int menu_x(void) { return START_X; }
static int menu_y(void) { return bar_y() - MENU_COUNT * MENU_ITEM_H - 4; }

static const char *menu_label(int i) {
  switch (i) {
    case 0:  return "Notes";
    case 1:  return "Paint";
    case 2:  return "About WinClone";
    default: return "Shut down";
  }
}

static const char *icon_label(int i) {
  return i == 0 ? "Notes" : (i == 1 ? "Paint" : "About");
}

static uint32_t icon_color(int i) {
  return i == 0 ? FB_RGB(240, 210, 80) : (i == 1 ? FB_RGB(220, 90, 90) : FB_RGB(90, 190, 130));
}

// ---- painting --------------------------------------------------------------

static void draw_wallpaper(void) {
  int i;

  fb_fill_gradient(0, 0, (int)fb_width(), (int)fb_height() - TASKBAR_H,
                   FB_RGB(20, 70, 160), FB_RGB(90, 170, 230));
  for (i = 0; i < ICON_COUNT; i++) {
    int y = ICON_Y0 + i * ICON_PITCH;
    fb_fill_rect(ICON_X, y, 40, 36, FB_RGB(235, 236, 242));
    fb_fill_rect(ICON_X + 4, y + 4, 32, 28, icon_color(i));
    fb_draw_text(ICON_X + 20 - fb_text_width(icon_label(i), 1) / 2, y + 44, icon_label(i), COL_WHITE, 1);
  }
}

static void draw_taskbar(void) {
  int w = (int)fb_width(), y = bar_y(), i, slot_x = TASK_X0;
  wm_window_t *top = wm_focused();

  fb_fill_rect(0, y, w, TASKBAR_H, COL_TASKBAR);
  fb_fill_rect(0, y, w, 1, COL_TASKBAR_EDGE);
  fb_fill_rect(START_X, y + 6, START_W, TASKBAR_H - 12, g_menu_open ? COL_START_HOT : COL_START);
  fb_draw_text(START_X + 12, y + 16, "Start", COL_WHITE, 1);

  for (i = 0; i < WM_MAX_WINDOWS; i++) {
    wm_window_t *win = wm_slot(i);
    char label[17];
    int n;

    if (!win) {
      continue;
    }
    fb_fill_rect(slot_x, y + 6, TASK_W, TASKBAR_H - 12,
                 (win == top) ? COL_TASK_ACTIVE : COL_TASK_BTN);
    if (win->minimized) {
      fb_fill_rect(slot_x, y + TASKBAR_H - 9, TASK_W, 3, COL_TASKBAR_EDGE);
    }
    for (n = 0; n < 15 && win->title[n]; n++) {
      label[n] = win->title[n];
    }
    label[n] = 0;
    fb_draw_text(slot_x + 10, y + 16, label, COL_WHITE, 1);
    slot_x += TASK_PITCH;
  }
  fb_draw_text(w - 8 * 5 - 14, y + 16, "12:00", COL_WHITE, 1);
}

static void draw_menu(void) {
  int i, x = menu_x(), y = menu_y();

  fb_fill_rect(x, y, MENU_W, MENU_COUNT * MENU_ITEM_H, COL_MENU);
  fb_draw_rect(x, y, MENU_W, MENU_COUNT * MENU_ITEM_H, COL_TASKBAR_EDGE);
  for (i = 0; i < MENU_COUNT; i++) {
    int iy = y + i * MENU_ITEM_H;
    if (i == g_menu_hover) {
      fb_fill_rect(x + 2, iy + 2, MENU_W - 4, MENU_ITEM_H - 4, COL_MENU_HOT);
    }
    fb_draw_text(x + 16, iy + 10, menu_label(i), COL_WHITE, 1);
  }
}

static void paint_scene(void) {
  draw_wallpaper();
  wm_paint_windows();
  draw_taskbar();
  if (g_menu_open) {
    draw_menu();
  }
}

// ---- cursor ----------------------------------------------------------------

#define CUR_W 12
#define CUR_H 19

static const char kCursor[CUR_H][CUR_W + 1] = {
  "X           ", "XX          ", "X.X         ", "X..X        ",
  "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
  "X.......X   ", "X........X  ", "X.........X ", "X......XXXXX",
  "X...X..X    ", "X..X X..X   ", "X.X  X..X   ", "XX    X..X  ",
  "X     X..X  ", "       X..X ", "       XXX  "
};

static uint32_t g_under[CUR_W * CUR_H];
static int g_cur_x, g_cur_y, g_cur_drawn = 0;

static void cursor_save_and_draw(void) {
  int r, c;
  g_cur_x = g_mouse_x;
  g_cur_y = g_mouse_y;
  for (r = 0; r < CUR_H; r++) {
    for (c = 0; c < CUR_W; c++) {
      g_under[r * CUR_W + c] = fb_get_pixel(g_cur_x + c, g_cur_y + r);
      if (kCursor[r][c] == 'X') {
        fb_put_pixel(g_cur_x + c, g_cur_y + r, FB_RGB(0, 0, 0));
      } else if (kCursor[r][c] == '.') {
        fb_put_pixel(g_cur_x + c, g_cur_y + r, FB_RGB(255, 255, 255));
      }
    }
  }
  g_cur_drawn = 1;
}

static void cursor_restore(void) {
  int r, c;
  if (!g_cur_drawn) {
    return;
  }
  for (r = 0; r < CUR_H; r++) {
    for (c = 0; c < CUR_W; c++) {
      if (kCursor[r][c] != ' ') {
        fb_put_pixel(g_cur_x + c, g_cur_y + r, g_under[r * CUR_W + c]);
      }
    }
  }
  g_cur_drawn = 0;
}

// ---- actions ---------------------------------------------------------------

static __attribute__((noreturn)) void shut_down(void) {
  fb_reset_clip();
  fb_clear(FB_RGB(0, 0, 0));
  fb_draw_text(((int)fb_width() - fb_text_width("It is now safe to turn off your computer.", 2)) / 2,
               (int)fb_height() / 2 - 8, "It is now safe to turn off your computer.",
               FB_RGB(255, 160, 40), 2);
  __asm__ volatile("cli");
  for (;;) {
    __asm__ volatile("hlt");
  }
}

static void launch(int which) {
  if (which == 0) {
    app_notes_open();
  } else if (which == 1) {
    app_paint_open();
  } else {
    app_about_open();
  }
}

static void close_menu(void) {
  if (g_menu_open) {
    g_menu_open = 0;
    g_menu_hover = -1;
    wm_invalidate(menu_x(), menu_y(), MENU_W + 1, MENU_COUNT * MENU_ITEM_H + 4);
    wm_invalidate(0, bar_y(), (int)fb_width(), TASKBAR_H);
  }
}

static void toggle_menu(void) {
  if (g_menu_open) {
    close_menu();
    return;
  }
  g_menu_open = 1;
  g_menu_hover = -1;
  wm_invalidate(menu_x(), menu_y(), MENU_W + 1, MENU_COUNT * MENU_ITEM_H + 4);
  wm_invalidate(0, bar_y(), (int)fb_width(), TASKBAR_H);
}

static void taskbar_click(int x) {
  int i, slot_x = TASK_X0;
  wm_window_t *top = wm_focused();

  for (i = 0; i < WM_MAX_WINDOWS; i++) {
    wm_window_t *win = wm_slot(i);
    if (!win) {
      continue;
    }
    if (x >= slot_x && x < slot_x + TASK_W) {
      if (win->minimized) {
        wm_restore(win);
      } else if (win == top) {
        wm_minimize(win);
      } else {
        if (top) {
          wm_invalidate_window(top);
        }
        wm_raise(win);
        wm_invalidate_window(win);
      }
      return;
    }
    slot_x += TASK_PITCH;
  }
}

static void handle_click(int x, int y) {
  int i;

  // Start button
  if (in_rect(x, y, START_X, bar_y() + 6, START_W, TASKBAR_H - 12)) {
    toggle_menu();
    return;
  }
  // Menu items
  if (g_menu_open) {
    for (i = 0; i < MENU_COUNT; i++) {
      if (in_rect(x, y, menu_x(), menu_y() + i * MENU_ITEM_H, MENU_W, MENU_ITEM_H)) {
        close_menu();
        if (i == MENU_COUNT - 1) {
          shut_down();
        }
        launch(i);
        return;
      }
    }
    close_menu();     // click elsewhere closes the menu and then acts normally
  }
  // Taskbar
  if (y >= bar_y()) {
    taskbar_click(x);
    return;
  }
  // Windows
  if (wm_mouse_down(x, y)) {
    return;
  }
  // Desktop icons
  for (i = 0; i < ICON_COUNT; i++) {
    if (in_rect(x, y, ICON_X - 8, ICON_Y0 + i * ICON_PITCH, 56, 60)) {
      launch(i);
      return;
    }
  }
}

static void clamp_pointer(void) {
  int w = (int)fb_width(), h = (int)fb_height();
  if (g_mouse_x < 0) g_mouse_x = 0;
  if (g_mouse_y < 0) g_mouse_y = 0;
  if (g_mouse_x > w - 1) g_mouse_x = w - 1;
  if (g_mouse_y > h - 1) g_mouse_y = h - 1;
}

static void update_menu_hover(void) {
  int i, hover = -1;
  if (!g_menu_open) {
    return;
  }
  for (i = 0; i < MENU_COUNT; i++) {
    if (in_rect(g_mouse_x, g_mouse_y, menu_x(), menu_y() + i * MENU_ITEM_H, MENU_W, MENU_ITEM_H)) {
      hover = i;
    }
  }
  if (hover != g_menu_hover) {
    g_menu_hover = hover;
    wm_invalidate(menu_x(), menu_y(), MENU_W + 1, MENU_COUNT * MENU_ITEM_H + 4);
  }
}

// ---- main loop -------------------------------------------------------------

static __attribute__((noreturn)) void desktop_run(void) {
  ps2_event_t ev;
  int moved = 0;
  int dx, dy, dw, dh;

  wm_init((int)fb_width(), (int)fb_height(), TASKBAR_H);
  g_mouse_x = (int)fb_width() / 2;
  g_mouse_y = (int)fb_height() / 2;
  ps2_init();

  app_notes_open();
  app_about_open();

  for (;;) {
    while (ps2_poll(&ev)) {
      if (ev.type == PS2_EVENT_KEY) {
        if (ev.pressed && ev.ch == 27) {
          close_menu();
        } else if (ev.pressed && ev.ch != 0) {
          wm_key(ev.ch);
        }
      } else if (ev.type == PS2_EVENT_MOUSE) {
        unsigned int pressed_now = ev.buttons & ~g_buttons;
        unsigned int released_now = g_buttons & ~ev.buttons;

        g_mouse_x += ev.dx;
        g_mouse_y += ev.dy;
        clamp_pointer();
        if (ev.dx != 0 || ev.dy != 0) {
          wm_mouse_move(g_mouse_x, g_mouse_y, ev.buttons);
          update_menu_hover();
          moved = 1;
        }
        if (pressed_now & PS2_MOUSE_LEFT) {
          handle_click(g_mouse_x, g_mouse_y);
        }
        if (released_now & PS2_MOUSE_LEFT) {
          wm_mouse_up(g_mouse_x, g_mouse_y);
        }
        g_buttons = ev.buttons;
      }
    }

    if (wm_take_dirty(&dx, &dy, &dw, &dh)) {
      cursor_restore();
      fb_set_clip(dx, dy, dw, dh);
      paint_scene();
      fb_reset_clip();
      cursor_save_and_draw();
      moved = 0;
    } else if (moved) {
      cursor_restore();
      cursor_save_and_draw();
      moved = 0;
    }
    __asm__ volatile("pause");
  }
}

// ---- entry from the scheduler ----------------------------------------------

// The scheduler idle callback runs on a small task stack, so the desktop gets
// its own stack. Interrupts stay off: input is polled.
static uint8_t g_desktop_stack[64 * 1024] __attribute__((aligned(16)));

void desktop_draw_demo(void) {
  uintptr_t top;

  if (!fb_available()) {
    return;
  }
  __asm__ volatile("cli");
  top = (uintptr_t)(g_desktop_stack + sizeof(g_desktop_stack));
  __asm__ volatile(
      "mov %0, %%rsp\n\t"
      "xor %%ebp, %%ebp\n\t"
      "call *%1\n\t"
      :
      : "r"(top), "r"(desktop_run)
      : "memory");
  __builtin_unreachable();
}
