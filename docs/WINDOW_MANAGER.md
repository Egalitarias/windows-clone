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

- No compositor: windows are repainted by their own callbacks into the shared off-screen
  copy, in z-order. A window cannot be translucent or moved without repainting what it covered.
- Presenting copies the dirty rectangle with a plain loop; there is no vsync or page flip.
- Input and timing are interrupt-driven (see `INTERRUPTS.md`), but the shell is a single
  loop on its own stack, not a scheduled task.
- Fixed pool of 16 windows, of which user programs may hold 8 (`SYSWIN_MAX`); Paint, About
  and Files are single instance, Notes opens a new window each time.
- No Alt+Tab, window snapping, or per-window keyboard shortcuts.
- Windows can run a once-a-second `tick` callback; there is no finer timer API yet.
