#ifndef KERNEL_DESKTOP_H
#define KERNEL_DESKTOP_H

// Draw the static demo desktop (no-op when there is no framebuffer).
void desktop_draw_demo(void);

// Repaint any pending changes now. Long-running operations (formatting, copying)
// call this between steps so progress shows on screen.
void desktop_pump(void);

#endif // KERNEL_DESKTOP_H
