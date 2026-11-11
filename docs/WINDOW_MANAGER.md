# Window manager and shell

Source: `src/kernel/wm.c`, `wm.h` (window manager), `desktop.c` (shell), `apps.c`
(demo applications), `fb.c` (drawing and clip rectangles).

## Layers

| Layer | File | Owns |
|---|---|---|
| Drawing | `fb.c` | pixels, rectangles, gradients, 8x8 text, clip rectangle |
| Window manager | `wm.c` | window pool, z-order, focus, move/resize/min/max/close, dirty rectangle |
| Shell | `desktop.c` | wallpaper, desktop icons, taskbar, Start menu, mouse cursor, main loop |
| Apps | `apps.c` | window contents and behaviour |

## Painting model

Anything that changes calls `wm_invalidate(x, y, w, h)`. The shell's main loop asks for
the accumulated dirty rectangle (`wm_take_dirty`), sets the fb clip to it, repaints the
whole scene back to front (wallpaper, windows bottom to top, taskbar, menu), then
resets the clip.

The scene is **double buffered** (`fb_set_buffered`, `fb_present` in `fb.c`): once the
desktop is running, every drawing call works on an off-screen copy of the screen (allocated
from the page allocator in `fb_init`, same layout and pixel format), and only the finished
dirty rectangle is copied to the screen with `fb_present`. The screen therefore never shows
a half-drawn picture (wallpaper, then window, then text). The mouse pointer is drawn
straight on the screen on top (`fb_put_pixel_front`), so the off-screen copy always holds the
clean picture and erasing the pointer is just presenting that small area again; pointer
movement costs no scene repaint. If the off-screen copy cannot be allocated, drawing stays
direct, as before. The shutdown screen turns buffering off.

## Compositor

Every window has its own **surface**: a `w x h` pixel buffer for the whole window (frame and
client area) in the screen's pixel format, from the page allocator and kept for the life of the
window. `wm_paint_windows()` composes the screen from them, back to front:

1. If the window's frame is stale (title changed, focus changed, resized) the frame is redrawn
   into the surface; if its content is stale the client area is cleared and the window's
   `paint` callback runs, still with screen coordinates (`fb_set_target` maps the surface's
   first pixel to the window's screen position, so no application code changed).
2. A soft drop shadow is blended under the window (`fb_fill_rect_alpha`).
3. The surface is copied to the off-screen screen copy under the dirty clip
   (`fb_composite`), blended when the window's opacity is below 255.

Two kinds of invalidation follow from that:

- `wm_invalidate_window(win)`: the window's **content** changed, so `paint` runs again. Apps
  call this.
- `wm_damage_window(win)`: only the **screen** needs updating (moved, uncovered, focus changed).
  The surface is reused and no `paint` callback runs. The window manager and shell use this.

So dragging a window, raising it or uncovering what is behind it costs only blits. While a
window is dragged by its title bar it is drawn at 215/255 opacity (`DRAG_OPACITY`);
`wm_set_opacity(win, 0..255)` is available to anything else. Windows that do not touch the
repaint area are skipped, and their surfaces stay as they are until they are next shown.
Minimized windows keep their surface, so restoring is instant. If a surface cannot be
allocated the window is drawn straight onto the screen as before.

## Windows

`wm_create(title, x, y, w, h)` returns a window; set its callbacks afterwards:

- `paint(win, cx, cy, cw, ch)`: draw the client area (clip already limited to it)
- `key(win, ch)`: a typed character to the focused window
- `mouse(win, lx, ly, event)`: client-relative; after a press inside the client area,
  move and release events keep coming to that window (`WM_MOUSE_DOWN/MOVE/UP`)
- `closing(win)`: cleanup before the window is destroyed

Call `wm_invalidate_window(win)` (or `wm_invalidate`) after changing what you draw.

**Callbacks must be assigned in code at run time.** The kernel is loaded without
relocations, so function pointers inside static initialisers are not valid.

## Input

Left click on a window raises it and focuses it. Title bar drags move; the dotted
bottom-right corner resizes; the three title buttons minimize, maximize and close.
Double-clicking a title bar toggles maximize. Taskbar buttons restore, focus or minimize.
`Esc` closes the Start menu. Desktop icons: click selects, double click launches.

## Known limitations

- User programs keep their own surface (`syswin.c`) and the window manager keeps another for
  the window, so their pixels are copied once more on each content change.
- Surfaces cost `w*h*4` bytes per window (a maximized window is about 4 MB), allocated on the
  first paint and when the window grows.
- Translucency is per window (not per pixel), and a translucent window's shadow is not faded.
- Presenting copies the dirty rectangle with a plain loop; there is no vsync or page flip.
- Input and timing are interrupt-driven (see `INTERRUPTS.md`), but the shell is a single
  loop on its own stack, not a scheduled task.
- Fixed pool of 16 windows, of which user programs may hold 8 (`SYSWIN_MAX`); Paint, About
  and Files are single instance, Notes opens a new window each time.
- No Alt+Tab, window snapping, or per-window keyboard shortcuts.
- Windows can run a once-a-second `tick` callback; there is no finer timer API yet.
