#include "fb.h"
#include "font8x8.h"
#include "paging.h"
#include "pmm.h"

static volatile uint32_t *g_fb = 0;        // the screen
static volatile uint32_t *g_back = 0;      // off-screen copy (same layout and pixel format), or 0
static volatile uint32_t *g_draw = 0;      // where drawing goes: g_back when buffered, else g_fb
static uint32_t g_width = 0;
static uint32_t g_height = 0;
static uint32_t g_stride = 0;   // pixels per scan line
static uint32_t g_format = 1;   // 0 = RGBX, 1 = BGRX

// Render target. Drawing calls take screen coordinates; the target is the screen (or its
// off-screen copy), or a surface whose top-left pixel sits at (g_org_x, g_org_y) in
// screen coordinates (fb_set_target). Drawing is limited to the target's bounds.
static int g_org_x = 0, g_org_y = 0;       // screen coordinates of the target's first pixel
static int g_tw = 0, g_th = 0;             // target size in pixels
static uint32_t g_tstride = 0;             // pixels per target row
static int g_on_surface = 0;
static volatile uint32_t *g_saved_draw = 0;
static int g_saved_clip[4];

// Clip rectangle [x0,x1) x [y0,y1); reset to the target bounds.
static int g_clip_x0 = 0, g_clip_y0 = 0, g_clip_x1 = 0, g_clip_y1 = 0;

void fb_reset_clip(void) {
  g_clip_x0 = g_org_x;
  g_clip_y0 = g_org_y;
  g_clip_x1 = g_org_x + g_tw;
  g_clip_y1 = g_org_y + g_th;
}

void fb_set_clip(int x, int y, int w, int h) {
  int x1 = x + w, y1 = y + h;
  if (x < g_org_x) x = g_org_x;
  if (y < g_org_y) y = g_org_y;
  if (x1 > g_org_x + g_tw) x1 = g_org_x + g_tw;
  if (y1 > g_org_y + g_th) y1 = g_org_y + g_th;
  g_clip_x0 = x;
  g_clip_y0 = y;
  g_clip_x1 = (x1 > x) ? x1 : x;
  g_clip_y1 = (y1 > y) ? y1 : y;
}

void fb_get_clip(int *x, int *y, int *w, int *h) {
  *x = g_clip_x0;
  *y = g_clip_y0;
  *w = g_clip_x1 - g_clip_x0;
  *h = g_clip_y1 - g_clip_y0;
}

static inline uint32_t fb_pack(uint32_t rgb) {
  if (g_format == 1) {
    // BGRX in memory is 0x00RRGGBB as a little-endian 32-bit word.
    return rgb & 0x00FFFFFFu;
  }
  // RGBX in memory is 0x00BBGGRR as a little-endian 32-bit word.
  return ((rgb & 0xFFu) << 16) | (rgb & 0x0000FF00u) | ((rgb >> 16) & 0xFFu);
}

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
  g_draw = g_fb;
  g_back = 0;
  g_org_x = 0;
  g_org_y = 0;
  g_tw = (int)width;
  g_th = (int)height;
  g_tstride = stride;
  g_on_surface = 0;
  {
    // An off-screen copy for double buffering. If memory is short, draw directly.
    uint64_t bytes = (uint64_t)stride * height * 4ULL;
    uint64_t pages = (bytes + 4095ULL) / 4096ULL;
    uint64_t phys = pmm_alloc_pages(pages);

    if (phys != 0 && paging_map_range(phys, pages * 4096ULL)) {
      g_back = (volatile uint32_t *)(uintptr_t)phys;
    }
  }
  fb_reset_clip();
  return 1;
}

int fb_set_buffered(int on) {
  uint64_t i, n;

  if (!g_fb) {
    return 0;
  }
  if (!on) {
    g_draw = g_fb;
    return 1;
  }
  if (!g_back) {
    return 0;
  }
  if (g_draw != g_back) {                                  // start from what is on the screen
    n = (uint64_t)g_stride * g_height;
    for (i = 0; i < n; i++) {
      g_back[i] = g_fb[i];
    }
    g_draw = g_back;
  }
  return 1;
}

