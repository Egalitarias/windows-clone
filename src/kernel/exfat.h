#ifndef KERNEL_EXFAT_H
#define KERNEL_EXFAT_H

#include "fat32.h"

// exFAT, behind the same interface as the FAT16/FAT32 driver (fat32.h): fat_mount() recognises an
// exFAT boot sector and the fat_* calls then dispatch here. Everything below is called with
// preemption already off (the fat_* entry points take care of that).
//
// Supported: 512-byte sectors, any cluster size, the allocation bitmap, FAT chains and
// contiguous ("no FAT chain") files, directory entry sets with long names (names are limited to
// ASCII when created; others show as '?'), read, create/replace, write at offsets, mkdir, delete,
// free space. Not supported: the upcase table (names compare case-insensitively for ASCII only),
// files over 4 GiB, writing to files whose valid data length is shorter than their size, TexFAT.

int exfat_probe(const uint8_t *sector0);
int exfat_mount(int dev, fat_volume_t *v, const uint8_t *sector0);
uint64_t exfat_free_bytes(fat_volume_t *v);
void exfat_iter_open(fat_volume_t *v, uint32_t dir_cluster, fat_iter_t *it);
int exfat_iter_next(fat_iter_t *it, fat_dirent_t *out);
int exfat_lookup(fat_volume_t *v, uint32_t dir_cluster, const char *name, fat_dirent_t *out);
int exfat_read_file(fat_volume_t *v, const fat_dirent_t *ent, void *buf, uint32_t max, uint32_t *out_len);
int exfat_write_file(fat_volume_t *v, uint32_t dir_cluster, const char *name, const void *data, uint32_t len);
int exfat_mkdir(fat_volume_t *v, uint32_t dir_cluster, const char *name, uint32_t *new_cluster);
int exfat_delete(fat_volume_t *v, const fat_dirent_t *ent);
int exfat_file_open(fat_volume_t *v, const fat_dirent_t *ent, fat_file_t *f);
int exfat_file_create(fat_volume_t *v, uint32_t dir_cluster, const char *name, fat_file_t *f);
int exfat_file_read(fat_file_t *f, uint32_t offset, void *buf, uint32_t len, uint32_t *out_len);
int exfat_file_write(fat_file_t *f, uint32_t offset, const void *data, uint32_t len, uint32_t *written);

#endif // KERNEL_EXFAT_H
