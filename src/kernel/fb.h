#ifndef KERNEL_FB_H
#define KERNEL_FB_H

#include "console.h"

// Colors are 0x00RRGGBB regardless of the hardware pixel format.
#define FB_RGB(r, g, b) ((((uint32_t)(r)) << 16) | (((uint32_t)(g)) << 8) | ((uint32_t)(b)))

#define FB_FONT_WIDTH  8
#define FB_FONT_HEIGHT 8

// Map the linear framebuffer handed over by the loader and make it the
// drawing target. Must be called after paging is active. Returns 1 on success,
// 0 if there is no usable framebuffer (all draw calls then do nothing).
// format: 0 = RGBX, 1 = BGRX (UEFI EFI_GRAPHICS_PIXEL_FORMAT).
int fb_init(uint64_t base, uint64_t size, uint32_t width, uint32_t height,
            uint32_t stride, uint32_t format);

int fb_available(void);
uint32_t fb_width(void);
uint32_t fb_height(void);

void fb_put_pixel(int x, int y, uint32_t rgb);
uint32_t fb_get_pixel(int x, int y);  // 0x00RRGGBB; 0 when out of range
void fb_fill_rect(int x, int y, int w, int h, uint32_t rgb);
void fb_draw_rect(int x, int y, int w, int h, uint32_t rgb);  // 1px outline
void fb_clear(uint32_t rgb);

// Vertical gradient from top to bottom inside the rectangle.
void fb_fill_gradient(int x, int y, int w, int h, uint32_t top_rgb, uint32_t bottom_rgb);

// Text uses the built-in 8x8 font, drawn at integer `scale` (1 = 8x8 pixels).
void fb_draw_char(int x, int y, char c, uint32_t rgb, int scale);
void fb_draw_text(int x, int y, const char *s, uint32_t rgb, int scale);
int fb_text_width(const char *s, int scale);

#endif // KERNEL_FB_H
