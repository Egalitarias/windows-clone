#include "desktop.h"
#include "fb.h"
#include "ps2.h"

// Interactive demo desktop: wallpaper, a desktop icon, one draggable window
// with a text area, a Start menu and a mouse cursor. The whole scene is
// repainted whenever something changes; the cursor is composited separately
// with a save-under buffer so plain pointer movement stays cheap.

#define COL_TASKBAR      FB_RGB(24, 28, 40)
#define COL_TASKBAR_EDGE FB_RGB(70, 80, 110)
#define COL_START        FB_RGB(40, 110, 220)
#define COL_START_HOT    FB_RGB(70, 140, 245)
#define COL_WHITE        FB_RGB(255, 255, 255)
#define COL_TEXT         FB_RGB(30, 30, 30)
#define COL_WINDOW       FB_RGB(244, 245, 248)
#define COL_TITLE        FB_RGB(40, 90, 190)
#define COL_MENU         FB_RGB(36, 42, 60)
#define COL_MENU_HOT     FB_RGB(60, 100, 190)

#define TASKBAR_H   40
#define START_X     6
#define START_W     88
#define MENU_W      200
#define MENU_ITEM_H 28
#define WIN_W       440
#define WIN_H       220
#define TITLE_H     28
#define TEXT_COLS   48
#define TEXT_ROWS   5
#define TEXT_MAX    (TEXT_COLS * TEXT_ROWS)

// NOTE: the kernel is a PIE and the loader applies no relocations, so never
// build static tables of pointers (e.g. `const char *names[]`): the stored
// addresses would be unrelocated. Use flat arrays or code instead.
#define MENU_COUNT 2

static const char *menu_label(int i) {
  return i == 0 ? "Notes" : "Shut down";
}

typedef struct {
  int win_x, win_y;
  int win_visible;
  int dragging;
  int drag_dx, drag_dy;
  int menu_open;
  int mouse_x, mouse_y;
  unsigned int buttons;
  char text[TEXT_MAX + 1];
  int text_len;
  int keys_typed;
} DesktopState;

static DesktopState g;

// ---- small helpers -----------------------------------------------------------

static int fmt_int(char *out, int value) {
  char tmp[12];
  int n = 0, len = 0;
  if (value < 0) {
    out[len++] = '-';
    value = -value;
  }
  if (value == 0) {
    tmp[n++] = '0';
  }
  while (value > 0) {
    tmp[n++] = (char)('0' + value % 10);
    value /= 10;
  }
  while (n > 0) {
    out[len++] = tmp[--n];
  }
  out[len] = 0;
  return len;
}

static int in_rect(int px, int py, int x, int y, int w, int h) {
  return px >= x && py >= y && px < x + w && py < y + h;
}

static int bar_y(void) { return (int)fb_height() - TASKBAR_H; }
static int menu_x(void) { return START_X; }
static int menu_y(void) { return bar_y() - MENU_COUNT * MENU_ITEM_H - 4; }

// ---- scene -------------------------------------------------------------------

static void draw_window(void) {
  int x = g.win_x, y = g.win_y, i, row, col;

  fb_fill_rect(x + 6, y + 6, WIN_W, WIN_H, FB_RGB(10, 20, 50));   // shadow
  fb_fill_rect(x, y, WIN_W, WIN_H, COL_WINDOW);
  fb_fill_rect(x, y, WIN_W, TITLE_H, COL_TITLE);
  fb_draw_rect(x, y, WIN_W, WIN_H, FB_RGB(20, 50, 120));
  fb_draw_text(x + 10, y + 10, "Notes", COL_WHITE, 1);
  fb_fill_rect(x + WIN_W - 26, y + 6, 16, 16, FB_RGB(200, 60, 60));
  fb_draw_text(x + WIN_W - 22, y + 10, "x", COL_WHITE, 1);

  fb_draw_text(x + 20, y + 44, "Hello, framebuffer!", COL_TEXT, 2);
  fb_draw_text(x + 20, y + 76, "Click and drag the title bar. Type below:", FB_RGB(90, 90, 100), 1);

  fb_fill_rect(x + 20, y + 94, WIN_W - 40, WIN_H - 114, FB_RGB(255, 255, 255));
  fb_draw_rect(x + 20, y + 94, WIN_W - 40, WIN_H - 114, FB_RGB(150, 150, 160));

  row = 0;
  col = 0;
  for (i = 0; i < g.text_len; i++) {
    if (g.text[i] == '\n' || col >= TEXT_COLS) {
      row++;
      col = 0;
      if (g.text[i] == '\n') {
        continue;
      }
    }
    if (row >= TEXT_ROWS) {
      break;
    }
    fb_draw_char(x + 26 + col * 8, y + 100 + row * 12, g.text[i], COL_TEXT, 1);
    col++;
  }
  if (row < TEXT_ROWS) {
    fb_draw_char(x + 26 + col * 8, y + 100 + row * 12, '_', COL_TITLE, 1);   // caret
  }
}

