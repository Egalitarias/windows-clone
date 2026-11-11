#ifndef KERNEL_APPS_H
#define KERNEL_APPS_H

// Desktop applications. Each opens a window through the window
// manager.
#include "fat32.h"

// Notes, Files, Paint and About are user programs (NOTES.BIN, EXPLORER.BIN, PAINT.BIN,
// ABOUT.BIN). The app_launch_*() functions start them from the desktop (call them from the
// desktop task). Files, Paint and About are single instance: a running one is raised.
// Notes opens a new window each time. When a program cannot be started (its file is
// missing, no window slot or task slot is free) a message window says why.
void app_launch_notes(void);
void app_launch_files(void);
void app_launch_paint(void);
void app_launch_about(void);
// A small window with up to three lines of text.
void app_message_open(const char *title, const char *line1, const char *line2, const char *line3);

// Programs: run user-mode programs from /APPS and show their output.
void app_programs_open(void);
void app_programs_run(const char *path, const char *arg);   // open the window and start `path` (arg may be 0)
void app_programs_poll(void);              // called by the desktop loop
// Console: a window showing the kernel log (what console_printf prints).
void app_console_open(void);
void app_console_poll(void);               // called by the desktop loop
// Installer: partitions and formats a disk and copies the boot files onto it.
void app_setup_open(void);

#endif // KERNEL_APPS_H
