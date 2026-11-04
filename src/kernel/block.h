#ifndef KERNEL_BLOCK_H
#define KERNEL_BLOCK_H

#include "console.h"

// Block devices: one entry per partition (or per whole disk when there is no
// partition table). Sector numbers passed to block_read/block_write are
// relative to the start of the device.

#define BLOCK_MAX_DEVICES 16
#define BLOCK_SECTOR_SIZE 512

typedef struct {
  int disk;             // index in the AHCI disk list
  uint64_t start;       // first sector on the disk
  uint64_t sectors;     // length in sectors
  int partition;        // 0 = whole disk, otherwise 1-based partition number
  char name[24];        // e.g. "disk0" or "disk1p1"
} block_dev_t;

// Bring up storage controllers and enumerate devices. Returns the number of
// block devices.
int block_init(void);

int block_count(void);
const block_dev_t *block_get(int index);

int block_read(int dev, uint64_t lba, uint32_t count, void *buf);
int block_write(int dev, uint64_t lba, uint32_t count, const void *buf);

#endif // KERNEL_BLOCK_H