static void draw_taskbar(void) {
  int w = (int)fb_width(), y = bar_y();
  char buf[40];
  int n;

  fb_fill_rect(0, y, w, TASKBAR_H, COL_TASKBAR);
  fb_fill_rect(0, y, w, 1, COL_TASKBAR_EDGE);
  fb_fill_rect(START_X, y + 6, START_W, TASKBAR_H - 12,
               g.menu_open ? COL_START_HOT : COL_START);
  fb_draw_text(START_X + 12, y + 16, "Start", COL_WHITE, 1);

  if (g.win_visible) {
    fb_fill_rect(106, y + 6, 100, TASKBAR_H - 12, FB_RGB(55, 65, 95));
    fb_draw_text(118, y + 16, "Notes", COL_WHITE, 1);
  }

  // Pointer position readout, handy while input is being brought up.
  n = 0;
  buf[n++] = 'x';
  buf[n++] = ':';
  n += fmt_int(buf + n, g.mouse_x);
  buf[n++] = ' ';
  buf[n++] = 'y';
  buf[n++] = ':';
  n += fmt_int(buf + n, g.mouse_y);
  buf[n] = 0;
  fb_draw_text(w - 8 * 24, y + 16, buf, FB_RGB(170, 180, 205), 1);
  fb_draw_text(w - 8 * 5 - 14, y + 16, "12:00", COL_WHITE, 1);
}

static void draw_menu(void) {
  int i, x = menu_x(), y = menu_y();

  fb_fill_rect(x, y, MENU_W, MENU_COUNT * MENU_ITEM_H, COL_MENU);
  fb_draw_rect(x, y, MENU_W, MENU_COUNT * MENU_ITEM_H, COL_TASKBAR_EDGE);
  for (i = 0; i < MENU_COUNT; i++) {
    int iy = y + i * MENU_ITEM_H;
    if (in_rect(g.mouse_x, g.mouse_y, x, iy, MENU_W, MENU_ITEM_H)) {
      fb_fill_rect(x + 2, iy + 2, MENU_W - 4, MENU_ITEM_H - 4, COL_MENU_HOT);
    }
    fb_draw_text(x + 16, iy + 10, menu_label(i), COL_WHITE, 1);
  }
}

static void draw_scene(void) {
  int w = (int)fb_width(), h = (int)fb_height();

  fb_fill_gradient(0, 0, w, h - TASKBAR_H, FB_RGB(20, 70, 160), FB_RGB(90, 170, 230));

  fb_fill_rect(24, 24, 40, 32, FB_RGB(230, 230, 235));
  fb_fill_rect(28, 28, 32, 22, FB_RGB(60, 120, 200));
  fb_draw_text(16, 64, "Notes", COL_WHITE, 1);

  if (g.win_visible) {
    draw_window();
  }
  draw_taskbar();
  if (g.menu_open) {
    draw_menu();
  }
}

// ---- cursor ------------------------------------------------------------------

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
  g_cur_x = g.mouse_x;
  g_cur_y = g.mouse_y;
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

// ---- input handling ----------------------------------------------------------

static void clamp_state(void) {
  int w = (int)fb_width(), h = (int)fb_height();
  if (g.mouse_x < 0) g.mouse_x = 0;
  if (g.mouse_y < 0) g.mouse_y = 0;
  if (g.mouse_x > w - 1) g.mouse_x = w - 1;
  if (g.mouse_y > h - 1) g.mouse_y = h - 1;
  if (g.win_x < 0) g.win_x = 0;
  if (g.win_y < 0) g.win_y = 0;
  if (g.win_x > w - 60) g.win_x = w - 60;
  if (g.win_y > h - TASKBAR_H - TITLE_H) g.win_y = h - TASKBAR_H - TITLE_H;
}

