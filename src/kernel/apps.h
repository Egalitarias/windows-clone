#ifndef KERNEL_APPS_H
#define KERNEL_APPS_H

// Built-in demo applications. Each opens a new window through the window
// manager (Paint allows a single instance; Notes allows several).
#include "fat32.h"

void app_notes_open(void);
// Open an existing text file in a new Notes window (Ctrl+S saves it back).
void app_notes_open_file(fat_volume_t *vol, const fat_dirent_t *ent);
// File explorer (single instance; reopening raises it).
void app_files_open(void);
void app_paint_open(void);
void app_about_open(void);

#endif // KERNEL_APPS_H
