#include "desktop.h"
#include "scheduler.h"
#include "apps.h"
#include "fb.h"
#include "fs.h"
#include "idt.h"
#include "irqlock.h"
#include "block.h"
#include "irq.h"
#include "ps2.h"
#include "rtc.h"
#include "timer.h"
#include "wm.h"
#include "syswin.h"
#include "usbstor.h"
#include "disk.h"

// The shell: wallpaper, desktop icons, taskbar (Start button, one button per
// window, clock), Start menu and the mouse cursor. Windows themselves belong to
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
#define MENU_COUNT   8
#define ICON_X       24
#define ICON_Y0      24
#define ICON_PITCH   84
#define ICON_COUNT   4

#define DOUBLE_CLICK_MS   400
#define DOUBLE_CLICK_SLOP 5

#define CLOCK_W      100
#define TZ_STEP_MIN  30
#define TZ_MIN       (-12 * 60)
#define TZ_MAX       (14 * 60)

static char g_clock[6] = "--:--";
static char g_date[11] = "";
static char g_zone[10] = "";
static int g_tz_offset_min = 0;     // local time = RTC (UTC) + this; saved in TZ.CFG
static int g_tz_dirty = 0;          // changed since it was last saved
static uint64_t g_tz_changed_ms = 0;
static void update_clock(void);

int desktop_tz_offset_min(void) {
  return g_tz_offset_min;
}

// ---- time zone setting -----------------------------------------------------
// Kept as text ("60" = UTC+01:00, the offset in minutes) in TZ.CFG on the default
// volume. Saving waits until the clicks stop, so stepping through several zones
// writes the disk once.

#define TZ_FILE        "TZ.CFG"
#define TZ_SAVE_DELAY_MS 2000

static void tz_load(void) {
  fat_volume_t *v = fs_default_volume();
  fat_dirent_t ent;
  char buf[16];
  uint32_t got = 0, i = 0;
  int sign = 1, value = 0, digits = 0;

  if (v == 0 || !fat_lookup(v, v->root_cluster, TZ_FILE, &ent) || (ent.attr & FAT_ATTR_DIR) ||
      ent.size == 0 || !fat_read_file(v, &ent, buf, sizeof(buf) - 1, &got)) {
    return;
  }
  buf[got] = 0;
  if (buf[i] == '-') {
    sign = -1;
    i++;
  } else if (buf[i] == '+') {
    i++;
  }
  for (; buf[i] >= '0' && buf[i] <= '9' && digits < 5; i++, digits++) {
    value = value * 10 + (buf[i] - '0');
  }
  value *= sign;
  if (digits > 0 && value >= TZ_MIN && value <= TZ_MAX) {
    g_tz_offset_min = value;
  }
}

static void tz_save(void) {
  fat_volume_t *v = fs_default_volume();
  char buf[16];
  int n = 0, value = g_tz_offset_min, started = 0, div;

  g_tz_dirty = 0;
  if (v == 0) {
    desktop_notify("Time zone not saved: no disk");
    return;
  }
  if (value < 0) {
    buf[n++] = '-';
    value = -value;
  }
  for (div = 10000; div >= 1; div /= 10) {
    int digit = (value / div) % 10;
    if (digit != 0 || started || div == 1) {
      buf[n++] = (char)('0' + digit);
      started = 1;
    }
  }
  buf[n++] = '\n';
  if (!fat_write_file(v, v->root_cluster, TZ_FILE, buf, (uint32_t)n)) {
    console_printf("[clock] could not save the time zone to %s\r\n", TZ_FILE);
    desktop_notify("Could not save the time zone");
  } else {
    desktop_notify("Time zone saved");
  }
}

// ---- notifications (toasts) --------------------------------------------------
// Short messages stacked above the taskbar's right end. Each fades out after
// TOAST_LIFE_MS or when clicked. Only the desktop task may call desktop_notify.

