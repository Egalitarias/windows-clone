#ifndef KERNEL_FAT32_H
#define KERNEL_FAT32_H

#include "console.h"

// FAT16, FAT32 and exFAT (exfat.h) file systems on a block device: long file names, read,
// create/replace files, directories, delete. (FAT12 is not supported.) Writes go
// straight through to the device (no write-back cache). The FSInfo sector is not
// maintained (its counts are only hints). Single-threaded: call from one
// context only.
//
// On FAT16 the root directory is a fixed region, not a cluster chain; it is
// addressed as directory "cluster" 0 (v->root_cluster == 0) and cannot grow.

#define FAT_NAME_MAX 128      // longest name we keep (long names are cut here)

#define FAT_ATTR_READONLY 0x01
#define FAT_ATTR_HIDDEN   0x02
#define FAT_ATTR_SYSTEM   0x04
#define FAT_ATTR_VOLUME   0x08
#define FAT_ATTR_DIR      0x10
#define FAT_ATTR_ARCHIVE  0x20

// exFAT: what a directory listing taught us about a sub-directory (see exfat.c).
#define FAT_EXDIR_CACHE 48
typedef struct {
  uint32_t cluster;            // first cluster of the directory (0 = unused slot)
  uint32_t parent;             // directory that holds its entry
  uint32_t abs;                // record index of its entry in the parent
  uint32_t length;             // bytes
  uint8_t nofat;               // clusters are contiguous, not chained in the FAT
} fat_exdir_t;

typedef struct {
  int dev;                     // block device index
  int exfat;                   // 1 = exFAT: the exfat_* fields below are in use
  uint32_t sectors_per_cluster;
  uint32_t reserved_sectors;
  uint32_t num_fats;
  uint32_t fat_size;           // sectors per FAT
  int fat_bits;                // 16 or 32
  uint32_t eoc_min;            // first end-of-chain value
  uint32_t eoc;                // value written for end-of-chain
  uint32_t root_dir_lba;       // FAT16: first sector of the fixed root directory
  uint32_t root_dir_entries;   // FAT16: capacity of the fixed root directory
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
  // exFAT only (the FAT cache above is not used there):
  uint32_t ex_bitmap_lba;      // first sector of the allocation bitmap
  uint32_t ex_bitmap_bytes;
  uint8_t ex_bm_buf[512];      // cache of the most recent bitmap sector
  uint32_t ex_bm_lba;
  int ex_bm_valid;
  uint8_t ex_fat_buf[512];     // cache of the most recent FAT sector
  uint32_t ex_fat_lba;
  int ex_fat_valid;
  fat_exdir_t ex_dirs[FAT_EXDIR_CACHE];
  uint32_t ex_dir_next;
} fat_volume_t;

typedef struct {
  char name[FAT_NAME_MAX];
  uint8_t sfn[11];             // raw 8.3 name
  uint8_t attr;
  uint32_t size;
  uint32_t first_cluster;
  uint8_t exflags;             // exFAT: stream flags (bit 1 = clusters are contiguous)
  uint32_t valid;              // exFAT: valid data length (never above size)
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
  int fixed_root;              // iterating the FAT16 fixed root directory
  uint8_t nofat;               // exFAT: directory clusters are contiguous
  uint32_t clusters_left;      // exFAT, contiguous directory: clusters after the current one
} fat_iter_t;

// Mount the FAT16, FAT32 or exFAT volume on block device `dev`. Returns 1 on success.
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

// ---- Open files (streaming, for files too big to hold in memory) ------------------------
// A fat_file_t reads and writes a file at byte offsets. It remembers where it last was in
// the cluster chain, so sequential access does not walk the chain from the start each time.
// Writes extend the file as needed and keep the directory record (size, first cluster) up
// to date, so the file is consistent after every call.
typedef struct {
  fat_volume_t *v;
  fat_dirent_t ent;            // the directory record: size and first cluster are kept current
  uint32_t cur_index;          // cursor: index of a cluster in the chain ...
  uint32_t cur_cluster;        // ... and its number (0 = none yet)
} fat_file_t;

// Open an existing file (from fat_lookup / fat_iter_next).
int fat_file_open(fat_volume_t *v, const fat_dirent_t *ent, fat_file_t *f);
// Create the file, or empty it if it exists (not for directories or read-only files).
int fat_file_create(fat_volume_t *v, uint32_t dir_cluster, const char *name, fat_file_t *f);
// Read up to len bytes at offset; *out_len gets the count (0 at or past the end).
int fat_file_read(fat_file_t *f, uint32_t offset, void *buf, uint32_t len, uint32_t *out_len);
// Write len bytes at offset, which must not be past the end of the file. *written gets
// the count (less than len if the volume is full or the file would pass 4 GiB). Returns 0
// only when nothing could be written.
int fat_file_write(fat_file_t *f, uint32_t offset, const void *data, uint32_t len, uint32_t *written);

// Bytes of free space (counted by scanning the FAT; takes a moment on big volumes).
uint64_t fat_free_bytes(fat_volume_t *v);

#endif // KERNEL_FAT32_H
