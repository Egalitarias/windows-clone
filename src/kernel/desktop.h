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

// Show a short message (up to 33 characters) as a toast above the taskbar. It goes
// away after about 5 seconds or when clicked. Call only from the desktop task (the
// shell and the kernel's desktop code), not from user programs' tasks.
void desktop_notify(const char *text);

// The clock's time zone: minutes to add to the RTC (UTC) for local time.
int desktop_tz_offset_min(void);

// Clear the screen and halt (the Start menu's "Shut down"). Does not return.
__attribute__((noreturn)) void desktop_shut_down(void);

#endif // KERNEL_DESKTOP_H