#define TOAST_MAX      4
#define TOAST_W        300
#define TOAST_H        32
#define TOAST_GAP      6
#define TOAST_CHARS    33
#define TOAST_LIFE_MS  5000
#define COL_TOAST      FB_RGB(36, 42, 60)
#define COL_TOAST_BAR  FB_RGB(90, 190, 130)

static int bar_y(void);
static char g_toast_text[TOAST_MAX][TOAST_CHARS + 1];
static uint64_t g_toast_expire[TOAST_MAX];
static int g_toast_count = 0;                // index 0 is the oldest, drawn lowest

static int toast_x(void) { return (int)fb_width() - TOAST_W - 8; }
static int toast_y(int i) { return bar_y() - (i + 1) * (TOAST_H + TOAST_GAP); }

static void toast_invalidate(void) {
  int top = toast_y(TOAST_MAX - 1);
  wm_invalidate(toast_x() - 1, top, TOAST_W + 2, bar_y() - top);
}

// USB sticks and input devices coming and going: remount the volumes after a disk change.
static void usb_hotplug(void) {
  int ev = usbstor_hotplug_poll();

  if (ev & (USB_HOT_DISK_ADDED | USB_HOT_DISK_REMOVED)) {
    scheduler_preempt_disable();                // nobody may be in the middle of a FAT operation
    disk_refresh_usb();
    fs_rescan();
    scheduler_preempt_enable();
    console_printf("[usb] disks changed: %d volume(s) mounted\r\n", fs_volume_count());
  }
  if (ev & USB_HOT_DISK_ADDED) {
    desktop_notify("USB drive connected");
  } else if (ev & USB_HOT_DISK_REMOVED) {
    desktop_notify("USB drive removed");
  } else if (ev & USB_HOT_INPUT_ADDED) {
    desktop_notify("USB keyboard or mouse connected");
  } else if (ev & USB_HOT_INPUT_REMOVED) {
    desktop_notify("USB keyboard or mouse removed");
  }
}

void desktop_notify(const char *text) {
  int i, n;

  if (g_toast_count == TOAST_MAX) {         // full: the oldest makes room
    for (i = 1; i < TOAST_MAX; i++) {
      for (n = 0; n <= TOAST_CHARS; n++) {
        g_toast_text[i - 1][n] = g_toast_text[i][n];
      }
      g_toast_expire[i - 1] = g_toast_expire[i];
    }
    g_toast_count--;
  }
  for (n = 0; n < TOAST_CHARS && text[n]; n++) {
    g_toast_text[g_toast_count][n] = text[n];
  }
  g_toast_text[g_toast_count][n] = 0;
  g_toast_expire[g_toast_count] = timer_get_ms() + TOAST_LIFE_MS;
  g_toast_count++;
  toast_invalidate();
}

// Notifications from other tasks (programs): a small queue the desktop loop drains.
#define POST_MAX 8
static char g_post_text[POST_MAX][TOAST_CHARS + 1];
static int g_post_head = 0, g_post_count = 0;

void desktop_notify_post(const char *text) {
  uint64_t f = irq_save();
  int slot, n;

  if (g_post_count < POST_MAX) {            // a flood of messages drops the newest
    slot = (g_post_head + g_post_count) % POST_MAX;
    for (n = 0; n < TOAST_CHARS && text[n]; n++) {
      g_post_text[slot][n] = text[n];
    }
    g_post_text[slot][n] = 0;
    g_post_count++;
  }
  irq_restore(f);
}

static void toast_drain_posts(void) {
  char text[TOAST_CHARS + 1];

  for (;;) {
    uint64_t f = irq_save();
    int n;

    if (g_post_count == 0) {
      irq_restore(f);
      return;
    }
    for (n = 0; n <= TOAST_CHARS; n++) {
      text[n] = g_post_text[g_post_head][n];
    }
    g_post_head = (g_post_head + 1) % POST_MAX;
    g_post_count--;
    irq_restore(f);
    desktop_notify(text);
  }
}

