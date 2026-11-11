#ifndef KERNEL_DISK_H
#define KERNEL_DISK_H

#include "console.h"

// Whole-disk access over every supported controller (SATA/AHCI, NVMe and USB mass storage). The
// block layer builds partitions on top of this. Sector size is 512 bytes.

#define DISK_MAX 12
#define DISK_SECTOR_SIZE 512

typedef enum {
  DISK_BUS_SATA = 1,
  DISK_BUS_NVME = 2,
  DISK_BUS_USB = 3
} disk_bus_t;

typedef struct {
  disk_bus_t bus;
  int driver_index;        // index inside the controller driver
  uint64_t sectors;
  char model[41];
  char name[16];           // "sata0", "nvme0", "usb0"
} disk_info_t;

// Bring up the controllers and enumerate disks. Returns the disk count.
int disk_init(void);

int disk_count(void);
const disk_info_t *disk_get(int index);

int disk_read(int index, uint64_t lba, uint32_t count, void *buf);
int disk_write(int index, uint64_t lba, uint32_t count, const void *buf);

#endif // KERNEL_DISK_H