void fb_present(int x, int y, int w, int h) {
  int x1, y1, row, col;

  if (!g_fb || !g_back || g_draw != g_back || w <= 0 || h <= 0) {
    return;
  }
  x1 = x + w;
  y1 = y + h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x1 > (int)g_width) x1 = (int)g_width;
  if (y1 > (int)g_height) y1 = (int)g_height;
  for (row = y; row < y1; row++) {
    volatile uint32_t *src = g_back + (uint64_t)row * g_stride;
    volatile uint32_t *dst = g_fb + (uint64_t)row * g_stride;

    for (col = x; col < x1; col++) {
      dst[col] = src[col];
    }
  }
}

void fb_put_pixel_front(int x, int y, uint32_t rgb) {
  if (!g_fb || x < 0 || y < 0 || x >= (int)g_width || y >= (int)g_height) {
    return;
  }
  g_fb[(uint64_t)y * g_stride + (uint32_t)x] = fb_pack(rgb);
}

int fb_available(void) { return g_fb != 0; }
uint32_t fb_width(void) { return g_fb ? g_width : 0; }
uint32_t fb_height(void) { return g_fb ? g_height : 0; }

void fb_put_pixel(int x, int y, uint32_t rgb) {
  if (!g_fb || x < g_clip_x0 || y < g_clip_y0 || x >= g_clip_x1 || y >= g_clip_y1) {
    return;
  }
  g_draw[(uint64_t)(y - g_org_y) * g_tstride + (uint32_t)(x - g_org_x)] = fb_pack(rgb);
}

uint32_t fb_get_pixel(int x, int y) {
  if (!g_fb || x < g_org_x || y < g_org_y || x >= g_org_x + g_tw || y >= g_org_y + g_th) {
    return 0;
  }
  return fb_pack(g_draw[(uint64_t)(y - g_org_y) * g_tstride + (uint32_t)(x - g_org_x)]);  // the pack swap is its own inverse
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
  int x1, y1, row, col;
  uint32_t px;

  if (!g_fb || w <= 0 || h <= 0) {
    return;
  }
  x1 = x + w;
  y1 = y + h;
  if (x < g_clip_x0) x = g_clip_x0;
  if (y < g_clip_y0) y = g_clip_y0;
  if (x1 > g_clip_x1) x1 = g_clip_x1;
  if (y1 > g_clip_y1) y1 = g_clip_y1;
  px = fb_pack(rgb);
  for (row = y; row < y1; row++) {
    volatile uint32_t *line = g_draw + (int64_t)(row - g_org_y) * g_tstride - g_org_x;
    for (col = x; col < x1; col++) {
      line[col] = px;
    }
  }
}

void fb_blit(int x, int y, int w, int h, const uint32_t *src, int src_stride) {
  int x0, y0, x1, y1, row, col;

  if (!g_fb || w <= 0 || h <= 0) {
    return;
  }
  x0 = x < g_clip_x0 ? g_clip_x0 : x;
  y0 = y < g_clip_y0 ? g_clip_y0 : y;
  x1 = x + w > g_clip_x1 ? g_clip_x1 : x + w;
  y1 = y + h > g_clip_y1 ? g_clip_y1 : y + h;
  for (row = y0; row < y1; row++) {
    volatile uint32_t *line = g_draw + (int64_t)(row - g_org_y) * g_tstride - g_org_x;
    const uint32_t *in = src + (int64_t)(row - y) * src_stride - x;

    for (col = x0; col < x1; col++) {
      line[col] = fb_pack(in[col]);
    }
  }
}

// ---- surfaces and alpha blending --------------------------------------------

void fb_set_target(uint32_t *pixels, int w, int h, int org_x, int org_y) {
  if (!g_fb || g_on_surface || !pixels || w <= 0 || h <= 0) {
    return;
  }
  g_saved_draw = g_draw;
  g_saved_clip[0] = g_clip_x0;
  g_saved_clip[1] = g_clip_y0;
  g_saved_clip[2] = g_clip_x1;
  g_saved_clip[3] = g_clip_y1;
  g_draw = (volatile uint32_t *)pixels;
  g_org_x = org_x;
  g_org_y = org_y;
  g_tw = w;
  g_th = h;
  g_tstride = (uint32_t)w;
  g_on_surface = 1;
  fb_reset_clip();
}