static void toast_remove(int idx) {
  int i, n;

  for (i = idx + 1; i < g_toast_count; i++) {
    for (n = 0; n <= TOAST_CHARS; n++) {
      g_toast_text[i - 1][n] = g_toast_text[i][n];
    }
    g_toast_expire[i - 1] = g_toast_expire[i];
  }
  g_toast_count--;
  toast_invalidate();
}

static void toast_expire(uint64_t now) {
  int i = 0;

  while (i < g_toast_count) {
    if (now >= g_toast_expire[i]) {
      toast_remove(i);
    } else {
      i++;
    }
  }
}

// Returns 1 if the click hit a toast (which is then dismissed).
static int toast_click(int x, int y) {
  int i;

  for (i = 0; i < g_toast_count; i++) {
    if (x >= toast_x() && x < toast_x() + TOAST_W && y >= toast_y(i) && y < toast_y(i) + TOAST_H) {
      toast_remove(i);
      return 1;
    }
  }
  return 0;
}

static void draw_toasts(void) {
  int i;

  for (i = 0; i < g_toast_count; i++) {
    int x = toast_x(), y = toast_y(i);
    fb_fill_rect(x, y, TOAST_W, TOAST_H, COL_TOAST);
    fb_draw_rect(x, y, TOAST_W, TOAST_H, COL_TASKBAR_EDGE);
    fb_fill_rect(x, y, 4, TOAST_H, COL_TOAST_BAR);
    fb_draw_text(x + 14, y + 12, g_toast_text[i], COL_WHITE, 1);
  }
}

static int g_icon_selected = -1;
static uint64_t g_last_click_ms = 0;
static int g_last_click_x = -9999, g_last_click_y = -9999;
static int g_menu_open = 0;
static int g_menu_hover = -1;
static int g_mouse_x = 0, g_mouse_y = 0;
static unsigned int g_buttons = 0;

// ---- geometry --------------------------------------------------------------

static int in_rect(int px, int py, int x, int y, int w, int h) {
  return px >= x && py >= y && px < x + w && py < y + h;
}

static int bar_y(void) {
  return (int)fb_height() - TASKBAR_H; }
static int menu_x(void) { return START_X; }
static int menu_y(void) { return bar_y() - MENU_COUNT * MENU_ITEM_H - 4; }

static const char *menu_label(int i) {
  switch (i) {
    case 0:  return "Files";
    case 1:  return "Notes";
    case 2:  return "Paint";
    case 3:  return "Programs";
    case 4:  return "About WinClone";
    case 5:  return "Console";
    case 6:  return "Setup";
    default: return "Shut down";
  }
}

static const char *icon_label(int i) {
  return i == 0 ? "Files" : (i == 1 ? "Notes" : (i == 2 ? "Paint" : "About"));
}

static uint32_t icon_color(int i) {
  return i == 0 ? FB_RGB(235, 190, 60)
       : (i == 1 ? FB_RGB(240, 235, 120)
       : (i == 2 ? FB_RGB(220, 90, 90) : FB_RGB(90, 190, 130)));
}

// ---- painting --------------------------------------------------------------

