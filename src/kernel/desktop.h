#ifndef KERNEL_DESKTOP_H
#define KERNEL_DESKTOP_H

#include "console.h"

// Start the desktop as a scheduler task (no-op when there is no framebuffer).
// Meant to be the scheduler's idle callback.
void desktop_start(void);

// Number of passes the desktop loop has made; zero until it is up and running.
uint64_t desktop_loop_count(void);

// Repaint any pending changes now. Long-running operations (formatting, copying)
// call this between steps so progress shows on screen.
void desktop_pump(void);

#endif // KERNEL_DESKTOP_H
