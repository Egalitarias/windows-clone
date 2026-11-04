#include "fb.h"
#include "font8x8.h"
#include "paging.h"

static volatile uint32_t *g_fb = 0;
static uint32_t g_width = 0;
static uint32_t g_height = 0;
static uint32_t g_stride = 0;   // pixels per scan line
static uint32_t g_format = 1;   // 0 = RGBX, 1 = BGRX

int fb_init(uint64_t base, uint64_t size, uint32_t width, uint32_t height,
            uint32_t stride, uint32_t format) {
  g_fb = 0;
  if (base == 0 || width == 0 || height == 0 || stride < width || format > 1) {
    return 0;
  }
  if (size < (uint64_t)stride * height * 4ULL) {
    size = (uint64_t)stride * height * 4ULL;
  }
  if (!paging_map_range(base, size)) {
    return 0;
  }
  g_width = width;
  g_height = height;
  g_stride = stride;
  g_format = format;
  g_fb = (volatile uint32_t *)(uintptr_t)base;
  return 1;
}

int fb_available(void) { return g_fb != 0; }
uint32_t fb_width(void) { return g_fb ? g_width : 0; }
uint32_t fb_height(void) { return g_fb ? g_height : 0; }

static inline uint32_t fb_pack(uint32_t rgb) {
  if (g_format == 1) {
    // BGRX in memory is 0x00RRGGBB as a little-endian 32-bit word.
    return rgb & 0x00FFFFFFu;
  }
  // RGBX in memory is 0x00BBGGRR as a little-endian 32-bit word.
  return ((rgb & 0xFFu) << 16) | (rgb & 0x0000FF00u) | ((rgb >> 16) & 0xFFu);
}

void fb_put_pixel(int x, int y, uint32_t rgb) {
  if (!g_fb || x < 0 || y < 0 || (uint32_t)x >= g_width || (uint32_t)y >= g_height) {
    return;
  }
  g_fb[(uint64_t)y * g_stride + (uint32_t)x] = fb_pack(rgb);
}

uint32_t fb_get_pixel(int x, int y) {
  if (!g_fb || x < 0 || y < 0 || (uint32_t)x >= g_width || (uint32_t)y >= g_height) {
    return 0;
  }
  return fb_pack(g_fb[(uint64_t)y * g_stride + (uint32_t)x]);  // the pack swap is its own inverse
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
  int x1, y1, row, col;
  uint32_t px;

  if (!g_fb || w <= 0 || h <= 0) {
    return;
  }
  x1 = x + w;
  y1 = y + h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x1 > (int)g_width) x1 = (int)g_width;
  if (y1 > (int)g_height) y1 = (int)g_height;
  px = fb_pack(rgb);
  for (row = y; row < y1; row++) {
    volatile uint32_t *line = g_fb + (uint64_t)row * g_stride;
    for (col = x; col < x1; col++) {
      line[col] = px;
    }
  }
}

void fb_draw_rect(int x, int y, int w, int h, uint32_t rgb) {
  if (w <= 0 || h <= 0) {
    return;
  }
  fb_fill_rect(x, y, w, 1, rgb);
  fb_fill_rect(x, y + h - 1, w, 1, rgb);
  fb_fill_rect(x, y, 1, h, rgb);
  fb_fill_rect(x + w - 1, y, 1, h, rgb);
}

void fb_clear(uint32_t rgb) {
  fb_fill_rect(0, 0, (int)g_width, (int)g_height, rgb);
}

void fb_fill_gradient(int x, int y, int w, int h, uint32_t top_rgb, uint32_t bottom_rgb) {
  int row;
  int tr = (int)((top_rgb >> 16) & 0xFF), tg = (int)((top_rgb >> 8) & 0xFF), tb = (int)(top_rgb & 0xFF);
  int br = (int)((bottom_rgb >> 16) & 0xFF), bg = (int)((bottom_rgb >> 8) & 0xFF), bb = (int)(bottom_rgb & 0xFF);

  if (h <= 0) {
    return;
  }
  for (row = 0; row < h; row++) {
    int d = (h > 1) ? (h - 1) : 1;
    int r = tr + (br - tr) * row / d;
    int g = tg + (bg - tg) * row / d;
    int b = tb + (bb - tb) * row / d;
    fb_fill_rect(x, y + row, w, 1, FB_RGB(r, g, b));
  }
}

void fb_draw_char(int x, int y, char c, uint32_t rgb, int scale) {
  int row, col;
  const unsigned char *glyph;

  if (scale < 1) {
    scale = 1;
  }
  if (c < 0x20 || c > 0x7E) {
    c = '?';
  }
  glyph = g_font8x8[c - 0x20];
  for (row = 0; row < FB_FONT_HEIGHT; row++) {
    for (col = 0; col < FB_FONT_WIDTH; col++) {
      if (glyph[row] & (1u << col)) {
        fb_fill_rect(x + col * scale, y + row * scale, scale, scale, rgb);
      }
    }
  }
}

void fb_draw_text(int x, int y, const char *s, uint32_t rgb, int scale) {
  int cx = x;

  if (scale < 1) {
    scale = 1;
  }
  while (s && *s) {
    if (*s == '\n') {
      cx = x;
      y += (FB_FONT_HEIGHT + 1) * scale;
    } else {
      fb_draw_char(cx, y, *s, rgb, scale);
      cx += FB_FONT_WIDTH * scale;
    }
    s++;
  }
}

int fb_text_width(const char *s, int scale) {
  int n = 0;

  if (scale < 1) {
    scale = 1;
  }
  while (s && s[n]) {
    n++;
  }
  return n * FB_FONT_WIDTH * scale;
}