static void draw_wallpaper(void) {
  int i;

  fb_fill_gradient(0, 0, (int)fb_width(), (int)fb_height() - TASKBAR_H,
                   FB_RGB(20, 70, 160), FB_RGB(90, 170, 230));
  for (i = 0; i < ICON_COUNT; i++) {
    int y = ICON_Y0 + i * ICON_PITCH;
    if (i == g_icon_selected) {
      fb_fill_rect(ICON_X - 8, y - 4, 56, 64, FB_RGB(60, 110, 200));
      fb_draw_rect(ICON_X - 8, y - 4, 56, 64, FB_RGB(180, 205, 255));
    }
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
  fb_draw_text(w - CLOCK_W + (CLOCK_W - 8 * 5) / 2, y + 5, g_clock, COL_WHITE, 1);
  fb_draw_text(w - CLOCK_W + (CLOCK_W - 8 * 10) / 2, y + 16, g_date, COL_WHITE, 1);
  fb_draw_text(w - CLOCK_W + (CLOCK_W - 8 * 9) / 2, y + 27, g_zone, FB_RGB(150, 165, 200), 1);
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
  draw_toasts();
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

static int g_cur_x, g_cur_y, g_cur_drawn = 0;

// The pointer is drawn straight on the screen, on top of the picture, which is kept
// clean in the off-screen copy: erasing the pointer is just copying that area back.
static void cursor_save_and_draw(void) {
  int r, c;
  g_cur_x = g_mouse_x;
  g_cur_y = g_mouse_y;
  for (r = 0; r < CUR_H; r++) {
    for (c = 0; c < CUR_W; c++) {
      if (kCursor[r][c] == 'X') {
        fb_put_pixel_front(g_cur_x + c, g_cur_y + r, FB_RGB(0, 0, 0));
      } else if (kCursor[r][c] == '.') {
        fb_put_pixel_front(g_cur_x + c, g_cur_y + r, FB_RGB(255, 255, 255));
      }
    }
  }
  g_cur_drawn = 1;
}

static void cursor_restore(void) {
  if (!g_cur_drawn) {
    return;
  }
  fb_present(g_cur_x, g_cur_y, CUR_W, CUR_H);
  g_cur_drawn = 0;
}

// ---- actions ---------------------------------------------------------------

__attribute__((noreturn)) void desktop_shut_down(void) {
  fb_set_buffered(0);                               // draw straight on the screen again
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
    app_launch_files();
  } else if (which == 1) {
    app_launch_notes();
  } else if (which == 2) {
    app_launch_paint();
  } else if (which == 3) {
    app_programs_open();
  } else if (which == 4) {
    app_launch_about();
  } else if (which == 5) {
    app_console_open();
  } else {
    app_setup_open();
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
          wm_damage_window(top);
        }
        wm_raise(win);
        wm_damage_window(win);
      }
      return;
    }
    slot_x += TASK_PITCH;
  }
}

static void select_icon(int which) {
  int i;
  if (which == g_icon_selected) {
    return;
  }
  for (i = 0; i < ICON_COUNT; i++) {
    if (i == g_icon_selected || i == which) {
      wm_invalidate(ICON_X - 8, ICON_Y0 + i * ICON_PITCH - 4, 56, 64);
    }
  }
  g_icon_selected = which;
}

static void handle_click(int x, int y, int double_click) {
  int i;

  if (toast_click(x, y)) {
    return;
  }
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
          desktop_shut_down();
        }
        launch(i);
        return;
      }
    }
    close_menu();     // click elsewhere closes the menu and then acts normally
  }
  // Taskbar
  if (y >= bar_y()) {
    // Clock: left half steps the time zone back, right half forward.
    if (x >= (int)fb_width() - CLOCK_W) {
      int step = (x < (int)fb_width() - CLOCK_W / 2) ? -TZ_STEP_MIN : TZ_STEP_MIN;
      int next = g_tz_offset_min + step;
      if (next >= TZ_MIN && next <= TZ_MAX) {
        g_tz_offset_min = next;
        g_tz_dirty = 1;
        g_tz_changed_ms = timer_get_ms();
        update_clock();
      }
      return;
    }
    taskbar_click(x);
    return;
  }
  // Windows
  if (wm_mouse_down(x, y, double_click)) {
    select_icon(-1);
    return;
  }
  // Desktop icons: click selects, double click launches.
  for (i = 0; i < ICON_COUNT; i++) {
    if (in_rect(x, y, ICON_X - 8, ICON_Y0 + i * ICON_PITCH - 4, 56, 64)) {
      select_icon(i);
      if (double_click) {
        launch(i);
      }
      return;
    }
  }
  select_icon(-1);
}

