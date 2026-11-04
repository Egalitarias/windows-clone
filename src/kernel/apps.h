#ifndef KERNEL_APPS_H
#define KERNEL_APPS_H

// Built-in demo applications. Each opens a new window through the window
// manager (Paint allows a single instance; Notes allows several).
void app_notes_open(void);
void app_paint_open(void);
void app_about_open(void);

#endif // KERNEL_APPS_H
