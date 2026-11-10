#include "ulib.h"

// Paint as a user program (PAINT.BIN): pick a colour from the palette strip and draw
// with the mouse; "Clear" wipes the canvas. The picture lives in the window's surface (the
// kernel keeps it), so the program stores no canvas of its own.

#define W            410                 // client area
#define H            294
#define CANVAS_W     400
#define CANVAS_H     260
#define CANVAS_X     4                   // canvas origin inside the window
#define CANVAS_Y     30
#define COLORS       8
#define SWATCH_W     22
#define SWATCH_H     20
#define SWATCH_PITCH 26
#define PALETTE_Y    4
#define CLEAR_X      (4 + COLORS * SWATCH_PITCH + 8)
#define CLEAR_W      50

#define COL_BG       0x00E4E6EE
#define COL_FRAME    0x0078787F
#define COL_ACTIVE   0x002A5ABE
#define COL_TEXT     0x001E1E28

static const uint32_t kPalette[COLORS] = {
  0x00FFFFFF,                            // 0: eraser (the paper colour)
  0x00000000, 0x00C82828, 0x00E68C14, 0x00F0D228, 0x0028A046, 0x002864DC, 0x008C3CBE,
};

static long g_win;
static int g_color = 1;
static int g_pen;                        // 1 while the button is held after a press on the canvas
static int g_last_x;                     // last pen position (canvas coordinates; may lie outside)
static int g_last_y;

static void draw_palette(void) {
  int i;
  int win = (int)g_win;
  static const ulib_button_style_t clear_style = {0x00D2D4DC, 0x00D2D4DC, COL_FRAME, COL_TEXT, COL_TEXT};

  for (i = 0; i < COLORS; ++i) {
    int x = 4 + i * SWATCH_PITCH;

    win_fill(win, x, PALETTE_Y, SWATCH_W, SWATCH_H, kPalette[i]);
    ulib_outline(win, x, PALETTE_Y, SWATCH_W, SWATCH_H, i == g_color ? COL_ACTIVE : COL_FRAME);
    if (i == g_color) {
      ulib_outline(win, x + 1, PALETTE_Y + 1, SWATCH_W - 2, SWATCH_H - 2, COL_ACTIVE);
    }
  }
  ulib_button(win, CLEAR_X, PALETTE_Y, CLEAR_W, SWATCH_H, "Clear", 1, &clear_style);
}

static void clear_canvas(void) {
  win_fill((int)g_win, CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H, kPalette[0]);
}

static void redraw_all(void) {
  win_fill((int)g_win, 0, 0, W, H, COL_BG);
  draw_palette();
  ulib_outline((int)g_win, CANVAS_X - 1, CANVAS_Y - 1, CANVAS_W + 2, CANVAS_H + 2, COL_FRAME);
  clear_canvas();
}

// A 3x3 dab of the current colour at canvas position (px, py), clipped to the canvas.
static void stamp(int px, int py) {
  int x0 = px - 1 < 0 ? 0 : px - 1;
  int y0 = py - 1 < 0 ? 0 : py - 1;
  int x1 = px + 2 > CANVAS_W ? CANVAS_W : px + 2;
  int y1 = py + 2 > CANVAS_H ? CANVAS_H : py + 2;

  if (x1 > x0 && y1 > y0) {
    win_fill((int)g_win, CANVAS_X + x0, CANVAS_Y + y0, x1 - x0, y1 - y0, kPalette[g_color]);
  }
}

static void draw_line(int x0, int y0, int x1, int y1) {
  int dx = x1 - x0;
  int dy = y1 - y0;
  int adx = dx < 0 ? -dx : dx;
  int ady = dy < 0 ? -dy : dy;
  int steps = adx > ady ? adx : ady;
  int i;

  if (steps == 0) {
    stamp(x0, y0);
    return;
  }
  for (i = 0; i <= steps; ++i) {
    stamp(x0 + dx * i / steps, y0 + dy * i / steps);
  }
}

int main(void) {
  win_event_t ev;

  g_win = win_create("Paint", W, H);
  if (g_win < 0) {
    puts("paint: could not create a window\r\n");
    return 1;
  }
  redraw_all();

  for (;;) {
    long n = win_event((int)g_win, &ev, 1);

    if (n != 1 || ev.type == EV_CLOSE) {
      break;
    }
    if (ev.type == EV_MOUSE_DOWN) {
      if (ulib_in_rect(ev.x, ev.y, 4, PALETTE_Y, COLORS * SWATCH_PITCH, SWATCH_H)) {
        int i = (ev.x - 4) / SWATCH_PITCH;

        if (ev.x - 4 - i * SWATCH_PITCH < SWATCH_W) {
          g_color = i;
          draw_palette();
        }
      } else if (ulib_in_rect(ev.x, ev.y, CLEAR_X, PALETTE_Y, CLEAR_W, SWATCH_H)) {
        clear_canvas();
      } else if (ulib_in_rect(ev.x, ev.y, CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H)) {
        g_pen = 1;
        g_last_x = ev.x - CANVAS_X;
        g_last_y = ev.y - CANVAS_Y;
        stamp(g_last_x, g_last_y);
      }
    } else if (ev.type == EV_MOUSE_MOVE) {            // only arrives while the button is held
      if (g_pen) {
        int px = ev.x - CANVAS_X;
        int py = ev.y - CANVAS_Y;

        draw_line(g_last_x, g_last_y, px, py);
        g_last_x = px;
        g_last_y = py;
      }
    } else if (ev.type == EV_MOUSE_UP) {
      g_pen = 0;
    }
  }
  win_close((int)g_win);
  return 0;
}
