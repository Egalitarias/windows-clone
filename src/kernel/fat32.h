#ifndef KERNEL_FAT32_H
#define KERNEL_FAT32_H

#include "console.h"

// FAT32 file system on a block device: long file names, read, create/replace
// files, directories, delete. Writes go straight through to the device (no
// write-back cache). The FSInfo sector is not maintained (its counts are only
// hints). Single-threaded: call from one context only.

#define FAT_NAME_MAX 128      // longest name we keep (long names are cut here)

#define FAT_ATTR_READONLY 0x01
#define FAT_ATTR_HIDDEN   0x02
#define FAT_ATTR_SYSTEM   0x04
#define FAT_ATTR_VOLUME   0x08
#define FAT_ATTR_DIR      0x10
#define FAT_ATTR_ARCHIVE  0x20

typedef struct {
  int dev;                     // block device index
  uint32_t sectors_per_cluster;
  uint32_t reserved_sectors;
  uint32_t num_fats;
  uint32_t fat_size;           // sectors per FAT
  uint32_t root_cluster;
  uint32_t total_sectors;
  uint32_t fat_lba;
  uint32_t data_lba;
  uint32_t cluster_count;
  uint32_t next_free;          // allocation hint
  char label[12];
  uint8_t fat_buf[512];        // cache of the most recent FAT sector
  uint32_t fat_buf_lba;
  int fat_buf_valid;
} fat_volume_t;

typedef struct {
  char name[FAT_NAME_MAX];
  uint8_t sfn[11];             // raw 8.3 name
  uint8_t attr;
  uint32_t size;
  uint32_t first_cluster;
  uint16_t mdate;              // DOS date / time of last write
  uint16_t mtime;
  // Where the record lives, for updates and deletes.
  uint32_t dir_cluster;        // first cluster of the containing directory
  uint32_t sfn_index;          // entry index of the 8.3 record
  uint32_t lfn_count;          // long-name records immediately before it
} fat_dirent_t;

typedef struct {
  fat_volume_t *v;
  uint32_t dir_cluster;
  uint32_t cluster;
  uint32_t index;              // entry index within the current cluster
  uint32_t abs;                // entry index within the whole directory
  uint8_t sec[512];
  uint32_t sec_lba;
  int sec_valid;
  int done;
} fat_iter_t;

// Mount the FAT32 volume on block device `dev`. Returns 1 on success.
int fat_mount(int dev, fat_volume_t *v);

// Directories are identified by their first cluster; the root is v->root_cluster.
void fat_iter_open(fat_volume_t *v, uint32_t dir_cluster, fat_iter_t *it);
// Next visible entry ("." / ".." and the volume label are skipped).
int fat_iter_next(fat_iter_t *it, fat_dirent_t *out);

// Case-insensitive lookup by name in one directory.
int fat_lookup(fat_volume_t *v, uint32_t dir_cluster, const char *name, fat_dirent_t *out);

// Read up to `max` bytes of a file into buf; *out_len gets the bytes read.
int fat_read_file(fat_volume_t *v, const fat_dirent_t *ent, void *buf, uint32_t max, uint32_t *out_len);

// Create the file or replace its contents.
int fat_write_file(fat_volume_t *v, uint32_t dir_cluster, const char *name,
                   const void *data, uint32_t len);

int fat_mkdir(fat_volume_t *v, uint32_t dir_cluster, const char *name, uint32_t *new_cluster);

// Delete a file or an empty directory.
int fat_delete(fat_volume_t *v, const fat_dirent_t *ent);

// Bytes of free space (counted by scanning the FAT; takes a moment on big volumes).
uint64_t fat_free_bytes(fat_volume_t *v);

#endif // KERNEL_FAT32_H