void fb_reset_target(void) {
  if (!g_on_surface) {
    return;
  }
  g_draw = g_saved_draw;
  g_org_x = 0;
  g_org_y = 0;
  g_tw = (int)g_width;
  g_th = (int)g_height;
  g_tstride = g_stride;
  g_on_surface = 0;
  g_clip_x0 = g_saved_clip[0];
  g_clip_y0 = g_saved_clip[1];
  g_clip_x1 = g_saved_clip[2];
  g_clip_y1 = g_saved_clip[3];
}

uint64_t fb_surface_pixels_bytes(int w, int h) {
  return (uint64_t)(w > 0 ? w : 0) * (uint64_t)(h > 0 ? h : 0) * 4u;
}

// dst = src * a + dst * (1 - a), a in 0..255, on two channels at a time.
static inline uint32_t blend(uint32_t s, uint32_t d, uint32_t a) {
  uint32_t ia = 255u - a;
  uint32_t rb = (s & 0x00FF00FFu) * a + (d & 0x00FF00FFu) * ia + 0x00800080u;
  uint32_t g = (s & 0x0000FF00u) * a + (d & 0x0000FF00u) * ia + 0x00008000u;

  rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
  g = ((g + ((g >> 8) & 0x0000FF00u)) >> 8) & 0x0000FF00u;
  return rb | g;
}

void fb_fill_rect_alpha(int x, int y, int w, int h, uint32_t rgb, int alpha) {
  int x1, y1, row, col;
  uint32_t px;

  if (!g_fb || w <= 0 || h <= 0 || alpha <= 0) {
    return;
  }
  if (alpha >= 255) {
    fb_fill_rect(x, y, w, h, rgb);
    return;
  }
  x1 = x + w;
  y1 = y + h;
  if (x < g_clip_x0) x = g_clip_x0;
  if (y < g_clip_y0) y = g_clip_y0;
  if (x1 > g_clip_x1) x1 = g_clip_x1;
  if (y1 > g_clip_y1) y1 = g_clip_y1;
  px = fb_pack(rgb);
  for (row = y; row < y1; row++) {
    volatile uint32_t *line = g_draw + (int64_t)(row - g_org_y) * g_tstride - g_org_x;
    for (col = x; col < x1; col++) {
      line[col] = blend(px, line[col], (uint32_t)alpha);
    }
  }
}

// Copy (alpha 255) or blend a surface that was drawn with fb_set_target.
void fb_composite(int x, int y, int w, int h, const uint32_t *src, int src_stride, int alpha) {
  int x0, y0, x1, y1, row, col;

  if (!g_fb || w <= 0 || h <= 0 || alpha <= 0) {
    return;
  }
  x0 = x < g_clip_x0 ? g_clip_x0 : x;
  y0 = y < g_clip_y0 ? g_clip_y0 : y;
  x1 = x + w > g_clip_x1 ? g_clip_x1 : x + w;
  y1 = y + h > g_clip_y1 ? g_clip_y1 : y + h;
  for (row = y0; row < y1; row++) {
    volatile uint32_t *line = g_draw + (int64_t)(row - g_org_y) * g_tstride - g_org_x;
    const uint32_t *in = src + (int64_t)(row - y) * src_stride - x;

    if (alpha >= 255) {
      for (col = x0; col < x1; col++) {
        line[col] = in[col];
      }
    } else {
      for (col = x0; col < x1; col++) {
        line[col] = blend(in[col], line[col], (uint32_t)alpha);
      }
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
  fb_fill_rect(g_clip_x0, g_clip_y0, g_clip_x1 - g_clip_x0, g_clip_y1 - g_clip_y0, rgb);
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
    if (y + row < g_clip_y0) {
      continue;
    }
    if (y + row >= g_clip_y1) {
      break;
    }
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
  if (x + FB_FONT_WIDTH * scale <= g_clip_x0 || x >= g_clip_x1 ||
      y + FB_FONT_HEIGHT * scale <= g_clip_y0 || y >= g_clip_y1) {
    return;
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
