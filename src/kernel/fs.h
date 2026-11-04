#ifndef KERNEL_FS_H
#define KERNEL_FS_H

#include "fat32.h"

// Mounted volumes. fs_init() brings up storage and mounts every FAT32
// partition it finds.

#define FS_MAX_VOLUMES 8

int fs_init(void);

// Re-read partition tables and remount everything (after partitioning or
// formatting). Volume numbers and fat_volume_t contents may change, so anything
// holding a volume pointer must compare fs_generation() first.
int fs_rescan(void);
unsigned int fs_generation(void);
int fs_volume_count(void);
fat_volume_t *fs_volume(int index);
// Where new user files go: the first volume that is not a boot volume (no \EFI
// directory), else the first volume.
fat_volume_t *fs_default_volume(void);
// Short display name: the volume label, or "Disk N" when it has none.
const char *fs_volume_name(int index);

#ifdef KERNEL_FS_SELFTEST
void fs_selftest(void);   // stress test of the FAT32 driver on volume 0
#endif

#endif // KERNEL_FS_H