static __attribute__((noreturn)) void shut_down(void) {
  fb_clear(FB_RGB(0, 0, 0));
  fb_draw_text(((int)fb_width() - fb_text_width("It is now safe to turn off your computer.", 2)) / 2,
               (int)fb_height() / 2 - 8, "It is now safe to turn off your computer.",
               FB_RGB(255, 160, 40), 2);
  __asm__ volatile("cli");
  for (;;) {
    __asm__ volatile("hlt");
  }
}

// Returns 1 if the scene needs repainting.
static int handle_click(int x, int y) {
  int i;

  if (g.menu_open) {
    for (i = 0; i < MENU_COUNT; i++) {
      if (in_rect(x, y, menu_x(), menu_y() + i * MENU_ITEM_H, MENU_W, MENU_ITEM_H)) {
        g.menu_open = 0;
        if (i == 0) {
          g.win_visible = 1;
        } else {
          shut_down();
        }
        return 1;
      }
    }
  }
  if (in_rect(x, y, START_X, bar_y() + 6, START_W, TASKBAR_H - 12)) {
    g.menu_open = !g.menu_open;
    return 1;
  }
  if (g.menu_open) {
    g.menu_open = 0;
    return 1;
  }
  if (g.win_visible && in_rect(x, y, g.win_x + WIN_W - 26, g.win_y + 6, 16, 16)) {
    g.win_visible = 0;
    return 1;
  }
  if (g.win_visible && in_rect(x, y, g.win_x, g.win_y, WIN_W, TITLE_H)) {
    g.dragging = 1;
    g.drag_dx = x - g.win_x;
    g.drag_dy = y - g.win_y;
    return 0;
  }
  if (in_rect(x, y, 16, 24, 56, 56)) {     // desktop icon
    g.win_visible = 1;
    return 1;
  }
  return 0;
}

static void handle_key(char ch) {
  if (!g.win_visible || ch == 0) {
    return;
  }
  g.keys_typed++;
  if (ch == '\b') {
    if (g.text_len > 0) {
      g.text_len--;
      g.text[g.text_len] = 0;
    }
  } else if (ch == '\n' || (ch >= 0x20 && ch <= 0x7E)) {
    if (g.text_len < TEXT_MAX) {
      g.text[g.text_len++] = ch;
      g.text[g.text_len] = 0;
    }
  }
}

static __attribute__((noreturn)) void desktop_run(void) {
  ps2_event_t ev;
  int dirty = 1, moved = 0;

  g.win_x = ((int)fb_width() - WIN_W) / 2;
  g.win_y = ((int)fb_height() - TASKBAR_H - WIN_H) / 2 - 60;
  g.win_visible = 1;
  g.mouse_x = (int)fb_width() / 2;
  g.mouse_y = (int)fb_height() / 2 + 120;
  ps2_init();

  for (;;) {
    while (ps2_poll(&ev)) {
      if (ev.type == PS2_EVENT_KEY) {
        if (ev.pressed) {
          handle_key(ev.ch);
          dirty = 1;
        }
      } else if (ev.type == PS2_EVENT_MOUSE) {
        unsigned int pressed_now = ev.buttons & ~g.buttons;
        g.mouse_x += ev.dx;
        g.mouse_y += ev.dy;
        if (g.dragging && (ev.buttons & PS2_MOUSE_LEFT)) {
          g.win_x = g.mouse_x - g.drag_dx;
          g.win_y = g.mouse_y - g.drag_dy;
          dirty = 1;
        }
        clamp_state();
        if (pressed_now & PS2_MOUSE_LEFT) {
          if (handle_click(g.mouse_x, g.mouse_y)) {
            dirty = 1;
          }
        }
        if (!(ev.buttons & PS2_MOUSE_LEFT)) {
          g.dragging = 0;
        }
        if (g.menu_open) {
          dirty = 1;     // hover highlight
        }
        g.buttons = ev.buttons;
        moved = 1;
      }
    }

    if (dirty) {
      draw_scene();
      cursor_save_and_draw();
      dirty = 0;
      moved = 0;
    } else if (moved) {
      cursor_restore();
      cursor_save_and_draw();
      moved = 0;
    }
    __asm__ volatile("pause");
  }
}

// ---- entry from the scheduler ------------------------------------------------

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
