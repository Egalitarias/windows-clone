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
// Copy a w x h block of 0x00RRGGBB pixels (src_stride pixels per source row) to (x, y).
void fb_blit(int x, int y, int w, int h, const uint32_t *src, int src_stride);

// Double buffering. With buffering on, every drawing call (and fb_get_pixel) works on an
// off-screen copy of the screen, so the screen never shows a half-drawn picture; call
// fb_present() for the area you finished to copy it to the screen. Turning it on first
// copies the screen into the copy. With buffering off, drawing goes straight to the
// screen (the default). Returns 0 when there is no off-screen copy (out of memory): then
// drawing stays direct and fb_present does nothing.
int fb_set_buffered(int on);
void fb_present(int x, int y, int w, int h);
// Draw one pixel straight on the screen, bypassing the off-screen copy and the clip. For
// things that sit on top of the picture, such as the mouse pointer.
void fb_put_pixel_front(int x, int y, uint32_t rgb);

// Clip rectangle: every drawing call is limited to it (reads are not).
// The default is the whole screen. Clips do not nest; save and restore the
// previous one with fb_get_clip() when intersecting.
void fb_set_clip(int x, int y, int w, int h);
void fb_reset_clip(void);
void fb_get_clip(int *x, int *y, int *w, int *h);

// Surfaces. fb_set_target makes `pixels` (w x h, fb_surface_pixels_bytes(w, h) bytes, in
// the screen's own pixel format) the drawing target; the surface's first pixel stands for
// screen position (org_x, org_y), so code that draws with screen coordinates works
// unchanged. The clip is reset to the surface; fb_reset_target restores the screen and
// the clip it had. No nesting, and not while presenting. fb_composite copies a surface
// onto the current target (alpha 255 = opaque, lower = translucent), limited by the clip;
// fb_fill_rect_alpha blends a colour over what is there.
void fb_set_target(uint32_t *pixels, int w, int h, int org_x, int org_y);
void fb_reset_target(void);
uint64_t fb_surface_pixels_bytes(int w, int h);
void fb_composite(int x, int y, int w, int h, const uint32_t *src, int src_stride, int alpha);
void fb_fill_rect_alpha(int x, int y, int w, int h, uint32_t rgb, int alpha);

// Vertical gradient from top to bottom inside the rectangle.
void fb_fill_gradient(int x, int y, int w, int h, uint32_t top_rgb, uint32_t bottom_rgb);

// Text uses the built-in 8x8 font, drawn at integer `scale` (1 = 8x8 pixels).
void fb_draw_char(int x, int y, char c, uint32_t rgb, int scale);
void fb_draw_text(int x, int y, const char *s, uint32_t rgb, int scale);
int fb_text_width(const char *s, int scale);

#endif // KERNEL_FB_H