static void put2(char *p, int v) {
  p[0] = (char)('0' + v / 10);
  p[1] = (char)('0' + v % 10);
}

static void update_clock(void) {
  int y, mo, d, h, m, i, off, changed = 0;
  char text[6], date[11], zone[10];

  if (!rtc_read_local(g_tz_offset_min, &y, &mo, &d, &h, &m)) {
    return;
  }
  put2(text, h);
  text[2] = ':';
  put2(text + 3, m);
  text[5] = 0;
  date[0] = (char)('0' + y / 1000 % 10);
  date[1] = (char)('0' + y / 100 % 10);
  date[2] = (char)('0' + y / 10 % 10);
  date[3] = (char)('0' + y % 10);
  date[4] = '-';
  put2(date + 5, mo);
  date[7] = '-';
  put2(date + 8, d);
  date[10] = 0;
  off = g_tz_offset_min < 0 ? -g_tz_offset_min : g_tz_offset_min;
  zone[0] = 'U'; zone[1] = 'T'; zone[2] = 'C';
  zone[3] = g_tz_offset_min < 0 ? '-' : '+';
  put2(zone + 4, off / 60);
  zone[6] = ':';
  put2(zone + 7, off % 60);
  zone[9] = 0;

  for (i = 0; i < 6; i++) {
    if (text[i] != g_clock[i]) { changed = 1; }
  }
  for (i = 0; i < 11; i++) {
    if (date[i] != g_date[i]) { changed = 1; }
  }
  for (i = 0; i < 10; i++) {
    if (zone[i] != g_zone[i]) { changed = 1; }
  }
  if (changed) {
    for (i = 0; i < 6; i++)  { g_clock[i] = text[i]; }
    for (i = 0; i < 11; i++) { g_date[i] = date[i]; }
    for (i = 0; i < 10; i++) { g_zone[i] = zone[i]; }
    wm_invalidate((int)fb_width() - CLOCK_W, bar_y(), CLOCK_W, TASKBAR_H);
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

// Repaint the accumulated dirty region, if any. Returns 1 if something was drawn.
static int repaint_dirty(void) {
  int dx, dy, dw, dh;
  if (!wm_take_dirty(&dx, &dy, &dw, &dh)) {
    return 0;
  }
  cursor_restore();
  fb_set_clip(dx, dy, dw, dh);
  paint_scene();                                    // into the off-screen copy
  fb_reset_clip();
  fb_present(dx, dy, dw, dh);                       // the screen only ever sees a finished picture
  cursor_save_and_draw();
  return 1;
}

void desktop_pump(void) {
  repaint_dirty();
}

static volatile uint64_t g_desktop_loops = 0;

uint64_t desktop_loop_count(void) {
  return g_desktop_loops;
}

static __attribute__((noreturn)) void desktop_run(void) {
  ps2_event_t ev;
  int moved = 0;

  uint64_t last_second;

  wm_init((int)fb_width(), (int)fb_height(), TASKBAR_H);
  fb_set_buffered(1);                               // from here on draw off-screen and present
  g_mouse_x = (int)fb_width() / 2;
  g_mouse_y = (int)fb_height() / 2;

  // Interrupts: mask every line, then enable only what we use. The timer stays
  // on vector 32 with the scheduler's tick handler, which also advances the
  // clock (timer_tick) and lets other tasks (user programs) run beside us.
  irq_init();
  irq_unmask(0);
  ps2_init();
  update_clock();

  // Storage: disks, partitions, FAT volumes. Logged to the console
  // (serial) before the first repaint covers the screen.
  {
    int volumes = fs_init();
    int i;
    console_printf("[storage] %d block device(s), %d FAT volume(s)\r\n", block_count(), volumes);
#ifdef KERNEL_FS_SELFTEST
    if (volumes > 0) {
      fs_selftest();
    }
#endif
    for (i = 0; i < volumes; i++) {
      console_printf("[storage]   volume %d: \"%s\" on %s\r\n", i, fs_volume_name(i),
                     block_get(fs_volume(i)->dev)->name);
    }
  }
  tz_load();
  update_clock();
  last_second = timer_get_ms();

#ifndef KERNEL_USER_SELFTEST                        // the self-tests count user windows: keep them out
  app_launch_notes();
  app_launch_files();
#endif

  for (;;) {
    g_desktop_loops++;
    app_programs_poll();
    app_console_poll();
    syswin_poll();
    toast_drain_posts();
    usb_hotplug();
    while (ps2_poll(&ev) || usbhid_poll(&ev)) {
      if (ev.type == PS2_EVENT_KEY) {
        if (ev.pressed && ev.ch == 27) {
          close_menu();
        } else if (ev.pressed && ev.ch != 0) {
          wm_set_key_mods(ev.mods);
          wm_key(ev.ch);
        }
      } else if (ev.type == PS2_EVENT_MOUSE) {
        unsigned int pressed_now = ev.buttons & ~g_buttons;

        wm_set_key_mods(ev.mods);                 // window callbacks may ask which modifiers are down
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
          uint64_t now = timer_get_ms();
          int near = (g_mouse_x - g_last_click_x < DOUBLE_CLICK_SLOP) &&
                     (g_last_click_x - g_mouse_x < DOUBLE_CLICK_SLOP) &&
                     (g_mouse_y - g_last_click_y < DOUBLE_CLICK_SLOP) &&
                     (g_last_click_y - g_mouse_y < DOUBLE_CLICK_SLOP);
          int is_double = near && (now - g_last_click_ms) < DOUBLE_CLICK_MS;

          handle_click(g_mouse_x, g_mouse_y, is_double);
          if (is_double) {
            g_last_click_x = -9999;       // a triple click is not a second double click
            g_last_click_y = -9999;
          } else {
            g_last_click_x = g_mouse_x;
            g_last_click_y = g_mouse_y;
            g_last_click_ms = now;
          }
        }
        if (released_now & PS2_MOUSE_LEFT) {
          wm_mouse_up(g_mouse_x, g_mouse_y);
        }
        g_buttons = ev.buttons;
        if (ev.wheel != 0) {
          wm_wheel(g_mouse_x, g_mouse_y, ev.wheel);
        }
      }
    }

    if (repaint_dirty()) {
      moved = 0;
    } else if (moved) {
      cursor_restore();
      cursor_save_and_draw();
      moved = 0;
    }
    {
      uint64_t now = timer_get_ms();
      if (now - last_second >= 1000) {
        last_second = now;
        update_clock();
        toast_expire(now);
        if (g_tz_dirty && now - g_tz_changed_ms >= TZ_SAVE_DELAY_MS) {
          tz_save();
        }
        wm_tick();
      }
    }

    // Sleep until the next interrupt (timer, keyboard or mouse). `sti; hlt`
    // is atomic: an interrupt arriving right after sti still wakes the hlt.
    __asm__ volatile("sti; hlt" : : : "memory");
  }
}

// ---- entry from the scheduler ----------------------------------------------

// The desktop is an ordinary scheduler task. Task stacks are small (4 KB), so the
// task switches to a private 64 KB stack before running the desktop; the
// scheduler saves and restores interrupt frames wherever the stack lives.
static uint8_t g_desktop_stack[64 * 1024] __attribute__((aligned(16)));

static int desktop_task(void *arg) {
  uintptr_t top;

  (void)arg;
  // Interrupts stay off until desktop_run has set up the IRQ layer.
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

// Scheduler idle callback: start the desktop as a task (no-op without a framebuffer).
void desktop_start(void) {
  if (!fb_available()) {
    return;
  }
  scheduler_add_task("desktop", desktop_task, 0);
}
